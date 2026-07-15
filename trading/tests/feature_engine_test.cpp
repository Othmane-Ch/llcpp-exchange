// feature_engine_test.cpp -- unit tests for Trading::FeatureEngine.
//
// Drives a real MarketOrderBook with synthetic MEMarketUpdate records,
// then invokes FeatureEngine::onOrderBookUpdate / onTradeUpdate and
// verifies the computed signals.

#include "doctest/doctest.h"

#include <cmath>
#include <memory>

#include "feature_engine.h"
#include "market_order_book.h"
#include "market_update.h"
#include "logging.h"

using namespace Trading;
using namespace Common;
using Exchange::MEMarketUpdate;
using Exchange::MarketUpdateType;

// ── Helper: synthesise a market update ─────────────────────────────────────
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
    Logger logger{"test_feature_engine.log"};
    std::unique_ptr<MarketOrderBook> book{
        new MarketOrderBook(TickerId{0}, &logger)};
    FeatureEngine fe{&logger};
}; } // anonymous namespace

// ── Test 1: initial signals are NaN ────────────────────────────────────────
TEST_CASE("FeatureEngine: initial signals are NaN") {
    Fixture f;
    CHECK(std::isnan(f.fe.getMktPrice()));
    CHECK(std::isnan(f.fe.getAggTradeQtyRatio()));
}

// ── Test 2: BBO with one side empty leaves mkt_price NaN ───────────────────
TEST_CASE("FeatureEngine: incomplete BBO does not update mkt_price") {
    Fixture f;
    auto u = mk(MarketUpdateType::ADD, Side::BUY, 100, 10, 1);
    f.book->onMarketUpdate(&u);
    f.fe.onOrderBookUpdate(TickerId{0}, Price{100}, Side::BUY, f.book.get());

    CHECK(std::isnan(f.fe.getMktPrice()));
}

// ── Test 3: balanced BBO yields mid = 100.5 ─────────────────────────────────
TEST_CASE("FeatureEngine: balanced BBO yields midpoint mkt_price") {
    Fixture f;
    auto b = mk(MarketUpdateType::ADD, Side::BUY,  100, 10, 1);
    auto a = mk(MarketUpdateType::ADD, Side::SELL, 101, 10, 2);
    f.book->onMarketUpdate(&b);
    f.book->onMarketUpdate(&a);
    f.fe.onOrderBookUpdate(TickerId{0}, Price{101}, Side::SELL, f.book.get());

    // (100*10 + 101*10) / 20 = 100.5
    CHECK(f.fe.getMktPrice() == doctest::Approx(100.5));
}

// ── Test 4: imbalanced BBO skews mkt_price toward the lighter side ─────────
TEST_CASE("FeatureEngine: thinner ask pulls mkt_price toward the ask") {
    Fixture f;
    auto b = mk(MarketUpdateType::ADD, Side::BUY,  100, 10, 1);
    auto a = mk(MarketUpdateType::ADD, Side::SELL, 101, 1,  2);
    f.book->onMarketUpdate(&b);
    f.book->onMarketUpdate(&a);
    f.fe.onOrderBookUpdate(TickerId{0}, Price{101}, Side::SELL, f.book.get());

    // (100*1 + 101*10) / 11 ≈ 100.909
    CHECK(f.fe.getMktPrice() == doctest::Approx((100.0 * 1 + 101.0 * 10) / 11.0));
    CHECK(f.fe.getMktPrice() > 100.5);
}

// ── Test 5: TRADE BUY computes ratio against the ask side ──────────────────
TEST_CASE("FeatureEngine: TRADE BUY ratio is trade_qty / ask_qty") {
    Fixture f;
    auto b = mk(MarketUpdateType::ADD, Side::BUY,  100, 10, 1);
    auto a = mk(MarketUpdateType::ADD, Side::SELL, 101, 4,  2);
    f.book->onMarketUpdate(&b);
    f.book->onMarketUpdate(&a);

    // A BUY trade aggressed against the 4-qty ask.
    auto trade = mk(MarketUpdateType::TRADE, Side::BUY, 101, 2, 0);
    f.fe.onTradeUpdate(&trade, f.book.get());

    CHECK(f.fe.getAggTradeQtyRatio() == doctest::Approx(0.5));   // 2 / 4
}

// ── Test 6: TRADE SELL ratio is trade_qty / bid_qty ────────────────────────
TEST_CASE("FeatureEngine: TRADE SELL ratio is trade_qty / bid_qty") {
    Fixture f;
    auto b = mk(MarketUpdateType::ADD, Side::BUY,  100, 8, 1);
    auto a = mk(MarketUpdateType::ADD, Side::SELL, 101, 5, 2);
    f.book->onMarketUpdate(&b);
    f.book->onMarketUpdate(&a);

    auto trade = mk(MarketUpdateType::TRADE, Side::SELL, 100, 4, 0);
    f.fe.onTradeUpdate(&trade, f.book.get());

    CHECK(f.fe.getAggTradeQtyRatio() == doctest::Approx(0.5));   // 4 / 8
}

// ── Test 7: full sweep yields ratio of 1.0 ─────────────────────────────────
TEST_CASE("FeatureEngine: TRADE that consumes the entire level yields ratio 1.0") {
    Fixture f;
    auto b = mk(MarketUpdateType::ADD, Side::BUY,  100, 10, 1);
    auto a = mk(MarketUpdateType::ADD, Side::SELL, 101, 5,  2);
    f.book->onMarketUpdate(&b);
    f.book->onMarketUpdate(&a);

    auto trade = mk(MarketUpdateType::TRADE, Side::BUY, 101, 5, 0);
    f.fe.onTradeUpdate(&trade, f.book.get());

    CHECK(f.fe.getAggTradeQtyRatio() == doctest::Approx(1.0));
}
