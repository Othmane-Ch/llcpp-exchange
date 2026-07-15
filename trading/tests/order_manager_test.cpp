// order_manager_test.cpp -- unit tests for Trading::OrderManager.
//
// Drives the per-side state machine and verifies what was sent to the
// (stubbed) TradeEngine. Inspects the captured request vector and the
// observable OMOrder state transitions.

#include "doctest/doctest.h"

#include "order_manager.h"
#include "risk_manager.h"
#include "trade_engine.h"
#include "position_keeper.h"
#include "om_order.h"
#include "client_request.h"
#include "client_response.h"
#include "logging.h"

using namespace Trading;
using namespace Common;
using Exchange::MEClientRequest;
using Exchange::MEClientResponse;
using Exchange::ClientRequestType;
using Exchange::ClientResponseType;

// ── Helper: synthesise an MEClientResponse for a given (oid, type, ...) ─────
static auto resp(ClientResponseType type, OrderId oid, Side side,
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

namespace { struct Fixture {
    Logger logger{"test_order_manager.log"};
    PositionKeeper keeper{&logger};
    TradeEngineCfgHashMap cfg{};
    TradeEngine te;

    Fixture() {
        // Permissive risk config for ticker 0 so most quoting is ALLOWED.
        cfg[0].risk_cfg_ = RiskCfg{Qty{50}, Qty{100}, -1000.0};
        te.setClientIdForTest(ClientId{1});
    }
}; } // anonymous namespace

// ── Test 1: moveOrders on flat state sends one NEW per side ────────────────
TEST_CASE("OrderManager: initial moveOrders sends NEW on both sides") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    OrderManager om{&f.logger, &f.te, rm};

    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});

    const auto &sent = f.te.getSentRequests();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].type_ == ClientRequestType::NEW);
    CHECK(sent[0].side_ == Side::BUY);
    CHECK(sent[0].price_.value == 99);
    CHECK(sent[0].qty_.value == 5);
    CHECK(sent[1].type_ == ClientRequestType::NEW);
    CHECK(sent[1].side_ == Side::SELL);
    CHECK(sent[1].price_.value == 101);

    const auto &bid = om.getOrder(TickerId{0}, Side::BUY);
    const auto &ask = om.getOrder(TickerId{0}, Side::SELL);
    CHECK(bid.order_state_ == OMOrderState::PENDING_NEW);
    CHECK(ask.order_state_ == OMOrderState::PENDING_NEW);
    CHECK(om.nextOrderId() == OrderId{3});  // started at 1, two sends
}

// ── Test 2: ACCEPTED transitions PENDING_NEW → LIVE ────────────────────────
TEST_CASE("OrderManager: ACCEPTED transitions PENDING_NEW to LIVE") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    OrderManager om{&f.logger, &f.te, rm};

    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});
    const auto bid_oid = om.getOrder(TickerId{0}, Side::BUY).order_id_;

    auto r = resp(ClientResponseType::ACCEPTED, bid_oid, Side::BUY, 99, 0, 5);
    om.onOrderUpdate(&r);

    CHECK(om.getOrder(TickerId{0}, Side::BUY).order_state_ == OMOrderState::LIVE);
}

// ── Test 3: re-issuing same params on LIVE order is a no-op ─────────────────
TEST_CASE("OrderManager: moveOrders with identical params on LIVE is a no-op") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    OrderManager om{&f.logger, &f.te, rm};

    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});
    auto r1 = resp(ClientResponseType::ACCEPTED, om.getOrder(TickerId{0}, Side::BUY).order_id_,
                   Side::BUY, 99, 0, 5);
    om.onOrderUpdate(&r1);
    auto r2 = resp(ClientResponseType::ACCEPTED, om.getOrder(TickerId{0}, Side::SELL).order_id_,
                   Side::SELL, 101, 0, 5);
    om.onOrderUpdate(&r2);

    const auto sent_before = f.te.getSentRequests().size();
    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});
    CHECK(f.te.getSentRequests().size() == sent_before);
    CHECK(om.getOrder(TickerId{0}, Side::BUY).order_state_  == OMOrderState::LIVE);
    CHECK(om.getOrder(TickerId{0}, Side::SELL).order_state_ == OMOrderState::LIVE);
}

// ── Test 4: changing price on LIVE order sends CANCEL ───────────────────────
TEST_CASE("OrderManager: price change on LIVE order sends CANCEL") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    OrderManager om{&f.logger, &f.te, rm};

    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});
    auto r = resp(ClientResponseType::ACCEPTED, om.getOrder(TickerId{0}, Side::BUY).order_id_,
                  Side::BUY, 99, 0, 5);
    om.onOrderUpdate(&r);

    om.moveOrders(TickerId{0}, Price{98}, Price{101}, Qty{5});  // bid moved 99→98

    const auto &sent = f.te.getSentRequests();
    REQUIRE(!sent.empty());
    const auto &last = sent.back();
    CHECK(last.type_ == ClientRequestType::CANCEL);
    CHECK(last.side_ == Side::BUY);
    CHECK(om.getOrder(TickerId{0}, Side::BUY).order_state_ == OMOrderState::PENDING_CANCEL);
}

