// market_data_consumer_test.cpp -- unit tests for Trading::MarketDataConsumer.
//
// We drive the consumer through its test hooks (onIncrementalForTest /
// onSnapshotForTest / startRecoveryForTest) to avoid real multicast sockets.
// Socket construction still happens in the ctor, but never sees traffic
// because start() is never called.

#include "doctest/doctest.h"

#include <memory>

#include "market_data_consumer.h"
#include "market_update.h"

using namespace Trading;
using namespace Common;
using Exchange::MEMarketUpdate;
using Exchange::MDPMarketUpdate;
using Exchange::MarketUpdateType;

static auto mkMsg(size_t seq, MarketUpdateType t, uint64_t price = 0,
                  uint32_t qty = 0, uint64_t oid = 0) -> MDPMarketUpdate {
    MDPMarketUpdate m;
    m.seq_num_                   = seq;
    m.me_market_update_.type_    = t;
    m.me_market_update_.price_   = Price{price};
    m.me_market_update_.qty_     = Qty{qty};
    m.me_market_update_.order_id_ = OrderId{oid};
    m.me_market_update_.side_    = Side::BUY;
    return m;
}

namespace { struct Fixture {
    // Downstream queue the consumer writes into.
    Exchange::MEMarketUpdateLFQueue out_q{ME_MAX_MARKET_UPDATES};
    // Consumer itself -- uses "lo" interface; never listens because we
    // don't call start(). Log file goes to CWD but that's fine for tests.
    std::unique_ptr<MarketDataConsumer> consumer{
        new MarketDataConsumer(
            &out_q, "lo",
            "239.0.0.101", 30100,    // snapshot (unused until recovery hook)
            "239.0.0.102", 30101,    // incremental
            "test_trading_mdc.log")};

    auto drain() -> std::vector<MEMarketUpdate> {
        std::vector<MEMarketUpdate> v;
        while (auto *u = out_q.getNextToRead()) {
            v.push_back(*u);
            out_q.updateReadIndex();
        }
        return v;
    }
}; } // anonymous namespace

// ── Test 1: in-order incrementals pass through ──────────────────────────────
TEST_CASE("MDC: contiguous incrementals are published in order") {
    Fixture f;

    const auto m1 = mkMsg(1, MarketUpdateType::ADD, 100, 10, 1);
    const auto m2 = mkMsg(2, MarketUpdateType::ADD, 101, 20, 2);
    const auto m3 = mkMsg(3, MarketUpdateType::ADD, 102, 30, 3);

    f.consumer->onIncrementalForTest(m1);
    f.consumer->onIncrementalForTest(m2);
    f.consumer->onIncrementalForTest(m3);

    const auto out = f.drain();
    REQUIRE(out.size() == 3);
    CHECK(out[0].price_.value == 100);
    CHECK(out[1].price_.value == 101);
    CHECK(out[2].price_.value == 102);
    CHECK(f.consumer->nextExpectedIncSeqNum() == 4);
    CHECK(f.consumer->isInRecovery() == false);
}

// ── Test 2: duplicate / stale incrementals are dropped ──────────────────────
TEST_CASE("MDC: duplicates and stale seq nums are silently dropped") {
    Fixture f;

    f.consumer->onIncrementalForTest(mkMsg(1, MarketUpdateType::ADD, 100, 10, 1));
    f.consumer->onIncrementalForTest(mkMsg(1, MarketUpdateType::ADD, 100, 10, 1)); // dup

    const auto out = f.drain();
    REQUIRE(out.size() == 1);
    CHECK(f.consumer->nextExpectedIncSeqNum() == 2);
    CHECK(f.consumer->isInRecovery() == false);
}

