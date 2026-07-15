// Performance instrumentation primitives.
//
// Provides:
//   - rdtsc() — read time-stamp counter for cycle-precision measurement
//   - START_MEASURE / END_MEASURE — RDTSC cycle-count delta logging
//   - TTT_MEASURE — wall-clock nanosecond timestamp logging at component
//     boundaries (tick-to-trade markers)
//
// Macro contract:
//   - Each measurement is exactly one rdtsc / getCurrentNanos read + one
//     log line. No allocations, no branches.
//   - START_MEASURE declares a const local TAG variable holding the start
//     timestamp; END_MEASURE expects that name back.
//   - END_MEASURE and TTT_MEASURE reference an unqualified `time_str_`
//     identifier — every class that uses these macros must declare
//     `std::string time_str_;` as a member.
//
#pragma once

#include <cstdint>
#include "logging.h"
#include "time_utils.h"

namespace Common {

  inline auto rdtsc() noexcept {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t) hi << 32) | lo;
  }

}

#define START_MEASURE(TAG) const auto TAG = Common::rdtsc()

#define END_MEASURE(TAG, LOGGER)                                    \
  do {                                                              \
    const auto end = Common::rdtsc();                               \
    LOGGER.log("% RDTSC " #TAG " %\n",                              \
               Common::getCurrentTimeStr(&time_str_),               \
               (end - TAG));                                        \
  } while(false)

#define TTT_MEASURE(TAG, LOGGER)                                    \
  do {                                                              \
    const auto TAG = Common::getCurrentNanos();                     \
    LOGGER.log("% TTT " #TAG " %\n",                                \
               Common::getCurrentTimeStr(&time_str_),               \
               TAG);                                                \
  } while(false)
