// exchange_integration_test.cpp — smoke tests verifying the end-to-end path:
// matching engine processes orders → produces responses + market updates.
//
// These tests use the same pattern as me_order_book_test.cpp: create a
// MatchingEngine without starting its thread, then submit requests via
// the LFQueue and let the engine process them synchronously via start/stop.
//
// This is the doctest main entry point for the communication_layer_tests
// executable. All other test files in this directory are headerless TUs.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <memory>
#include <vector>

#include "matcher/matching_engine.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "order_server/order_server.h"
#include "market_data/market_update.h"
#include "logging.h"

using namespace Exchange;
using namespace Common;

static auto drainResponses(ClientResponseLFQueue &q) -> std::vector<MEClientResponse> {
    std::vector<MEClientResponse> v;
    while (const auto *r = q.getNextToRead()) {
        v.push_back(*r);
        q.updateReadIndex();
    }
    return v;
}

static auto drainUpdates(MEMarketUpdateLFQueue &q) -> std::vector<MEMarketUpdate> {
    std::vector<MEMarketUpdate> v;
    while (const auto *u = q.getNextToRead()) {
        v.push_back(*u);
        q.updateReadIndex();
    }
    return v;
}

/// Fixture that mirrors me_order_book_test.cpp's approach:
/// real queues + real MatchingEngine (no thread started), submit via queue.
struct IntegrationFixture {
    ClientRequestLFQueue  req_q{ME_MAX_CLIENT_UPDATES};
    ClientResponseLFQueue resp_q{ME_MAX_CLIENT_UPDATES};
    MEMarketUpdateLFQueue upd_q{ME_MAX_MARKET_UPDATES};
    Logger                logger{"test_integration.log"};
    MatchingEngine        engine{&req_q, &resp_q, &upd_q, "test_integration_engine.log"};

    // Heap-allocated order book (same as me_order_book_test).
    std::unique_ptr<MEOrderBook> book_owner{
        new MEOrderBook{TickerId{0}, &logger, &engine}};
    MEOrderBook &book{*book_owner};

    auto add(uint32_t cid, uint64_t oid, Side side, uint64_t price, uint32_t qty) {
        book.add(ClientId{cid}, OrderId{oid}, TickerId{0}, side, Price{price}, Qty{qty});
    }
    auto responses() { return drainResponses(resp_q); }
    auto updates()   { return drainUpdates(upd_q); }
};

TEST_CASE("Integration: passive order produces ACCEPTED + ADD through engine queues") {
    IntegrationFixture f;
    f.add(1, 1, Side::BUY, 100, 50);

    auto resp = f.responses();
    auto upds = f.updates();

    REQUIRE(resp.size() == 1);
    CHECK(resp[0].type_ == ClientResponseType::ACCEPTED);
    CHECK(resp[0].client_id_.value == 1);
    CHECK(resp[0].leaves_qty_.value == 50);

    REQUIRE(upds.size() == 1);
    CHECK(upds[0].type_ == MarketUpdateType::ADD);
    CHECK(upds[0].qty_.value == 50);
    CHECK(upds[0].price_.value == 100);
}

TEST_CASE("Integration: full match produces TRADE + CANCEL through engine queues") {
    IntegrationFixture f;

    // Passive SELL 20@100
    f.add(1, 1, Side::SELL, 100, 20);
    f.responses(); // discard
    f.updates();

    // Aggressive BUY 20@100
    f.add(2, 2, Side::BUY, 100, 20);

    auto resp = f.responses();
    auto upds = f.updates();

    // 1x ACCEPTED + 2x FILLED
    REQUIRE(resp.size() == 3);
    CHECK(resp[0].type_ == ClientResponseType::ACCEPTED);
    CHECK(resp[1].type_ == ClientResponseType::FILLED);
    CHECK(resp[2].type_ == ClientResponseType::FILLED);

    // TRADE + CANCEL (passive fully consumed)
    REQUIRE(upds.size() == 2);
    CHECK(upds[0].type_ == MarketUpdateType::TRADE);
    CHECK(upds[1].type_ == MarketUpdateType::CANCEL);
}

TEST_CASE("Integration: partial match produces TRADE + MODIFY") {
    IntegrationFixture f;

    // Passive SELL 30@100
    f.add(1, 1, Side::SELL, 100, 30);
    f.responses();
    f.updates();

    // Aggressive BUY 10@100 — partial fill
    f.add(2, 2, Side::BUY, 100, 10);

    auto upds = f.updates();

    REQUIRE(upds.size() == 2);
    CHECK(upds[0].type_ == MarketUpdateType::TRADE);
    CHECK(upds[0].qty_.value == 10);
    CHECK(upds[1].type_ == MarketUpdateType::MODIFY);
    CHECK(upds[1].qty_.value == 20); // 30 - 10
}

TEST_CASE("Integration: producer-first shutdown keeps client_requests single-producer") {
    // Regression test for the exchange teardown order. OrderServer is the
    // sole producer of client_requests; MatchingEngine::stop() writes the
    // SHUTDOWN pill into that same SPSC queue from the caller's thread, so
    // the OrderServer thread MUST be quiesced before the engine is deleted
    // (exchange_main used to delete the engine first — two producers racing
    // on one SPSC queue). The TSAN build of this suite is the real oracle
    // for the race; this test pins the lifecycle and the pill hand-off.
    auto req_q  = std::make_unique<ClientRequestLFQueue>(ME_MAX_CLIENT_UPDATES);
    auto resp_q = std::make_unique<ClientResponseLFQueue>(ME_MAX_CLIENT_UPDATES);
    auto upd_q  = std::make_unique<MEMarketUpdateLFQueue>(ME_MAX_MARKET_UPDATES);

    auto *engine = new MatchingEngine(req_q.get(), resp_q.get(), upd_q.get(),
                                      "test_shutdown_engine.log");
    auto *server = new OrderServer(req_q.get(), resp_q.get(), "lo", 12470,
                                   "test_shutdown_order_server.log");

    engine->start();
    server->start();
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(100ms); // both busy-spin threads up

    // Producer-first teardown — mirrors exchange_main.
    delete server; // stops + drains the sole client_requests producer
    delete engine; // now safe: the pill is written with no competing producer

    // The engine thread exits by CONSUMING the pill (run() breaks on
    // SHUTDOWN), so after both dtors the queue must be fully drained and
    // coherent.
    CHECK(req_q->getNextToRead() == nullptr);
}