// ── Test 3: gap triggers recovery; snapshot replays state; normal resumes ───
TEST_CASE("MDC: gap triggers recovery and snapshot replay restores sync") {
    Fixture f;

    // seq 1 arrives fine.
    f.consumer->onIncrementalForTest(mkMsg(1, MarketUpdateType::ADD, 100, 10, 1));
    // seq 2 dropped (never delivered). seq 3 arrives -> gap detected.
    // startRecovery() would normally init the snapshot socket; we use the
    // test hook to skip socket re-init and drive the snapshot path directly.
    f.consumer->startRecoveryForTest();
    f.consumer->onIncrementalForTest(mkMsg(3, MarketUpdateType::ADD, 103, 30, 3));
    f.consumer->onIncrementalForTest(mkMsg(4, MarketUpdateType::ADD, 104, 40, 4));
    CHECK(f.consumer->isInRecovery() == true);

    // Snapshot: CLEAR -> SNAPSHOT_START -> ADDs (two synthetic resting orders)
    // -> SNAPSHOT_END whose order_id_ = last_inc_seq_num captured by exchange.
    // We simulate that the snapshot captured state up to inc seq = 2.
    f.consumer->onSnapshotForTest(mkMsg(10, MarketUpdateType::CLEAR));
    f.consumer->onSnapshotForTest(mkMsg(11, MarketUpdateType::SNAPSHOT_START));
    f.consumer->onSnapshotForTest(mkMsg(12, MarketUpdateType::ADD, 100, 10, 1));
    f.consumer->onSnapshotForTest(mkMsg(13, MarketUpdateType::ADD, 102, 20, 2));
    // SNAPSHOT_END.order_id_ carries last_inc_seq_num_ == 2.
    f.consumer->onSnapshotForTest(mkMsg(14, MarketUpdateType::SNAPSHOT_END,
                                        /*price*/0, /*qty*/0, /*oid*/2));

    // Recovery should complete; buffered seq 3 + 4 get replayed; seq 5 resumes.
    CHECK(f.consumer->isInRecovery() == false);
    CHECK(f.consumer->nextExpectedIncSeqNum() == 5);

    const auto out = f.drain();
    // Expected order: seq 1 (pre-gap) + CLEAR + ADD(oid1) + ADD(oid2) +
    // replay of buffered seq 3 + 4.
    REQUIRE(out.size() == 6);
    CHECK(out[0].price_.value == 100);                     // seq 1
    CHECK(out[1].type_ == MarketUpdateType::CLEAR);        // snapshot CLEAR
    CHECK(out[2].type_ == MarketUpdateType::ADD);
    CHECK(out[2].order_id_.value == 1);
    CHECK(out[3].type_ == MarketUpdateType::ADD);
    CHECK(out[3].order_id_.value == 2);
    CHECK(out[4].order_id_.value == 3);                    // buffered seq 3
    CHECK(out[5].order_id_.value == 4);                    // buffered seq 4
}

// ── Test 4: incrementals received between gap and snapshot are buffered ─────
TEST_CASE("MDC: incrementals during recovery are buffered and replayed") {
    Fixture f;

    f.consumer->startRecoveryForTest();
    CHECK(f.consumer->isInRecovery() == true);

    // Buffer 3, 4, 5 while recovering.
    f.consumer->onIncrementalForTest(mkMsg(5, MarketUpdateType::ADD, 105, 5, 5));
    f.consumer->onIncrementalForTest(mkMsg(3, MarketUpdateType::ADD, 103, 3, 3));
    f.consumer->onIncrementalForTest(mkMsg(4, MarketUpdateType::ADD, 104, 4, 4));

    // Nothing published yet.
    CHECK(f.drain().empty());

    // Snapshot covers up to seq 2 -> replay 3, 4, 5 in order.
    f.consumer->onSnapshotForTest(mkMsg(20, MarketUpdateType::CLEAR));
    f.consumer->onSnapshotForTest(mkMsg(21, MarketUpdateType::SNAPSHOT_START));
    f.consumer->onSnapshotForTest(mkMsg(22, MarketUpdateType::SNAPSHOT_END,
                                        0, 0, /*last_inc*/2));

    CHECK(f.consumer->isInRecovery() == false);
    CHECK(f.consumer->nextExpectedIncSeqNum() == 6);

    const auto out = f.drain();
    REQUIRE(out.size() == 4);                   // CLEAR + 3 buffered
    CHECK(out[0].type_ == MarketUpdateType::CLEAR);
    CHECK(out[1].order_id_.value == 3);
    CHECK(out[2].order_id_.value == 4);
    CHECK(out[3].order_id_.value == 5);
}

// ── Test 5: a gap inside the snapshot cycle aborts it; the next complete ────
//            cycle recovers and the book matches that second snapshot.
TEST_CASE("MDC: snapshot cycle with a missing ADD must not complete recovery") {
    Fixture f;

    f.consumer->startRecoveryForTest();
    // One incremental buffered during recovery -- replayed only on success.
    f.consumer->onIncrementalForTest(mkMsg(6, MarketUpdateType::ADD, 106, 60, 6));

    // First cycle: seq 20..24 with the ADD at seq 22 lost in transit.
    f.consumer->onSnapshotForTest(mkMsg(20, MarketUpdateType::CLEAR));
    f.consumer->onSnapshotForTest(mkMsg(21, MarketUpdateType::SNAPSHOT_START));
    // seq 22 (ADD oid 1) never arrives.
    f.consumer->onSnapshotForTest(mkMsg(23, MarketUpdateType::ADD, 102, 20, 2));
    f.consumer->onSnapshotForTest(mkMsg(24, MarketUpdateType::SNAPSHOT_END,
                                        0, 0, /*last_inc*/5));

    // Gap detected: recovery must NOT complete, expected seq not advanced.
    CHECK(f.consumer->isInRecovery() == true);
    CHECK(f.consumer->nextExpectedIncSeqNum() == 1);

    // Next periodic cycle (seq 25..29) arrives complete.
    f.consumer->onSnapshotForTest(mkMsg(25, MarketUpdateType::CLEAR));
    f.consumer->onSnapshotForTest(mkMsg(26, MarketUpdateType::SNAPSHOT_START));
    f.consumer->onSnapshotForTest(mkMsg(27, MarketUpdateType::ADD, 101, 10, 1));
    f.consumer->onSnapshotForTest(mkMsg(28, MarketUpdateType::ADD, 102, 20, 2));
    f.consumer->onSnapshotForTest(mkMsg(29, MarketUpdateType::SNAPSHOT_END,
                                        0, 0, /*last_inc*/5));

    CHECK(f.consumer->isInRecovery() == false);
    CHECK(f.consumer->nextExpectedIncSeqNum() == 7); // 5+1, then buffered 6 replayed

    // Downstream: the aborted cycle contributed ONLY its CLEAR (its post-gap
    // ADD was rejected); the second cycle's CLEAR wipes anything partial and
    // is followed by exactly the second snapshot's image + the replayed inc.
    const auto out = f.drain();
    REQUIRE(out.size() == 5);
    CHECK(out[0].type_ == MarketUpdateType::CLEAR);   // aborted cycle's wipe
    CHECK(out[1].type_ == MarketUpdateType::CLEAR);   // second cycle's wipe
    CHECK(out[2].type_ == MarketUpdateType::ADD);
    CHECK(out[2].order_id_.value == 1);
    CHECK(out[3].type_ == MarketUpdateType::ADD);
    CHECK(out[3].order_id_.value == 2);
    CHECK(out[4].order_id_.value == 6);               // buffered inc replayed
}

