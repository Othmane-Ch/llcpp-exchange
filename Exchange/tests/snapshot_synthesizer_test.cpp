// snapshot_synthesizer_test.cpp — unit tests for SnapshotSynthesizer order
// book reconstruction from incremental updates.
//
// SnapshotSynthesizer embeds ~100MB of arrays/pools, so it MUST be
// heap-allocated (stack would SIGSEGV).

#include "doctest/doctest.h"

#include <memory>

#include "market_data/snapshot_synthesizer.h"
#include "market_data/market_update.h"
#include "time_utils.h"

using namespace Exchange;
using namespace Common;

TEST_CASE("SnapshotSynthesizer consumes ADD/MODIFY/CANCEL from queue") {
    MDPMarketUpdateLFQueue snap_queue(1024);

    // Heap-allocated — too large for stack (~100MB internal arrays).
    auto synth = std::make_unique<SnapshotSynthesizer>(
        &snap_queue, "lo", "239.0.0.10", 30000,
        "test_snapshot_synthesizer.log");

    // ADD order: ticker=0, order_id=1, BUY 100@50, priority=1
    auto *slot = snap_queue.getNextToWriteTo();
    slot->seq_num_ = 1;
    slot->me_market_update_ = {
        MarketUpdateType::ADD,
        TickerId{0}, Side::BUY, Price{50}, Qty{100},
        OrderId{1}, Priority{1}
    };
    snap_queue.updateWriteIndex();

    // MODIFY: reduce qty to 60
    slot = snap_queue.getNextToWriteTo();
    slot->seq_num_ = 2;
    slot->me_market_update_ = {
        MarketUpdateType::MODIFY,
        TickerId{0}, Side::BUY, Price{50}, Qty{60},
        OrderId{1}, Priority{1}
    };
    snap_queue.updateWriteIndex();

    // ADD a second order: ticker=0, order_id=2, SELL 200@55
    slot = snap_queue.getNextToWriteTo();
    slot->seq_num_ = 3;
    slot->me_market_update_ = {
        MarketUpdateType::ADD,
        TickerId{0}, Side::SELL, Price{55}, Qty{200},
        OrderId{2}, Priority{2}
    };
    snap_queue.updateWriteIndex();

    // CANCEL the first order
    slot = snap_queue.getNextToWriteTo();
    slot->seq_num_ = 4;
    slot->me_market_update_ = {
        MarketUpdateType::CANCEL,
        TickerId{0}, Side::BUY, Price{50}, Qty{60},
        OrderId{1}, Priority{1}
    };
    snap_queue.updateWriteIndex();

    synth->start();
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(2s);
    synth->stop();
    std::this_thread::sleep_for(1s);

    // The queue should be fully drained.
    CHECK(snap_queue.getNextToRead() == nullptr);
}

TEST_CASE("SnapshotSynthesizer handles TRADE updates without crashing") {
    MDPMarketUpdateLFQueue snap_queue(1024);

    auto synth = std::make_unique<SnapshotSynthesizer>(
        &snap_queue, "lo", "239.0.0.11", 30001,
        "test_snapshot_synth_trade.log");

    // TRADE update — synthesizer should ignore it gracefully.
    auto *slot = snap_queue.getNextToWriteTo();
    slot->seq_num_ = 1;
    slot->me_market_update_ = {
        MarketUpdateType::TRADE,
        TickerId{0}, Side::BUY, Price{50}, Qty{10},
        OrderId{1}, Priority{1}
    };
    snap_queue.updateWriteIndex();

    synth->start();
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(2s);
    synth->stop();
    std::this_thread::sleep_for(1s);

    CHECK(snap_queue.getNextToRead() == nullptr);
}
