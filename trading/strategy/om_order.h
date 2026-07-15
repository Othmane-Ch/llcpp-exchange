// trading/strategy/om_order.h
//
// OMOrder -- the strategy's view of its own orders.
//
// Each instrument has at most one order per side (BUY / SELL). The
// OrderManager transitions orders through INVALID → PENDING_NEW → LIVE →
// PENDING_CANCEL → DEAD, driven by exchange responses.
//
// OMOrderSideHashMap / OMOrderTickerSideHashMap provide O(1) lookup by
// (ticker_id, side) using direct array indexing (no hash table).
//

#pragma once

#include <array>
#include <sstream>
#include <string>

#include "types.h"

namespace Trading {

using namespace Common;

// ── OMOrderState ────────────────────────────────────────────────────────────

enum class OMOrderState : uint8_t {
    INVALID        = 0,
    PENDING_NEW    = 1,
    LIVE           = 2,
    PENDING_CANCEL = 3,
    DEAD           = 4
};

inline auto omOrderStateToString(OMOrderState state) -> std::string {
    switch (state) {
        case OMOrderState::INVALID:        return "INVALID";
        case OMOrderState::PENDING_NEW:    return "PENDING_NEW";
        case OMOrderState::LIVE:           return "LIVE";
        case OMOrderState::PENDING_CANCEL: return "PENDING_CANCEL";
        case OMOrderState::DEAD:           return "DEAD";
    }
    return "UNKNOWN";
}

// ── OMOrder ─────────────────────────────────────────────────────────────────

struct OMOrder {
    TickerId     ticker_id_{};
    OrderId      order_id_{};
    Side         side_        = Side::INVALID;
    Price        price_{};
    Qty          qty_{};
    OMOrderState order_state_ = OMOrderState::INVALID;

    auto toString() const -> std::string {
        std::stringstream ss;
        ss << "OMOrder["
           << "ticker:" << TickerId::toString(ticker_id_)
           << " oid:"   << OrderId::toString(order_id_)
           << " side:"  << sideToString(side_)
           << " price:" << Price::toString(price_)
           << " qty:"   << Qty::toString(qty_)
           << " state:" << omOrderStateToString(order_state_)
           << "]";
        return ss.str();
    }
};

/// One order per side (BUY at index 2, SELL at index 0 via sideToIndex).
using OMOrderSideHashMap = std::array<OMOrder, sideToIndex(Side::MAX) + 1>;

/// Per-ticker, per-side order map. TickerId IS the outer index.
using OMOrderTickerSideHashMap = std::array<OMOrderSideHashMap, ME_MAX_TICKERS>;

} // namespace Trading
