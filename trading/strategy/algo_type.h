// trading/strategy/algo_type.h
//
// AlgoType -- selects which strategy plug-in trading_main instantiates.
//
// RANDOM is for end-to-end smoke testing (random new + cancel orders).
// MAKER  selects MarketMaker     (passive quoting around fair value).
// TAKER  selects LiquidityTaker  (aggressive follow-the-aggressor).
//

#pragma once

#include <cstdint>
#include <string>

namespace Trading {

enum class AlgoType : uint8_t {
    INVALID = 0,
    RANDOM  = 1,
    MAKER   = 2,
    TAKER   = 3,
    MAX     = 4
};

inline auto algoTypeToString(AlgoType t) -> std::string {
    switch (t) {
        case AlgoType::RANDOM: return "RANDOM";
        case AlgoType::MAKER:  return "MAKER";
        case AlgoType::TAKER:  return "TAKER";
        case AlgoType::INVALID:return "INVALID";
        case AlgoType::MAX:    return "MAX";
    }
    return "UNKNOWN";
}

inline auto stringToAlgoType(const std::string &s) -> AlgoType {
    if (s == "RANDOM") return AlgoType::RANDOM;
    if (s == "MAKER")  return AlgoType::MAKER;
    if (s == "TAKER")  return AlgoType::TAKER;
    return AlgoType::INVALID;
}

} // namespace Trading
