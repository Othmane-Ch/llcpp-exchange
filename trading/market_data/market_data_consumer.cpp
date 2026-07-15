// trading/market_data/market_data_consumer.cpp

#include "market_data_consumer.h"

#include <cstring>

namespace Trading {

using namespace Common;
using Exchange::MEMarketUpdate;
using Exchange::MDPMarketUpdate;
using Exchange::MarketUpdateType;

// ── Construction ─────────────────────────────────────────────────────────────

MarketDataConsumer::MarketDataConsumer(
    Exchange::MEMarketUpdateLFQueue *incoming_md_updates,
    const std::string               &iface,
    const std::string               &snapshot_ip,    int snapshot_port,
    const std::string               &incremental_ip, int incremental_port,
    const std::string               &log_file)
    : outgoing_md_updates_(incoming_md_updates),
      logger_(log_file),
      incremental_socket_(logger_),
      snapshot_socket_(logger_),
      iface_(iface),
      snapshot_ip_(snapshot_ip),       snapshot_port_(snapshot_port),
      incremental_ip_(incremental_ip), incremental_port_(incremental_port) {

    // Wire callbacks. McastSocket::rcv_callback_ fires inside sendAndRecv()
    // on our own thread when data arrives -- no cross-thread concerns.
    incremental_socket_.rcv_callback_ = [this](McastSocket *s) { recvIncremental(s); };
    snapshot_socket_.rcv_callback_    = [this](McastSocket *s) { recvSnapshot(s);    };

    ASSERT(incremental_socket_.init(incremental_ip_, iface_, incremental_port_,
                                    /*is_listening=*/true) >= 0,
           "MDC incremental socket init failed on " +
           incremental_ip_ + ":" + std::to_string(incremental_port_));

    // Snapshot socket is only init'd on demand (during recovery) to avoid
    // processing snapshots when the stream is healthy.
    logger_.log("%:% %() % MarketDataConsumer created inc:%:% snap:%:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                incremental_ip_, incremental_port_,
                snapshot_ip_, snapshot_port_);
}

MarketDataConsumer::~MarketDataConsumer() {
    stop();
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);
}

auto MarketDataConsumer::start() -> void {
    run_.store(true);
    auto *t = Common::createAndStartThread(
        -1, "Trading/MarketDataConsumer", [this]() { run(); });
    ASSERT(t != nullptr, "Failed to start MarketDataConsumer thread.");
    t->detach();
    delete t;
    logger_.log("%:% %() % MarketDataConsumer started.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
}

auto MarketDataConsumer::stop() -> void {
    run_.store(false);
}

// ── Busy-spin loop ───────────────────────────────────────────────────────────

auto MarketDataConsumer::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));

    while (run_.load()) {
        // sendAndRecv flushes our (empty) send buffer and pulls any pending
        // datagrams, firing rcv_callback_ which dispatches parsing.
        incremental_socket_.sendAndRecv();
        if (UNLIKELY(in_recovery_.load())) {
            snapshot_socket_.sendAndRecv();
        }
    }
}

// ── Incremental receive path ─────────────────────────────────────────────────

auto MarketDataConsumer::recvIncremental(McastSocket *socket) noexcept -> void {
    TTT_MEASURE(T7_MarketDataConsumer_UDP_read, logger_);
    START_MEASURE(Trading_MarketDataConsumer_recvCallback);
    // Drain complete MDPMarketUpdate frames from the socket rcv buffer.
    // Read with an offset and compact once — per-frame memmove of the whole
    // remaining buffer made a K-frame burst cost O(K * buffered_bytes).
    size_t offset = 0;
    while (socket->next_rcv_valid_index_ - offset >= sizeof(MDPMarketUpdate)) {
        MDPMarketUpdate msg;
        std::memcpy(&msg, socket->rcv_buffer_ + offset, sizeof(MDPMarketUpdate));
        offset += sizeof(MDPMarketUpdate);

        processIncremental(msg);
    }
    if (offset > 0) {
        std::memmove(socket->rcv_buffer_,
                     socket->rcv_buffer_ + offset,
                     socket->next_rcv_valid_index_ - offset);
        socket->next_rcv_valid_index_ -= offset;
    }
    END_MEASURE(Trading_MarketDataConsumer_recvCallback, logger_);
}

