// position_keeper_test.cpp -- unit tests for Trading::PositionKeeper / PositionInfo.
//
// Drives the VWAP-based PnL accounting via synthetic MEClientResponse fills
// and verifies position, realized/unrealized/total PnL, volume, and the
// reset behaviour at flat / flipped positions.

#include "doctest/doctest.h"

#include "position_keeper.h"
#include "client_response.h"
#include "logging.h"

using namespace Trading;
using namespace Common;
using Exchange::MEClientResponse;
using Exchange::ClientResponseType;

// ── Helper: build an MEClientResponse FILLED-fill record on the stack ───────
static auto fill(Side side, uint64_t price, uint32_t exec_qty,
                 uint32_t ticker = 0) -> MEClientResponse {
    MEClientResponse r{};
    r.type_      = ClientResponseType::FILLED;
    r.client_id_ = ClientId{1};
    r.ticker_id_ = TickerId{ticker};
    r.side_      = side;
    r.price_     = Price{price};
    r.exec_qty_  = Qty{exec_qty};
    r.leaves_qty_ = Qty{0};
    return r;
}

namespace { struct Fixture {
    Logger logger{"test_position_keeper.log"};
    PositionKeeper keeper{&logger};
}; } // anonymous namespace

// ── Test 1: initial state is flat zero ─────────────────────────────────────
TEST_CASE("PositionKeeper: initial state is zero across all fields") {
    Fixture f;
    const auto *pi = f.keeper.getPositionInfo(TickerId{0});
    CHECK(pi->position_ == 0);
    CHECK(pi->volume_.value == 0);
    CHECK(pi->real_pnl_   == doctest::Approx(0.0));
    CHECK(pi->unreal_pnl_ == doctest::Approx(0.0));
    CHECK(pi->total_pnl_  == doctest::Approx(0.0));
}

// ── Test 2: open long establishes VWAP, no realized PnL ─────────────────────
TEST_CASE("PositionKeeper: opening a long sets position and VWAP, no realized PnL") {
    Fixture f;
    const auto r = fill(Side::BUY, 100, 10);
    f.keeper.addFill(&r);

    const auto *pi = f.keeper.getPositionInfo(TickerId{0});
    CHECK(pi->position_  == 10);
    CHECK(pi->volume_.value == 10);
    CHECK(pi->real_pnl_  == doctest::Approx(0.0));
    // open_vwap_[BUY] / |position| == 100 (the open VWAP)
    CHECK(pi->open_vwap_[sideToIndex(Side::BUY)] / 10.0 == doctest::Approx(100.0));
}

// ── Test 3: adding to a long updates VWAP, no realized PnL ──────────────────
TEST_CASE("PositionKeeper: averaging up a long updates VWAP correctly") {
    Fixture f;
    auto r1 = fill(Side::BUY, 100, 10); f.keeper.addFill(&r1);  // pos=10 vwap=100
    auto r2 = fill(Side::BUY, 110, 10); f.keeper.addFill(&r2);  // pos=20 vwap=105

    const auto *pi = f.keeper.getPositionInfo(TickerId{0});
    CHECK(pi->position_ == 20);
    CHECK(pi->volume_.value == 20);
    CHECK(pi->real_pnl_ == doctest::Approx(0.0));
    CHECK(pi->open_vwap_[sideToIndex(Side::BUY)] / 20.0 == doctest::Approx(105.0));
    // Unrealized: last fill price was 110, vwap is 105, position=20 long.
    CHECK(pi->unreal_pnl_ == doctest::Approx((110.0 - 105.0) * 20.0));
}

// ── Test 4: partial close of a long realizes PnL ────────────────────────────
TEST_CASE("PositionKeeper: partial close of a long realizes profit") {
    Fixture f;
    auto open  = fill(Side::BUY,  100, 10); f.keeper.addFill(&open);   // long 10 @ 100
    auto close = fill(Side::SELL, 110, 4);  f.keeper.addFill(&close);  // sell 4 @ 110

    const auto *pi = f.keeper.getPositionInfo(TickerId{0});
    CHECK(pi->position_ == 6);
    CHECK(pi->volume_.value == 14);
    // realized = 4 * (110 - 100) = 40
    CHECK(pi->real_pnl_ == doctest::Approx(40.0));
    // remaining 6 shares still have VWAP 100, last trade price 110 → unreal = 60
    CHECK(pi->unreal_pnl_ == doctest::Approx(60.0));
    CHECK(pi->total_pnl_  == doctest::Approx(100.0));
}

