# Limit Order-Book Matching Engine

A single-header, price-time priority matching engine written in modern C++17.
It models the core of what a stock exchange runs: it accepts buy/sell orders,
matches them against each other according to exchange rules, and reports the
resulting trades — all in memory, with O(1) cancels and multi-million
events/sec throughput.

---
## Why this exists

Every stock/crypto exchange (NSE, NASDAQ, Binance, etc.) runs some version of
this component at its heart. It's a great systems-design exercise because it
forces you to reason about:

- **Fairness rules** (price-time priority) that must be enforced exactly.
- **Throughput** — real exchanges process millions of orders per second.
- **O(1) mutation** on a data structure that must also stay sorted.

This project implements that component from scratch, with unit tests for
correctness and a benchmark for performance.

---

## Core concepts

| Term | Meaning |
|---|---|
| **Limit order** | "Buy/sell N units at price P or better." Rests in the book if it can't fully match immediately. |
| **Market order** | "Buy/sell N units right now, at whatever price is available." Never rests — unfilled quantity is discarded. |
| **Bid** | A resting buy order. The **best bid** is the highest price a buyer is willing to pay. |
| **Ask** | A resting sell order. The **best ask** is the lowest price a seller is willing to accept. |
| **Spread** | Best ask − best bid. |
| **Maker** | The order that was already resting in the book (it "made" liquidity). |
| **Taker** | The incoming order that matches against a maker (it "takes" liquidity). |
| **Fill** | A (partial or full) execution of an order's quantity. |

---

## How matching works

### 1. Price priority
An incoming **buy** order matches against the **cheapest** resting sell.
An incoming **sell** order matches against the **highest** resting buy.
This maximizes value for whoever is willing to trade at the best price.

### 2. Time priority (FIFO)
Among multiple orders resting at the *same* price, the **oldest one fills
first**. This rewards being early and prevents queue-jumping.

### 3. Crossing condition
A trade can only happen if the incoming order's price "crosses" the resting
order's price:

- Incoming **buy** at price `P` crosses a resting **sell** at price `Q` if `P >= Q`.
- Incoming **sell** at price `P` crosses a resting **buy** at price `Q` if `P <= Q`.

Trades always execute **at the maker's price** (the resting order's price),
not the taker's — this is standard exchange behavior.

### Example walkthrough

```
Book starts empty.

1. SELL 5 @ 101  (order #1)         -> rests, no cross
   Book: asks = [101: 5]

2. SELL 5 @ 101  (order #2)         -> rests behind #1 (same price, later)
   Book: asks = [101: 5+5]  (FIFO: #1 then #2)

3. BUY  5 @ 101  (order #3)         -> crosses; fills against #1 (oldest first)
   Trade: maker=#1, taker=#3, price=101, qty=5
   Book: asks = [101: 5]  (#2 remains)

4. BUY  10 @ 105 (order #4)         -> crosses; fills #2 fully (qty 5),
                                        then 5 units have no more sellers,
                                        so 5 units rest as a new BID @ 105
   Trade: maker=#2, taker=#4, price=101, qty=5
   Book: bids = [105: 5], asks = []
```

---

## Design & data structures

```
Side::Buy  -> bids_  : std::map<Price, Level>   (best = highest price = rbegin())
Side::Sell -> asks_  : std::map<Price, Level>   (best = lowest price  = begin())

Level {
    std::list<Order> orders;   // FIFO = time priority within a price level
    Quantity total_qty;
}

index_ : std::unordered_map<OrderId, Locator>   // Locator = {side, price, list::iterator}
```

**Why a `std::map` for price levels?**
It keeps prices sorted automatically (O(log n) insert), so "best price" is
always just `begin()` or `rbegin()` — no scanning needed.

**Why a `std::list` inside each level, not a `std::vector`?**
A `std::list` gives O(1) removal from the middle via a stored iterator, and
iterators into a `std::list` remain valid even as other elements are added or
removed. A `std::vector` would need O(n) shifting on every cancel and would
invalidate iterators on reallocation — both unacceptable at high throughput.

**How cancel becomes O(1):**
When an order is placed, its exact position (a `std::list<Order>::iterator`)
is stored in a hash map (`index_`) keyed by order ID. Canceling is then a
hash lookup + direct list-node erase — no searching through price levels at
all.

**Why integer prices ("ticks"), not floats?**
Floating-point arithmetic (`0.1 + 0.2 != 0.3`) can silently corrupt price
comparisons. Real exchanges represent prices as integers (e.g. paise/cents,
or a fixed tick size) for exactness — this engine does the same.

---

## API reference

All types and functions live in namespace `lob` (header: `order_book.hpp`).

### Types

```cpp
enum class Side : uint8_t { Buy, Sell };

using OrderId  = uint64_t;
using Price    = int64_t;   // integer ticks, not float
using Quantity = uint64_t;

struct Trade {
    OrderId  taker_id;   // the incoming order that caused the match
    OrderId  maker_id;   // the resting order that got matched
    Price    price;      // trade price = the maker's price
    Quantity qty;        // quantity executed in this fill
};
```

### `class OrderBook`