// ── Test 6: gap right before SNAPSHOT_END (or a lost END) leaves recovery ───
//            pending -- the cycle end must be contiguous too.
TEST_CASE("MDC: gap at SNAPSHOT_END must not complete recovery") {
    Fixture f;

    f.consumer->startRecoveryForTest();

    f.consumer->onSnapshotForTest(mkMsg(30, MarketUpdateType::CLEAR));
    f.consumer->onSnapshotForTest(mkMsg(31, MarketUpdateType::SNAPSHOT_START));
    f.consumer->onSnapshotForTest(mkMsg(32, MarketUpdateType::ADD, 100, 10, 1));
    // seq 33 (an ADD) lost; SNAPSHOT_END arrives at seq 34.
    f.consumer->onSnapshotForTest(mkMsg(34, MarketUpdateType::SNAPSHOT_END,
                                        0, 0, /*last_inc*/5));

    CHECK(f.consumer->isInRecovery() == true);        // NOT recovered
    CHECK(f.consumer->nextExpectedIncSeqNum() == 1);  // untouched

    // Only the wipe and the one contiguous ADD reached downstream.
    const auto out = f.drain();
    REQUIRE(out.size() == 2);
    CHECK(out[0].type_ == MarketUpdateType::CLEAR);
    CHECK(out[1].order_id_.value == 1);
}

// ── Test 7: joining the snapshot stream mid-cycle (first message an ADD) ────
//            is ignored until the next CLEAR anchors a fresh cycle.
TEST_CASE("MDC: snapshot messages before a CLEAR are ignored") {
    Fixture f;

    f.consumer->startRecoveryForTest();

    // Joined mid-cycle: ADD + END without a preceding CLEAR do nothing.
    f.consumer->onSnapshotForTest(mkMsg(41, MarketUpdateType::ADD, 100, 10, 1));
    f.consumer->onSnapshotForTest(mkMsg(42, MarketUpdateType::SNAPSHOT_END,
                                        0, 0, /*last_inc*/5));
    CHECK(f.consumer->isInRecovery() == true);
    CHECK(f.drain().empty());

    // The next full cycle recovers normally.
    f.consumer->onSnapshotForTest(mkMsg(43, MarketUpdateType::CLEAR));
    f.consumer->onSnapshotForTest(mkMsg(44, MarketUpdateType::SNAPSHOT_START));
    f.consumer->onSnapshotForTest(mkMsg(45, MarketUpdateType::ADD, 100, 10, 1));
    f.consumer->onSnapshotForTest(mkMsg(46, MarketUpdateType::SNAPSHOT_END,
                                        0, 0, /*last_inc*/5));
    CHECK(f.consumer->isInRecovery() == false);
    CHECK(f.consumer->nextExpectedIncSeqNum() == 6);

    const auto out = f.drain();
    REQUIRE(out.size() == 2);                          // CLEAR + one ADD
    CHECK(out[0].type_ == MarketUpdateType::CLEAR);
    CHECK(out[1].order_id_.value == 1);
}

// ── Test 8: SNAPSHOT_START without its anchoring CLEAR is rejected even if ──
//            the seq happens to look contiguous with stale state.
TEST_CASE("MDC: SNAPSHOT_START without a preceding CLEAR does not open a cycle") {
    Fixture f;

    f.consumer->startRecoveryForTest();

    // Mid-stream join directly at SNAPSHOT_START (the CLEAR was lost).
    f.consumer->onSnapshotForTest(mkMsg(1, MarketUpdateType::SNAPSHOT_START));
    f.consumer->onSnapshotForTest(mkMsg(2, MarketUpdateType::ADD, 100, 10, 1));
    f.consumer->onSnapshotForTest(mkMsg(3, MarketUpdateType::SNAPSHOT_END,
                                        0, 0, /*last_inc*/5));

    // Without the CLEAR the book was never wiped -- the cycle must not count.
    CHECK(f.consumer->isInRecovery() == true);
    CHECK(f.drain().empty());
}
