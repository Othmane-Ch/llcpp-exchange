// release_benchmark.cpp -- Common::MemPool vs OptCommon::OptMemPool
//
// Both pools allocate / deallocate MDPMarketUpdate objects in 256-object
// batches for kIterations loops. The 256-batch matches the
// ME_MAX_PRICE_LEVELS sizing used in production.
//
// CRITICAL: must be compiled with -DNDEBUG, otherwise the baseline
// MemPool still pays the assertion cost and the result is meaningless.
// CMakeLists.txt enforces this with $<$<CONFIG:Release>:-DNDEBUG>; the
// Makefile target below adds -DNDEBUG explicitly to keep it
// configuration-independent.
//
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "mem_pool.h"
#include "opt_mem_pool.h"
#include "market_update.h"
#include "perf_utils.h"

namespace {

constexpr size_t kBatch      = 255;       // one fewer than pool capacity
constexpr size_t kPoolSize   = 256;       // ME_MAX_PRICE_LEVELS
constexpr size_t kIterations = 10'000;

struct Result {
    uint64_t total_cycles = 0;
    double   cycles_per_op = 0.0;
    double   ns_per_op = 0.0;
};

auto runBaseline(double cpu_ghz) -> Result {
    Common::MemPool<Exchange::MDPMarketUpdate> pool(kPoolSize);
    Exchange::MDPMarketUpdate *batch[kBatch];

    const auto t0 = Common::rdtsc();
    for (size_t i = 0; i < kIterations; ++i) {
        for (size_t j = 0; j < kBatch; ++j) {
            batch[j] = pool.allocate();
        }
        for (size_t j = 0; j < kBatch; ++j) {
            pool.deallocate(batch[j]);
        }
    }
    const auto t1 = Common::rdtsc();

    Result r;
    r.total_cycles  = t1 - t0;
    const double ops = static_cast<double>(kIterations) * kBatch * 2.0;
    r.cycles_per_op = static_cast<double>(r.total_cycles) / ops;
    r.ns_per_op     = r.cycles_per_op / cpu_ghz;
    return r;
}

auto runOptimized(double cpu_ghz) -> Result {
    OptCommon::OptMemPool<Exchange::MDPMarketUpdate> pool(kPoolSize);
    Exchange::MDPMarketUpdate *batch[kBatch];

    const auto t0 = Common::rdtsc();
    for (size_t i = 0; i < kIterations; ++i) {
        for (size_t j = 0; j < kBatch; ++j) {
            batch[j] = pool.allocate();
        }
        for (size_t j = 0; j < kBatch; ++j) {
            pool.deallocate(batch[j]);
        }
    }
    const auto t1 = Common::rdtsc();

    Result r;
    r.total_cycles  = t1 - t0;
    const double ops = static_cast<double>(kIterations) * kBatch * 2.0;
    r.cycles_per_op = static_cast<double>(r.total_cycles) / ops;
    r.ns_per_op     = r.cycles_per_op / cpu_ghz;
    return r;
}

} // anonymous namespace

int main(int argc, char **argv) {
    double cpu_ghz = 3.29;
    if (argc > 1) cpu_ghz = std::atof(argv[1]);

#if !defined(NDEBUG)
    std::cerr << "WARNING: release_benchmark compiled WITHOUT -DNDEBUG; "
              << "baseline numbers will include assert cost. "
              << "This is the configuration the report wants to expose, "
              << "but check the build flags." << std::endl;
#endif

    const auto baseline = runBaseline(cpu_ghz);
    const auto optimized = runOptimized(cpu_ghz);
    const double speedup = baseline.cycles_per_op / optimized.cycles_per_op;

    std::ostringstream out;
    out << "release_benchmark (NDEBUG="
#if defined(NDEBUG)
        << "on"
#else
        << "OFF"
#endif
        << ")\n"
        << "  iterations:       " << kIterations << " batches of " << kBatch << "\n"
        << "  ops_measured:     " << (kIterations * kBatch * 2) << " (alloc+dealloc)\n"
        << "  cpu_ghz:          " << cpu_ghz << "\n"
        << "  baseline cycles/op:  " << baseline.cycles_per_op << "\n"
        << "  baseline ns/op:      " << baseline.ns_per_op << "\n"
        << "  optimized cycles/op: " << optimized.cycles_per_op << "\n"
        << "  optimized ns/op:     " << optimized.ns_per_op << "\n"
        << "  speedup:             " << speedup << "x\n";

    std::cout << out.str();
    std::ofstream f("release_benchmark_results.txt", std::ios::app);
    if (f.is_open()) f << out.str() << "----\n";

    return 0;
}
