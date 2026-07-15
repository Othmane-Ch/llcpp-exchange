
#pragma once

#include <array>
#include "me_order.h"
#include "mem_pool.h"
#include "logging.h"
#include "latency_tracker.h"
#include "client_request.h"
#include "client_response.h"
#include "market_update.h"

using namespace Common;

namespace Exchange {

class MatchingEngine;

class MEOrderBook final {
public:
    explicit MEOrderBook(TickerId ticker_id, Logger *logger, MatchingEngine *matching_engine);
    ~MEOrderBook();

    auto add(ClientId client_id, OrderId client_order_id, TickerId ticker_id,
             Side side, Price price, Qty qty) noexcept -> void;

    auto cancel(ClientId client_id, OrderId order_id, TickerId ticker_id) noexcept -> void;

    auto toString(bool detailed, bool validity_check) const -> std::string;

    // Public for testing -- O(1) direct-mapped lookup by price hash.
    auto getOrdersAtPrice(Price price) const noexcept -> MEOrdersAtPrice * {
        return price_orders_at_price_.at(priceToIndex(price));
    }

    // Read-only access to performance stats (aggregated in C++).
    auto getPerfStats() const -> Common::PerfStats { return latency_tracker_.getStats(); }
    auto resetPerfStats() noexcept -> void { latency_tracker_.reset(); }

    MEOrderBook() = delete;
    MEOrderBook(const MEOrderBook &) = delete;
    MEOrderBook(const MEOrderBook &&) = delete;
    MEOrderBook &operator=(const MEOrderBook &) = delete;
    MEOrderBook &operator=(const MEOrderBook &&) = delete;

private:
    auto generateNextMarketOrderId() noexcept -> OrderId {
        return next_market_order_id_++;
    }

    static auto priceToIndex(Price price) noexcept -> size_t {
        return static_cast<size_t>(price.value % ME_MAX_PRICE_LEVELS);
    }

    auto getNextPriority(Price price) noexcept -> Priority;

    auto addOrdersAtPrice(MEOrdersAtPrice *oap) noexcept -> void;
    auto removeOrdersAtPrice(Side side, Price price) noexcept -> void;

    auto addOrder(MEOrder *order) noexcept -> void;
    auto removeOrder(MEOrder *order) noexcept -> void;

    auto checkForMatch(ClientId client_id, OrderId client_oid, TickerId ticker_id,
                       Side side, Price price, Qty qty,
                       OrderId new_moid) noexcept -> Qty;

    auto match(TickerId ticker_id, ClientId client_id, Side side,
               OrderId client_oid, OrderId new_moid,
               MEOrder *passive, Qty *leaves_qty) noexcept -> void;

    TickerId        ticker_id_;
    MatchingEngine *matching_engine_ = nullptr;

    // Reused per-call; avoids repeated construction on the hot path.
    MEClientResponse client_response_{};
    MEMarketUpdate   market_update_{};

    // Sorted level lists: bids descending, asks ascending by price.
    MEOrdersAtPrice *bids_by_price_ = nullptr;
    MEOrdersAtPrice *asks_by_price_ = nullptr;

    // O(1) direct-mapped lookups -- std::array, no heap allocation.
    ClientOrderHashMap   cid_oid_to_order_{};
    OrdersAtPriceHashMap price_orders_at_price_{};

    OrderId next_market_order_id_ = OrderId(1);

    MemPool<MEOrdersAtPrice> orders_at_price_pool_;
    MemPool<MEOrder>         order_pool_;

    mutable Common::LatencyTracker latency_tracker_;

    std::string time_str_;
    Logger     *logger_ = nullptr;
};

using OrderBookHashMap = std::array<MEOrderBook *, ME_MAX_TICKERS>;

} // namespace Exchange
