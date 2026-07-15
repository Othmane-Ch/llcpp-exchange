// market_data_publisher_test.cpp — regression tests for the incremental→
// snapshot forwarding path in MarketDataPublisher::poll().
//
// Contract under test: every update drained from the matching engine queue
// must be copied into the snapshot queue BEFORE the LFQueue slot is released
// back to the producer (updateReadIndex). Releasing first lets the matching
// engine overwrite the slot under backpressure, so the snapshot synthesizer
// would see torn/foreign data — silent snapshot corruption.
//
// The publisher is constructed but never start()ed: no publisher thread, no
// synthesizer thread, so the tests own both ends of the snapshot queue.

#include "doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "market_data/market_data_publisher.h"
#include "market_data/market_update.h"

using namespace Exchange;
using namespace Common;

namespace {

/// Encode a monotone sequence i into every payload field we later verify.
auto mkUpdate(uint64_t i) -> MEMarketUpdate {
    MEMarketUpdate u{};
    u.type_      = MarketUpdateType::ADD;
    u.ticker_id_ = TickerId{0};
    u.side_      = Side::BUY;
    u.price_     = Price{i};
    u.qty_       = Qty{static_cast<uint32_t>(i)};
    u.order_id_  = OrderId{i};
    u.priority_  = Priority{1};
    return u;
}

} // anonymous namespace

TEST_CASE("MDP: drained updates are forwarded to the snapshot queue in order") {
    // Deliberately tiny queue — the forwarding path must not depend on slack.
    MEMarketUpdateLFQueue md_q{8};
    auto mdp = std::make_unique<MarketDataPublisher>(
        &md_q, "lo", "239.0.0.20", 30020, "239.0.0.21", 30021,
        "test_mdp_ordering.log");

    constexpr size_t K = 5;
    for (size_t i = 1; i <= K; ++i) {
        auto *slot = md_q.getNextToWriteTo();
        *slot = mkUpdate(i);
        md_q.updateWriteIndex();
    }

    mdp->poll(); // one synchronous drain pass — no threads involved

    auto *snap_q = mdp->snapshotQueueForTest();
    for (size_t i = 1; i <= K; ++i) {
        const auto *fwd = snap_q->getNextToRead();
        REQUIRE(fwd != nullptr);
        CHECK(fwd->seq_num_ == i); // publisher seq numbering starts at 1
        CHECK(fwd->me_market_update_.price_.value == i);
        CHECK(fwd->me_market_update_.qty_.value == i);
        CHECK(fwd->me_market_update_.order_id_.value == i);
        snap_q->updateReadIndex();
    }
    CHECK(snap_q->getNextToRead() == nullptr); // exactly K forwarded, no more
    CHECK(md_q.getNextToRead() == nullptr);    // incremental queue fully drained
}

TEST_CASE("MDP: two-thread stress keeps the snapshot sequence exact under backpressure") {
    // An 8-slot queue forces constant slot reuse: under the old
    // release-then-forward ordering the producer overwrites slots the
    // publisher is still copying and the snapshot stream tears. Under the
    // fixed ordering the snapshot side must see exactly 1..N.
    MEMarketUpdateLFQueue md_q{8};
    auto mdp = std::make_unique<MarketDataPublisher>(
        &md_q, "lo", "239.0.0.22", 30022, "239.0.0.23", 30023,
        "test_mdp_stress.log");

#if defined(__SANITIZE_THREAD__)
    constexpr size_t N = 20'000;  // TSAN is ~10-20x slower; same interleavings
#else
    constexpr size_t N = 100'000;
#endif
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        for (size_t i = 1; i <= N; ++i) {
            // Throttle on size(): writing into a full LFQueue is FATAL.
            while (md_q.size() >= 6) {
            }
            auto *slot = md_q.getNextToWriteTo();
            *slot = mkUpdate(i);
            md_q.updateWriteIndex();
            // Paced flow control: the drain path pushes ~30 async-Logger
            // elements per update, and the Logger's flush thread naps 1ms
            // whenever its queue goes empty — a nap the kernel timer rounds
            // up to 5-15ms and that can stretch to whole fractions of a
            // second on a noisy VM. Unpaced, the drain side outruns the
            // flusher across one bad nap and overflows the 64K-element log
            // queue (FATAL). Cap sustained inflow at ~1.6k updates/s (~50k
            // elements/s) so even a full one-second flusher stall fits the
            // queue — reliability of the suite beats runtime here. Spin on
            // the clock rather than sleeping (sleeps get the same timer
            // rounding). Within each burst of 64 the 8-slot queue still gets
            // slammed, which is the slot-reuse contention this test exists
            // for; the pacing only spaces the bursts out.
            if ((i & 63) == 0) {
                // First wait for the consumer to fully drain this burst:
                // poll()'s inner loop only returns once it observes an empty
                // queue, and the test's snapshot-side drain runs between
                // poll() calls. A producer that kept feeding a slow consumer
                // would keep poll() from ever returning and let the snapshot
                // queue fill unread.
                while (md_q.size() != 0) {
                }
                const auto until = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(40);
                while (std::chrono::steady_clock::now() < until) {
                }
            }
        }
        producer_done.store(true);
    });

    // Consumer side: run the REAL publisher drain logic on this thread and
    // pop the snapshot queue as we go (its capacity is below N). This thread
    // is both producer and consumer of the snapshot queue — SPSC holds.
    auto *snap_q = mdp->snapshotQueueForTest();
    size_t next_expected = 1;
    size_t mismatches    = 0;
    auto drainSnapshots = [&]() {
        while (const auto *fwd = snap_q->getNextToRead()) {
            if (fwd->seq_num_ != next_expected ||
                fwd->me_market_update_.price_.value != next_expected ||
                fwd->me_market_update_.qty_.value != next_expected ||
                fwd->me_market_update_.order_id_.value != next_expected) {
                ++mismatches;
            }
            ++next_expected;
            snap_q->updateReadIndex();
        }
    };

    while (!producer_done.load()) {
        // Gate on occupancy: poll() ends with a socket flush, and calling it
        // in a tight loop against an empty queue is a pure syscall storm.
        // The empty case spins on size() — cheap loads, no syscalls, and no
        // timer-rounded sleeps (this kernel rounds 1ms sleeps to 5-15ms).
        if (md_q.size() > 0) {
            mdp->poll();
            drainSnapshots();
        }
    }
    producer.join();
    mdp->poll(); // producer finished — one final pass drains the residue
    drainSnapshots();

    CHECK(mismatches == 0);
    CHECK(next_expected == N + 1); // saw exactly 1..N, in order
}
