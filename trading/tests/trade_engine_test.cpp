// trade_engine_test.cpp -- integration-style tests for Trading::TradeEngine.
//
// The production TradeEngine wires three LFQueues, per-ticker MarketOrderBooks,
// FeatureEngine, PositionKeeper, OrderManager, and RiskManager. Tests drive
// it deterministically via drainOnceForTest() instead of starting the
// busy-spin thread, so we can assert state after each batch of inputs.

#include "doctest/doctest.h"

#include <cmath>
#include <memory>

#include "trade_engine.h"
#include "market_maker.h"
#include "liquidity_taker.h"

using namespace Trading;
using namespace Common;
using Exchange::ClientRequestLFQueue;
using Exchange::ClientResponseLFQueue;
using Exchange::MEMarketUpdateLFQueue;
using Exchange::MEMarketUpdate;
using Exchange::MEClientRequest;
using Exchange::MEClientResponse;
using Exchange::MarketUpdateType;
using Exchange::ClientResponseType;

// ── Helpers ─────────────────────────────────────────────────────────────────

static auto pushResponse(ClientResponseLFQueue *q, const MEClientResponse &r) -> void {
    auto *slot = q->getNextToWriteTo();
    new (slot) MEClientResponse(r);
    q->updateWriteIndex();
}

static auto pushMarketUpdate(MEMarketUpdateLFQueue *q, const MEMarketUpdate &u) -> void {
    auto *slot = q->getNextToWriteTo();
    new (slot) MEMarketUpdate(u);
    q->updateWriteIndex();
}

static auto mkMU(MarketUpdateType t, Side side, uint64_t price, uint32_t qty,
                 uint64_t oid, uint32_t ticker = 0,
                 uint64_t prio = 1) -> MEMarketUpdate {
    MEMarketUpdate u{};
    u.type_      = t;
    u.ticker_id_ = TickerId{ticker};
    u.side_      = side;
    u.price_     = Price{price};
    u.qty_       = Qty{qty};
    u.order_id_  = OrderId{oid};
    u.priority_  = Priority{prio};
    return u;
}

static auto mkResp(ClientResponseType type, OrderId oid, Side side,
                   uint64_t price, uint32_t exec_qty, uint32_t leaves_qty,
                   uint32_t ticker = 0) -> MEClientResponse {
    MEClientResponse r{};
    r.type_           = type;
    r.client_id_      = ClientId{1};
    r.ticker_id_      = TickerId{ticker};
    r.client_order_id_ = oid;
    r.market_order_id_ = oid;
    r.side_           = side;
    r.price_          = Price{price};
    r.exec_qty_       = Qty{exec_qty};
    r.leaves_qty_     = Qty{leaves_qty};
    return r;
}

// ── Production-mode fixture (uses real LFQueues, no thread) ─────────────────

namespace { struct ProdFixture {
    ClientRequestLFQueue  outgoing_requests{ME_MAX_CLIENT_UPDATES};
    ClientResponseLFQueue incoming_responses{ME_MAX_CLIENT_UPDATES};
    MEMarketUpdateLFQueue incoming_market_updates{ME_MAX_MARKET_UPDATES};

    TradeEngineCfgHashMap cfg{};
    std::unique_ptr<TradeEngine> te;

    ProdFixture() {
        cfg[0].clip_       = Qty{5};
        cfg[0].threshold_  = 0.0;
        cfg[0].risk_cfg_   = RiskCfg{Qty{50}, Qty{100}, -1000.0};

        te = std::make_unique<TradeEngine>(
            ClientId{1},
            &outgoing_requests, &incoming_responses, &incoming_market_updates,
            cfg, "test_trade_engine.log");
    }

    auto outgoingCount() -> size_t {
        size_t n = 0;
        while (const auto *r = outgoing_requests.getNextToRead()) {
            (void)r;
            outgoing_requests.updateReadIndex();
            ++n;
        }
        return n;
    }
}; } // anonymous namespace

