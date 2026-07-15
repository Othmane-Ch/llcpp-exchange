// trading/market_data/market_data_consumer.h
//
// MarketDataConsumer -- subscribes to the exchange's UDP multicast streams,
// detects sequence gaps, and recovers lost state via the snapshot stream.
//
// Steady-state flow:
//   incremental_socket_.sendAndRecv()  // fires rcv_callback -> recvCallback
//     → parse as many MDPMarketUpdate messages as the buffer holds
//     → if seq_num_ == next_exp_inc_seq_num_ :
//         publish MEMarketUpdate into the outgoing LFQueue
//         ++next_exp_inc_seq_num_
//     → else (gap detected):
//         set in_recovery_ = true
//         buffer out-of-order messages by seq_num until recovery completes
//
// Recovery flow (SBE-style snapshot protocol):
//   - Destroy and re-init the incremental socket for gap-mode; the consumer
//     continues to buffer incrementals but does NOT forward them yet.
//   - Subscribe to the snapshot stream.
//   - Receive CLEAR -> SNAPSHOT_START -> ADD* -> SNAPSHOT_END, produced with
//     consecutive seq_nums by the SnapshotSynthesizer.
//     SNAPSHOT_END.order_id_ carries the last incremental seq_num embedded
//     in the snapshot (see Exchange/market_data/snapshot_synthesizer.cpp).
//   - The snapshot stream is UDP too: every message inside the cycle must be
//     exactly snapshot_last_seen_ + 1. Any gap means the book image is
//     incomplete — the cycle is aborted (no finishRecovery) and the consumer
//     waits for the next periodic CLEAR, whose downstream wipe also erases
//     any partially applied ADDs. Only a gap-free cycle completes recovery.
//   - Forward the CLEAR + ADD* to the book, ignoring SNAPSHOT_START/END.
//   - next_exp_inc_seq_num_ = SNAPSHOT_END.order_id_.value + 1
//   - Drain the out-of-order buffer: forward anything >= new expected seq_num,
//     drop anything older.
//   - Leave the snapshot multicast group (destroy + re-init only the
//     incremental socket) and resume steady-state.
//
// Low-latency design:
//   - Two McastSockets: incremental (239.0.0.2:20001) + snapshot (239.0.0.1:20000).
//   - MEMarketUpdateLFQueue out to downstream (strategy / MarketOrderBook).
//   - Out-of-order buffer is a std::map<size_t, MEMarketUpdate> -- std::map
//     is acceptable ONLY during recovery (off the hot path; trading paused).
//   - noexcept on all hot-path methods.
//   - std::atomic<bool> for run flag -- never volatile.
//

#pragma once

#include <atomic>
#include <map>
#include <string>

#include "mcast_socket.h"
#include "logging.h"
#include "thread_utils.h"
#include "time_utils.h"
#include "macros.h"

#include "market_update.h"   // Exchange::MEMarketUpdate / MDPMarketUpdate

namespace Trading {

class MarketDataConsumer final {
public:
    MarketDataConsumer(Exchange::MEMarketUpdateLFQueue *incoming_md_updates,
                       const std::string               &iface,
                       const std::string               &snapshot_ip,
                       int                              snapshot_port,
                       const std::string               &incremental_ip,
                       int                              incremental_port,
                       const std::string               &log_file = "trading_market_data_consumer.log");

    ~MarketDataConsumer();

    auto start() -> void;
    auto stop()  -> void;

    // ── Accessors for tests and diagnostics ────────────────────────────

    auto isInRecovery() const noexcept -> bool { return in_recovery_.load(); }
    auto nextExpectedIncSeqNum() const noexcept -> size_t { return next_exp_inc_seq_num_; }

    /// Test hook: inject an already-decoded MDPMarketUpdate directly into the
    /// incremental-path logic, bypassing the socket. Lets unit tests exercise
    /// gap detection / recovery without real multicast plumbing.
    auto onIncrementalForTest(const Exchange::MDPMarketUpdate &msg) noexcept -> void {
        processIncremental(msg);
    }