| Method | Description |
|---|---|
| `OrderBook(TradeCallback cb = nullptr)` | Optional callback fired on every trade, in addition to the returned vector. |
| `std::vector<Trade> add_limit(OrderId id, Side side, Price px, Quantity qty)` | Places a limit order. Matches immediately as far as possible; any remainder rests in the book. Returns all trades generated. |
| `std::vector<Trade> add_market(OrderId id, Side side, Quantity qty)` | Places a market order. Matches at any available price until filled or the opposite side is empty. Unfilled remainder is **discarded** (never rests). |
| `bool cancel(OrderId id)` | Removes a resting order in O(1). Returns `false` if the ID doesn't exist (already filled or already canceled). |
| `std::optional<Price> best_bid() const` | Highest resting buy price, if any. |
| `std::optional<Price> best_ask() const` | Lowest resting sell price, if any. |
| `size_t open_orders() const` | Number of currently-resting orders. |

---

## Usage

Drop `order_book.hpp` into your project — it's header-only, no build step,
no dependencies beyond the standard library.

```cpp
#include "order_book.hpp"
#include <cstdio>

int main() {
    using namespace lob;

    // Optional: get a live callback on every trade (e.g. to publish a market feed)
    OrderBook book([](const Trade& t) {
        std::printf("TRADE  maker=%llu taker=%llu price=%lld qty=%llu\n",
                    (unsigned long long)t.maker_id, (unsigned long long)t.taker_id,
                    (long long)t.price, (unsigned long long)t.qty);
    });

    OrderId next_id = 1;

    // Two sellers rest in the book
    book.add_limit(next_id++, Side::Sell, /*price=*/101, /*qty=*/5);
    book.add_limit(next_id++, Side::Sell, /*price=*/102, /*qty=*/5);

    // A buyer crosses the spread -> generates trades
    auto trades = book.add_limit(next_id++, Side::Buy, /*price=*/103, /*qty=*/8);
    std::printf("Trades from this order: %zu\n", trades.size());

    // Market order: fill immediately at whatever price is available
    book.add_market(next_id++, Side::Buy, /*qty=*/2);

    // Cancel a still-resting order
    book.cancel(/*id=*/1);   // no-op / false if already filled

    if (auto bb = book.best_bid()) std::printf("Best bid: %lld\n", (long long)*bb);
    if (auto ba = book.best_ask()) std::printf("Best ask: %lld\n", (long long)*ba);
}
```

**Typical integration pattern:** feed it a stream of order events (from a
network socket, a CSV of historical orders, a UI, etc.), call `add_limit` /
`add_market` / `cancel` for each, and consume the returned `Trade` vectors
(or the callback) to update a ledger, a UI, or a market-data feed.

---

## Building & running

Requires a C++17 compiler (g++, clang++). No external dependencies.

```bash
# Compile the test suite + benchmark
g++ -std=c++17 -O2 -Wall -Wextra -o engine tests.cpp

# Run it
./engine
```

Expected output:
```
All 8 test groups passed.
Processed 5000000 events in ~1.0s -> ~5.0M events/sec (open orders: N)
```

To use just the engine in your own program (without the test file):
```bash
g++ -std=c++17 -O2 -o myapp myapp.cpp   # as long as myapp.cpp includes order_book.hpp
```

---

## Testing

`tests.cpp` includes 8 focused unit-test groups covering:

1. Resting order with no cross
2. Full match at the maker's price
3. Partial fill with remainder resting
4. Time priority (FIFO) at the same price level
5. Price priority across multiple price levels
6. Market order sweeping the book, with unfilled remainder discarded
7. Cancel correctness, including double-cancel returning `false`
8. Canceling a mid-queue order and verifying matching correctly skips it

Each check uses a `CHECK(condition)` macro that prints the failing file/line
and exits with a non-zero code, so it's CI-friendly (no external test
framework needed).

---

## Benchmark

The benchmark in `tests.cpp` generates 5,000,000 randomized events (40% limit
buys, 40% limit sells, 10% cancels, 10% market orders) across a 21-tick price
range and times the whole run:

```
Processed 5000000 events in 1.00s -> 4.99M events/sec
```

Actual numbers depend on your CPU; run it locally to reproduce.

---

## Complexity summary

| Operation | Complexity | Why |
|---|---|---|
| Add limit order (no match) | O(log L) | `L` = number of distinct price levels; map insert |
| Add limit order (k fills) | O(k · 1 + log L) | Each fill pops from a list front (O(1)); level removal is O(log L) |
| Cancel | O(1) | Hash lookup + list-node erase via stored iterator |
| Best bid / best ask | O(1) | `map::rbegin()` / `map::begin()` |

---

## Limitations & possible extensions

This engine intentionally keeps scope tight for clarity. Natural next steps:

- **Order types**: IOC (immediate-or-cancel), FOK (fill-or-kill), stop orders.
- **Order modification**: cancel-replace ("amend") without losing time priority incorrectly.
- **Multi-symbol support**: currently one `OrderBook` = one instrument; wrap in a map of symbol → `OrderBook`.
- **Persistence / recovery**: currently entirely in-memory; a real system would journal events for crash recovery.
- **Concurrency**: single-threaded by design (matching engines are typically single-threaded per symbol to keep ordering deterministic); a multi-symbol system would run one engine per thread/core.
- **Self-trade prevention**: no check currently prevents the same participant from trading with themselves.
