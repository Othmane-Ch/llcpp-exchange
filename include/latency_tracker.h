// LatencyTracker -- zero-allocation hot-path performance instrumentation.
// Guarded by ENABLE_PERF_TRACKING: when not defined, start()/stop() are no-ops.

#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <time.h>

namespace Common {

struct PerfStats {
    uint64_t count        = 0;
    double   min_ns       = 0.0;
    double   max_ns       = 0.0;
    double   mean_ns      = 0.0;
    double   p50_ns       = 0.0;
    double   p99_ns       = 0.0;
    double   p999_ns      = 0.0;
    double   throughput_per_sec = 0.0;
};

#ifdef ENABLE_PERF_TRACKING

class LatencyTracker {
public:
    // Pre-allocate a fixed circular buffer for raw samples.
    static constexpr size_t MAX_SAMPLES = 1 << 20; // ~1M samples

    LatencyTracker() {
        std::memset(samples_, 0, sizeof(samples_));
    }

    // Call at hot-path entry.
    inline void start() noexcept {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        start_ns_ = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
                   + static_cast<uint64_t>(ts.tv_nsec);
    }

    // Call at hot-path exit. Records delta into circular buffer.
    inline void stop() noexcept {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t end_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
                        + static_cast<uint64_t>(ts.tv_nsec);
        uint64_t delta = end_ns - start_ns_;

        samples_[write_idx_ & (MAX_SAMPLES - 1)] = delta;
        ++write_idx_;
        if (count_ < MAX_SAMPLES) ++count_;
    }

    auto getStats() const -> PerfStats {
        if (count_ == 0) return {};

        const size_t n = count_;

        // Copy valid samples into a scratch buffer for sorting.
        // We sort a copy to avoid disturbing the circular buffer.
        auto *scratch = new uint64_t[n];
        if (count_ < MAX_SAMPLES) {
            std::memcpy(scratch, samples_, n * sizeof(uint64_t));
        } else {
            // Buffer has wrapped; start from current write position.
            size_t start = write_idx_ & (MAX_SAMPLES - 1);
            size_t first_chunk = MAX_SAMPLES - start;
            std::memcpy(scratch, samples_ + start, first_chunk * sizeof(uint64_t));
            std::memcpy(scratch + first_chunk, samples_, start * sizeof(uint64_t));
        }

        std::sort(scratch, scratch + n);

        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) sum += static_cast<double>(scratch[i]);

        // Compute first and last timestamps for throughput.
        // Approximate: use sum of deltas as total elapsed time.
        double elapsed_ns = sum;

        PerfStats stats;
        stats.count    = n;
        stats.min_ns   = static_cast<double>(scratch[0]);
        stats.max_ns   = static_cast<double>(scratch[n - 1]);
        stats.mean_ns  = sum / static_cast<double>(n);
        stats.p50_ns   = static_cast<double>(scratch[percentileIndex(n, 50)]);
        stats.p99_ns   = static_cast<double>(scratch[percentileIndex(n, 99)]);
        stats.p999_ns  = static_cast<double>(scratch[percentileIndex(n, 999)]);
        stats.throughput_per_sec = (elapsed_ns > 0.0)
            ? static_cast<double>(n) / (elapsed_ns / 1'000'000'000.0)
            : 0.0;

        delete[] scratch;
        return stats;
    }

    void reset() noexcept {
        count_     = 0;
        write_idx_ = 0;
    }

private:
    static constexpr size_t percentileIndex(size_t n, size_t pct_x10) noexcept {
        // pct_x10: 50 means p50, 99 means p99, 999 means p99.9
        size_t idx;
        if (pct_x10 <= 100) {
            idx = (n * pct_x10) / 100;
        } else {
            idx = (n * pct_x10) / 1000;
        }
        return (idx > 0) ? idx - 1 : 0;
    }

    uint64_t samples_[MAX_SAMPLES];
    uint64_t start_ns_  = 0;
    size_t   count_     = 0;
    size_t   write_idx_ = 0;
};

#else // !ENABLE_PERF_TRACKING

// No-op stub: zero overhead when perf tracking is disabled.
class LatencyTracker {
public:
    inline void start() noexcept {}
    inline void stop()  noexcept {}
    auto getStats() const -> PerfStats { return {}; }
    void reset()    noexcept {}
};

#endif // ENABLE_PERF_TRACKING

} // namespace Common
