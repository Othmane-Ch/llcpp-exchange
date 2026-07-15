//
// SnapshotSynthesizer — reconstructs full order book from incremental updates.
//
// Architecture:
//   - Runs on its own thread (spawned by MarketDataPublisher).
//   - Reads MDPMarketUpdate from an internal LFQueue fed by the publisher.
//   - Maintains a local reconstruction of the order book via direct-indexed
//     arrays: ticker_orders_[ticker_id][order_id] → MEMarketUpdate*.
//   - Periodically publishes full book snapshots over a separate UDP multicast
//     stream so clients can recover from sequence gaps.
//
// Low-latency design:
//   - MemPool<MEMarketUpdate> — zero allocation on hot path.
//   - Direct-indexed arrays — O(1) lookup by ticker_id × order_id, no hashing.
//   - std::atomic<bool> for run flag — never volatile.
//   - Busy-spin loop — no sleep, no condition variables.
//   - noexcept on all hot path methods.
//

#pragma once

#include <array>
#include <atomic>
#include <string>

#include "market_update.h"
#include "mcast_socket.h"
#include "mem_pool.h"
#include "logging.h"
#include "time_utils.h"
#include "thread_utils.h"
#include "macros.h"

namespace Exchange {

class SnapshotSynthesizer final {
public:
    SnapshotSynthesizer(MDPMarketUpdateLFQueue *snapshot_md_updates,
                        const std::string      &iface,
                        const std::string      &snapshot_ip,
                        int                     snapshot_port,
                        const std::string      &log_file = "exchange_snapshot_synthesizer.log");

    ~SnapshotSynthesizer();

    auto start() -> void;
    auto stop()  -> void;

    SnapshotSynthesizer() = delete;
    SnapshotSynthesizer(const SnapshotSynthesizer &) = delete;
    SnapshotSynthesizer(SnapshotSynthesizer &&) = delete;
    SnapshotSynthesizer &operator=(const SnapshotSynthesizer &) = delete;
    SnapshotSynthesizer &operator=(SnapshotSynthesizer &&) = delete;

private:
    /// Busy-spin loop: consume incremental updates, periodically publish snapshots.
    auto run() noexcept -> void;

    /// Apply a single incremental update to the local order book reconstruction.
    auto addToSnapshot(const MDPMarketUpdate *update) noexcept -> void;

    /// Walk the reconstructed book and publish a full snapshot over multicast.
    auto publishSnapshot() noexcept -> void;

    // ── Data members ─────────────────────────────────────────────────

    MDPMarketUpdateLFQueue *snapshot_md_updates_ = nullptr;

    // logger_ must be declared BEFORE snapshot_socket_ (constructor dependency).
    Common::Logger logger_;
    Common::McastSocket snapshot_socket_;

    /// Local order book reconstruction.
    /// ticker_orders_[ticker_id][order_id] → pointer into order_pool_.
    /// Direct indexing: TickerId IS outer index, OrderId IS inner index.
    std::array<std::array<MEMarketUpdate *, ME_MAX_ORDER_IDS>, ME_MAX_TICKERS> ticker_orders_{};

    /// Pool for order snapshots — no heap allocation on hot path.
    Common::MemPool<MEMarketUpdate> order_pool_{ME_MAX_ORDER_IDS};

    /// Last processed incremental sequence number (for SNAPSHOT_END marker).
    size_t last_inc_seq_num_ = 0;

    /// Snapshot sequence number (independent from incremental stream).
    size_t next_snap_seq_num_ = 1;

    /// Time-based snapshot interval: 60 seconds in nanoseconds.
    static constexpr Common::Nanos SNAPSHOT_INTERVAL = 60 * Common::NANOS_TO_SECS;
    Common::Nanos last_snapshot_time_ = 0;

    /// Pre-allocated snapshot update — reused every send (no heap).
    MDPMarketUpdate snapshot_update_{};

    std::atomic<bool> run_ = {false};
    std::string time_str_;
};

} // namespace Exchange
