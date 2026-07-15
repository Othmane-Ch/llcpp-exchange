// trading/strategy/trade_engine.h
//
// TradeEngine -- the strategy-side orchestrator.
//
// Owns the per-ticker MarketOrderBook array and the strategy sub-components
// (FeatureEngine, PositionKeeper, OrderManager, RiskManager). Drains two
// inbound LFQueues in a busy-spin loop:
//   - MEMarketUpdateLFQueue (from MarketDataConsumer)
//   - ClientResponseLFQueue (from OrderGateway)
// and pushes outbound MEClientRequest into ClientRequestLFQueue (to
// OrderGateway).
//
// The strategy itself is plugged in through three callbacks set after
// construction:
//   - setBookUpdateCallback   — fires on every BBO change
//   - setTradeUpdateCallback  — fires on TRADE updates
//   - setOrderUpdateCallback  — fires on every exchange response
//
// Two construction modes:
//   - Test mode (default ctor): no LFQueues are wired; sendClientRequest()
//     captures into sent_requests_ for inspection by unit tests.
//   - Production mode (full ctor): wires LFQueues, owns sub-components,
//     starts a busy-spin run() loop on start().
//

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "types.h"
#include "logging.h"
#include "macros.h"
#include "time_utils.h"
#include "thread_utils.h"

#include "client_request.h"   // Exchange::MEClientRequest, ClientRequestLFQueue
#include "client_response.h"  // Exchange::MEClientResponse, ClientResponseLFQueue
#include "market_update.h"    // Exchange::MEMarketUpdate, MEMarketUpdateLFQueue

#include "market_order_book.h"
#include "feature_engine.h"
#include "position_keeper.h"
#include "order_manager.h"
#include "risk_manager.h"

namespace Trading {

class TradeEngine final {
public:
    using BookUpdateCallback  = std::function<void(Common::TickerId, Common::Price,
                                                   Common::Side, MarketOrderBook *)>;
    using TradeUpdateCallback = std::function<void(const Exchange::MEMarketUpdate *,
                                                   MarketOrderBook *)>;
    using OrderUpdateCallback = std::function<void(const Exchange::MEClientResponse *)>;

    /// Test-mode ctor: no I/O, no sub-components. sendClientRequest() captures
    /// requests into sent_requests_ for inspection. Used by OrderManager unit
    /// tests that want to observe what would have been sent.
    TradeEngine() = default;

    /// Production ctor. Wires up the three LFQueues, allocates per-ticker
    /// MarketOrderBooks, and constructs the strategy sub-components.
    /// Caller owns the LFQueues; this object does not.
    TradeEngine(Common::ClientId            client_id,
                Exchange::ClientRequestLFQueue  *outgoing_requests,
                Exchange::ClientResponseLFQueue *incoming_responses,
                Exchange::MEMarketUpdateLFQueue *incoming_market_updates,
                const Common::TradeEngineCfgHashMap &cfg_map,
                const std::string &log_file = "trade_engine.log");

    ~TradeEngine();

    auto start() -> void;
    auto stop()  -> void;

    // ── Hot-path interface used by OrderManager and other components ────

    auto clientId() const noexcept -> Common::ClientId { return client_id_; }

    /// Send a request to the OrderGateway via the outbound LFQueue.
    /// In test mode (no LFQueue), the request is captured in sent_requests_.
    auto sendClientRequest(const Exchange::MEClientRequest *req) noexcept -> void {
        if (outgoing_requests_) {
            auto *slot = outgoing_requests_->getNextToWriteTo();
            new (slot) Exchange::MEClientRequest(*req);
            outgoing_requests_->updateWriteIndex();
            TTT_MEASURE(T10_TradeEngine_LFQueue_write, (*logger_));
        } else {
            sent_requests_.push_back(*req);
        }
    }

