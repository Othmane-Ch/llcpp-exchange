// trading/strategy/market_maker.h
//
// MarketMaker -- passive quoting strategy.
//
// Quotes both sides of the book, joining or stepping back from the BBO based on
// where the FeatureEngine's qty-weighted mid (mkt_price_) sits relative to the
// best bid/ask:
//
//   bid_offset = (fair_price - best_bid >= threshold) ? 0 : 1
//   ask_offset = (best_ask - fair_price >= threshold) ? 0 : 1
//
// The 1-tick offset penalises sides where the fair value does not support the
// best price -- the MM steps back rather than getting picked off. When fair
// value DOES support a side, the MM joins the BBO directly.
//
// Reacts only to book updates -- trade events are no-ops here.
//
// Not the owner of any sub-component. Holds non-owning pointers to the
// FeatureEngine (read-only, fair-price source) and OrderManager (downstream
// effector). The TradeEngine owns the lifetimes; the MarketMaker is wired in
// via the engine's existing callback API in its constructor.
//

#pragma once

#include <string>

#include "types.h"
#include "logging.h"
#include "macros.h"
#include "time_utils.h"

#include "client_response.h"   // Exchange::MEClientResponse
#include "market_update.h"     // Exchange::MEMarketUpdate
#include "feature_engine.h"
#include "order_manager.h"
#include "market_order_book.h" // Trading::MarketOrderBook + BBO

namespace Trading {

class TradeEngine;  // forward decl -- constructor takes a TradeEngine*

class MarketMaker final {
public:
    MarketMaker(Common::Logger *logger,
                TradeEngine *trade_engine,
                const FeatureEngine *feature_engine,
                OrderManager *order_manager,
                const Common::TradeEngineCfgHashMap &ticker_cfg) noexcept;

    // ── Strategy callbacks (installed on TradeEngine in ctor) ───────────

    auto onOrderBookUpdate(Common::TickerId ticker_id, Common::Price price,
                           Common::Side side, MarketOrderBook *book) noexcept -> void;

    /// MM is BBO-driven; trade events are observed only for logging.
    auto onTradeUpdate(const Exchange::MEMarketUpdate *update,
                       MarketOrderBook *book) noexcept -> void;

    /// Observes order responses (logging only). TradeEngine::handleResponse
    /// is the single dispatcher into the OrderManager state machine.
    auto onOrderUpdate(const Exchange::MEClientResponse *response) noexcept -> void;

    MarketMaker() = delete;
    MarketMaker(const MarketMaker &) = delete;
    MarketMaker(MarketMaker &&) = delete;
    MarketMaker &operator=(const MarketMaker &) = delete;
    MarketMaker &operator=(MarketMaker &&) = delete;

private:
    const FeatureEngine                 *feature_engine_ = nullptr;
    OrderManager                        *order_manager_  = nullptr;
    const Common::TradeEngineCfgHashMap  ticker_cfg_;
    Common::Logger                      *logger_         = nullptr;
    std::string                          time_str_;
};

} // namespace Trading
