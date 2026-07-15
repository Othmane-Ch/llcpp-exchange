// market_order_book_test.cpp -- unit tests for Trading::MarketOrderBook.
// Client-side reconstruction fed synthetic MEMarketUpdate inputs.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <memory>

#include "market_order_book.h"
#include "market_update.h"
#include "logging.h"

using namespace Trading;
using namespace Common;
using Exchange::MEMarketUpdate;
using Exchange::MarketUpdateType;

// ── Helper: synthesise an incremental update for the client book ─────────────
static auto mk(MarketUpdateType t, Side side, uint64_t price, uint32_t qty,
               uint64_t oid, uint64_t prio = 1) -> MEMarketUpdate {
    MEMarketUpdate u{};
    u.type_      = t;
    u.ticker_id_ = TickerId{0};
    u.side_      = side;
    u.price_     = Price{price};
    u.qty_       = Qty{qty};
    u.order_id_  = OrderId{oid};
    u.priority_  = Priority{prio};
    return u;
}

namespace { struct Fixture {
    Logger logger{"test_trading_market_order_book.log"};
    // Heap-allocated: MarketOrderBook embeds a ~8MB oid_to_order_ array.
    std::unique_ptr<MarketOrderBook> book{
        new MarketOrderBook(TickerId{0}, &logger)};
}; } // anonymous namespace

// ── Test 1: ADD populates BBO ───────────────────────────────────────────────
TEST_CASE("Trading: ADD bid establishes best bid, BBO reflects it") {
    Fixture f;
    const auto u = mk(MarketUpdateType::ADD, Side::BUY, 100, 10, 1);
    f.book->onMarketUpdate(&u);

    const auto &bbo = f.book->getBBO();
    CHECK(bbo.bid_price_.value == 100);
    CHECK(bbo.bid_qty_.value   == 10);
    CHECK(bbo.ask_price_.isValid() == false);
    CHECK(f.book->getOrder(OrderId{1}) != nullptr);
}

// ── Test 2: two ADDs at same price aggregate bid qty ────────────────────────
TEST_CASE("Trading: two orders at same price aggregate into BBO") {
    Fixture f;
    const auto u1 = mk(MarketUpdateType::ADD, Side::BUY, 100, 10, 1, 1);
    const auto u2 = mk(MarketUpdateType::ADD, Side::BUY, 100, 5,  2, 2);
    f.book->onMarketUpdate(&u1);
    f.book->onMarketUpdate(&u2);

    const auto &bbo = f.book->getBBO();
    CHECK(bbo.bid_price_.value == 100);
    CHECK(bbo.bid_qty_.value   == 15);

    // Both orders sit in the same level (FIFO).
    const auto *lvl = f.book->getOrdersAtPrice(Price{100});
    REQUIRE(lvl != nullptr);
    REQUIRE(lvl->first_mkt_order_ != nullptr);
    CHECK(lvl->first_mkt_order_->order_id_.value == 1);
    CHECK(lvl->first_mkt_order_->next_order_->order_id_.value == 2);
}

// ── Test 3: MODIFY updates resting qty, BBO refreshes ───────────────────────
TEST_CASE("Trading: MODIFY updates resting qty") {
    Fixture f;
    const auto add = mk(MarketUpdateType::ADD, Side::SELL, 101, 20, 7);
    f.book->onMarketUpdate(&add);

    const auto modify = mk(MarketUpdateType::MODIFY, Side::SELL, 101, 8, 7);
    f.book->onMarketUpdate(&modify);

    CHECK(f.book->getBBO().ask_qty_.value == 8);
    CHECK(f.book->getOrder(OrderId{7})->qty_.value == 8);
}

// ── Test 4: CANCEL removes the order + level when it was the last ───────────
TEST_CASE("Trading: CANCEL removes order and level when empty") {
    Fixture f;
    const auto add = mk(MarketUpdateType::ADD, Side::BUY, 99, 10, 3);
    f.book->onMarketUpdate(&add);

    const auto cancel = mk(MarketUpdateType::CANCEL, Side::BUY, 99, 10, 3);
    f.book->onMarketUpdate(&cancel);

    CHECK(f.book->getOrder(OrderId{3}) == nullptr);
    CHECK(f.book->getOrdersAtPrice(Price{99}) == nullptr);
    CHECK(f.book->getBBO().bid_price_.isValid() == false);
}

