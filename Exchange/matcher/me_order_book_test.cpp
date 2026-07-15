// me_order_book_test.cpp -- unit tests for MEOrderBook
// Uses doctest (single-header). Outputs captured via real LFQueues.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <memory>
#include <vector>

#include "matcher/matching_engine.h"
#include "matcher/me_order_book.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "market_data/market_update.h"
#include "logging.h"

using namespace Exchange;
using namespace Common;

// -- Queue drain helpers ------------------------------------------------------

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

// -- Test fixture -------------------------------------------------------------
//
// MEOrderBook contains a ~2 GB array (cid_oid_to_order_) and must always be
// heap-allocated. We create real LFQueues and a real MatchingEngine (not
// started -- no thread). MEOrderBook calls matching_engine_->sendXxx which
// writes into the queues; we drain them to verify call counts and values.

struct Fixture {
    ClientRequestLFQueue           req_q{ME_MAX_CLIENT_UPDATES};
    ClientResponseLFQueue          resp_q{ME_MAX_CLIENT_UPDATES};
    MEMarketUpdateLFQueue          upd_q{ME_MAX_MARKET_UPDATES};
    Logger                         logger{"test_me_order_book.log"};
    MatchingEngine                 engine{&req_q, &resp_q, &upd_q};
    // Heap-allocated: MEOrderBook embeds a ~2 GB flat array.
    std::unique_ptr<MEOrderBook>   book_owner{
        new MEOrderBook{TickerId{0}, &logger, &engine}};
    MEOrderBook                   &book{*book_owner};

    // Convenience wrappers so tests read cleanly.
    auto add(uint32_t cid, uint64_t oid, Side side, uint64_t price, uint32_t qty) {
        book.add(ClientId{cid}, OrderId{oid}, TickerId{0},
                 side, Price{price}, Qty{qty});
    }
    auto cancel(uint32_t cid, uint64_t oid) {
        book.cancel(ClientId{cid}, OrderId{oid}, TickerId{0});
    }
    auto responses() { return drainResponses(resp_q); }
    auto updates()   { return drainUpdates(upd_q); }
    auto level(uint64_t price) { return book.getOrdersAtPrice(Price{price}); }
};

// -- Test 1: Passive order rests in book --------------------------------------

TEST_CASE("Passive BUY rests in book when no matching asks exist") {
    Fixture f;
    f.add(1, 1, Side::BUY, 100, 10);

    const auto resp = f.responses();
    const auto upds = f.updates();

    REQUIRE(resp.size() == 1);
    CHECK(resp[0].type_ == ClientResponseType::ACCEPTED);
    CHECK(resp[0].exec_qty_.value == 0);
    CHECK(resp[0].leaves_qty_.value == 10);

    REQUIRE(upds.size() == 1);
    CHECK(upds[0].type_ == MarketUpdateType::ADD);
    CHECK(upds[0].qty_.value == 10);

    CHECK(f.level(100) != nullptr);
}

// -- Test 2: Full match -- aggressive fully consumed --------------------------

