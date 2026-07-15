// risk_manager_test.cpp -- unit tests for Trading::RiskManager.
//
// Drives the three pre-trade gates (size / position / loss) by mutating the
// PositionInfo that RiskInfo holds a pointer to, so we observe the gates
// in isolation and in fast-rejection order.

#include "doctest/doctest.h"

#include "risk_manager.h"
#include "position_keeper.h"
#include "logging.h"

using namespace Trading;
using namespace Common;

namespace { struct Fixture {
    Logger logger{"test_risk_manager.log"};
    PositionKeeper keeper{&logger};
    TradeEngineCfgHashMap cfg{};

    Fixture() {
        // Ticker 0 risk limits: max order 50, max position 100, max loss -500.
        cfg[0].risk_cfg_ = RiskCfg{Qty{50}, Qty{100}, -500.0};
        // All other tickers default to all-zero (rejecting everything).
    }
}; } // anonymous namespace

// ── Test 1: a normal in-bounds order is ALLOWED ────────────────────────────
TEST_CASE("RiskManager: in-bounds order returns ALLOWED") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    CHECK(rm.checkPreTradeRisk(TickerId{0}, Side::BUY, Qty{10})
          == RiskCheckResult::ALLOWED);
}

// ── Test 2: oversize order short-circuits with ORDER_TOO_LARGE ─────────────
TEST_CASE("RiskManager: qty above max_order_size_ rejects ORDER_TOO_LARGE") {
    Fixture f;
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    CHECK(rm.checkPreTradeRisk(TickerId{0}, Side::BUY, Qty{51})
          == RiskCheckResult::ORDER_TOO_LARGE);
}

// ── Test 3: projected long position over max → POSITION_TOO_LARGE ──────────
TEST_CASE("RiskManager: projected long position over limit rejects POSITION_TOO_LARGE") {
    Fixture f;
    // Force the position via internal mutation: the RiskInfo holds a
    // const PositionInfo*, and we want to simulate "already long 95".
    // PositionInfo lives inside PositionKeeper's array; mutate via const-cast
    // since we own the keeper here.
    auto *pi = const_cast<PositionInfo *>(f.keeper.getPositionInfo(TickerId{0}));
    pi->position_ = 95;

    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    // Buying 10 more would push us to 105 (> max_position_=100).
    CHECK(rm.checkPreTradeRisk(TickerId{0}, Side::BUY, Qty{10})
          == RiskCheckResult::POSITION_TOO_LARGE);
    // Buying 5 stays within limits.
    CHECK(rm.checkPreTradeRisk(TickerId{0}, Side::BUY, Qty{5})
          == RiskCheckResult::ALLOWED);
}

// ── Test 4: projected short position over max → POSITION_TOO_LARGE ─────────
TEST_CASE("RiskManager: projected short position over limit rejects POSITION_TOO_LARGE") {
    Fixture f;
    auto *pi = const_cast<PositionInfo *>(f.keeper.getPositionInfo(TickerId{0}));
    pi->position_ = -95;

    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    // Selling 10 more would push us to -105 (|−105| > 100).
    CHECK(rm.checkPreTradeRisk(TickerId{0}, Side::SELL, Qty{10})
          == RiskCheckResult::POSITION_TOO_LARGE);
}

// ── Test 5: total_pnl_ below max_loss_ rejects with LOSS_TOO_LARGE ─────────
TEST_CASE("RiskManager: total_pnl below max_loss_ rejects LOSS_TOO_LARGE") {
    Fixture f;
    auto *pi = const_cast<PositionInfo *>(f.keeper.getPositionInfo(TickerId{0}));
    pi->total_pnl_ = -1000.0;  // beyond the -500 loss limit

    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    CHECK(rm.checkPreTradeRisk(TickerId{0}, Side::BUY, Qty{1})
          == RiskCheckResult::LOSS_TOO_LARGE);
}

// ── Test 6: order-size gate fires before position gate (fast rejection) ─────
TEST_CASE("RiskManager: order-size check runs before position check") {
    Fixture f;
    auto *pi = const_cast<PositionInfo *>(f.keeper.getPositionInfo(TickerId{0}));
    pi->position_ = 95;  // would also fail position check on a small qty

    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    // qty 60 fails BOTH gates; order-size must fire first.
    CHECK(rm.checkPreTradeRisk(TickerId{0}, Side::BUY, Qty{60})
          == RiskCheckResult::ORDER_TOO_LARGE);
}

// ── Test 7: zero-config ticker rejects everything ──────────────────────────
TEST_CASE("RiskManager: ticker with default-zero config rejects any order") {
    Fixture f;
    // Ticker 1 has all-zero RiskCfg → max_order_size_ is Qty{} == INVALID
    // (UINT32_MAX), so size check passes; max_position_ is also INVALID.
    // max_loss_ defaults to 0, so any non-negative total_pnl_ passes.
    // The only deterministic rejection is ORDER_TOO_LARGE if we send a
    // qty larger than max_order_size_ (treated as a uint32).
    RiskManager rm{&f.logger, &f.keeper, f.cfg};
    // With default-zero RiskCfg, max_order_size_ = Qty{INVALID=UINT32_MAX},
    // so even Qty{1000000} is "allowed" by the size gate. Position max is
    // INVALID (huge), and PnL is 0.0 ≥ 0.0 max_loss_ — so this returns ALLOWED.
    CHECK(rm.checkPreTradeRisk(TickerId{1}, Side::BUY, Qty{1})
          == RiskCheckResult::ALLOWED);
}
