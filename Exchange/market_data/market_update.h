#pragma once

#include <cstdint>
#include <cstdio>
#include "types.h"
#include "lq_free.h"

using namespace Common;

namespace Exchange {
#pragma pack(push, 1)

    enum class MarketUpdateType : uint8_t {
        INVALID        = 0,
        CLEAR          = 1,
        ADD            = 2,
        MODIFY         = 3,
        CANCEL         = 4,
        TRADE          = 5,
        SNAPSHOT_START = 6,
        SNAPSHOT_END   = 7
    };

    inline std::string marketUpdateTypeToString(MarketUpdateType type) {
        switch (type) {
            case MarketUpdateType::CLEAR:          return "CLEAR";
            case MarketUpdateType::ADD:            return "ADD";
            case MarketUpdateType::MODIFY:         return "MODIFY";
            case MarketUpdateType::CANCEL:         return "CANCEL";
            case MarketUpdateType::TRADE:          return "TRADE";
            case MarketUpdateType::SNAPSHOT_START: return "SNAPSHOT_START";
            case MarketUpdateType::SNAPSHOT_END:   return "SNAPSHOT_END";
            case MarketUpdateType::INVALID:        return "INVALID";
        }
        return "UNKNOWN";
    }

    /// Market update message sent from Matching Engine to Market Data Publisher.
    /// Field ordering: hot-path fields first (type, ticker, side, price, qty),
    /// book-management fields last (order_id, priority).
    /// 34 bytes packed — fits in a single cache line with room to spare.
    struct MEMarketUpdate {
        MarketUpdateType type_ = MarketUpdateType::INVALID; // 1 byte  — offset 0
        TickerId ticker_id_;                                 // 4 bytes — offset 1
        Side side_ = Side::INVALID;                          // 1 byte  — offset 5
        Price price_;                                        // 8 bytes — offset 6
        Qty qty_;                                            // 4 bytes — offset 14
        OrderId order_id_;                                   // 8 bytes — offset 18
        Priority priority_;                                  // 8 bytes — offset 26
                                                             // total:    34 bytes

        auto toString() const -> std::string {
            // Stack buffer, no heap allocation. 256 bytes is plenty for fixed-width fields.
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "MEMarketUpdate [type:%s ticker:%s side:%s price:%s qty:%s oid:%s priority:%s]",
                     marketUpdateTypeToString(type_).c_str(),
                     TickerId::toString(ticker_id_).c_str(),
                     sideToString(side_).c_str(),
                     Price::toString(price_).c_str(),
                     Qty::toString(qty_).c_str(),
                     OrderId::toString(order_id_).c_str(),
                     Priority::toString(priority_).c_str());
            return buf;
        }
    };

    // Compile-time guarantee: struct size must not silently change if fields are
    // added, reordered, or types widened. Any breakage here means the UDP wire
    // format has changed and all consumers must be updated in lockstep.
    static_assert(sizeof(MEMarketUpdate) == 34,
                  "MEMarketUpdate size changed — wire format break. "
                  "Expected 34 bytes with #pragma pack(1).");

    /// Public wire-format wrapper for market data.
    /// Adds a sequence number for UDP gap detection (SBE-style, packed).
    struct MDPMarketUpdate {
        size_t seq_num_ = 0;
        MEMarketUpdate me_market_update_;

        auto toString() const -> std::string {
            char buf[320];
            snprintf(buf, sizeof(buf), "MDPMarketUpdate [seq:%zu %s]",
                     seq_num_, me_market_update_.toString().c_str());
            return buf;
        }
    };

#pragma pack(pop)

    typedef LFQueue<MEMarketUpdate> MEMarketUpdateLFQueue;
    typedef LFQueue<MDPMarketUpdate> MDPMarketUpdateLFQueue;
}
