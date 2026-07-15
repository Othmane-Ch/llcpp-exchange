// trading/strategy/market_order.h
//
// Client-side order, price-level, and BBO types.
//
// These mirror the exchange-side MEOrder / MEOrdersAtPrice structures but
// carry no client-id state: the client's reconstructed book reflects what
// the market looks like, not who owns what. Only the strategy's own orders
// (tracked separately inside OrderManager, out of scope here) need client
// identity.
//
// Design rationale:
//   - Intrusive circular doubly-linked lists -- FIFO within a price, sorted
//     across a side. Same approach as Exchange::MEOrder / MEOrdersAtPrice
//     so that client and exchange book implementations stay in lockstep.
//   - No heap allocation: pointers come from MemPool on the book side.
//   - Zero-cost strong types from Common::types.
//

#pragma once

#include <array>
#include <sstream>
#include <string>

#include "types.h"

namespace Trading {

using namespace Common;

// ── MarketOrder ──────────────────────────────────────────────────────────────
//
// A single resting order on the client-reconstructed book. The order_id here
// is the MARKET order id assigned by the exchange (what the incremental feed
// carries). No client_id -- the client doesn't know which participant placed
// which order, only that it exists.

struct MarketOrder {
    OrderId  order_id_{};
    Side     side_     = Side::INVALID;
    Price    price_{};
    Qty      qty_{};
    Priority priority_{};

    MarketOrder *prev_order_ = nullptr;
    MarketOrder *next_order_ = nullptr;

    MarketOrder() = default;

    MarketOrder(OrderId order_id, Side side, Price price, Qty qty,
                Priority priority,
                MarketOrder *prev_order, MarketOrder *next_order) noexcept
        : order_id_(order_id), side_(side), price_(price), qty_(qty),
          priority_(priority),
          prev_order_(prev_order), next_order_(next_order) {}

    auto toString() const -> std::string {
        std::stringstream ss;
        ss << "MarketOrder["
           << "oid:"      << OrderId::toString(order_id_)
           << " side:"    << sideToString(side_)
           << " price:"   << Price::toString(price_)
           << " qty:"     << Qty::toString(qty_)
           << " prio:"    << Priority::toString(priority_)
           << " prev:"    << (prev_order_ ? OrderId::toString(prev_order_->order_id_) : "null")
           << " next:"    << (next_order_ ? OrderId::toString(next_order_->order_id_) : "null")
           << "]";
        return ss.str();
    }
};

/// Direct-indexed hash map: OrderId IS the index.
/// Matches Exchange::OrderHashMap shape so the reconstruction logic is symmetric.
using OrderMap = std::array<MarketOrder *, ME_MAX_ORDER_IDS>;

// ── MarketOrdersAtPrice ──────────────────────────────────────────────────────
//
// One price level, holding a circular doubly-linked FIFO list of orders at
// that price. Levels themselves form a sorted circular doubly-linked list
// across the side (bids descending, asks ascending).

struct MarketOrdersAtPrice {
    Side  side_  = Side::INVALID;
    Price price_{};

    MarketOrder *first_mkt_order_ = nullptr;

    MarketOrdersAtPrice *prev_entry_ = nullptr;
    MarketOrdersAtPrice *next_entry_ = nullptr;

    MarketOrdersAtPrice() = default;

    MarketOrdersAtPrice(Side side, Price price, MarketOrder *first_mkt_order,
                        MarketOrdersAtPrice *prev_entry,
                        MarketOrdersAtPrice *next_entry) noexcept
        : side_(side), price_(price), first_mkt_order_(first_mkt_order),
          prev_entry_(prev_entry), next_entry_(next_entry) {}

    auto toString() const -> std::string {
        std::stringstream ss;
        ss << "MarketOrdersAtPrice["
           << "side:"  << sideToString(side_)
           << " price:" << Price::toString(price_)
           << " first:" << (first_mkt_order_ ? first_mkt_order_->toString() : "null")
           << " prev:"  << (prev_entry_ ? Price::toString(prev_entry_->price_) : "null")
           << " next:"  << (next_entry_ ? Price::toString(next_entry_->price_) : "null")
           << "]";
        return ss.str();
    }
};

/// Direct-mapped hash array from price-hash to level pointer (O(1) lookup).
using OrdersAtPriceMap = std::array<MarketOrdersAtPrice *, ME_MAX_PRICE_LEVELS>;

// ── BBO (Best Bid / Offer) ───────────────────────────────────────────────────
//
// A compact snapshot of the top of book. Strategies that care only about the
// inside market (market makers, mean-reversion, etc.) read the BBO instead of
// walking the full book on every update -- cheaper and cache-friendlier.
//
// The MarketOrderBook updates the BBO after every applied MEMarketUpdate.

struct BBO {
    Price bid_price_ = Price{}; // INVALID if no bids
    Price ask_price_ = Price{}; // INVALID if no asks
    Qty   bid_qty_   = Qty{0};  // aggregate qty at top bid
    Qty   ask_qty_   = Qty{0};  // aggregate qty at top ask

    auto toString() const -> std::string {
        std::stringstream ss;
        ss << "BBO["
           << "bid:"   << Price::toString(bid_price_)
           << " x "    << Qty::toString(bid_qty_)
           << " | ask:" << Price::toString(ask_price_)
           << " x "    << Qty::toString(ask_qty_)
           << "]";
        return ss.str();
    }
};

} // namespace Trading
