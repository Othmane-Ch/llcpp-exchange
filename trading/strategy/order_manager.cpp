// trading/strategy/order_manager.cpp

#include "order_manager.h"
#include "trade_engine.h"
#include "risk_manager.h"
#include "client_request.h"
#include "client_response.h"
#include "time_utils.h"
#include "perf_utils.h"

namespace Trading {

using namespace Common;

// ── Constructor ─────────────────────────────────────────────────────────────

OrderManager::OrderManager(Logger *logger, TradeEngine *trade_engine,
                           const RiskManager &risk_manager) noexcept
    : logger_(logger),
      trade_engine_(trade_engine),
      risk_manager_(risk_manager) {}

// ── Send NEW order ──────────────────────────────────────────────────────────

auto OrderManager::newOrder(OMOrder *order, TickerId ticker_id,
                            Price price, Side side, Qty qty) noexcept -> void {
    Exchange::MEClientRequest request{};
    request.type_      = Exchange::ClientRequestType::NEW;
    request.client_id_ = trade_engine_->clientId();
    request.ticker_id_ = ticker_id;
    request.order_id_  = next_order_id_;
    request.side_      = side;
    request.price_     = price;
    request.qty_       = qty;

    trade_engine_->sendClientRequest(&request);

    order->ticker_id_   = ticker_id;
    order->order_id_    = next_order_id_;
    order->side_        = side;
    order->price_       = price;
    order->qty_         = qty;
    order->order_state_ = OMOrderState::PENDING_NEW;

    ++next_order_id_;

    logger_->log("%:% %() % Sent NEW %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 getCurrentTimeStr(&time_str_),
                 order->toString());
}

// ── Send CANCEL ─────────────────────────────────────────────────────────────

auto OrderManager::cancelOrder(OMOrder *order) noexcept -> void {
    Exchange::MEClientRequest request{};
    request.type_      = Exchange::ClientRequestType::CANCEL;
    request.client_id_ = trade_engine_->clientId();
    request.ticker_id_ = order->ticker_id_;
    request.order_id_  = order->order_id_;
    request.side_      = order->side_;
    request.price_     = order->price_;
    request.qty_       = order->qty_;

    trade_engine_->sendClientRequest(&request);

    order->order_state_ = OMOrderState::PENDING_CANCEL;

    logger_->log("%:% %() % Sent CANCEL %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 getCurrentTimeStr(&time_str_),
                 order->toString());
}

// ── Single-order state machine ──────────────────────────────────────────────

auto OrderManager::moveOrder(OMOrder *order, TickerId ticker_id,
                             Price price, Side side, Qty qty) noexcept -> void {
    switch (order->order_state_) {
        case OMOrderState::LIVE: {
            if (order->price_ != price || order->qty_ != qty) {
                START_MEASURE(Trading_OrderManager_cancelOrder);
                cancelOrder(order);
                END_MEASURE(Trading_OrderManager_cancelOrder, (*logger_));
            }
        } break;

        case OMOrderState::INVALID:
        case OMOrderState::DEAD: {
            if (price.isValid()) {
                START_MEASURE(Trading_RiskManager_checkPreTradeRisk);
                const auto risk = risk_manager_.checkPreTradeRisk(ticker_id, side, qty);
                END_MEASURE(Trading_RiskManager_checkPreTradeRisk, (*logger_));
                if (LIKELY(risk == RiskCheckResult::ALLOWED)) {
                    START_MEASURE(Trading_OrderManager_newOrder);
                    newOrder(order, ticker_id, price, side, qty);
                    END_MEASURE(Trading_OrderManager_newOrder, (*logger_));
                } else {
                    logger_->log("%:% %() % Risk REJECTED ticker:% side:% qty:% reason:%\n",
                                 __FILE__, __LINE__, __FUNCTION__,
                                 getCurrentTimeStr(&time_str_),
                                 ticker_id.value, sideToString(side), qty.value,
                                 riskCheckResultToString(risk));
                }
            }
        } break;

        case OMOrderState::PENDING_NEW:
        case OMOrderState::PENDING_CANCEL:
            // Wait for exchange response before taking further action.
            break;
    }
}

// ── Primary strategy entry point ────────────────────────────────────────────

auto OrderManager::moveOrders(TickerId ticker_id, Price bid_price,
                              Price ask_price, Qty clip) noexcept -> void {
    auto *bid_order = &ticker_side_order_[ticker_id.value][sideToIndex(Side::BUY)];
    START_MEASURE(Trading_OrderManager_moveOrder_bid);
    moveOrder(bid_order, ticker_id, bid_price, Side::BUY, clip);
    END_MEASURE(Trading_OrderManager_moveOrder_bid, (*logger_));

    auto *ask_order = &ticker_side_order_[ticker_id.value][sideToIndex(Side::SELL)];
    START_MEASURE(Trading_OrderManager_moveOrder_ask);
    moveOrder(ask_order, ticker_id, ask_price, Side::SELL, clip);
    END_MEASURE(Trading_OrderManager_moveOrder_ask, (*logger_));
}

// ── Exchange response handler ───────────────────────────────────────────────

auto OrderManager::onOrderUpdate(const Exchange::MEClientResponse *response) noexcept -> void {
    auto *order = &ticker_side_order_[response->ticker_id_.value][sideToIndex(response->side_)];

    logger_->log("%:% %() % %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 getCurrentTimeStr(&time_str_),
                 response->toString());

    switch (response->type_) {
        case Exchange::ClientResponseType::ACCEPTED:
            order->order_state_ = OMOrderState::LIVE;
            break;

        case Exchange::ClientResponseType::CANCELED:
            order->order_state_ = OMOrderState::DEAD;
            break;

        case Exchange::ClientResponseType::FILLED:
            order->qty_ = response->leaves_qty_;
            if (order->qty_ == Qty{0}) {
                order->order_state_ = OMOrderState::DEAD;
            }
            break;

        case Exchange::ClientResponseType::CANCEL_REJECTED:
        case Exchange::ClientResponseType::INVALID:
            break;
    }
}

} // namespace Trading
