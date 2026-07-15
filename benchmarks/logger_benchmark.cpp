// logger_benchmark.cpp -- Common::Logger vs OptCommon::OptLogger
//
// Measures producer-side cycles per pushValue(const char*) call.
//
// Each iteration pushes a 128-byte random ASCII string. Cycles are read via
// rdtsc() around the entire push loop. The output file streams handle the
// actual I/O on a background thread; we are measuring only the producer-side
// enqueue cost, which is exactly what the hot path pays.
//
// IMPORTANT: ~Logger blocks until the flush thread fully drains the queue
// before joining. We deliberately bypass destruction via _exit(0) once the
// measurement is captured; the producer cost is the metric of interest.
//
// Output is printed to stdout AND appended to logger_benchmark_results.txt.
//

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unistd.h>

#include "logging.h"
#include "opt_logging.h"
#include "perf_utils.h"

namespace {

constexpr size_t  kIterations    = 100'000;
constexpr size_t  kStringLength  = 128;

auto generateRandomString(std::mt19937 &rng, char *buf) -> void {
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    std::uniform_int_distribution<size_t> dist(0, sizeof(alphabet) - 2);
    for (size_t i = 0; i < kStringLength - 1; ++i) buf[i] = alphabet[dist(rng)];
    buf[kStringLength - 1] = '\0';
}

struct Result {
    uint64_t total_cycles = 0;
    double   cycles_per_op = 0.0;
    double   ns_per_op = 0.0;
};

// Heap-allocated, intentionally leaked: see header comment. The destructor of
// Common::Logger blocks until the flush thread drains the queue; we let
// main() call _exit(0) and skip both destructors entirely. The OS reclaims
// memory on process exit.

auto runBaseline(const std::vector<std::string> &payload, double cpu_ghz) -> Result {
    auto *logger = new Common::Logger("/tmp/logger_baseline.log");

    const auto t0 = Common::rdtsc();
    for (const auto &s : payload) {
        logger->pushValue(s.c_str());
    }
    const auto t1 = Common::rdtsc();

    Result r;
    r.total_cycles  = t1 - t0;
    r.cycles_per_op = static_cast<double>(r.total_cycles) / static_cast<double>(payload.size());
    r.ns_per_op     = r.cycles_per_op / cpu_ghz;
    return r;
}

auto runOptimized(const std::vector<std::string> &payload, double cpu_ghz) -> Result {
    auto *logger = new OptCommon::OptLogger("/tmp/logger_optimized.log");

    const auto t0 = Common::rdtsc();
    for (const auto &s : payload) {
        logger->pushValue(s.c_str());
    }
    const auto t1 = Common::rdtsc();

    Result r;
    r.total_cycles  = t1 - t0;
    r.cycles_per_op = static_cast<double>(r.total_cycles) / static_cast<double>(payload.size());
    r.ns_per_op     = r.cycles_per_op / cpu_ghz;
    return r;
}

} // anonymous namespace

int main(int argc, char **argv) {
    double cpu_ghz = 3.29; // matches measured value from Phase 1.4
    if (argc > 1) cpu_ghz = std::atof(argv[1]);

    std::mt19937 rng(42);
    std::vector<std::string> payload;
    payload.reserve(kIterations);
    char buf[kStringLength];
    for (size_t i = 0; i < kIterations; ++i) {
        generateRandomString(rng, buf);
        payload.emplace_back(buf);
    }

    const auto baseline = runBaseline(payload, cpu_ghz);
    const auto optimized = runOptimized(payload, cpu_ghz);
    const double speedup = baseline.cycles_per_op / optimized.cycles_per_op;

    // Print to stdout and capture as text for the report.
    std::ostringstream out;
    out << "logger_benchmark\n"
        << "  iterations:       " << kIterations << "\n"
        << "  string length:    " << kStringLength << " bytes\n"
        << "  cpu_ghz:          " << cpu_ghz << "\n"
        << "  baseline cycles/op:  " << baseline.cycles_per_op << "\n"
        << "  baseline ns/op:      " << baseline.ns_per_op << "\n"
        << "  optimized cycles/op: " << optimized.cycles_per_op << "\n"
        << "  optimized ns/op:     " << optimized.ns_per_op << "\n"
        << "  speedup:             " << speedup << "x\n";

    std::cout << out.str();
    std::cout.flush();
    {
        std::ofstream f("logger_benchmark_results.txt", std::ios::app);
        if (f.is_open()) f << out.str() << "----\n";
    }

    // See header comment: bypass ~Logger's drain-and-join wait. Producer-side
    // cycles are the metric we measured.
    _exit(0);
}