// ── Test 5: CANCELED → DEAD; subsequent moveOrders re-quotes ───────────────
TEST_CASE("OrderManager: after CANCELED, re-quote sends a fresh NEW") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    OrderManager om{&f.logger, &f.te, rm};

    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});
    auto a = resp(ClientResponseType::ACCEPTED, om.getOrder(TickerId{0}, Side::BUY).order_id_,
                  Side::BUY, 99, 0, 5);
    om.onOrderUpdate(&a);

    om.moveOrders(TickerId{0}, Price{98}, Price{101}, Qty{5});  // triggers CANCEL
    auto c = resp(ClientResponseType::CANCELED, om.getOrder(TickerId{0}, Side::BUY).order_id_,
                  Side::BUY, 99, 0, 0);
    om.onOrderUpdate(&c);
    CHECK(om.getOrder(TickerId{0}, Side::BUY).order_state_ == OMOrderState::DEAD);

    const auto sent_before = f.te.getSentRequests().size();
    om.moveOrders(TickerId{0}, Price{98}, Price{101}, Qty{5});  // re-quote bid

    const auto &sent = f.te.getSentRequests();
    CHECK(sent.size() == sent_before + 1);
    CHECK(sent.back().type_ == ClientRequestType::NEW);
    CHECK(sent.back().price_.value == 98);
    CHECK(om.getOrder(TickerId{0}, Side::BUY).order_state_ == OMOrderState::PENDING_NEW);
}

// ── Test 6: partial FILL updates leaves_qty, stays LIVE ─────────────────────
TEST_CASE("OrderManager: partial FILLED updates qty, order stays LIVE") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    OrderManager om{&f.logger, &f.te, rm};

    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{10});
    auto a = resp(ClientResponseType::ACCEPTED, om.getOrder(TickerId{0}, Side::BUY).order_id_,
                  Side::BUY, 99, 0, 10);
    om.onOrderUpdate(&a);

    auto fill = resp(ClientResponseType::FILLED, om.getOrder(TickerId{0}, Side::BUY).order_id_,
                     Side::BUY, 99, /*exec*/3, /*leaves*/7);
    om.onOrderUpdate(&fill);

    const auto &bid = om.getOrder(TickerId{0}, Side::BUY);
    CHECK(bid.qty_.value == 7);
    CHECK(bid.order_state_ == OMOrderState::LIVE);
}

// ── Test 7: full FILL transitions LIVE → DEAD ───────────────────────────────
TEST_CASE("OrderManager: FILLED with leaves_qty=0 transitions to DEAD") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    OrderManager om{&f.logger, &f.te, rm};

    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});
    auto a = resp(ClientResponseType::ACCEPTED, om.getOrder(TickerId{0}, Side::BUY).order_id_,
                  Side::BUY, 99, 0, 5);
    om.onOrderUpdate(&a);
    auto fill = resp(ClientResponseType::FILLED, om.getOrder(TickerId{0}, Side::BUY).order_id_,
                     Side::BUY, 99, 5, 0);
    om.onOrderUpdate(&fill);

    CHECK(om.getOrder(TickerId{0}, Side::BUY).order_state_ == OMOrderState::DEAD);
    CHECK(om.getOrder(TickerId{0}, Side::BUY).qty_.value == 0);
}

// ── Test 8: risk REJECTED (oversize qty) blocks NEW ─────────────────────────
TEST_CASE("OrderManager: risk-rejected order does not produce a NEW request") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    OrderManager om{&f.logger, &f.te, rm};

    // max_order_size_ = 50; ask Qty{200} should be rejected.
    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{200});

    CHECK(f.te.getSentRequests().empty());
    CHECK(om.getOrder(TickerId{0}, Side::BUY).order_state_  == OMOrderState::INVALID);
    CHECK(om.getOrder(TickerId{0}, Side::SELL).order_state_ == OMOrderState::INVALID);
}

// ── Test 9: PENDING_NEW state ignores subsequent moveOrders ────────────────
TEST_CASE("OrderManager: moveOrders during PENDING_NEW does not send another NEW") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    OrderManager om{&f.logger, &f.te, rm};

    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});
    const auto sent_before = f.te.getSentRequests().size();
    // No ACCEPTED received; both sides are still PENDING_NEW.
    om.moveOrders(TickerId{0}, Price{99}, Price{101}, Qty{5});
    CHECK(f.te.getSentRequests().size() == sent_before);
}

// ── Test 10: invalid bid price (Price{}) leaves that side untouched on flat ─
TEST_CASE("OrderManager: invalid bid price on flat state issues no order") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    OrderManager om{&f.logger, &f.te, rm};

    // Pass Price{} for the bid side → should not produce a NEW for BUY.
    om.moveOrders(TickerId{0}, Price{}, Price{101}, Qty{5});

    const auto &sent = f.te.getSentRequests();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].side_ == Side::SELL);
    CHECK(om.getOrder(TickerId{0}, Side::BUY).order_state_  == OMOrderState::INVALID);
    CHECK(om.getOrder(TickerId{0}, Side::SELL).order_state_ == OMOrderState::PENDING_NEW);
}
