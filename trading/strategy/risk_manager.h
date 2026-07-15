// trading/strategy/risk_manager.h
//
// RiskManager -- pre-trade risk checks for the trading client.
//
// Three checks in order (fast rejection):
//   1. Order size vs max_order_size_
//   2. Projected position vs max_position_
//   3. Current total PnL vs max_loss_ threshold
//
// All checks are per-instrument. RiskInfo holds the per-ticker state
// (a const pointer to PositionInfo for position/PnL reads, plus the
// configured limits). RiskManager routes by TickerId.
//

#pragma once

#include <array>
#include <cmath>
#include <string>

#include "types.h"
#include "position_keeper.h"
#include "logging.h"
#include "macros.h"

namespace Trading {

using namespace Common;

// ── RiskCheckResult ─────────────────────────────────────────────────────────

enum class RiskCheckResult : uint8_t {
    INVALID            = 0,
    ORDER_TOO_LARGE    = 1,
    POSITION_TOO_LARGE = 2,
    LOSS_TOO_LARGE     = 3,
    ALLOWED            = 4
};

inline auto riskCheckResultToString(RiskCheckResult r) -> std::string {
    switch (r) {
        case RiskCheckResult::INVALID:            return "INVALID";
        case RiskCheckResult::ORDER_TOO_LARGE:    return "ORDER_TOO_LARGE";
        case RiskCheckResult::POSITION_TOO_LARGE: return "POSITION_TOO_LARGE";
        case RiskCheckResult::LOSS_TOO_LARGE:     return "LOSS_TOO_LARGE";
        case RiskCheckResult::ALLOWED:            return "ALLOWED";
    }
    return "UNKNOWN";
}

// ── RiskInfo (per-instrument) ───────────────────────────────────────────────

struct RiskInfo {
    const PositionInfo *position_info_ = nullptr;
    RiskCfg risk_cfg_;

    auto checkPreTradeRisk(Side side, Qty qty) const noexcept -> RiskCheckResult {
        // 1. Order size check.
        if (UNLIKELY(qty > risk_cfg_.max_order_size_))
            return RiskCheckResult::ORDER_TOO_LARGE;

        // 2. Position limit check.
        const auto new_pos = position_info_->position_
                           + sideToValue(side) * static_cast<int32_t>(qty.value);
        if (UNLIKELY(static_cast<int64_t>(std::abs(new_pos)) >
                     static_cast<int64_t>(risk_cfg_.max_position_.value)))
            return RiskCheckResult::POSITION_TOO_LARGE;

        // 3. Loss limit check.
        if (UNLIKELY(position_info_->total_pnl_ < risk_cfg_.max_loss_))
            return RiskCheckResult::LOSS_TOO_LARGE;

        return RiskCheckResult::ALLOWED;
    }
};

// ── RiskManager ─────────────────────────────────────────────────────────────

class RiskManager final {
public:
    RiskManager(Logger *logger, const PositionKeeper *position_keeper,
                const TradeEngineCfgHashMap &cfg_map);

    auto checkPreTradeRisk(TickerId ticker_id, Side side, Qty qty) const noexcept
        -> RiskCheckResult {
        return ticker_risk_.at(ticker_id.value).checkPreTradeRisk(side, qty);
    }

    RiskManager() = delete;
    RiskManager(const RiskManager &) = delete;
    RiskManager(RiskManager &&) = delete;
    RiskManager &operator=(const RiskManager &) = delete;
    RiskManager &operator=(RiskManager &&) = delete;

private:
    std::array<RiskInfo, ME_MAX_TICKERS> ticker_risk_{};
    Logger *logger_ = nullptr;
};

} // namespace Trading
