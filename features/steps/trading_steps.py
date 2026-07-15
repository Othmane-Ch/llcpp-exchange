"""Step definitions for the llcpp client-side Trading::MarketOrderBook BDD tests.

Uses the trading_core pybind11 module to drive the client book from Python.
Each scenario starts with a fresh TestMarketOrderBook (see the @given step)."""

import sys
from pathlib import Path

from behave import given, when, then

# environment.py has already inserted the build dir onto sys.path for
# exchange_core; trading_core lives in the same dir so the same path works.
_build_dir = Path(__file__).resolve().parents[2] / "build" / "bindings" / "python"
if str(_build_dir) not in sys.path:
    sys.path.insert(0, str(_build_dir))

import trading_core  # noqa: E402


# ---------------------------------------------------------------------------
# Given
# ---------------------------------------------------------------------------

@given("a fresh market order book")
def step_fresh_book(context):
    context.book = trading_core.TestMarketOrderBook()


# ---------------------------------------------------------------------------
# When
# ---------------------------------------------------------------------------

@when("the book receives an ADD for {side} {qty:d} @ {price:d} oid {oid:d}")
def step_add(context, side, qty, price, oid):
    context.book.on_add(side, price, qty, oid)


@when("the book receives a MODIFY for {side} {qty:d} @ {price:d} oid {oid:d}")
def step_modify(context, side, qty, price, oid):
    context.book.on_modify(side, price, qty, oid)


@when("the book receives a CANCEL for {side} {price:d} oid {oid:d}")
def step_cancel(context, side, price, oid):
    context.book.on_cancel(side, price, oid)


@when("the book receives a TRADE for {side} {qty:d} @ {price:d}")
def step_trade(context, side, qty, price):
    context.book.on_trade(side, price, qty)


@when("the book receives a CLEAR")
def step_clear(context):
    context.book.on_clear()


# ---------------------------------------------------------------------------
# Then
# ---------------------------------------------------------------------------

@then("the best bid should be {price:d} with quantity {qty:d}")
def step_best_bid(context, price, qty):
    bbo = context.book.get_bbo()
    assert bbo["bid_price"] == price, f"expected bid price {price}, got {bbo['bid_price']}"
    assert bbo["bid_qty"] == qty, f"expected bid qty {qty}, got {bbo['bid_qty']}"


@then("the best ask should be {price:d} with quantity {qty:d}")
def step_best_ask(context, price, qty):
    bbo = context.book.get_bbo()
    assert bbo["ask_price"] == price, f"expected ask price {price}, got {bbo['ask_price']}"
    assert bbo["ask_qty"] == qty, f"expected ask qty {qty}, got {bbo['ask_qty']}"


@then("the best bid should be absent")
def step_no_bid(context):
    bbo = context.book.get_bbo()
    assert bbo["bid_price"] is None, f"expected no bid, got {bbo['bid_price']}"


@then("the best ask should be absent")
def step_no_ask(context):
    bbo = context.book.get_bbo()
    assert bbo["ask_price"] is None, f"expected no ask, got {bbo['ask_price']}"


@then("the book should have order {oid:d} resting")
def step_order_resting(context, oid):
    assert context.book.has_order(oid), f"order {oid} not resting"


@then("the book should not have order {oid:d} resting")
def step_order_not_resting(context, oid):
    assert not context.book.has_order(oid), f"order {oid} unexpectedly resting"


@then("the bid levels in order should be {prices}")
def step_bid_levels(context, prices):
    expected = [int(p.strip()) for p in prices.split(",")]
    actual = [lvl["price"] for lvl in context.book.get_bid_levels()]
    assert actual == expected, f"expected bid levels {expected}, got {actual}"


@then("the ask levels in order should be {prices}")
def step_ask_levels(context, prices):
    expected = [int(p.strip()) for p in prices.split(",")]
    actual = [lvl["price"] for lvl in context.book.get_ask_levels()]
    assert actual == expected, f"expected ask levels {expected}, got {actual}"
