//
// MarketDataPublisher — the exchange's loudspeaker for market data.
//
// Architecture:
//   - Runs on its own thread, separate from matching engine and order server.
//   - Reads MEMarketUpdate from the matching engine via SPSC lock-free queue.
//   - Publishes each update over UDP multicast incremental stream using the
//     SBE two-send pattern (seq_num then payload — no temp struct).
//   - Feeds a SnapshotSynthesizer via an internal MDPMarketUpdateLFQueue.
//     The synthesizer runs on its own thread and periodically publishes
//     full order book snapshots on a separate multicast stream.
//
// Low-latency design:
//   - UDP multicast — no per-client connection overhead, minimal latency.
//   - Busy-spin loop polling the LFQueue.
//   - SBE two-send: seq_num + payload written separately to send buffer,
//     flushed together — zero-copy, no intermediate MDPMarketUpdate temp.
//   - sendAndRecv() called OUTSIDE the inner drain loop — batched flush.
//   - std::atomic<bool> for run flag — never volatile.
//   - noexcept on all hot path functions.
//

#pragma once

#include <atomic>
#include <string>

#include "market_update.h"
#include "snapshot_synthesizer.h"
#include "mcast_socket.h"
#include "thread_utils.h"
#include "logging.h"
#include "time_utils.h"
#include "macros.h"

namespace Exchange {

class MarketDataPublisher final {
public:
    MarketDataPublisher(MEMarketUpdateLFQueue *market_updates,
                        const std::string     &iface,
                        const std::string     &snapshot_ip,  int snapshot_port,
                        const std::string     &incremental_ip, int incremental_port,
                        const std::string     &log_file = "exchange_market_data.log");

    ~MarketDataPublisher();

    auto start() -> void;
    auto stop()  -> void;

    /// One full drain pass over the incremental queue — the body of run()'s
    /// busy-spin loop. Public so tests can drive the drain synchronously
    /// without starting the publisher thread.
    auto poll() noexcept -> void;

    // ── Accessors for tests and diagnostics ─────────────────────────

    /// Internal queue feeding the SnapshotSynthesizer. Exposed so tests can
    /// verify the incremental→snapshot forwarding path; in production the
    /// synthesizer (started by start()) is its only consumer.
    auto snapshotQueueForTest() noexcept -> MDPMarketUpdateLFQueue * {
        return &snapshot_md_updates_;
    }

    MarketDataPublisher() = delete;
    MarketDataPublisher(const MarketDataPublisher &)             = delete;
    MarketDataPublisher(MarketDataPublisher &&)                  = delete;
    MarketDataPublisher &operator=(const MarketDataPublisher &)  = delete;
    MarketDataPublisher &operator=(MarketDataPublisher &&)       = delete;

private:
    /// Hot path: the busy-spin loop.
    auto run() noexcept -> void;

    // ── Data members ─────────────────────────────────────────────────

    /// Queue from matching engine (owned by exchange_main, shared via pointer).
    MEMarketUpdateLFQueue *outgoing_md_updates_ = nullptr;

    /// Internal queue feeding the snapshot synthesizer (owned by publisher).
    MDPMarketUpdateLFQueue snapshot_md_updates_;

    // logger_ must be declared BEFORE sockets because McastSocket takes
    // a Logger& in its constructor.
    Common::Logger logger_;

    Common::McastSocket incremental_socket_;

    /// Snapshot synthesizer — heap allocated in constructor (startup, not hot path).
    SnapshotSynthesizer *snapshot_synthesizer_ = nullptr;

    size_t next_inc_seq_num_ = 1;

    std::atomic<bool> run_ = {false};
    std::string time_str_;
};

} // namespace Exchange
