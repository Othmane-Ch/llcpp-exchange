// order_gateway_test.cpp -- unit tests for Trading::OrderGateway.
//
// We exercise only the receive path via onBytesForTest(), which injects a
// synthesised byte stream as if it had arrived on the TCP socket. The send
// path requires a running peer and the background thread, which we do not
// start here.
//
// The gateway ctor calls tcp_socket_.connect() with an ASSERT, so we spin
// up a dummy TCPServer on localhost to satisfy construction. After that
// the socket is never used in these tests (run() is never invoked).

#include "doctest/doctest.h"

#include <cstring>
#include <memory>

#include "order_gateway.h"
#include "client_request.h"
#include "client_response.h"
#include "tcp_server.h"
#include "logging.h"

using namespace Trading;
using namespace Common;
using Exchange::MEClientRequest;
using Exchange::MEClientResponse;
using Exchange::OMClientResponse;
using Exchange::ClientResponseType;
using Exchange::ClientRequestLFQueue;
using Exchange::ClientResponseLFQueue;

// Port used for the fixture's dummy listener. Chosen to avoid collisions
// with the exchange's default (12345) and with MD ports (20000/20001).
static constexpr int kTestPort = 33456;

// ── Helper: build a fully-populated OMClientResponse on the stack ───────────
static auto mkResp(size_t seq, uint32_t client_id,
                   ClientResponseType type = ClientResponseType::ACCEPTED,
                   uint64_t client_oid     = 1,
                   uint64_t market_oid     = 1,
                   uint64_t price          = 100,
                   uint32_t exec_qty       = 0,
                   uint32_t leaves_qty     = 10) -> OMClientResponse {
    OMClientResponse m{};
    m.seq_num_ = seq;
    m.me_client_response_.type_             = type;
    m.me_client_response_.client_id_        = ClientId{client_id};
    m.me_client_response_.ticker_id_        = TickerId{0};
    m.me_client_response_.client_order_id_  = OrderId{client_oid};
    m.me_client_response_.market_order_id_  = OrderId{market_oid};
    m.me_client_response_.side_             = Side::BUY;
    m.me_client_response_.price_            = Price{price};
    m.me_client_response_.exec_qty_         = Qty{exec_qty};
    m.me_client_response_.leaves_qty_       = Qty{leaves_qty};
    return m;
}

// ── Fixture: queues + dummy listener + gateway ──────────────────────────────
namespace { struct Fixture {
    // The gateway writes decoded responses into incoming_responses_; a caller
    // would normally push outgoing requests via outgoing_requests_.
    ClientRequestLFQueue  outgoing_requests{ME_MAX_CLIENT_UPDATES};
    ClientResponseLFQueue incoming_responses{ME_MAX_CLIENT_UPDATES};

    // Dummy listener so OrderGateway's connect() ASSERT passes.
    Logger    server_logger{"test_trading_gw_server.log"};
    TCPServer dummy_listener{server_logger};

    static inline const ClientId kClientId = ClientId{7};

    std::unique_ptr<OrderGateway> gw;

    Fixture() {
        dummy_listener.listen("lo", kTestPort);
        // Construct gateway; this will connect to the dummy listener.
        // run() is never started -- the socket is idle for the whole test.
        gw.reset(new OrderGateway(
            kClientId,
            &outgoing_requests,
            &incoming_responses,
            "127.0.0.1", "lo", kTestPort,
            "test_trading_order_gateway.log"));
    }

    auto drainResponses() -> std::vector<MEClientResponse> {
        std::vector<MEClientResponse> v;
        while (const auto *r = incoming_responses.getNextToRead()) {
            v.push_back(*r);
            incoming_responses.updateReadIndex();
        }
        return v;
    }

    // Serialise a sequence of OMClientResponse values into a contiguous byte
    // buffer, exactly as they would appear on the wire.
    static auto serialise(const std::vector<OMClientResponse> &msgs)
        -> std::vector<char> {
        std::vector<char> buf(msgs.size() * sizeof(OMClientResponse));
        for (size_t i = 0; i < msgs.size(); ++i) {
            std::memcpy(buf.data() + i * sizeof(OMClientResponse),
                        &msgs[i], sizeof(OMClientResponse));
        }
        return buf;
    }
}; } // anonymous namespace

