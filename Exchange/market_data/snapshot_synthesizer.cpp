//
// SnapshotSynthesizer implementation.
//

#include "snapshot_synthesizer.h"

namespace Exchange {

SnapshotSynthesizer::SnapshotSynthesizer(
    MDPMarketUpdateLFQueue *snapshot_md_updates,
    const std::string      &iface,
    const std::string      &snapshot_ip,
    int                     snapshot_port,
    const std::string      &log_file)
    : snapshot_md_updates_(snapshot_md_updates),
      logger_(log_file),
      snapshot_socket_(logger_) {

    // Zero-initialise the order book reconstruction.
    for (auto &ticker : ticker_orders_)
        ticker.fill(nullptr);

    ASSERT(snapshot_socket_.init(snapshot_ip, iface, snapshot_port, /*is_listening=*/false) >= 0,
           "SnapshotSynthesizer socket init failed on " +
           snapshot_ip + ":" + std::to_string(snapshot_port));

    logger_.log("%:% %() % SnapshotSynthesizer created. snap:%:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                snapshot_ip, snapshot_port);
}

SnapshotSynthesizer::~SnapshotSynthesizer() {
    stop();
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);
}

auto SnapshotSynthesizer::start() -> void {
    run_ = true;
    auto *t = Common::createAndStartThread(
        -1, "Exchange/SnapshotSynthesizer", [this]() { run(); });
    ASSERT(t != nullptr, "Failed to start SnapshotSynthesizer thread.");
    t->detach();
    delete t;
    logger_.log("%:% %() % SnapshotSynthesizer started.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
}

auto SnapshotSynthesizer::stop() -> void {
    run_ = false;
}

// ── Hot path ───────────────────────────────────────────────────────────────

auto SnapshotSynthesizer::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));

    while (run_) {
        // Drain all available incremental updates.
        const auto *update = snapshot_md_updates_->getNextToRead();
        if (LIKELY(!update)) {
            // No updates — check if it's time for a periodic snapshot.
            if (UNLIKELY(last_snapshot_time_ > 0 &&
                         Common::getCurrentNanos() - last_snapshot_time_ > SNAPSHOT_INTERVAL)) {
                publishSnapshot();
            }
            continue;
        }

        logger_.log("%:% %() % Processing %\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    update->toString());

        addToSnapshot(update);
        snapshot_md_updates_->updateReadIndex();

        // Time-based snapshot trigger.
        if (UNLIKELY(last_snapshot_time_ == 0)) {
            last_snapshot_time_ = Common::getCurrentNanos();
        } else if (UNLIKELY(Common::getCurrentNanos() - last_snapshot_time_ > SNAPSHOT_INTERVAL)) {
            publishSnapshot();
        }
    }
}

auto SnapshotSynthesizer::addToSnapshot(const MDPMarketUpdate *update) noexcept -> void {
    const auto &mu = update->me_market_update_;
    last_inc_seq_num_ = update->seq_num_;

    const auto ticker_id = mu.ticker_id_.value;
    const auto order_id = mu.order_id_.value;

    // ADD/MODIFY/CANCEL index ticker_orders_[ticker][order_id] directly.
    // Market order ids grow monotonically, so a long-running session can
    // exceed ME_MAX_ORDER_IDS — drop (and log) instead of writing out of
    // bounds. TRADE etc. carry sentinel ids and fall through the switch.
    const bool indexes_book = (mu.type_ == MarketUpdateType::ADD ||
                               mu.type_ == MarketUpdateType::MODIFY ||
                               mu.type_ == MarketUpdateType::CANCEL);
    if (UNLIKELY(indexes_book &&
                 (ticker_id >= ME_MAX_TICKERS || order_id >= ME_MAX_ORDER_IDS))) {
        logger_.log("%:% %() % out-of-range ids in % — dropping\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    update->toString());
        return;
    }

    switch (mu.type_) {
        case MarketUpdateType::ADD: {
            // Allocate from pool, populate, store pointer.
            auto *order = order_pool_.allocate(mu);
            ticker_orders_[ticker_id][order_id] = order;
        } break;

        case MarketUpdateType::MODIFY: {
            // Update qty in place — no allocation or deallocation.
            auto *order = ticker_orders_[ticker_id][order_id];
            if (LIKELY(order)) {
                order->qty_ = mu.qty_;
            }
        } break;

        case MarketUpdateType::CANCEL: {
            // Deallocate back to pool, null out pointer.
            auto *order = ticker_orders_[ticker_id][order_id];
            if (LIKELY(order)) {
                order_pool_.deallocate(order);
                ticker_orders_[ticker_id][order_id] = nullptr;
            }
        } break;

        default:
            break;
    }
}

auto SnapshotSynthesizer::publishSnapshot() noexcept -> void {
    // Step 1: CLEAR — tells clients to wipe their local order book.
    snapshot_update_.seq_num_ = next_snap_seq_num_++;
    snapshot_update_.me_market_update_ = {};
    snapshot_update_.me_market_update_.type_ = MarketUpdateType::CLEAR;
    snapshot_socket_.send(&snapshot_update_, sizeof(MDPMarketUpdate));
    snapshot_socket_.sendAndRecv();

    // Step 2: SNAPSHOT_START
    snapshot_update_.seq_num_ = next_snap_seq_num_++;
    snapshot_update_.me_market_update_.type_ = MarketUpdateType::SNAPSHOT_START;
    snapshot_socket_.send(&snapshot_update_, sizeof(MDPMarketUpdate));
    snapshot_socket_.sendAndRecv();

    // Step 3: Walk every ticker × order, emit each resting order as ADD.
    for (size_t ticker = 0; ticker < ME_MAX_TICKERS; ++ticker) {
        for (size_t oid = 0; oid < ME_MAX_ORDER_IDS; ++oid) {
            const auto *order = ticker_orders_[ticker][oid];
            if (!order)
                continue;

            snapshot_update_.seq_num_ = next_snap_seq_num_++;
            snapshot_update_.me_market_update_ = *order;
            snapshot_update_.me_market_update_.type_ = MarketUpdateType::ADD;
            snapshot_socket_.send(&snapshot_update_, sizeof(MDPMarketUpdate));
            snapshot_socket_.sendAndRecv();
        }
    }

    // Step 4: SNAPSHOT_END — embed last_inc_seq_num_ in order_id_ so clients
    // know where to resume the incremental stream.
    snapshot_update_.seq_num_ = next_snap_seq_num_++;
    snapshot_update_.me_market_update_ = {};
    snapshot_update_.me_market_update_.type_ = MarketUpdateType::SNAPSHOT_END;
    snapshot_update_.me_market_update_.order_id_ = Common::OrderId{last_inc_seq_num_};
    snapshot_socket_.send(&snapshot_update_, sizeof(MDPMarketUpdate));
    snapshot_socket_.sendAndRecv();

    last_snapshot_time_ = Common::getCurrentNanos();

    logger_.log("%:% %() % Snapshot published. snap_seq:% last_inc_seq:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                next_snap_seq_num_ - 1, last_inc_seq_num_);
}

} // namespace Exchange
