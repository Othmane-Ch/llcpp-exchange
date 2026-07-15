// trading/strategy/market_order_book.h
//
// Client-side reconstructed order book.
//
// Consumes MEMarketUpdate messages (ADD / MODIFY / CANCEL / TRADE / CLEAR /
// SNAPSHOT_START / SNAPSHOT_END) and mirrors the exchange's order book state
// locally. It does NOT match -- matching happens on the exchange side and we
// only observe the outcome via TRADE + follow-up CANCEL/MODIFY updates.
//
// Symmetry with Exchange::MEOrderBook:
//   - Same intrusive circular DLL structure for orders (FIFO) and levels
//     (sorted by aggressiveness).
//   - Same direct-indexed hash arrays: OrderId IS the outer index, and
//     price_orders_at_price_[priceToIndex(p)] lookups are O(1).
//   - Same reuse of addOrder / removeOrder / addOrdersAtPrice /
//     removeOrdersAtPrice as the core mutation primitives.
//
// Low-latency guarantees:
//   - MemPool<MarketOrder> and MemPool<MarketOrdersAtPrice> -- zero heap on
//     the hot path.
//   - noexcept on all hot-path methods.
//   - LIKELY / UNLIKELY branch hints.
//   - No virtual functions, class is final.
//

#pragma once

#include <string>

#include "types.h"
#include "mem_pool.h"
#include "logging.h"
#include "macros.h"

#include "market_update.h"   // Exchange::MEMarketUpdate
#include "market_order.h"    // Trading::MarketOrder, MarketOrdersAtPrice, BBO

namespace Trading {

class MarketOrderBook final {
public:
    /// ticker_id is informational -- one MarketOrderBook per ticker.
    /// logger is owned by the caller and must outlive this object.
    MarketOrderBook(Common::TickerId ticker_id, Common::Logger *logger) noexcept;

    ~MarketOrderBook();

    /// Dispatch a single incremental update. Safe to call with any
    /// MarketUpdateType -- TRADE/CLEAR/SNAPSHOT_* are handled as needed.
    auto onMarketUpdate(const Exchange::MEMarketUpdate *update) noexcept -> void;

    // ── Accessors (used by strategies and tests) ────────────────────────

    /// Top-of-book snapshot. Refreshed after every applied update.
    auto getBBO() const noexcept -> const BBO & { return bbo_; }

    /// O(1) lookup of a resting order by its exchange-assigned market order id.
    auto getOrder(Common::OrderId oid) const noexcept -> const MarketOrder * {
        return (oid.value < oid_to_order_.size()) ? oid_to_order_[oid.value] : nullptr;
    }

    /// O(1) lookup of a price level by price (via the priceToIndex hash).
    auto getOrdersAtPrice(Common::Price price) const noexcept
        -> const MarketOrdersAtPrice * {
        return price_orders_at_price_[priceToIndex(price)];
    }

    /// Head of the sorted side chain (bids descending, asks ascending).
    auto getBidsByPrice() const noexcept -> const MarketOrdersAtPrice * { return bids_by_price_; }
    auto getAsksByPrice() const noexcept -> const MarketOrdersAtPrice * { return asks_by_price_; }

    /// Human-readable book snapshot (side-walk, O(levels * orders)).
    auto toString(bool detailed) const -> std::string;

    MarketOrderBook() = delete;
    MarketOrderBook(const MarketOrderBook &) = delete;
    MarketOrderBook(MarketOrderBook &&) = delete;
    MarketOrderBook &operator=(const MarketOrderBook &) = delete;
    MarketOrderBook &operator=(MarketOrderBook &&) = delete;

private:
    // ── Mutation primitives (mirror Exchange::MEOrderBook) ──────────────

    static auto priceToIndex(Common::Price price) noexcept -> size_t {
        return static_cast<size_t>(price.value % Common::ME_MAX_PRICE_LEVELS);
    }

    /// Insert a new level into the sorted side chain.
    auto addOrdersAtPrice(MarketOrdersAtPrice *new_level) noexcept -> void;

    /// Remove the level at (side, price). Deallocates it back to the pool.
    /// Caller must ensure the level is already empty of orders.
    auto removeOrdersAtPrice(Common::Side side, Common::Price price) noexcept -> void;

    /// Add an order to its price level (tail insert, FIFO). Creates the level
    /// if it does not exist.
    auto addOrder(MarketOrder *order) noexcept -> void;

    /// Remove an order from its price level. Deallocates it back to the pool.
    /// If it was the last order at that price, the level is also removed.
    auto removeOrder(MarketOrder *order) noexcept -> void;

    /// Recompute top-of-book from the current bids / asks heads. Cheap.
    auto updateBBO() noexcept -> void;

    /// Wipe all state -- used for CLEAR and SNAPSHOT_START.
    auto clearBook() noexcept -> void;

    // ── Data members ────────────────────────────────────────────────────

    Common::TickerId ticker_id_;
    Common::Logger  *logger_ = nullptr;

    /// Sorted side chains (circular DLLs). Head is the most aggressive level.
    MarketOrdersAtPrice *bids_by_price_ = nullptr;
    MarketOrdersAtPrice *asks_by_price_ = nullptr;

    /// Direct-mapped hash by price index -- O(1) level lookup.
    OrdersAtPriceMap price_orders_at_price_{};

    /// Direct-indexed hash by market order id -- O(1) order lookup.
    /// The client reconstructs only exchange-visible orders, so a single
    /// ME_MAX_ORDER_IDS array suffices (no client dimension).
    OrderMap oid_to_order_{};

    /// Zero-allocation pools for book nodes.
    Common::MemPool<MarketOrdersAtPrice> orders_at_price_pool_;
    Common::MemPool<MarketOrder>         order_pool_;

    /// Cached top-of-book, refreshed after each applied update.
    BBO bbo_{};

    std::string time_str_;
};

} // namespace Trading