TEST_CASE("Full match consumes aggressive BUY and removes price level") {
    Fixture f;

    // Seed: SELL 20@100
    f.add(1, 1, Side::SELL, 100, 20);
    f.responses(); // discard seed ACCEPTED
    f.updates();   // discard seed ADD

    // Aggressive BUY 20@100
    f.add(2, 2, Side::BUY, 100, 20);

    const auto resp = f.responses();
    const auto upds = f.updates();

    // 1x ACCEPTED (aggressive), 2x FILLED
    REQUIRE(resp.size() == 3);
    CHECK(resp[0].type_ == ClientResponseType::ACCEPTED);
    CHECK(resp[1].type_ == ClientResponseType::FILLED);
    CHECK(resp[2].type_ == ClientResponseType::FILLED);

    // Verify fill quantities: both sides filled 20.
    const auto &agg_fill = (resp[1].client_id_.value == 2) ? resp[1] : resp[2];
    const auto &pas_fill = (resp[1].client_id_.value == 1) ? resp[1] : resp[2];
    CHECK(agg_fill.exec_qty_.value == 20);
    CHECK(agg_fill.leaves_qty_.value == 0);
    CHECK(pas_fill.exec_qty_.value == 20);
    CHECK(pas_fill.leaves_qty_.value == 0);

    // 1x TRADE + 1x CANCEL
    REQUIRE(upds.size() == 2);
    CHECK(upds[0].type_ == MarketUpdateType::TRADE);
    CHECK(upds[1].type_ == MarketUpdateType::CANCEL);

    // Level must be gone -- fully consumed.
    CHECK(f.level(100) == nullptr);

    // No ADD update -- BUY was fully matched, leaves_qty == 0.
    for (const auto &u : upds) {
        CHECK(u.type_ != MarketUpdateType::ADD);
    }
}

// -- Test 3: Partial match -- passive partially filled ------------------------

TEST_CASE("Partial match: passive SELL 30 filled by BUY 10, level remains") {
    Fixture f;

    f.add(1, 1, Side::SELL, 100, 30);
    f.responses();
    f.updates();

    f.add(2, 2, Side::BUY, 100, 10);

    const auto resp = f.responses();
    const auto upds = f.updates();

    // 1x ACCEPTED + 2x FILLED
    REQUIRE(resp.size() == 3);
    CHECK(resp[0].type_ == ClientResponseType::ACCEPTED);

    int filled_count = 0;
    for (const auto &r : resp)
        if (r.type_ == ClientResponseType::FILLED) ++filled_count;
    CHECK(filled_count == 2);

    // All fills are for qty=10
    for (const auto &r : resp) {
        if (r.type_ == ClientResponseType::FILLED)
            CHECK(r.exec_qty_.value == 10);
    }

    // 1x TRADE + 1x MODIFY (not CANCEL)
    REQUIRE(upds.size() == 2);
    CHECK(upds[0].type_ == MarketUpdateType::TRADE);
    CHECK(upds[1].type_ == MarketUpdateType::MODIFY);
    CHECK(upds[1].qty_.value == 20); // 30 - 10

    // Level stays -- 20 qty remain.
    CHECK(f.level(100) != nullptr);
}

// -- Test 4: FIFO priority within a price level -------------------------------

TEST_CASE("FIFO priority: first-in order matched first") {
    Fixture f;

    // Order A: SELL 5@100 -- arrives first, gets priority 1.
    f.add(1, 1, Side::SELL, 100, 5);
    const auto upds_a = f.updates();
    f.responses();
    REQUIRE(upds_a.size() == 1);
    const auto moid_a = upds_a[0].order_id_.value; // ADD carries market order id
    const auto prio_a = upds_a[0].priority_.value;

    // Order B: SELL 5@100 -- arrives second, gets priority 2.
    f.add(1, 2, Side::SELL, 100, 5);
    const auto upds_b = f.updates();
    f.responses();
    REQUIRE(upds_b.size() == 1);
    const auto prio_b = upds_b[0].priority_.value;

    // A must have strictly lower priority value (FIFO = lower number = earlier).
    CHECK(prio_a < prio_b);

    // BUY 5@100 -- matches order A (head of FIFO queue).
    f.add(2, 3, Side::BUY, 100, 5);
    const auto resp = f.responses();
    const auto upds = f.updates();

    // Passive FILLED should reference order A's market order id.
    bool saw_a_filled = false;
    for (const auto &r : resp) {
        if (r.type_ == ClientResponseType::FILLED &&
            r.market_order_id_.value == moid_a) {
            saw_a_filled = true;
        }
    }
    CHECK(saw_a_filled);

    // Order B remains in the book.
    CHECK(f.level(100) != nullptr);
    CHECK(f.level(100)->first_me_order_ != nullptr);
    // The remaining order's qty should be 5 (B is untouched).
    CHECK(f.level(100)->first_me_order_->qty_.value == 5);
}