// ── Test 1: production ctor wires sub-components ───────────────────────────
TEST_CASE("TradeEngine: production constructor wires all sub-components") {
    ProdFixture f;
    CHECK(f.te->featureEngine()  != nullptr);
    CHECK(f.te->positionKeeper() != nullptr);
    CHECK(f.te->orderManager()   != nullptr);
    CHECK(f.te->riskManager()    != nullptr);
    CHECK(f.te->getMarketOrderBook(TickerId{0}) != nullptr);
    CHECK(f.te->clientId().value == 1);
}

// ── Test 2: market updates propagate book → FeatureEngine ──────────────────
TEST_CASE("TradeEngine: market updates flow into MarketOrderBook and FeatureEngine") {
    ProdFixture f;

    // Two ADDs to establish a balanced top of book.
    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::ADD, Side::BUY,  100, 10, 1));
    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::ADD, Side::SELL, 101, 10, 2));

    f.te->drainOnceForTest();

    const auto &bbo = f.te->getMarketOrderBook(TickerId{0})->getBBO();
    CHECK(bbo.bid_price_.value == 100);
    CHECK(bbo.ask_price_.value == 101);
    // mkt_price = (100*10 + 101*10) / 20 = 100.5
    CHECK(f.te->featureEngine()->getMktPrice() == doctest::Approx(100.5));
}

// ── Test 3: TRADE updates compute the aggressive-trade-qty ratio ───────────
TEST_CASE("TradeEngine: TRADE updates flow into FeatureEngine") {
    ProdFixture f;

    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::ADD, Side::BUY,  100, 10, 1));
    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::ADD, Side::SELL, 101, 4,  2));
    f.te->drainOnceForTest();

    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::TRADE, Side::BUY, 101, 2, 0));
    f.te->drainOnceForTest();

    CHECK(f.te->featureEngine()->getAggTradeQtyRatio() == doctest::Approx(0.5));
}

// ── Test 4: FILLED responses update PositionKeeper PnL ─────────────────────
TEST_CASE("TradeEngine: FILLED response updates PositionKeeper") {
    ProdFixture f;

    // Need a BBO so updateBBO inside handleResponse has something to mark to.
    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::ADD, Side::BUY,  100, 10, 1));
    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::ADD, Side::SELL, 101, 10, 2));
    f.te->drainOnceForTest();

    pushResponse(&f.incoming_responses,
                 mkResp(ClientResponseType::FILLED, OrderId{1}, Side::BUY,
                        100, /*exec*/10, /*leaves*/0));
    f.te->drainOnceForTest();

    const auto *pi = f.te->positionKeeper()->getPositionInfo(TickerId{0});
    CHECK(pi->position_  == 10);
    CHECK(pi->volume_.value == 10);
    // Mid is 100.5, vwap is 100, position 10 → unreal_pnl ≈ 5
    CHECK(pi->unreal_pnl_ == doctest::Approx(5.0));
}

// ── Test 5: ACCEPTED → LIVE; cancel-rejected leaves state alone ────────────
TEST_CASE("TradeEngine: order-update callback fires on every response") {
    ProdFixture f;

    // Drive a quote from the strategy via the OrderManager directly.
    f.te->orderManager()->moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});

    // Outgoing requests should now be on the queue.
    const auto outgoing = f.outgoingCount();
    CHECK(outgoing == 2);  // BUY + SELL

    // Capture order-update callbacks fired by handleResponse.
    int cb_count = 0;
    f.te->setOrderUpdateCallback([&](const MEClientResponse *) { ++cb_count; });

    pushResponse(&f.incoming_responses,
                 mkResp(ClientResponseType::ACCEPTED,
                        f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_id_,
                        Side::BUY, 99, 0, 5));
    f.te->drainOnceForTest();

    CHECK(cb_count == 1);
    CHECK(f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_state_
          == OMOrderState::LIVE);
}

// ── Test 6: book-update callback fires on book mutations, not on TRADEs ─────
TEST_CASE("TradeEngine: book-update vs trade-update callback routing") {
    ProdFixture f;

    int book_calls = 0, trade_calls = 0;
    f.te->setBookUpdateCallback ([&](TickerId, Price, Side, MarketOrderBook *) { ++book_calls; });
    f.te->setTradeUpdateCallback([&](const MEMarketUpdate *, MarketOrderBook *) { ++trade_calls; });

    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::ADD, Side::BUY,  100, 10, 1));
    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::ADD, Side::SELL, 101, 10, 2));
    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::TRADE, Side::BUY, 101, 3, 0));
    f.te->drainOnceForTest();

    CHECK(book_calls  == 2);
    CHECK(trade_calls == 1);
}