// ── Test 5: full close zeroes everything except realized/total/volume ──────
TEST_CASE("PositionKeeper: full close zeroes position and unrealized PnL") {
    Fixture f;
    auto open  = fill(Side::BUY,  100, 10); f.keeper.addFill(&open);   // long 10 @ 100
    auto close = fill(Side::SELL, 105, 10); f.keeper.addFill(&close);  // sell 10 @ 105

    const auto *pi = f.keeper.getPositionInfo(TickerId{0});
    CHECK(pi->position_ == 0);
    CHECK(pi->volume_.value == 20);
    CHECK(pi->real_pnl_   == doctest::Approx(50.0));
    CHECK(pi->unreal_pnl_ == doctest::Approx(0.0));
    CHECK(pi->total_pnl_  == doctest::Approx(50.0));
    CHECK(pi->open_vwap_[sideToIndex(Side::BUY)]  == doctest::Approx(0.0));
    CHECK(pi->open_vwap_[sideToIndex(Side::SELL)] == doctest::Approx(0.0));
}

// ── Test 6: open and close a short position ────────────────────────────────
TEST_CASE("PositionKeeper: opening a short and covering at lower price profits") {
    Fixture f;
    auto open  = fill(Side::SELL, 100, 10); f.keeper.addFill(&open);   // short 10 @ 100
    auto cover = fill(Side::BUY,   95, 10); f.keeper.addFill(&cover);  // cover @ 95

    const auto *pi = f.keeper.getPositionInfo(TickerId{0});
    CHECK(pi->position_ == 0);
    CHECK(pi->real_pnl_ == doctest::Approx(50.0));   // +5 per share * 10
    CHECK(pi->unreal_pnl_ == doctest::Approx(0.0));
    CHECK(pi->total_pnl_  == doctest::Approx(50.0));
}

// ── Test 7: position flip through zero realizes against old, re-opens new ──
TEST_CASE("PositionKeeper: oversize sell flips long to short and re-opens VWAP") {
    Fixture f;
    auto open = fill(Side::BUY,  100, 10); f.keeper.addFill(&open);   // long 10 @ 100
    auto flip = fill(Side::SELL, 110, 15); f.keeper.addFill(&flip);   // sell 15 @ 110

    const auto *pi = f.keeper.getPositionInfo(TickerId{0});
    CHECK(pi->position_ == -5);
    CHECK(pi->volume_.value == 25);
    // Realized: 10 shares closed at +10 each = 100
    CHECK(pi->real_pnl_ == doctest::Approx(100.0));
    // The remaining -5 short was opened at the flip price (110).
    CHECK(pi->open_vwap_[sideToIndex(Side::SELL)] / 5.0 == doctest::Approx(110.0));
    // Last trade price was 110, short VWAP 110, so unrealized = 0.
    CHECK(pi->unreal_pnl_ == doctest::Approx(0.0));
    CHECK(pi->total_pnl_  == doctest::Approx(100.0));
}

// ── Test 8: updateBBO adjusts unrealized PnL on a long ──────────────────────
TEST_CASE("PositionKeeper: updateBBO marks a long to mid price") {
    Fixture f;
    auto open = fill(Side::BUY, 100, 10); f.keeper.addFill(&open);  // long 10 @ 100

    BBO bbo{};
    bbo.bid_price_ = Price{109};
    bbo.ask_price_ = Price{111};
    bbo.bid_qty_   = Qty{1};
    bbo.ask_qty_   = Qty{1};
    f.keeper.updateBBO(TickerId{0}, &bbo);

    const auto *pi = f.keeper.getPositionInfo(TickerId{0});
    // mid = 110, vwap = 100, position = 10 → unreal = 100
    CHECK(pi->unreal_pnl_ == doctest::Approx(100.0));
    CHECK(pi->total_pnl_  == doctest::Approx(100.0));
}

// ── Test 9: per-ticker isolation ───────────────────────────────────────────
TEST_CASE("PositionKeeper: per-ticker position isolation") {
    Fixture f;
    // Derive ticker ids from ME_MAX_TICKERS so the test is portable across
    // capacity profiles (LLCPP_SMALL_FOOTPRINT shrinks ME_MAX_TICKERS).
    constexpr uint32_t last = static_cast<uint32_t>(ME_MAX_TICKERS) - 1;
    auto a = fill(Side::BUY,  100, 10, /*ticker*/0);    f.keeper.addFill(&a);
    auto b = fill(Side::SELL, 200, 5,  /*ticker*/last); f.keeper.addFill(&b);

    const auto *pi0 = f.keeper.getPositionInfo(TickerId{0});
    const auto *piL = f.keeper.getPositionInfo(TickerId{last});
    const auto *pi1 = f.keeper.getPositionInfo(TickerId{1});

    CHECK(pi0->position_ == 10);
    CHECK(piL->position_ == -5);
    CHECK(pi1->position_ == 0);
}