// ── Test 1: a correctly addressed, in-order response is published ──────────
TEST_CASE("OrderGateway: in-order response for our client is published") {
    Fixture f;

    const auto msg = mkResp(/*seq*/1, /*client*/7);
    const auto bytes = Fixture::serialise({msg});

    f.gw->onBytesForTest(bytes.data(), bytes.size());

    const auto out = f.drainResponses();
    REQUIRE(out.size() == 1);
    CHECK(out[0].type_ == ClientResponseType::ACCEPTED);
    CHECK(out[0].client_id_.value == 7);
    CHECK(out[0].leaves_qty_.value == 10);
    CHECK(f.gw->nextExpectedIncomingSeqNum() == 2);
}

// ── Test 2: response addressed to a different client is dropped ─────────────
TEST_CASE("OrderGateway: response for wrong client_id is dropped") {
    Fixture f;

    // kClientId is 7; send a response addressed to client 99.
    const auto msg = mkResp(/*seq*/1, /*client*/99);
    const auto bytes = Fixture::serialise({msg});

    f.gw->onBytesForTest(bytes.data(), bytes.size());

    CHECK(f.drainResponses().empty());
    // Sequence should NOT advance for a foreign-client message.
    CHECK(f.gw->nextExpectedIncomingSeqNum() == 1);
}

// ── Test 3: out-of-order seq_num is dropped ────────────────────────────────
TEST_CASE("OrderGateway: out-of-order seq_num is dropped") {
    Fixture f;

    // Expected seq is 1; skip ahead to 2.
    const auto msg = mkResp(/*seq*/2, /*client*/7);
    const auto bytes = Fixture::serialise({msg});

    f.gw->onBytesForTest(bytes.data(), bytes.size());

    CHECK(f.drainResponses().empty());
    // Still expecting 1 -- no advance because the gap was detected.
    CHECK(f.gw->nextExpectedIncomingSeqNum() == 1);
}

// ── Test 4: multiple contiguous responses are parsed in order ───────────────
TEST_CASE("OrderGateway: multiple contiguous responses drain in order") {
    Fixture f;

    const auto m1 = mkResp(/*seq*/1, /*client*/7, ClientResponseType::ACCEPTED,
                           /*client_oid*/1, /*market_oid*/1, 100, 0, 10);
    const auto m2 = mkResp(/*seq*/2, /*client*/7, ClientResponseType::FILLED,
                           /*client_oid*/1, /*market_oid*/1, 100, 5, 5);
    const auto m3 = mkResp(/*seq*/3, /*client*/7, ClientResponseType::CANCELED,
                           /*client_oid*/1, /*market_oid*/1, 100, 5, 0);
    const auto bytes = Fixture::serialise({m1, m2, m3});

    f.gw->onBytesForTest(bytes.data(), bytes.size());

    const auto out = f.drainResponses();
    REQUIRE(out.size() == 3);
    CHECK(out[0].type_ == ClientResponseType::ACCEPTED);
    CHECK(out[1].type_ == ClientResponseType::FILLED);
    CHECK(out[1].exec_qty_.value == 5);
    CHECK(out[2].type_ == ClientResponseType::CANCELED);
    CHECK(f.gw->nextExpectedIncomingSeqNum() == 4);
}

// ── Test 5: duplicate seq_num after a good one is dropped ───────────────────
TEST_CASE("OrderGateway: duplicate seq_num after delivery is dropped") {
    Fixture f;

    const auto m1 = mkResp(/*seq*/1, /*client*/7);
    const auto m1_dup = mkResp(/*seq*/1, /*client*/7);
    const auto bytes = Fixture::serialise({m1, m1_dup});

    f.gw->onBytesForTest(bytes.data(), bytes.size());

    const auto out = f.drainResponses();
    REQUIRE(out.size() == 1);
    CHECK(f.gw->nextExpectedIncomingSeqNum() == 2);
}

// ── Test 6: initial outgoing seq num is 1 (observable invariant) ────────────
TEST_CASE("OrderGateway: outgoing seq num initialised to 1") {
    Fixture f;
    CHECK(f.gw->nextOutgoingSeqNum() == 1);
}
