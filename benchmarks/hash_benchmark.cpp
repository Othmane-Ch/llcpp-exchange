// hash_benchmark.cpp -- Exchange::MEOrderBook vs UnorderedMapMEOrderBook.
//
// Drives kIterations mixed NEW/CANCEL client requests against each book and
// measures cycles per request. To avoid running the full MatchingEngine (which
// pulls in TCP server, market data publisher, etc.), we plug in a minimal
// matching engine that simply discards client responses + market updates by
// receiving them into pre-allocated LFQueues.
//
// The order book takes a MatchingEngine* (full class) -- not an interface --
// so we go through a real MatchingEngine here. Its event-loop thread is not
// started: we only need its sendClientResponse / sendMarketUpdate methods
// (which placement-new into the LFQueues; the LFQueues are simply drained by
// our drainQueues() helper).
//

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "perf_utils.h"
#include "logging.h"
#include "matcher/me_order_book.h"
#include "matcher/matching_engine.h"
#include "matcher/unordered_map_me_order_book.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "market_data/market_update.h"

using namespace Common;
using namespace Exchange;

namespace {

constexpr size_t kIterations = 100'000;
constexpr size_t kPriceRange = 50;        // restrict to keep matches happening
constexpr uint64_t kBasePrice = 1000;

struct Result {
    uint64_t total_cycles = 0;
    double   cycles_per_op = 0.0;
    double   ns_per_op = 0.0;
};

struct GeneratedRequest {
    bool is_new;
    ClientId client_id;
    OrderId  order_id;
    Side     side;
    Price    price;
    Qty      qty;
};

auto generateWorkload(size_t n) -> std::vector<GeneratedRequest> {
    std::mt19937 rng(123);
    std::uniform_int_distribution<uint32_t> side_dist(0, 1);
    std::uniform_int_distribution<uint64_t> price_dist(0, kPriceRange - 1);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);
    std::uniform_int_distribution<int>      op_dist(0, 3);

    std::vector<GeneratedRequest> out;
    out.reserve(n);

    std::vector<OrderId> live_orders;
    live_orders.reserve(n);

    uint64_t next_oid = 1;
    for (size_t i = 0; i < n; ++i) {
        GeneratedRequest r{};
        r.client_id = ClientId{1};
        // 25% cancels when there is at least one live order
        if (!live_orders.empty() && op_dist(rng) == 0) {
            r.is_new = false;
            std::uniform_int_distribution<size_t> idx(0, live_orders.size() - 1);
            const size_t pick = idx(rng);
            r.order_id = live_orders[pick];
            live_orders[pick] = live_orders.back();
            live_orders.pop_back();
        } else {
            r.is_new = true;
            r.order_id = OrderId{next_oid++};
            r.side  = side_dist(rng) ? Side::BUY : Side::SELL;
            r.price = Price{kBasePrice + price_dist(rng)};
            r.qty   = Qty{qty_dist(rng)};
            live_orders.push_back(r.order_id);
        }
        out.push_back(r);
    }
    return out;
}

// Drain a queue: simulate the matching engine's downstream consumer doing
// nothing useful -- we only care about producer throughput.
template<typename Q>
auto drainQueue(Q &q) -> void {
    while (q.size() > 0) {
        q.getNextToRead();
        q.updateReadIndex();
    }
}

template<typename Book>
auto runBook(Book &book,
             ClientRequestLFQueue &requests,
             ClientResponseLFQueue &responses,
             MEMarketUpdateLFQueue &updates,
             const std::vector<GeneratedRequest> &workload,
             double cpu_ghz) -> Result {
    const TickerId tid{0};

    const auto t0 = Common::rdtsc();
    for (const auto &r : workload) {
        if (r.is_new) {
            book.add(r.client_id, r.order_id, tid, r.side, r.price, r.qty);
        } else {
            book.cancel(r.client_id, r.order_id, tid);
        }
        // Drain occasionally to keep queues from filling.
        if (responses.size() > 1000) drainQueue(responses);
        if (updates.size()   > 1000) drainQueue(updates);
    }
    const auto t1 = Common::rdtsc();
    (void) requests; // unused -- we drive the book directly

    drainQueue(responses);
    drainQueue(updates);

    Result r;
    r.total_cycles  = t1 - t0;
    r.cycles_per_op = static_cast<double>(r.total_cycles) / static_cast<double>(workload.size());
    r.ns_per_op     = r.cycles_per_op / cpu_ghz;
    return r;
}

} // anonymous namespace

int main(int argc, char **argv) {
    double cpu_ghz = 3.29;
    if (argc > 1) cpu_ghz = std::atof(argv[1]);

    const auto workload = generateWorkload(kIterations);

    // Shared queues. Reused across both books.
    ClientRequestLFQueue  requests(ME_MAX_CLIENT_UPDATES);
    ClientResponseLFQueue responses(ME_MAX_CLIENT_UPDATES);
    MEMarketUpdateLFQueue updates(ME_MAX_MARKET_UPDATES);

    // We need a MatchingEngine because both books take a MatchingEngine*.
    // We do not call start() -- only use its enqueue helpers.
    MatchingEngine me(&requests, &responses, &updates, "/tmp/hash_bench_me.log");
    Logger book_logger("/tmp/hash_bench_book.log");

    Result baseline;
    {
        // MEOrderBook contains a 2 GB ClientOrderHashMap (std::array) --
        // must be heap-allocated or it blows the stack.
        auto *book = new MEOrderBook(TickerId{0}, &book_logger, &me);
        baseline = runBook(*book, requests, responses, updates, workload, cpu_ghz);
        delete book;
    }

    Result optimized;
    {
        auto *book = new UnorderedMapMEOrderBook(TickerId{0}, &book_logger, &me);
        optimized = runBook(*book, requests, responses, updates, workload, cpu_ghz);
        delete book;
    }

    // Baseline is the std::array book -- so "speedup" here is overhead of
    // unordered_map relative to baseline. We report both raw numbers and
    // the ratio so the sign is unambiguous.
    const double overhead = optimized.cycles_per_op / baseline.cycles_per_op;

    std::ostringstream out;
    out << "hash_benchmark\n"
        << "  iterations:       " << kIterations << " mixed NEW/CANCEL requests\n"
        << "  cpu_ghz:          " << cpu_ghz << "\n"
        << "  std::array book cycles/op:     " << baseline.cycles_per_op << "\n"
        << "  std::array book ns/op:         " << baseline.ns_per_op << "\n"
        << "  std::unordered_map cycles/op:  " << optimized.cycles_per_op << "\n"
        << "  std::unordered_map ns/op:      " << optimized.ns_per_op << "\n"
        << "  overhead (unordered/array):    " << overhead << "x\n";

    std::cout << out.str();
    std::cout.flush();
    {
        std::ofstream f("hash_benchmark_results.txt", std::ios::app);
        if (f.is_open()) f << out.str() << "----\n";
    }

    // Bypass ~MatchingEngine (sleep_for + logger drain) to avoid hanging.
    _exit(0);
}