    /// Test hook: inject an already-decoded MDPMarketUpdate into the snapshot
    /// path (runs CLEAR / SNAPSHOT_START / ADD / SNAPSHOT_END assembly).
    auto onSnapshotForTest(const Exchange::MDPMarketUpdate &msg) noexcept -> void {
        processSnapshot(msg);
    }

    /// Test hook: start a synthetic recovery cycle (no socket re-init).
    auto startRecoveryForTest() noexcept -> void { in_recovery_.store(true); }

    MarketDataConsumer() = delete;
    MarketDataConsumer(const MarketDataConsumer &) = delete;
    MarketDataConsumer(MarketDataConsumer &&) = delete;
    MarketDataConsumer &operator=(const MarketDataConsumer &) = delete;
    MarketDataConsumer &operator=(MarketDataConsumer &&) = delete;

private:
    /// Busy-spin loop -- polls both mcast sockets.
    auto run() noexcept -> void;

    /// Incremental socket rcv callback -- parses whole MDPMarketUpdate msgs.
    auto recvIncremental(Common::McastSocket *socket) noexcept -> void;

    /// Snapshot socket rcv callback -- parses whole MDPMarketUpdate msgs.
    auto recvSnapshot(Common::McastSocket *socket) noexcept -> void;

    /// Apply the sequence-gap / steady-state logic to a decoded incremental msg.
    auto processIncremental(const Exchange::MDPMarketUpdate &msg) noexcept -> void;

    /// Apply a decoded snapshot msg to the in-flight snapshot reassembly.
    auto processSnapshot(const Exchange::MDPMarketUpdate &msg) noexcept -> void;

    /// Start snapshot subscription: subscribe and flip to recovery mode.
    auto startRecovery() noexcept -> void;

    /// Finish snapshot reassembly: unsubscribe, drain the buffer, resume.
    auto finishRecovery(size_t last_inc_seq_num) noexcept -> void;

    /// Push a single market update into the outgoing queue for downstream.
    auto publishDownstream(const Exchange::MEMarketUpdate &u) noexcept -> void;

    // ── Data members ────────────────────────────────────────────────────

    /// Downstream queue: consumer → MarketOrderBook (or whatever reads it).
    Exchange::MEMarketUpdateLFQueue *outgoing_md_updates_ = nullptr;

    // logger_ declared BEFORE sockets (McastSocket ctor takes Logger&).
    Common::Logger      logger_;
    Common::McastSocket incremental_socket_;
    Common::McastSocket snapshot_socket_;

    // Saved connection parameters for re-init during recovery.
    std::string iface_;
    std::string snapshot_ip_;
    int         snapshot_port_ = 0;
    std::string incremental_ip_;
    int         incremental_port_ = 0;

    /// Next incremental seq_num we expect (1-indexed; matches the publisher).
    size_t next_exp_inc_seq_num_ = 1;

    /// Recovery-mode flag. Atomic so tests and cross-thread inspection work.
    std::atomic<bool> in_recovery_ = {false};

    /// Out-of-order buffer: incrementals received during recovery OR
    /// incrementals received ahead of the next expected seq_num.
    /// std::map sorted by seq_num -- correctness over speed here.
    std::map<size_t, Exchange::MEMarketUpdate> out_of_order_buf_;

    // Snapshot reassembly state -- valid only while in_recovery_.
    bool   snapshot_armed_       = false;    // saw CLEAR; awaiting contiguous SNAPSHOT_START
    bool   in_snapshot_          = false;    // inside a CLEAR→…→SNAPSHOT_END cycle
    size_t snapshot_last_seen_   = 0;        // last seq accepted this cycle (contiguity guard)
    size_t pending_last_inc_seq_ = 0;        // carried by SNAPSHOT_END.order_id_

    std::atomic<bool> run_ = {false};
    std::string time_str_;
};

} // namespace Trading
