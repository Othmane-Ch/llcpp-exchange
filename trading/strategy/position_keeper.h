// trading/strategy/position_keeper.h
//
// PositionInfo  -- per-instrument position, PnL, and VWAP tracking.
// PositionKeeper -- routes fills and BBO updates to the correct PositionInfo.
//
// Core accounting engine for the trading client. Every fill from the exchange
// flows through addFill(), which updates position, realized PnL, unrealized
// PnL, and open VWAP. BBO changes (without fills) update unrealized PnL via
// updateBBO().
//

#pragma once

#include <array>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

#include "types.h"
#include "logging.h"

#include "market_order.h"      // Trading::BBO
#include "client_response.h"   // Exchange::MEClientResponse

namespace Trading {

using namespace Common;

// ── PositionInfo ────────────────────────────────────────────────────────────

struct PositionInfo {
    int32_t  position_  = 0;
    double   real_pnl_  = 0;
    double   unreal_pnl_ = 0;
    double   total_pnl_ = 0;
    Qty      volume_    = Qty{0};
    const BBO *bbo_     = nullptr;

    // Running sum of price * qty per side. Used for VWAP computation.
    // Indexed by sideToIndex(). Array size covers all Side values.
    std::array<double, sideToIndex(Side::MAX) + 1> open_vwap_{};

    /// Process a fill from the exchange. Updates position, VWAP, realized
    /// and unrealized PnL. This is the core PnL engine.
    auto addFill(const Exchange::MEClientResponse *response,
                 Logger *logger) noexcept -> void {
        const auto side = response->side_;
        const int s = sideToValue(side);
        const double p = static_cast<double>(response->price_.value);
        const auto q = static_cast<int32_t>(response->exec_qty_.value);
        const auto old_position = position_;

        // Step 1: Update position.
        position_ += s * q;

        // Step 2: Update volume.
        volume_ = Qty{volume_.value + response->exec_qty_.value};

        // Step 3: Branch on position direction change.
        if (old_position * s >= 0) {
            // Increasing or opening position.
            open_vwap_[sideToIndex(side)] += p * q;
        } else {
            // Decreasing or flipping position.
            const auto opp_side = (side == Side::BUY) ? Side::SELL : Side::BUY;
            const auto oi = sideToIndex(opp_side);
            const double vwap_opp = open_vwap_[oi] / std::abs(old_position);
            open_vwap_[oi] = vwap_opp * std::abs(position_);

            const auto qty_closed = std::min(q, std::abs(old_position));
            real_pnl_ += qty_closed * (vwap_opp - p) * s;

            // Check if position actually flipped sign.
            if (static_cast<int64_t>(position_) * old_position < 0) {
                open_vwap_[sideToIndex(side)] = p * std::abs(position_);
                open_vwap_[oi] = 0;
            }
        }

        // Step 4: Unrealized PnL.
        if (position_ == 0) {
            open_vwap_.fill(0);
            unreal_pnl_ = 0;
        } else if (position_ > 0) {
            unreal_pnl_ = (p - open_vwap_[sideToIndex(Side::BUY)] / std::abs(position_))
                          * std::abs(position_);
        } else {
            unreal_pnl_ = (open_vwap_[sideToIndex(Side::SELL)] / std::abs(position_) - p)
                          * std::abs(position_);
        }

        // Step 5: Total PnL.
        total_pnl_ = real_pnl_ + unreal_pnl_;

        if (logger) {
            logger->log("PositionInfo::addFill pos:% vol:% rpnl:% upnl:% tpnl:%\n",
                        position_, volume_.value, real_pnl_, unreal_pnl_, total_pnl_);
        }
    }

    /// Recompute unrealized PnL from the current mid price (no fill).
    /// Called when the order book changes but no execution occurred.
    auto updateBBO(const BBO *bbo, Logger *logger) noexcept -> void {
        bbo_ = bbo;

        if (position_ != 0 && bbo_->bid_price_.isValid() && bbo_->ask_price_.isValid()) {
            const double mid = (static_cast<double>(bbo_->bid_price_.value) +
                                static_cast<double>(bbo_->ask_price_.value)) * 0.5;

            if (position_ > 0) {
                unreal_pnl_ = (mid - open_vwap_[sideToIndex(Side::BUY)] / std::abs(position_))
                              * std::abs(position_);
            } else {
                unreal_pnl_ = (open_vwap_[sideToIndex(Side::SELL)] / std::abs(position_) - mid)
                              * std::abs(position_);
            }
            total_pnl_ = real_pnl_ + unreal_pnl_;
        }

        if (logger) {
            logger->log("PositionInfo::updateBBO pos:% upnl:% tpnl:%\n",
                        position_, unreal_pnl_, total_pnl_);
        }
    }

    auto toString() const -> std::string {
        std::stringstream ss;
        ss << "PositionInfo["
           << "pos:" << position_
           << " vol:" << volume_.value
           << " rpnl:" << real_pnl_
           << " upnl:" << unreal_pnl_
           << " tpnl:" << total_pnl_
           << "]";
        return ss.str();
    }
};

// ── PositionKeeper ──────────────────────────────────────────────────────────

class PositionKeeper final {
public:
    explicit PositionKeeper(Logger *logger) noexcept
        : logger_(logger) {}

    auto addFill(const Exchange::MEClientResponse *response) noexcept -> void {
        ticker_position_.at(response->ticker_id_.value).addFill(response, logger_);
    }

    auto updateBBO(TickerId ticker_id, const BBO *bbo) noexcept -> void {
        ticker_position_.at(ticker_id.value).updateBBO(bbo, logger_);
    }

    auto getPositionInfo(TickerId ticker_id) const noexcept -> const PositionInfo * {
        return &ticker_position_.at(ticker_id.value);
    }

    auto toString() const -> std::string {
        std::string out = "PositionKeeper:\n";
        for (size_t i = 0; i < ticker_position_.size(); ++i) {
            const auto &pi = ticker_position_[i];
            if (pi.position_ != 0 || pi.volume_.value != 0) {
                out += "  ticker:" + std::to_string(i) + " " + pi.toString() + "\n";
            }
        }
        return out;
    }

    PositionKeeper() = delete;
    PositionKeeper(const PositionKeeper &) = delete;
    PositionKeeper(PositionKeeper &&) = delete;
    PositionKeeper &operator=(const PositionKeeper &) = delete;
    PositionKeeper &operator=(PositionKeeper &&) = delete;

private:
    std::array<PositionInfo, ME_MAX_TICKERS> ticker_position_{};
    Logger *logger_ = nullptr;
};

} // namespace Trading
