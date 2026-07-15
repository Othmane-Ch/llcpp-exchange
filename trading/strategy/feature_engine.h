// trading/strategy/feature_engine.h
//
// FeatureEngine -- computes quantitative trading signals from market data.
//
// Pure arithmetic on BBO data. Two features:
//   - mkt_price_:           quantity-weighted mid price (order book imbalance)
//   - agg_trade_qty_ratio_: fraction of best level consumed by aggressor
//
// No data structures, no allocations, no MemPool. Header-only.
//

#pragma once

#include <cmath>
#include <limits>

#include "types.h"
#include "logging.h"
#include "macros.h"
#include "time_utils.h"

#include "market_order_book.h"  // MarketOrderBook::getBBO()
#include "market_update.h"      // Exchange::MEMarketUpdate

namespace Trading {

constexpr double Feature_INVALID = std::numeric_limits<double>::quiet_NaN();

class FeatureEngine final {
public:
    explicit FeatureEngine(Common::Logger *logger) noexcept
        : logger_(logger) {}

    // ── Signal computation ──────────────────────────────────────────────

    /// Called by TradeEngine after every order book mutation (ADD/MODIFY/CANCEL/CLEAR).
    /// Computes the quantity-weighted mid price:
    ///   mkt_price = (bid_price * ask_qty + ask_price * bid_qty) / (bid_qty + ask_qty)
    auto onOrderBookUpdate(Common::TickerId ticker_id, Common::Price price,
                           Common::Side side, MarketOrderBook *book) noexcept -> void {
        const auto &bbo = book->getBBO();

        if (LIKELY(bbo.bid_price_.isValid() && bbo.ask_price_.isValid())) {
            const double bp = static_cast<double>(bbo.bid_price_.value);
            const double ap = static_cast<double>(bbo.ask_price_.value);
            const double bq = static_cast<double>(bbo.bid_qty_.value);
            const double aq = static_cast<double>(bbo.ask_qty_.value);
            mkt_price_ = (bp * aq + ap * bq) / (bq + aq);
        }

        logger_->log("%:% %() % ticker:% price:% side:% mkt_price:%\n",
                     __FILE__, __LINE__, __FUNCTION__,
                     Common::getCurrentTimeStr(&time_str_),
                     ticker_id.value, price.value, Common::sideToString(side),
                     mkt_price_);
    }

    /// Called by TradeEngine on TRADE market updates.
    /// Computes how much of the best level the aggressor consumed:
    ///   agg_trade_qty_ratio = trade_qty / aggressed_side_qty
    auto onTradeUpdate(const Exchange::MEMarketUpdate *update,
                       MarketOrderBook *book) noexcept -> void {
        const auto &bbo = book->getBBO();

        if (LIKELY(bbo.bid_price_.isValid() && bbo.ask_price_.isValid())) {
            const double trade_qty = static_cast<double>(update->qty_.value);
            // Aggressed side: if trade is BUY (aggressor bought), passive side is ASK.
            const double aggressed_qty = (update->side_ == Common::Side::BUY)
                ? static_cast<double>(bbo.ask_qty_.value)
                : static_cast<double>(bbo.bid_qty_.value);

            if (LIKELY(aggressed_qty > 0)) {
                agg_trade_qty_ratio_ = trade_qty / aggressed_qty;
            }
        }

        logger_->log("%:% %() % trade_qty:% side:% ratio:%\n",
                     __FILE__, __LINE__, __FUNCTION__,
                     Common::getCurrentTimeStr(&time_str_),
                     update->qty_.value,
                     Common::sideToString(update->side_),
                     agg_trade_qty_ratio_);
    }

    // ── Getters ─────────────────────────────────────────────────────────

    auto getMktPrice() const noexcept -> double { return mkt_price_; }
    auto getAggTradeQtyRatio() const noexcept -> double { return agg_trade_qty_ratio_; }

    FeatureEngine() = delete;
    FeatureEngine(const FeatureEngine &) = delete;
    FeatureEngine(FeatureEngine &&) = delete;
    FeatureEngine &operator=(const FeatureEngine &) = delete;
    FeatureEngine &operator=(FeatureEngine &&) = delete;

private:
    double mkt_price_           = Feature_INVALID;
    double agg_trade_qty_ratio_ = Feature_INVALID;

    Common::Logger *logger_ = nullptr;
    std::string time_str_;
};

} // namespace Trading
