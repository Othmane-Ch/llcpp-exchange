// trading/strategy/liquidity_taker.h
//
// LiquidityTaker -- aggressive follow-the-flow strategy.
//
// Reacts to TRADE updates only. When the FeatureEngine's
// agg_trade_qty_ratio_ (fraction of best level consumed by the aggressor)
// exceeds the per-ticker threshold, the strategy takes liquidity in the
// direction of the aggressor:
//
//   aggressor BUY  → buy at the ASK (cross spread → immediate fill)
//   aggressor SELL → sell at the BID (cross spread → immediate fill)
//
// It deliberately quotes Price_INVALID on the opposite side so the
// OrderManager will not place a passive order there. The LT never holds
// both sides simultaneously.
//
// Reacts only to trade updates -- book updates are no-ops here.
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

class TradeEngine;  // forward decl -- ctor takes a TradeEngine*

class LiquidityTaker final {
public:
    LiquidityTaker(Common::Logger *logger,
                   TradeEngine *trade_engine,
                   const FeatureEngine *feature_engine,
                   OrderManager *order_manager,
                   const Common::TradeEngineCfgHashMap &ticker_cfg) noexcept;

    // ── Strategy callbacks (installed on TradeEngine in ctor) ───────────

    /// LT is trade-driven; book updates are observed only for logging.
    auto onOrderBookUpdate(Common::TickerId ticker_id, Common::Price price,
                           Common::Side side, MarketOrderBook *book) noexcept -> void;

    auto onTradeUpdate(const Exchange::MEMarketUpdate *update,
                       MarketOrderBook *book) noexcept -> void;

    /// Observes order responses (logging only). TradeEngine::handleResponse
    /// is the single dispatcher into the OrderManager state machine.
    auto onOrderUpdate(const Exchange::MEClientResponse *response) noexcept -> void;

    LiquidityTaker() = delete;
    LiquidityTaker(const LiquidityTaker &) = delete;
    LiquidityTaker(LiquidityTaker &&) = delete;
    LiquidityTaker &operator=(const LiquidityTaker &) = delete;
    LiquidityTaker &operator=(LiquidityTaker &&) = delete;

private:
    const FeatureEngine                 *feature_engine_ = nullptr;
    OrderManager                        *order_manager_  = nullptr;
    const Common::TradeEngineCfgHashMap  ticker_cfg_;
    Common::Logger                      *logger_         = nullptr;
    std::string                          time_str_;
};

} // namespace Trading