// ── Test 7: end-to-end strategy quote-then-fill loop ───────────────────────
TEST_CASE("TradeEngine: end-to-end quote, accept, fill flow") {
    ProdFixture f;

    // Build a balanced book first so risk gates have a reference point.
    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::ADD, Side::BUY,  100, 10, 100));
    pushMarketUpdate(&f.incoming_market_updates,
                     mkMU(MarketUpdateType::ADD, Side::SELL, 102, 10, 101));
    f.te->drainOnceForTest();

    // Strategy: quote both sides.
    f.te->orderManager()->moveOrders(TickerId{0}, Price{99}, Price{103}, Qty{5});

    // Drain outgoing: should see two NEW requests.
    int new_count = 0, cancel_count = 0;
    while (const auto *r = f.outgoing_requests.getNextToRead()) {
        if (r->type_ == Exchange::ClientRequestType::NEW)    ++new_count;
        if (r->type_ == Exchange::ClientRequestType::CANCEL) ++cancel_count;
        f.outgoing_requests.updateReadIndex();
    }
    CHECK(new_count == 2);
    CHECK(cancel_count == 0);

    // Get back ACCEPTED for the bid; bid order goes LIVE.
    const auto bid_oid = f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_id_;
    pushResponse(&f.incoming_responses,
                 mkResp(ClientResponseType::ACCEPTED, bid_oid, Side::BUY, 99, 0, 5));
    f.te->drainOnceForTest();
    CHECK(f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_state_
          == OMOrderState::LIVE);

    // FILLED at 99 — position long 5; total volume 5; some unrealized PnL
    // depending on the current BBO mid (101.0).
    pushResponse(&f.incoming_responses,
                 mkResp(ClientResponseType::FILLED, bid_oid, Side::BUY, 99, 5, 0));
    f.te->drainOnceForTest();

    const auto *pi = f.te->positionKeeper()->getPositionInfo(TickerId{0});
    CHECK(pi->position_ == 5);
    CHECK(pi->volume_.value == 5);
    // Mid 101.0, VWAP 99.0 → unrealized = 2.0 * 5 = 10.0
    CHECK(pi->unreal_pnl_ == doctest::Approx(10.0));
    CHECK(f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_state_
          == OMOrderState::DEAD);
}

// ── Test 9: TradeEngine is the SINGLE dispatcher into OrderManager ─────────
//
// Regression test for the double-dispatch defect: handleResponse used to
// call order_manager_->onOrderUpdate() AND fire order_cb_, while the
// strategies' onOrderUpdate forwarded to OrderManager again — every response
// processed twice. Strategies must now be pure observers. The direct-call
// probes below fail if a strategy ever forwards to OrderManager again.
TEST_CASE("TradeEngine: responses reach OrderManager exactly once (MarketMaker)") {
    ProdFixture f;

    // Real strategy wired through the engine's callback API.
    Logger mm_logger{"test_te_mm_dispatch.log"};
    MarketMaker mm{&mm_logger, f.te.get(), f.te->featureEngine(),
                   f.te->orderManager(), f.cfg};

    // Re-wrap the order callback so invocations are counted while still
    // running exactly what the strategy runs for each response.
    int cb_count = 0;
    f.te->setOrderUpdateCallback([&](const MEClientResponse *r) {
        ++cb_count;
        mm.onOrderUpdate(r);
    });

    // Quote → bid goes PENDING_NEW.
    f.te->orderManager()->moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});
    const auto bid_oid =
        f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_id_;

    // ACCEPTED through the engine: exactly one callback, state LIVE.
    pushResponse(&f.incoming_responses,
                 mkResp(ClientResponseType::ACCEPTED, bid_oid, Side::BUY, 99, 0, 5));
    f.te->drainOnceForTest();
    CHECK(cb_count == 1);
    CHECK(f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_state_
          == OMOrderState::LIVE);

    // The double-dispatch probe: feed a CANCELED response to the STRATEGY
    // alone. A pure observer must not touch OrderManager state; under the
    // old forwarding behaviour this transitions LIVE → DEAD and fails here.
    const auto cancel_resp =
        mkResp(ClientResponseType::CANCELED, bid_oid, Side::BUY, 99, 0, 0);
    mm.onOrderUpdate(&cancel_resp);
    CHECK(f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_state_
          == OMOrderState::LIVE);

    // The same response through the engine DOES transition the state:
    // exactly one dispatch per response, owned by TradeEngine.
    pushResponse(&f.incoming_responses, cancel_resp);
    f.te->drainOnceForTest();
    CHECK(cb_count == 2);
    CHECK(f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_state_
          == OMOrderState::DEAD);
}

