#pragma once
// Limit Order-Book Matching Engine (C++17)
// Price-time priority: best price matches first; within a price level,
// earliest order matches first (FIFO). O(1) cancels via iterator map.

#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>
#include <functional>
#include <optional>

namespace lob {

enum class Side : uint8_t { Buy, Sell };

using OrderId  = uint64_t;
using Price    = int64_t;   // price in ticks (integer, avoids float errors)
using Quantity = uint64_t;

struct Order {
    OrderId  id;
    Side     side;
    Price    price;
    Quantity qty;      // remaining quantity
};

struct Trade {
    OrderId  taker_id;   // incoming order
    OrderId  maker_id;   // resting order
    Price    price;      // executes at the maker's (resting) price
    Quantity qty;
};

class OrderBook {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    explicit OrderBook(TradeCallback cb = nullptr) : on_trade_(std::move(cb)) {}

    // Add a limit order. Matches immediately against the opposite side
    // as far as prices cross; any remainder rests in the book.
    // Returns trades executed.
    std::vector<Trade> add_limit(OrderId id, Side side, Price px, Quantity qty) {
        std::vector<Trade> trades;
        if (side == Side::Buy)  match_incoming(asks_, id, side, px, qty, trades);
        else                    match_incoming(bids_, id, side, px, qty, trades);
        if (qty > 0) rest_order(id, side, px, qty);   // remainder rests
        return trades;
    }

    // Market order: match at any price until filled or book side is empty.
    // Unfilled remainder is discarded (no resting).
    std::vector<Trade> add_market(OrderId id, Side side, Quantity qty) {
        std::vector<Trade> trades;
        constexpr Price kNoLimit_Buy  = INT64_MAX;
        constexpr Price kNoLimit_Sell = INT64_MIN;
        if (side == Side::Buy)  match_incoming(asks_, id, side, kNoLimit_Buy,  qty, trades);
        else                    match_incoming(bids_, id, side, kNoLimit_Sell, qty, trades);
        return trades;
    }

    // O(1) cancel: hash lookup -> erase list node via stored iterator.
    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        const Locator& loc = it->second;
        auto& level = (loc.side == Side::Buy) ? bids_[loc.price] : asks_[loc.price];
        level.orders.erase(loc.pos);              // O(1) list erase
        level.total_qty -= loc.pos->qty;
        if (level.orders.empty()) {
            if (loc.side == Side::Buy) bids_.erase(loc.price);
            else                       asks_.erase(loc.price);
        }
        index_.erase(it);
        return true;
    }

    std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.rbegin()->first;             // highest buy price
    }
    std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;              // lowest sell price
    }
    size_t open_orders() const { return index_.size(); }

private:
    struct Level {
        std::list<Order> orders;   // FIFO queue = time priority
        Quantity total_qty = 0;
    };
    // bids: iterate from rbegin (highest first). asks: from begin (lowest first).
    using BookSide = std::map<Price, Level>;

    struct Locator {
        Side side;
        Price price;
        std::list<Order>::iterator pos;   // stable iterator -> O(1) erase
    };

    BookSide bids_, asks_;
    std::unordered_map<OrderId, Locator> index_;
    TradeCallback on_trade_;

    static bool crosses(Side incoming, Price incoming_px, Price resting_px) {
        return incoming == Side::Buy ? incoming_px >= resting_px
                                     : incoming_px <= resting_px;
    }

    void match_incoming(BookSide& opposite, OrderId id, Side side,
                        Price px, Quantity& qty, std::vector<Trade>& out) {
        while (qty > 0 && !opposite.empty()) {
            // Best opposite level: lowest ask for a buyer, highest bid for a seller.
            auto lvl_it = (side == Side::Buy) ? opposite.begin()
                                              : std::prev(opposite.end());
            Price resting_px = lvl_it->first;
            if (!crosses(side, px, resting_px)) break;   // prices no longer cross

            Level& level = lvl_it->second;
            while (qty > 0 && !level.orders.empty()) {
                Order& maker = level.orders.front();     // oldest = time priority
                Quantity fill = std::min(qty, maker.qty);
                Trade t{id, maker.id, resting_px, fill};
                out.push_back(t);
                if (on_trade_) on_trade_(t);
                qty            -= fill;
                maker.qty      -= fill;
                level.total_qty-= fill;
                if (maker.qty == 0) {                    // maker fully filled
                    index_.erase(maker.id);
                    level.orders.pop_front();
                }
            }
            if (level.orders.empty()) opposite.erase(lvl_it);
        }
    }

    void rest_order(OrderId id, Side side, Price px, Quantity qty) {
        Level& level = (side == Side::Buy) ? bids_[px] : asks_[px];
        level.orders.push_back(Order{id, side, px, qty});
        level.total_qty += qty;
        index_[id] = Locator{side, px, std::prev(level.orders.end())};
    }
};

} // namespace lob