    // FeatureEngine forwarding helpers (called by run() handlers).
    // These exist so the existing FeatureEngine signature
    //   onTradeUpdate(MEMarketUpdate*, MarketOrderBook*)
    // can be reached without exposing FeatureEngine internals.
    auto onTradeUpdate(const Exchange::MEMarketUpdate *u,
                       MarketOrderBook *book) noexcept -> void {
        if (feature_engine_) feature_engine_->onTradeUpdate(u, book);
    }
    auto onOrderBookUpdate(Common::TickerId ticker, Common::Price price,
                           Common::Side side) noexcept -> void {
        if (feature_engine_ && market_order_books_[ticker.value]) {
            feature_engine_->onOrderBookUpdate(ticker, price, side,
                                               market_order_books_[ticker.value]);
        }
    }

    // ── Sub-component access for strategies ─────────────────────────────

    auto featureEngine()  noexcept -> FeatureEngine *  { return feature_engine_.get(); }
    auto positionKeeper() noexcept -> PositionKeeper * { return position_keeper_.get(); }
    auto orderManager()   noexcept -> OrderManager *   { return order_manager_.get(); }
    auto riskManager()    noexcept -> RiskManager *    { return risk_manager_.get(); }
    auto getMarketOrderBook(Common::TickerId ticker) noexcept -> MarketOrderBook * {
        return market_order_books_[ticker.value];
    }

    // ── Strategy callback installation ──────────────────────────────────

    auto setBookUpdateCallback(BookUpdateCallback cb)   noexcept { book_cb_  = std::move(cb); }
    auto setTradeUpdateCallback(TradeUpdateCallback cb) noexcept { trade_cb_ = std::move(cb); }
    auto setOrderUpdateCallback(OrderUpdateCallback cb) noexcept { order_cb_ = std::move(cb); }

    // ── Test-only access ────────────────────────────────────────────────

    auto getSentRequests() const noexcept
        -> const std::vector<Exchange::MEClientRequest> & { return sent_requests_; }

    auto setClientIdForTest(Common::ClientId cid) noexcept { client_id_ = cid; }

    /// Drain one cycle of both inbound LFQueues without spinning. Used by
    /// integration-style unit tests that drive the engine deterministically.
    auto drainOnceForTest() noexcept -> void;

    TradeEngine(const TradeEngine &)             = delete;
    TradeEngine(TradeEngine &&)                  = delete;
    TradeEngine &operator=(const TradeEngine &)  = delete;
    TradeEngine &operator=(TradeEngine &&)       = delete;

private:
    // Busy-spin loop: drains response and market-update queues, dispatches.
    auto run() noexcept -> void;

    // Internal handlers wired into run() and drainOnceForTest().
    auto handleResponse(const Exchange::MEClientResponse *r)   noexcept -> void;
    auto handleMarketUpdate(const Exchange::MEMarketUpdate *u) noexcept -> void;

    // Identity and queues. Null in test mode.
    Common::ClientId                  client_id_{};
    Exchange::ClientRequestLFQueue   *outgoing_requests_        = nullptr;
    Exchange::ClientResponseLFQueue  *incoming_responses_       = nullptr;
    Exchange::MEMarketUpdateLFQueue  *incoming_market_updates_  = nullptr;

    // Per-ticker books. Lazily allocated on first reference (allocate-once
    // pattern -- big arrays inside MarketOrderBook).
    std::array<MarketOrderBook *, Common::ME_MAX_TICKERS> market_order_books_{};

    // Strategy sub-components. Constructed in production ctor only.
    std::unique_ptr<FeatureEngine>  feature_engine_;
    std::unique_ptr<PositionKeeper> position_keeper_;
    std::unique_ptr<OrderManager>   order_manager_;
    std::unique_ptr<RiskManager>    risk_manager_;

    // Strategy callbacks (no-op if unset).
    BookUpdateCallback  book_cb_;
    TradeUpdateCallback trade_cb_;
    OrderUpdateCallback order_cb_;

    std::atomic<bool> run_{false};

    // Production-mode logger lifetime. We own a Logger only in production
    // mode; in test mode logger_ stays nullptr and sub-components are also
    // null (so no logging path is exercised).
    std::unique_ptr<Common::Logger> logger_;
    std::string time_str_;

    // Test-mode capture buffer (unused in production mode).
    std::vector<Exchange::MEClientRequest> sent_requests_{};
};

} // namespace Trading
