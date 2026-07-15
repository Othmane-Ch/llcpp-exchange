// trading/strategy/order_manager.h
//
// OrderManager -- manages the strategy's active orders.
//
// At most one order per side per instrument. Transitions orders through
// INVALID → PENDING_NEW → LIVE → PENDING_CANCEL → DEAD driven by exchange
// responses. Pre-trade risk checks via RiskManager before every new order.
//
// The primary entry point is moveOrders(ticker, bid_price, ask_price, clip),
// called by trading strategies each iteration. Passing Price{} (INVALID)
// for a side causes that side's order to be cancelled.
//

#pragma once

#include "types.h"
#include "om_order.h"
#include "logging.h"
#include "macros.h"

namespace Exchange {
    struct MEClientResponse;
}

namespace Trading {

class TradeEngine;
class RiskManager;

class OrderManager final {
public:
    OrderManager(Common::Logger *logger, TradeEngine *trade_engine,
                 const RiskManager &risk_manager) noexcept;

    /// Primary strategy interface: place/update/cancel orders for one ticker.
    /// Pass Price{} (INVALID) to cancel a side.
    auto moveOrders(Common::TickerId ticker_id, Common::Price bid_price,
                    Common::Price ask_price, Common::Qty clip) noexcept -> void;

    /// Handle exchange responses (ACCEPTED, CANCELED, FILLED, etc.).
    auto onOrderUpdate(const Exchange::MEClientResponse *response) noexcept -> void;

    // ── Accessors ───────────────────────────────────────────────────────

    auto getOrder(Common::TickerId tid, Common::Side side) const noexcept
        -> const OMOrder & {
        return ticker_side_order_[tid.value][Common::sideToIndex(side)];
    }

    auto nextOrderId() const noexcept -> Common::OrderId { return next_order_id_; }

    OrderManager() = delete;
    OrderManager(const OrderManager &) = delete;
    OrderManager(OrderManager &&) = delete;
    OrderManager &operator=(const OrderManager &) = delete;
    OrderManager &operator=(OrderManager &&) = delete;

private:
    /// Send a NEW order to the exchange via TradeEngine.
    auto newOrder(OMOrder *order, Common::TickerId ticker_id,
                  Common::Price price, Common::Side side,
                  Common::Qty qty) noexcept -> void;

    /// Send a CANCEL for an existing order via TradeEngine.
    auto cancelOrder(OMOrder *order) noexcept -> void;

    /// Single-order state machine: place, cancel, or wait.
    auto moveOrder(OMOrder *order, Common::TickerId ticker_id,
                   Common::Price price, Common::Side side,
                   Common::Qty qty) noexcept -> void;

    // ── Data members ────────────────────────────────────────────────────

    Common::Logger *logger_ = nullptr;
    TradeEngine    *trade_engine_ = nullptr;
    const RiskManager &risk_manager_;

    OMOrderTickerSideHashMap ticker_side_order_{};
    Common::OrderId next_order_id_{1};

    std::string time_str_;
};

} // namespace Trading
