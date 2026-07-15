#pragma once

#include <chrono>
#include <ctime>
#include <cstdio>
#include <string>

namespace Common
{
    typedef  int64_t Nanos;

    constexpr  Nanos NANOS_TO_MICROS = 1000;
    constexpr  Nanos MICROS_TO_MILLIS = 1000;
    constexpr  Nanos MILLIS_TO_SECS = 1000;
    constexpr  Nanos NANOS_TO_MILLIS = NANOS_TO_MICROS * MICROS_TO_MILLIS;
    constexpr  Nanos NANOS_TO_SECS = NANOS_TO_MILLIS * MILLIS_TO_SECS;

    inline auto getCurrentNanos() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    inline auto& getCurrentTimeStr(std::string * time_str) noexcept
    {
        // Format: HH:MM:SS.nnnnnnnnn — drop the date, keep nanosecond precision.
        const auto clock = std::chrono::system_clock::now();
        const auto time  = std::chrono::system_clock::to_time_t(clock);

        char ctime_buf[32]; // ctime_r() requires at least 26 bytes
        char nanos_str[32];
        // ctime_r() (thread-safe; std::ctime shares one static buffer across
        // all threads) returns "Day Mon DD HH:MM:SS YYYY\n"; +11 lands on HH.
        std::snprintf(nanos_str, sizeof(nanos_str), "%.8s.%09ld",
                      ctime_r(&time, ctime_buf) + 11,
                      static_cast<long>(
                          std::chrono::duration_cast<std::chrono::nanoseconds>(
                              clock.time_since_epoch()).count() % NANOS_TO_SECS));

        time_str->assign(nanos_str);
        return *time_str;
    }
}

// Pulled AFTER the Common:: definitions above so that perf_utils.h, which
// includes this file back, can use them. #pragma once breaks the cycle.
#include "perf_utils.h"
