// unordered_map_me_order_book.h
//
// Hash-map variant of MEOrderBook for §5 benchmark.
//
// Replaces the two `std::array`-based direct-mapped lookups with
// `std::unordered_map`:
//
//   - cid_oid_to_order_     : std::unordered_map<ClientId,
//                                std::unordered_map<OrderId, MEOrder *>>
//   - price_orders_at_price_: std::unordered_map<Price, MEOrdersAtPrice *>
//
// Everything else (intrusive doubly-linked lists for price levels and
// orders, MemPool-backed allocation, matching algorithm, response /
// market-update emission) is unchanged.
//
// This class exists only as a benchmark target. The production matcher
// keeps using MEOrderBook.
//

#pragma once

#include <array>
#include <unordered_map>
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

// Hash specialization for StrongType so we can use Price / ClientId /
// OrderId as unordered_map keys without writing custom hashers everywhere.
struct StrongTypeHash {
    template<typename T>
    size_t operator()(const T &k) const noexcept {
        return std::hash<typename std::remove_reference<decltype(k.value)>::type>{}(k.value);
    }
};

class UnorderedMapMEOrderBook final {
public:
    explicit UnorderedMapMEOrderBook(TickerId ticker_id, Logger *logger, MatchingEngine *matching_engine);
    ~UnorderedMapMEOrderBook();

    auto add(ClientId client_id, OrderId client_order_id, TickerId ticker_id,
             Side side, Price price, Qty qty) noexcept -> void;

    auto cancel(ClientId client_id, OrderId order_id, TickerId ticker_id) noexcept -> void;

    auto toString(bool detailed, bool validity_check) const -> std::string;

    auto getOrdersAtPrice(Price price) const noexcept -> MEOrdersAtPrice * {
        auto it = price_orders_at_price_.find(price);
        return (it != price_orders_at_price_.end()) ? it->second : nullptr;
    }

    auto getPerfStats() const -> Common::PerfStats { return latency_tracker_.getStats(); }
    auto resetPerfStats() noexcept -> void { latency_tracker_.reset(); }

    UnorderedMapMEOrderBook() = delete;
    UnorderedMapMEOrderBook(const UnorderedMapMEOrderBook &) = delete;
    UnorderedMapMEOrderBook(const UnorderedMapMEOrderBook &&) = delete;
    UnorderedMapMEOrderBook &operator=(const UnorderedMapMEOrderBook &) = delete;
    UnorderedMapMEOrderBook &operator=(const UnorderedMapMEOrderBook &&) = delete;

private:
    auto generateNextMarketOrderId() noexcept -> OrderId {
        return next_market_order_id_++;
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

    MEClientResponse client_response_{};
    MEMarketUpdate   market_update_{};

    MEOrdersAtPrice *bids_by_price_ = nullptr;
    MEOrdersAtPrice *asks_by_price_ = nullptr;

    // Hash-map variants: chained-buckets, heap-backed nodes.
    std::unordered_map<ClientId,
        std::unordered_map<OrderId, MEOrder *, StrongTypeHash>,
        StrongTypeHash> cid_oid_to_order_;
    std::unordered_map<Price, MEOrdersAtPrice *, StrongTypeHash> price_orders_at_price_;

    OrderId next_market_order_id_ = OrderId(1);

    MemPool<MEOrdersAtPrice> orders_at_price_pool_;
    MemPool<MEOrder>         order_pool_;

    mutable Common::LatencyTracker latency_tracker_;

    std::string time_str_;
    Logger     *logger_ = nullptr;
};

} // namespace Exchange
