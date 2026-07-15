//
// MarketDataPublisher implementation.
//

#include "market_data_publisher.h"

namespace Exchange {

MarketDataPublisher::MarketDataPublisher(
    MEMarketUpdateLFQueue *market_updates,
    const std::string     &iface,
    const std::string     &snapshot_ip,     int snapshot_port,
    const std::string     &incremental_ip,  int incremental_port,
    const std::string     &log_file)
    : outgoing_md_updates_(market_updates),
      snapshot_md_updates_(ME_MAX_MARKET_UPDATES),
      logger_(log_file),
      incremental_socket_(logger_) {

    ASSERT(incremental_socket_.init(incremental_ip, iface, incremental_port, /*is_listening=*/false) >= 0,
           "MDP incremental socket init failed on " +
           incremental_ip + ":" + std::to_string(incremental_port));

    // Heap allocation at startup — not on the hot path.
    snapshot_synthesizer_ = new SnapshotSynthesizer(
        &snapshot_md_updates_, iface, snapshot_ip, snapshot_port);

    logger_.log("%:% %() % MarketDataPublisher created. inc:%:% snap:%:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                incremental_ip, incremental_port,
                snapshot_ip, snapshot_port);
}

MarketDataPublisher::~MarketDataPublisher() {
    stop();
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);

    delete snapshot_synthesizer_;
    snapshot_synthesizer_ = nullptr;
}

auto MarketDataPublisher::start() -> void {
    run_ = true;
    snapshot_synthesizer_->start();
    auto *t = Common::createAndStartThread(
        -1, "Exchange/MarketDataPublisher", [this]() { run(); });
    ASSERT(t != nullptr, "Failed to start MarketDataPublisher thread.");
    t->detach();
    delete t;
    logger_.log("%:% %() % MarketDataPublisher started.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
}

auto MarketDataPublisher::stop() -> void {
    run_ = false;
    if (snapshot_synthesizer_)
        snapshot_synthesizer_->stop();
}

// ── Hot path ───────────────────────────────────────────────────────────────

/// Flush the multicast send buffer before it crosses one Ethernet MTU.
/// Batching whole bursts into a single sendto() risked (a) IP fragmentation
/// past ~1500 bytes and (b) EMSGSIZE — total loss of the batch — past 64KB.
static constexpr size_t MDP_FLUSH_THRESHOLD_BYTES = 1400;

auto MarketDataPublisher::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));

    while (run_) {
        poll();
    }
}

auto MarketDataPublisher::poll() noexcept -> void {
    // Drain all available updates from the matching engine.
    for (const auto *update = outgoing_md_updates_->getNextToRead();
         update != nullptr;
         update = outgoing_md_updates_->getNextToRead()) {

        TTT_MEASURE(T5_MarketDataPublisher_LFQueue_read, logger_);

        logger_.log("%:% %() % Processing %\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    update->toString());

        // 1. SBE two-send on incremental stream: seq_num then payload.
        //    Both land in the send buffer contiguously, forming
        //    MDPMarketUpdate on the wire without constructing a temp.
        //    (McastSocket::send memcpys, so *update is fully consumed here.)
        START_MEASURE(Exchange_McastSocket_send);
        incremental_socket_.send(&next_inc_seq_num_, sizeof(next_inc_seq_num_));
        incremental_socket_.send(update, sizeof(MEMarketUpdate));
        END_MEASURE(Exchange_McastSocket_send, logger_);

        // 2. Forward to snapshot synthesizer via internal LFQueue. This MUST
        //    happen before updateReadIndex(): releasing the slot first hands
        //    it back to the matching engine, which may overwrite it
        //    immediately under backpressure — the snapshot path would then
        //    copy torn/foreign data.
        auto *snap_slot = snapshot_md_updates_.getNextToWriteTo();
        snap_slot->seq_num_ = next_inc_seq_num_;
        snap_slot->me_market_update_ = *update;
        snapshot_md_updates_.updateWriteIndex();

        // 3. Consume from matching engine queue — the slot may be reused by
        //    the producer from this point on.
        outgoing_md_updates_->updateReadIndex();
        TTT_MEASURE(T6_MarketDataPublisher_UDP_write, logger_);

        ++next_inc_seq_num_;

        // 4. MTU-aware flush: keep each datagram under one MTU while
        //    still batching small bursts.
        if (UNLIKELY(incremental_socket_.next_send_valid_index_ +
                     sizeof(Exchange::MDPMarketUpdate) > MDP_FLUSH_THRESHOLD_BYTES)) {
            incremental_socket_.sendAndRecv();
        }
    }

    // Batched flush — outside the inner loop. One syscall for all
    // updates accumulated this pass, not one per message.
    incremental_socket_.sendAndRecv();
}

} // namespace Exchange
