// trading/strategy/trade_engine.cpp

#include "trade_engine.h"

#include <chrono>
#include <thread>

namespace Trading {

using namespace Common;

// ── Production constructor ─────────────────────────────────────────────────

TradeEngine::TradeEngine(ClientId             client_id,
                         Exchange::ClientRequestLFQueue  *outgoing_requests,
                         Exchange::ClientResponseLFQueue *incoming_responses,
                         Exchange::MEMarketUpdateLFQueue *incoming_market_updates,
                         const TradeEngineCfgHashMap &cfg_map,
                         const std::string &log_file)
    : client_id_(client_id),
      outgoing_requests_(outgoing_requests),
      incoming_responses_(incoming_responses),
      incoming_market_updates_(incoming_market_updates),
      logger_(std::make_unique<Logger>(log_file)) {
    // Per-ticker MarketOrderBook allocation. Each book embeds a multi-MB
    // direct-indexed order array, so allocate once at startup.
    for (size_t i = 0; i < market_order_books_.size(); ++i) {
        market_order_books_[i] = new MarketOrderBook(TickerId{static_cast<uint32_t>(i)},
                                                     logger_.get());
    }

    // Sub-components.
    feature_engine_  = std::make_unique<FeatureEngine>(logger_.get());
    position_keeper_ = std::make_unique<PositionKeeper>(logger_.get());
    risk_manager_    = std::make_unique<RiskManager>(logger_.get(),
                                                     position_keeper_.get(), cfg_map);
    order_manager_   = std::make_unique<OrderManager>(logger_.get(), this, *risk_manager_);

    logger_->log("%:% %() % TradeEngine created. client:%\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 getCurrentTimeStr(&time_str_), client_id_.value);
}

// ── Destructor ─────────────────────────────────────────────────────────────

TradeEngine::~TradeEngine() {
    stop();

    // Allow detached run() thread to exit before queue/pointer teardown.
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);

    // Drop sub-components before books, since they may have pointers to books.
    order_manager_.reset();
    risk_manager_.reset();
    position_keeper_.reset();
    feature_engine_.reset();

    for (auto &b : market_order_books_) {
        delete b;
        b = nullptr;
    }

    outgoing_requests_       = nullptr;
    incoming_responses_      = nullptr;
    incoming_market_updates_ = nullptr;
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

auto TradeEngine::start() -> void {
    run_ = true;
    auto *t = createAndStartThread(-1, "Trading/TradeEngine",
                                    [this]() { run(); });
    ASSERT(t != nullptr, "Failed to start TradeEngine thread.");
    t->detach();
    delete t;
    if (logger_) {
        logger_->log("%:% %() % TradeEngine started.\n",
                     __FILE__, __LINE__, __FUNCTION__,
                     getCurrentTimeStr(&time_str_));
    }
}

auto TradeEngine::stop() -> void {
    run_ = false;
    if (logger_) {
        logger_->log("%:% %() % TradeEngine stopping.\n",
                     __FILE__, __LINE__, __FUNCTION__,
                     getCurrentTimeStr(&time_str_));
    }
}

// ── Inbound dispatch ───────────────────────────────────────────────────────

auto TradeEngine::handleResponse(const Exchange::MEClientResponse *r) noexcept -> void {
    // ticker_id arrives from the gateway queue — validate it before it
    // indexes market_order_books_ or PositionKeeper's per-ticker arrays
    // (addFill uses .at() inside noexcept: an out-of-range id would
    // std::terminate the client). Mirrors handleMarketUpdate's validation.
    if (UNLIKELY(r->ticker_id_.value >= market_order_books_.size())) {
        if (logger_) {
            logger_->log("%:% %() % Dropping response with invalid ticker: %\n",
                         __FILE__, __LINE__, __FUNCTION__,
                         getCurrentTimeStr(&time_str_), r->toString());
        }
        return;
    }

    // FILLED → addFill on PositionKeeper, then update unrealized PnL via the
    // ticker's BBO. Then forward to OrderManager: handleResponse is the
    // SINGLE dispatcher into the order state machine — strategies observe
    // responses via order_cb_ and must not forward to OrderManager again.
    if (r->type_ == Exchange::ClientResponseType::FILLED && position_keeper_) {
        START_MEASURE(Trading_PositionKeeper_addFill);
        position_keeper_->addFill(r);
        END_MEASURE(Trading_PositionKeeper_addFill, (*logger_));
        if (auto *book = market_order_books_[r->ticker_id_.value]) {
            position_keeper_->updateBBO(r->ticker_id_, &book->getBBO());
        }
    }

    if (order_manager_) order_manager_->onOrderUpdate(r);
    if (order_cb_) {
        START_MEASURE(Trading_TradeEngine_algoOnOrderUpdate_);
        order_cb_(r);
        END_MEASURE(Trading_TradeEngine_algoOnOrderUpdate_, (*logger_));
    }
}

auto TradeEngine::handleMarketUpdate(const Exchange::MEMarketUpdate *u) noexcept -> void {
    const auto tid = u->ticker_id_.value;

    // ticker_id must be validated before it indexes market_order_books_.
    // The snapshot stream's CLEAR is built with the sentinel (INVALID)
    // ticker id — it is an exchange-wide instruction, so broadcast it to
    // every book; indexing with the sentinel used to read out of bounds
    // and crash the client on its first snapshot recovery.
    if (UNLIKELY(tid >= market_order_books_.size())) {
        if (u->type_ == Exchange::MarketUpdateType::CLEAR) {
            for (auto *b : market_order_books_) {
                if (b) b->onMarketUpdate(u);
            }
        }
        return;
    }

    auto *book = market_order_books_[tid];
    if (UNLIKELY(!book)) return;

    // Apply to the local book first (mutates BBO).
    book->onMarketUpdate(u);

    if (u->type_ == Exchange::MarketUpdateType::TRADE) {
        if (feature_engine_) {
            START_MEASURE(Trading_FeatureEngine_onTradeUpdate);
            feature_engine_->onTradeUpdate(u, book);
            END_MEASURE(Trading_FeatureEngine_onTradeUpdate, (*logger_));
        }
        if (trade_cb_) {
            START_MEASURE(Trading_TradeEngine_algoOnTradeUpdate_);
            trade_cb_(u, book);
            END_MEASURE(Trading_TradeEngine_algoOnTradeUpdate_, (*logger_));
        }
    } else {
        // ADD / MODIFY / CANCEL / CLEAR / SNAPSHOT_*: BBO may have changed.
        if (feature_engine_) {
            START_MEASURE(Trading_FeatureEngine_onOrderBookUpdate);
            feature_engine_->onOrderBookUpdate(u->ticker_id_, u->price_, u->side_, book);
            END_MEASURE(Trading_FeatureEngine_onOrderBookUpdate, (*logger_));
        }
        if (position_keeper_) {
            START_MEASURE(Trading_PositionKeeper_updateBBO);
            position_keeper_->updateBBO(u->ticker_id_, &book->getBBO());
            END_MEASURE(Trading_PositionKeeper_updateBBO, (*logger_));
        }
        if (book_cb_) {
            START_MEASURE(Trading_TradeEngine_algoOnOrderBookUpdate_);
            book_cb_(u->ticker_id_, u->price_, u->side_, book);
            END_MEASURE(Trading_TradeEngine_algoOnOrderBookUpdate_, (*logger_));
        }
    }
}

// ── Busy-spin run loop ─────────────────────────────────────────────────────

auto TradeEngine::run() noexcept -> void {
    if (logger_) {
        logger_->log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                     getCurrentTimeStr(&time_str_));
    }
    while (run_.load(std::memory_order_relaxed)) {
        // Drain one response if present.
        if (incoming_responses_) {
            const auto *r = incoming_responses_->getNextToRead();
            if (UNLIKELY(r != nullptr)) {
                TTT_MEASURE(T9t_TradeEngine_LFQueue_read, (*logger_));
                handleResponse(r);
                incoming_responses_->updateReadIndex();
            }
        }
        // Drain one market update if present.
        if (incoming_market_updates_) {
            const auto *u = incoming_market_updates_->getNextToRead();
            if (UNLIKELY(u != nullptr)) {
                TTT_MEASURE(T9_TradeEngine_LFQueue_read, (*logger_));
                handleMarketUpdate(u);
                incoming_market_updates_->updateReadIndex();
            }
        }
    }
}

// ── Test-only deterministic drain ──────────────────────────────────────────

auto TradeEngine::drainOnceForTest() noexcept -> void {
    if (incoming_responses_) {
        while (const auto *r = incoming_responses_->getNextToRead()) {
            handleResponse(r);
            incoming_responses_->updateReadIndex();
        }
    }
    if (incoming_market_updates_) {
        while (const auto *u = incoming_market_updates_->getNextToRead()) {
            handleMarketUpdate(u);
            incoming_market_updates_->updateReadIndex();
        }
    }
}

} // namespace Trading