// ── Test 5: price-level ordering: bids descending, asks ascending ───────────
TEST_CASE("Trading: bid side descending, ask side ascending by price") {
    Fixture f;
    // Bids at 98, 100, 99 -- head should be 100 (most aggressive).
    const auto b98  = mk(MarketUpdateType::ADD, Side::BUY,  98, 5, 1, 1);
    const auto b100 = mk(MarketUpdateType::ADD, Side::BUY, 100, 5, 2, 1);
    const auto b99  = mk(MarketUpdateType::ADD, Side::BUY,  99, 5, 3, 1);
    f.book->onMarketUpdate(&b98);
    f.book->onMarketUpdate(&b100);
    f.book->onMarketUpdate(&b99);

    const auto *head = f.book->getBidsByPrice();
    REQUIRE(head != nullptr);
    CHECK(head->price_.value == 100);
    CHECK(head->next_entry_->price_.value == 99);
    CHECK(head->next_entry_->next_entry_->price_.value == 98);

    // Asks at 102, 101, 103 -- head should be 101.
    const auto a102 = mk(MarketUpdateType::ADD, Side::SELL, 102, 5, 4, 1);
    const auto a101 = mk(MarketUpdateType::ADD, Side::SELL, 101, 5, 5, 1);
    const auto a103 = mk(MarketUpdateType::ADD, Side::SELL, 103, 5, 6, 1);
    f.book->onMarketUpdate(&a102);
    f.book->onMarketUpdate(&a101);
    f.book->onMarketUpdate(&a103);

    const auto *asks = f.book->getAsksByPrice();
    REQUIRE(asks != nullptr);
    CHECK(asks->price_.value == 101);
    CHECK(asks->next_entry_->price_.value == 102);
    CHECK(asks->next_entry_->next_entry_->price_.value == 103);

    // BBO reflects the inside.
    CHECK(f.book->getBBO().bid_price_.value == 100);
    CHECK(f.book->getBBO().ask_price_.value == 101);
}

// ── Test 6: CLEAR wipes every order and level ───────────────────────────────
TEST_CASE("Trading: CLEAR wipes the book entirely") {
    Fixture f;
    const auto a1 = mk(MarketUpdateType::ADD, Side::BUY, 100, 5, 1);
    const auto a2 = mk(MarketUpdateType::ADD, Side::SELL, 101, 7, 2);
    f.book->onMarketUpdate(&a1);
    f.book->onMarketUpdate(&a2);

    MEMarketUpdate clear{};
    clear.type_ = MarketUpdateType::CLEAR;
    f.book->onMarketUpdate(&clear);

    CHECK(f.book->getOrder(OrderId{1}) == nullptr);
    CHECK(f.book->getOrder(OrderId{2}) == nullptr);
    CHECK(f.book->getBBO().bid_price_.isValid() == false);
    CHECK(f.book->getBBO().ask_price_.isValid() == false);
    CHECK(f.book->getBidsByPrice() == nullptr);
    CHECK(f.book->getAsksByPrice() == nullptr);
}

// ── Test 7: TRADE is informational -- does not mutate the book ──────────────
TEST_CASE("Trading: TRADE update does not mutate book state") {
    Fixture f;
    const auto add = mk(MarketUpdateType::ADD, Side::BUY, 100, 10, 1);
    f.book->onMarketUpdate(&add);

    const auto before = f.book->getBBO();

    MEMarketUpdate trade{};
    trade.type_     = MarketUpdateType::TRADE;
    trade.side_     = Side::SELL;
    trade.price_    = Price{100};
    trade.qty_      = Qty{3};
    trade.order_id_ = OrderId{};
    f.book->onMarketUpdate(&trade);

    const auto &after = f.book->getBBO();
    CHECK(after.bid_price_ == before.bid_price_);
    CHECK(after.bid_qty_   == before.bid_qty_);
}

// ── Test 8: ADD aliasing a live level's direct-map slot is dropped ──────────
//
// priceToIndex is price % ME_MAX_PRICE_LEVELS, so a price exactly
// ME_MAX_PRICE_LEVELS away from a live level maps to the same slot. A
// well-behaved exchange rejects such orders before they hit the feed; if
// one arrives anyway, the client must drop it rather than corrupt the book.
TEST_CASE("Trading: ADD at a price aliasing a live level is dropped") {
    Fixture f;
    constexpr uint64_t p         = 100;
    constexpr uint64_t colliding = p + ME_MAX_PRICE_LEVELS;

    const auto add = mk(MarketUpdateType::ADD, Side::BUY, p, 10, 1);
    f.book->onMarketUpdate(&add);

    // Aliasing ADD: ignored entirely.
    const auto bad = mk(MarketUpdateType::ADD, Side::BUY, colliding, 5, 2);
    f.book->onMarketUpdate(&bad);

    CHECK(f.book->getOrder(OrderId{2}) == nullptr);        // never entered

    // BBO and the level list are unchanged.
    const auto &bbo = f.book->getBBO();
    CHECK(bbo.bid_price_.value == p);
    CHECK(bbo.bid_qty_.value   == 10);
    const auto *lvl = f.book->getOrdersAtPrice(Price{p});
    REQUIRE(lvl != nullptr);
    CHECK(lvl->price_.value == p);
    CHECK(f.book->getBidsByPrice() == lvl);
    CHECK(f.book->getBidsByPrice()->next_entry_ == lvl);   // single level

    // CANCEL of the original order still cleans up correctly.
    const auto cancel = mk(MarketUpdateType::CANCEL, Side::BUY, p, 10, 1);
    f.book->onMarketUpdate(&cancel);
    CHECK(f.book->getOrder(OrderId{1}) == nullptr);
    CHECK(f.book->getOrdersAtPrice(Price{p}) == nullptr);
    CHECK(f.book->getBBO().bid_price_.isValid() == false);
}
