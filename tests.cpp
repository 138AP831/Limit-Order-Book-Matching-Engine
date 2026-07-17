// Unit tests + throughput benchmark for the matching engine.
#include "order_book.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>

using namespace lob;

#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); return 1; } \
    } while (0)

static int run_tests() {
    // 1. Resting order, no cross
    {
        OrderBook ob;
        auto t = ob.add_limit(1, Side::Buy, 100, 10);
        CHECK(t.empty());
        CHECK(ob.best_bid() == Price{100});
        CHECK(!ob.best_ask().has_value());
    }
    // 2. Simple full match at maker's price
    {
        OrderBook ob;
        ob.add_limit(1, Side::Sell, 101, 5);
        auto t = ob.add_limit(2, Side::Buy, 105, 5);  // crosses; fills at 101
        CHECK(t.size() == 1);
        CHECK(t[0].price == 101 && t[0].qty == 5);
        CHECK(t[0].maker_id == 1 && t[0].taker_id == 2);
        CHECK(ob.open_orders() == 0);
    }
    // 3. Partial fill, remainder rests
    {
        OrderBook ob;
        ob.add_limit(1, Side::Sell, 101, 5);
        auto t = ob.add_limit(2, Side::Buy, 101, 8);
        CHECK(t.size() == 1 && t[0].qty == 5);
        CHECK(ob.best_bid() == Price{101});           // 3 left resting
        CHECK(ob.open_orders() == 1);
    }
    // 4. Time priority (FIFO) within a price level
    {
        OrderBook ob;
        ob.add_limit(1, Side::Sell, 101, 5);
        ob.add_limit(2, Side::Sell, 101, 5);          // same price, later
        auto t = ob.add_limit(3, Side::Buy, 101, 5);
        CHECK(t.size() == 1 && t[0].maker_id == 1);   // oldest fills first
    }
    // 5. Price priority across levels
    {
        OrderBook ob;
        ob.add_limit(1, Side::Sell, 102, 5);
        ob.add_limit(2, Side::Sell, 101, 5);          // better price
        auto t = ob.add_limit(3, Side::Buy, 105, 10);
        CHECK(t.size() == 2);
        CHECK(t[0].maker_id == 2 && t[0].price == 101);  // best price first
        CHECK(t[1].maker_id == 1 && t[1].price == 102);
    }
    // 6. Market order sweeps, remainder discarded
    {
        OrderBook ob;
        ob.add_limit(1, Side::Sell, 101, 4);
        auto t = ob.add_market(2, Side::Buy, 10);
        CHECK(t.size() == 1 && t[0].qty == 4);
        CHECK(ob.open_orders() == 0);                 // nothing rests
    }
    // 7. Cancel removes order; double-cancel fails
    {
        OrderBook ob;
        ob.add_limit(1, Side::Buy, 100, 10);
        CHECK(ob.cancel(1));
        CHECK(!ob.cancel(1));
        CHECK(!ob.best_bid().has_value());
        auto t = ob.add_limit(2, Side::Sell, 99, 5);  // would have crossed
        CHECK(t.empty());                              // but 1 is gone
    }
    // 8. Cancel middle of a FIFO queue, matching skips it
    {
        OrderBook ob;
        ob.add_limit(1, Side::Sell, 101, 5);
        ob.add_limit(2, Side::Sell, 101, 5);
        ob.add_limit(3, Side::Sell, 101, 5);
        ob.cancel(2);
        auto t = ob.add_limit(4, Side::Buy, 101, 10);
        CHECK(t.size() == 2);
        CHECK(t[0].maker_id == 1 && t[1].maker_id == 3);
    }
    std::printf("All 8 test groups passed.\n");
    return 0;
}

static void run_benchmark() {
    OrderBook ob;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> px(90, 110);
    std::uniform_int_distribution<int> qty(1, 100);
    std::uniform_int_distribution<int> action(0, 9);

    const size_t N = 5'000'000;
    std::vector<OrderId> live; live.reserve(N);
    OrderId next_id = 1;

    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < N; ++i) {
        int a = action(rng);
        if (a < 4) {                                   // 40% limit buy
            ob.add_limit(next_id, Side::Buy, px(rng), qty(rng));
            live.push_back(next_id++);
        } else if (a < 8) {                            // 40% limit sell
            ob.add_limit(next_id, Side::Sell, px(rng), qty(rng));
            live.push_back(next_id++);
        } else if (a == 8 && !live.empty()) {          // 10% cancel
            ob.cancel(live[rng() % live.size()]);
        } else {                                       // 10% market
            ob.add_market(next_id++, (a & 1) ? Side::Buy : Side::Sell, qty(rng));
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("Processed %zu events in %.2fs -> %.2fM events/sec (open orders: %zu)\n",
                N, sec, N / sec / 1e6, ob.open_orders());
}

int main() {
    if (int rc = run_tests()) return rc;
    run_benchmark();
    return 0;
}