TEST_CASE("TradeEngine: responses reach OrderManager exactly once (LiquidityTaker)") {
    ProdFixture f;

    Logger lt_logger{"test_te_lt_dispatch.log"};
    LiquidityTaker lt{&lt_logger, f.te.get(), f.te->featureEngine(),
                      f.te->orderManager(), f.cfg};

    int cb_count = 0;
    f.te->setOrderUpdateCallback([&](const MEClientResponse *r) {
        ++cb_count;
        lt.onOrderUpdate(r);
    });

    f.te->orderManager()->moveOrders(TickerId{0}, Price{99}, Price{}, Qty{5});
    const auto bid_oid =
        f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_id_;

    pushResponse(&f.incoming_responses,
                 mkResp(ClientResponseType::ACCEPTED, bid_oid, Side::BUY, 99, 0, 5));
    f.te->drainOnceForTest();
    CHECK(cb_count == 1);
    CHECK(f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_state_
          == OMOrderState::LIVE);

    // Direct call on the strategy must leave OrderManager state untouched.
    const auto cancel_resp =
        mkResp(ClientResponseType::CANCELED, bid_oid, Side::BUY, 99, 0, 0);
    lt.onOrderUpdate(&cancel_resp);
    CHECK(f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_state_
          == OMOrderState::LIVE);

    pushResponse(&f.incoming_responses, cancel_resp);
    f.te->drainOnceForTest();
    CHECK(cb_count == 2);
    CHECK(f.te->orderManager()->getOrder(TickerId{0}, Side::BUY).order_state_
          == OMOrderState::DEAD);
}

// ── Test 10: malformed responses are dropped before touching state ─────────
TEST_CASE("TradeEngine: response with out-of-range ticker id is dropped") {
    ProdFixture f;

    int cb_count = 0;
    f.te->setOrderUpdateCallback([&](const MEClientResponse *) { ++cb_count; });

    // ticker == ME_MAX_TICKERS is one past the last valid book. Without the
    // bounds check this reaches PositionKeeper::addFill, whose .at() inside
    // a noexcept method terminates the process.
    pushResponse(&f.incoming_responses,
                 mkResp(ClientResponseType::FILLED, OrderId{1}, Side::BUY,
                        100, /*exec*/10, /*leaves*/0,
                        /*ticker*/ static_cast<uint32_t>(ME_MAX_TICKERS)));
    f.te->drainOnceForTest(); // must not crash

    CHECK(cb_count == 0); // dropped before any fan-out
    const auto *pi = f.te->positionKeeper()->getPositionInfo(TickerId{0});
    CHECK(pi->position_ == 0);
    CHECK(pi->volume_.value == 0);
}

// ── Test 11: default ctor preserves stub semantics for OrderManager tests ──
TEST_CASE("TradeEngine: default constructor records sent requests") {
    TradeEngine te;  // test-mode
    te.setClientIdForTest(ClientId{42});
    CHECK(te.clientId().value == 42);

    MEClientRequest req{};
    req.type_ = Exchange::ClientRequestType::NEW;
    req.client_id_ = ClientId{42};
    req.ticker_id_ = TickerId{0};
    req.order_id_  = OrderId{5};
    req.side_      = Side::BUY;
    req.price_     = Price{100};
    req.qty_       = Qty{1};
    te.sendClientRequest(&req);

    const auto &sent = te.getSentRequests();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].order_id_.value == 5);
}
