// trading/strategy/risk_manager.cpp

#include "risk_manager.h"

namespace Trading {

using namespace Common;

RiskManager::RiskManager(Logger *logger, const PositionKeeper *position_keeper,
                         const TradeEngineCfgHashMap &cfg_map)
    : logger_(logger) {
    for (uint32_t i = 0; i < ME_MAX_TICKERS; ++i) {
        ticker_risk_[i].position_info_ = position_keeper->getPositionInfo(TickerId{i});
        ticker_risk_[i].risk_cfg_      = cfg_map[i].risk_cfg_;
    }

    logger_->log("RiskManager initialised for % tickers\n", ME_MAX_TICKERS);
}

} // namespace Trading
