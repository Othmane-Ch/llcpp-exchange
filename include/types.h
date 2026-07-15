#pragma once
#include <array>
#include <cstdint>
#include <limits>

#include "macros.h"

namespace Common {
    // Capacity profile. The direct-indexed arrays sized off these constants
    // dominate the process footprint (the per-book client×order-id map alone
    // is ME_MAX_NUM_CLIENTS × ME_MAX_ORDER_IDS × 8 bytes). Two profiles:
    //
    //   default              — dedicated hardware; full-size direct-indexed
    //                          arrays (multi-GB footprint).
    //   LLCPP_SMALL_FOOTPRINT — identical architecture and code paths with
    //                          right-sized capacities; whole exchange +
    //                          client fit comfortably in <1 GB RAM. Enable
    //                          with: cmake -B build -DLLCPP_SMALL_FOOTPRINT=ON
#if defined(LLCPP_SMALL_FOOTPRINT)
    constexpr size_t LOG_QUEUE_SIZE = 64 * 1024;

    constexpr size_t ME_MAX_TICKERS = 4;

    constexpr size_t ME_MAX_CLIENT_UPDATES = 64 * 1024;
    constexpr size_t ME_MAX_MARKET_UPDATES = 64 * 1024;

    constexpr size_t ME_MAX_NUM_CLIENTS = 16;
    constexpr size_t ME_MAX_ORDER_IDS = 128 * 1024;
    constexpr size_t ME_MAX_PRICE_LEVELS = 256;
#else
    constexpr size_t LOG_QUEUE_SIZE = 1024 * 1024;

    constexpr size_t ME_MAX_TICKERS = 8;

    constexpr size_t ME_MAX_CLIENT_UPDATES = 256 * 1024;
    constexpr size_t ME_MAX_MARKET_UPDATES = 256 * 1024;

    constexpr size_t ME_MAX_NUM_CLIENTS = 256;
    constexpr size_t ME_MAX_ORDER_IDS = 1024 * 1024;
    constexpr size_t ME_MAX_PRICE_LEVELS = 256;
#endif

    template<typename T, typename Tag>
    struct StrongType {
        T value;

        // default and explicit constructors
        StrongType() : value(INVALID) {};
        explicit StrongType(T v) : value(v) {};

        // Sentinel
        static constexpr T INVALID = std::numeric_limits<T>::max();

        // Comparison operators
        bool operator==(const StrongType& other) const { return value == other.value; }
        bool operator!=(const StrongType& other) const { return value != other.value; }
        bool operator<(const StrongType& other) const { return value < other.value; }
        bool operator>(const StrongType& other) const { return value > other.value; }

        // Increment operators
        StrongType& operator++() { ++value; return *this; }  // pre-increment
        StrongType operator++(int) { StrongType tmp = *this; ++value; return tmp; }  // post-increment

        // check validity
        bool isValid() const { return value != INVALID; }

        // logging
        static inline auto toString(const StrongType& t) -> std::string {
            if (UNLIKELY(t.value == INVALID)) {
                return "INVALID";
            }
            return std::to_string(t.value);
        }
    };

    // Tags for different strong types (zero bytes, can be used for type safety without runtime overhead)
    struct OrderIdTag {};
    struct TickerIdTag {};
    struct ClientIdTag {};
    struct PriceTag {};
    struct QtyTag {};
    struct PriorityTag {};

    // Type aliases for strong types
    using OrderId = StrongType<uint64_t, OrderIdTag>;
    using TickerId = StrongType<uint32_t, TickerIdTag>;
    using ClientId = StrongType<uint32_t, ClientIdTag>;
    using Price = StrongType<uint64_t, PriceTag>;
    using Qty = StrongType<uint32_t, QtyTag>;
    using Priority = StrongType<uint64_t, PriorityTag>;

    // Side Enum of an order
    enum class Side : int8_t {
        INVALID = 0,
        BUY = 1,
        SELL = -1,
        MAX = 2
    };

    inline auto sideToString(Side side) -> std::string {
        switch (side) {
            case Side::BUY: return "BUY";
            case Side::SELL: return "SELL";
            case Side::INVALID: return "INVALID";
            case Side::MAX: return "MAX";
        }
        return "UNKNOWN";
    }

    inline constexpr auto sideToIndex(Side side) noexcept -> size_t {
        return static_cast<size_t>(static_cast<int8_t>(side)) + 1;
    }

    inline constexpr auto sideToValue(Side side) noexcept -> int {
        return static_cast<int>(side);
    }

    struct RiskCfg {
        Qty max_order_size_;
        Qty max_position_;
        double max_loss_ = 0;
    };

    struct TradeEngineCfg {
        Qty clip_;
        double threshold_ = 0;
        RiskCfg risk_cfg_;
    };

    using TradeEngineCfgHashMap = std::array<TradeEngineCfg, ME_MAX_TICKERS>;
}