auto MarketDataConsumer::processIncremental(const MDPMarketUpdate &msg) noexcept -> void {
    const auto seq = msg.seq_num_;

    // Recovery mode: buffer everything, don't forward yet.
    if (UNLIKELY(in_recovery_.load())) {
        out_of_order_buf_[seq] = msg.me_market_update_;
        logger_.log("%:% %() % (recovery) buffered inc seq:%\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), seq);
        return;
    }

    // Steady state.
    if (LIKELY(seq == next_exp_inc_seq_num_)) {
        publishDownstream(msg.me_market_update_);
        ++next_exp_inc_seq_num_;

        // Drain any previously-buffered contiguous messages (seq ahead of us).
        while (!out_of_order_buf_.empty() &&
               out_of_order_buf_.begin()->first == next_exp_inc_seq_num_) {
            publishDownstream(out_of_order_buf_.begin()->second);
            out_of_order_buf_.erase(out_of_order_buf_.begin());
            ++next_exp_inc_seq_num_;
        }
        return;
    }

    if (seq < next_exp_inc_seq_num_) {
        // Duplicate / stale. Drop.
        logger_.log("%:% %() % Dropping stale inc seq:% (expected:%)\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    seq, next_exp_inc_seq_num_);
        return;
    }

    // seq > next_exp_inc_seq_num_ → gap detected. Buffer and start recovery.
    out_of_order_buf_[seq] = msg.me_market_update_;
    logger_.log("%:% %() % GAP expected:% got:% -- starting recovery\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                next_exp_inc_seq_num_, seq);
    startRecovery();
}

auto MarketDataConsumer::startRecovery() noexcept -> void {
    if (in_recovery_.load()) return; // already recovering
    in_recovery_.store(true);
    snapshot_armed_ = false;
    in_snapshot_ = false;
    snapshot_last_seen_ = 0;

    // Subscribe to the snapshot multicast stream.
    ASSERT(snapshot_socket_.init(snapshot_ip_, iface_, snapshot_port_,
                                 /*is_listening=*/true) >= 0,
           "MDC snapshot socket init failed on " +
           snapshot_ip_ + ":" + std::to_string(snapshot_port_));

    logger_.log("%:% %() % Recovery started -- snapshot stream %:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                snapshot_ip_, snapshot_port_);
}

// ── Snapshot receive path ────────────────────────────────────────────────────

auto MarketDataConsumer::recvSnapshot(McastSocket *socket) noexcept -> void {
    size_t offset = 0;
    while (socket->next_rcv_valid_index_ - offset >= sizeof(MDPMarketUpdate)) {
        MDPMarketUpdate msg;
        std::memcpy(&msg, socket->rcv_buffer_ + offset, sizeof(MDPMarketUpdate));
        offset += sizeof(MDPMarketUpdate);

        processSnapshot(msg);
    }
    if (offset > 0) {
        std::memmove(socket->rcv_buffer_,
                     socket->rcv_buffer_ + offset,
                     socket->next_rcv_valid_index_ - offset);
        socket->next_rcv_valid_index_ -= offset;
    }
}

auto MarketDataConsumer::processSnapshot(const MDPMarketUpdate &msg) noexcept -> void {
    if (!in_recovery_.load()) return;

    // A snapshot cycle is CLEAR → SNAPSHOT_START → ADD* → SNAPSHOT_END with
    // consecutive seq_nums (see Exchange/market_data/snapshot_synthesizer.cpp).
    // The snapshot stream is UDP: a lost datagram inside the cycle means the
    // book image is incomplete, so ANY gap aborts the cycle — declaring
    // recovery complete on a partial image is silent book corruption. The
    // next periodic CLEAR starts a fresh attempt, and its downstream wipe
    // erases any partially applied ADDs from the aborted one.
    const auto seq = msg.seq_num_;

    switch (msg.me_market_update_.type_) {
        case MarketUpdateType::CLEAR: {
            // Begin a fresh reassembly attempt anchored at this seq. Tell
            // downstream to wipe.
            in_snapshot_ = false;
            snapshot_armed_ = true;
            snapshot_last_seen_ = seq;
            publishDownstream(msg.me_market_update_);
        } break;

        case MarketUpdateType::SNAPSHOT_START: {
            // Valid only immediately after the CLEAR that anchored the cycle.
            if (LIKELY(snapshot_armed_ && seq == snapshot_last_seen_ + 1)) {
                in_snapshot_ = true;
                snapshot_last_seen_ = seq;
            } else {
                in_snapshot_ = false; // gap → abort; wait for the next CLEAR
                logger_.log("%:% %() % snapshot gap at START seq:% last_seen:% -- cycle aborted\n",
                            __FILE__, __LINE__, __FUNCTION__,
                            Common::getCurrentTimeStr(&time_str_),
                            seq, snapshot_last_seen_);
            }
            snapshot_armed_ = false;
        } break;

        case MarketUpdateType::ADD: {
            if (LIKELY(in_snapshot_)) {
                if (LIKELY(seq == snapshot_last_seen_ + 1)) {
                    // Replay as ADD to the downstream book.
                    publishDownstream(msg.me_market_update_);
                    snapshot_last_seen_ = seq;
                } else {
                    // Lost a datagram mid-cycle -- the image is incomplete.
                    in_snapshot_ = false;
                    logger_.log("%:% %() % snapshot gap at ADD seq:% last_seen:% -- cycle aborted\n",
                                __FILE__, __LINE__, __FUNCTION__,
                                Common::getCurrentTimeStr(&time_str_),
                                seq, snapshot_last_seen_);
                }
            }
        } break;

        case MarketUpdateType::SNAPSHOT_END: {
            // SNAPSHOT_END.order_id_ carries last_inc_seq_num_ (see
            // Exchange/market_data/snapshot_synthesizer.cpp).
            if (LIKELY(in_snapshot_)) {
                if (LIKELY(seq == snapshot_last_seen_ + 1)) {
                    snapshot_last_seen_ = seq;
                    pending_last_inc_seq_ = msg.me_market_update_.order_id_.value;
                    finishRecovery(pending_last_inc_seq_);
                } else {
                    in_snapshot_ = false;
                    logger_.log("%:% %() % snapshot gap at END seq:% last_seen:% -- cycle aborted\n",
                                __FILE__, __LINE__, __FUNCTION__,
                                Common::getCurrentTimeStr(&time_str_),
                                seq, snapshot_last_seen_);
                }
            }
        } break;

        default:
            // MODIFY/CANCEL/TRADE do not appear in a snapshot stream; ignore.
            break;
    }
}

auto MarketDataConsumer::finishRecovery(size_t last_inc_seq_num) noexcept -> void {
    // Resume expected seq num at (snapshot end + 1).
    next_exp_inc_seq_num_ = last_inc_seq_num + 1;

    // Replay buffered incrementals that are at or beyond the new expected seq.
    // std::map is ordered -- iterate in seq order.
    auto it = out_of_order_buf_.begin();
    while (it != out_of_order_buf_.end()) {
        if (it->first < next_exp_inc_seq_num_) {
            // Snapshot already reflects these -- drop.
            it = out_of_order_buf_.erase(it);
        } else if (it->first == next_exp_inc_seq_num_) {
            publishDownstream(it->second);
            ++next_exp_inc_seq_num_;
            it = out_of_order_buf_.erase(it);
        } else {
            // Still a gap between snapshot and buffered incrementals. Leave
            // the rest in the buffer; they'll be drained as incrementals catch up.
            break;
        }
    }

    // Leave the snapshot multicast group (destroy + no re-init).
    // McastSocket has no leave(); destroy() closes the fd which implicitly
    // drops the group membership.
    snapshot_socket_.destroy();

    snapshot_armed_ = false;
    in_snapshot_ = false;
    in_recovery_.store(false);
    logger_.log("%:% %() % Recovery complete -- resuming at inc seq:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                next_exp_inc_seq_num_);
}

// ── Downstream publish ───────────────────────────────────────────────────────

auto MarketDataConsumer::publishDownstream(const MEMarketUpdate &u) noexcept -> void {
    auto *slot = outgoing_md_updates_->getNextToWriteTo();
    *slot = u;
    outgoing_md_updates_->updateWriteIndex();
    TTT_MEASURE(T8_MarketDataConsumer_LFQueue_write, logger_);
}

} // namespace Trading
