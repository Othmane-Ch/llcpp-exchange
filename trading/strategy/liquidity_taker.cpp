// trading/strategy/liquidity_taker.cpp

#include "liquidity_taker.h"
#include "trade_engine.h"
#include "perf_utils.h"

namespace Trading {

using namespace Common;

// ── Constructor ─────────────────────────────────────────────────────────────

LiquidityTaker::LiquidityTaker(Logger *logger,
                               TradeEngine *trade_engine,
                               const FeatureEngine *feature_engine,
                               OrderManager *order_manager,
                               const TradeEngineCfgHashMap &ticker_cfg) noexcept
    : feature_engine_(feature_engine),
      order_manager_(order_manager),
      ticker_cfg_(ticker_cfg),
      logger_(logger) {
    trade_engine->setBookUpdateCallback(
        [this](TickerId t, Price p, Side s, MarketOrderBook *b) noexcept {
            onOrderBookUpdate(t, p, s, b);
        });
    trade_engine->setTradeUpdateCallback(
        [this](const Exchange::MEMarketUpdate *u, MarketOrderBook *b) noexcept {
            onTradeUpdate(u, b);
        });
    trade_engine->setOrderUpdateCallback(
        [this](const Exchange::MEClientResponse *r) noexcept {
            onOrderUpdate(r);
        });
}

// ── Book observer (no-op for LT) ────────────────────────────────────────────

auto LiquidityTaker::onOrderBookUpdate(TickerId ticker_id, Price price, Side side,
                                       MarketOrderBook * /*book*/) noexcept -> void {
    logger_->log("%:% %() % LT book observed ticker:% price:% side:%\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 getCurrentTimeStr(&time_str_),
                 ticker_id.value, Price::toString(price),
                 sideToString(side));
}

// ── Trade-driven aggression logic ───────────────────────────────────────────

auto LiquidityTaker::onTradeUpdate(const Exchange::MEMarketUpdate *update,
                                   MarketOrderBook *book) noexcept -> void {
    logger_->log("%:% %() % LT trade %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 getCurrentTimeStr(&time_str_),
                 update->toString());

    const auto &bbo = book->getBBO();
    const double agg_qty_ratio = feature_engine_->getAggTradeQtyRatio();

    if (LIKELY(bbo.bid_price_.isValid() && bbo.ask_price_.isValid()
               && !std::isnan(agg_qty_ratio))) {
        const auto &cfg       = ticker_cfg_.at(update->ticker_id_.value);
        const auto  clip      = cfg.clip_;
        const auto  threshold = cfg.threshold_;

        if (agg_qty_ratio >= threshold) {
            START_MEASURE(OrderManager_moveOrders);
            if (update->side_ == Side::BUY) {
                // Aggressor bought → join the chase: buy at ask (immediate cross).
                order_manager_->moveOrders(update->ticker_id_,
                                           bbo.ask_price_, Price{}, clip);
            } else {
                // Aggressor sold → sell at bid (immediate cross).
                order_manager_->moveOrders(update->ticker_id_,
                                           Price{}, bbo.bid_price_, clip);
            }
            END_MEASURE(OrderManager_moveOrders, (*logger_));
        }
    }
}

// ── Order-update observer ───────────────────────────────────────────────────
//
// TradeEngine::handleResponse is the single dispatcher into the OrderManager
// state machine; forwarding here as well would process every response twice.
// The strategy only observes its order updates.

auto LiquidityTaker::onOrderUpdate(const Exchange::MEClientResponse *response) noexcept -> void {
    logger_->log("%:% %() % LT order update %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 getCurrentTimeStr(&time_str_),
                 response->toString());
}

} // namespace Trading