// -- Test 5: Cancel valid order -----------------------------------------------

TEST_CASE("Cancel valid order removes it and sends CANCELED + CANCEL update") {
    Fixture f;

    f.add(1, 1, Side::BUY, 100, 10);
    f.responses();
    f.updates();

    f.cancel(1, 1);

    const auto resp = f.responses();
    const auto upds = f.updates();

    REQUIRE(resp.size() == 1);
    CHECK(resp[0].type_ == ClientResponseType::CANCELED);
    CHECK(resp[0].client_id_.value == 1);

    REQUIRE(upds.size() == 1);
    CHECK(upds[0].type_ == MarketUpdateType::CANCEL);
    CHECK(upds[0].qty_.value == 10);

    // Level must be removed.
    CHECK(f.level(100) == nullptr);
}

// -- Test 6: Cancel non-existent order ----------------------------------------

TEST_CASE("Cancel non-existent order sends CANCEL_REJECTED, no market update") {
    Fixture f;

    f.cancel(5, 9999);

    const auto resp = f.responses();
    const auto upds = f.updates();

    REQUIRE(resp.size() == 1);
    CHECK(resp[0].type_ == ClientResponseType::CANCEL_REJECTED);

    CHECK(upds.empty());
}

// -- Test 7: price-collision rejection -----------------------------------------
//
// priceToIndex is price % ME_MAX_PRICE_LEVELS, so two live prices exactly
// ME_MAX_PRICE_LEVELS apart alias to one direct-map slot. The book must
// reject the aliasing order instead of appending it to the wrong level.

TEST_CASE("Order at a price aliasing a live level is rejected; level untouched") {
    Fixture f;
    constexpr uint64_t p         = 100;
    constexpr uint64_t colliding = p + ME_MAX_PRICE_LEVELS;

    // Rest an order at p.
    f.add(1, 1, Side::BUY, p, 10);
    f.responses(); f.updates(); // discard seed ACCEPTED + ADD

    // Same-slot different-price order must be rejected outright.
    f.add(2, 2, Side::BUY, colliding, 5);
    const auto resp = f.responses();
    const auto upds = f.updates();
    REQUIRE(resp.size() == 1);
    CHECK(resp[0].type_ == ClientResponseType::INVALID);
    CHECK(resp[0].client_id_.value == 2);
    CHECK(upds.empty()); // nothing reached the book or the market data feed

    // The level at p is untouched by the rejection...
    REQUIRE(f.level(p) != nullptr);
    CHECK(f.level(p)->price_.value == p);
    CHECK(f.level(p)->first_me_order_->qty_.value == 10);

    // ...and cancelling the resting order still works.
    f.cancel(1, 1);
    const auto cancel_resp = f.responses();
    REQUIRE(cancel_resp.size() == 1);
    CHECK(cancel_resp[0].type_ == ClientResponseType::CANCELED);
    CHECK(f.level(p) == nullptr);
}

TEST_CASE("Colliding price is accepted again once the aliased slot is freed") {
    Fixture f;
    constexpr uint64_t p         = 100;
    constexpr uint64_t colliding = p + ME_MAX_PRICE_LEVELS;

    // Occupy the slot, then free it (cancel empties the level).
    f.add(1, 1, Side::BUY, p, 10);
    f.cancel(1, 1);
    f.responses(); f.updates();

    // The previously-colliding price now maps to an empty slot — accepted.
    f.add(2, 2, Side::BUY, colliding, 5);
    const auto resp = f.responses();
    REQUIRE(resp.size() == 1);
    CHECK(resp[0].type_ == ClientResponseType::ACCEPTED);
    REQUIRE(f.level(colliding) != nullptr);
    CHECK(f.level(colliding)->price_.value == colliding);
    CHECK(f.level(colliding)->first_me_order_->qty_.value == 5);
}
