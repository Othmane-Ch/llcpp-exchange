// trading/strategy/market_maker.cpp

#include "market_maker.h"
#include "trade_engine.h"
#include "perf_utils.h"

namespace Trading {

using namespace Common;

// ── Constructor ─────────────────────────────────────────────────────────────
//
// Wires the three callbacks onto the TradeEngine. The existing TradeEngine
// uses setBookUpdateCallback / setTradeUpdateCallback / setOrderUpdateCallback
// (functionally identical to writing public algo* std::function members).
//
MarketMaker::MarketMaker(Logger *logger,
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

// ── BBO-driven quoting logic ────────────────────────────────────────────────

auto MarketMaker::onOrderBookUpdate(TickerId ticker_id, Price price, Side side,
                                    MarketOrderBook *book) noexcept -> void {
    logger_->log("%:% %() % MM book update ticker:% price:% side:%\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 getCurrentTimeStr(&time_str_),
                 ticker_id.value, Price::toString(price),
                 sideToString(side));

    const auto &bbo = book->getBBO();
    const double fair_price = feature_engine_->getMktPrice();

    if (LIKELY(bbo.bid_price_.isValid() && bbo.ask_price_.isValid()
               && !std::isnan(fair_price))) {
        const auto &cfg       = ticker_cfg_.at(ticker_id.value);
        const auto  clip      = cfg.clip_;
        const auto  threshold = cfg.threshold_;

        const double bid_p = static_cast<double>(bbo.bid_price_.value);
        const double ask_p = static_cast<double>(bbo.ask_price_.value);

        // Join (offset 0) when fair value supports the price; step back
        // (offset 1) when it does not.
        const auto bid_price = Price{bbo.bid_price_.value
                                     - ((fair_price - bid_p) >= threshold ? 0u : 1u)};
        const auto ask_price = Price{bbo.ask_price_.value
                                     + ((ask_p - fair_price) >= threshold ? 0u : 1u)};

        START_MEASURE(Trading_OrderManager_moveOrders);
        order_manager_->moveOrders(ticker_id, bid_price, ask_price, clip);
        END_MEASURE(Trading_OrderManager_moveOrders, (*logger_));
    }
}

// ── Trade observer (no-op for MM) ───────────────────────────────────────────

auto MarketMaker::onTradeUpdate(const Exchange::MEMarketUpdate *update,
                                MarketOrderBook * /*book*/) noexcept -> void {
    logger_->log("%:% %() % MM trade observed %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 getCurrentTimeStr(&time_str_),
                 update->toString());
}

// ── Order-update observer ───────────────────────────────────────────────────
//
// TradeEngine::handleResponse is the single dispatcher into the OrderManager
// state machine; forwarding here as well would process every response twice.
// The strategy only observes its order updates.

auto MarketMaker::onOrderUpdate(const Exchange::MEClientResponse *response) noexcept -> void {
    logger_->log("%:% %() % MM order update %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 getCurrentTimeStr(&time_str_),
                 response->toString());
}

} // namespace Trading
