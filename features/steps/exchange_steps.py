"""Step definitions for the llcpp exchange BDD tests."""

from behave import given, when, then


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _drain_responses(context):
    context.responses = [dict(r) for r in context.engine.get_responses()]
    return context.responses


def _drain_updates(context):
    context.market_updates = [dict(u) for u in context.engine.get_market_updates()]
    return context.market_updates


def _next_oid(context):
    oid = context.next_order_id
    context.next_order_id += 1
    return oid


# ---------------------------------------------------------------------------
# Given
# ---------------------------------------------------------------------------

@given("a fresh exchange engine")
def step_fresh_engine(context):
    # Already handled by environment.py before_scenario
    pass


@given("a resting {side} order for {qty:d} shares at price {price:d} from client {cid:d}")
def step_resting_order(context, side, qty, price, cid):
    oid = _next_oid(context)
    context.engine.add_order(
        client_id=cid, order_id=oid, ticker_id=0,
        side=side, price=price, qty=qty,
    )
    _drain_responses(context)
    _drain_updates(context)


@given("a resting {side} order for {qty:d} shares at price {price:d} from client {cid:d} with order id {oid:d}")
def step_resting_order_with_id(context, side, qty, price, cid, oid):
    # Keep next_order_id in sync
    if oid >= context.next_order_id:
        context.next_order_id = oid + 1
    context.engine.add_order(
        client_id=cid, order_id=oid, ticker_id=0,
        side=side, price=price, qty=qty,
    )
    _drain_responses(context)
    updates = _drain_updates(context)
    # Track first resting order's market_order_id for FIFO assertion
    if not hasattr(context, "_first_resting_market_oid"):
        context._first_resting_market_oid = updates[0]["order_id"] if updates else None


@given("a crossing {side} order for {qty:d} shares at price {price:d} from client {cid:d} with order id {oid:d}")
def step_crossing_order_given(context, side, qty, price, cid, oid):
    if oid >= context.next_order_id:
        context.next_order_id = oid + 1
    context.engine.add_order(
        client_id=cid, order_id=oid, ticker_id=0,
        side=side, price=price, qty=qty,
    )
    _drain_responses(context)
    _drain_updates(context)


# ---------------------------------------------------------------------------
# When
# ---------------------------------------------------------------------------

@when("I submit a {side} order for {qty:d} shares at price {price:d}")
def step_submit_order(context, side, qty, price):
    oid = _next_oid(context)
    context.engine.add_order(
        client_id=1, order_id=oid, ticker_id=0,
        side=side, price=price, qty=qty,
    )
    _drain_responses(context)
    _drain_updates(context)


@when("client {cid:d} submits a {side} order for {qty:d} shares at price {price:d}")
def step_client_submits_order(context, cid, side, qty, price):
    oid = _next_oid(context)
    context.engine.add_order(
        client_id=cid, order_id=oid, ticker_id=0,
        side=side, price=price, qty=qty,
    )
    _drain_responses(context)
    _drain_updates(context)


@when("client {cid:d} cancels order {oid:d} on ticker {tid:d}")
def step_cancel_order(context, cid, oid, tid):
    context.engine.cancel_order(client_id=cid, order_id=oid, ticker_id=tid)
    _drain_responses(context)
    _drain_updates(context)


# ---------------------------------------------------------------------------
# Then — response assertions
# ---------------------------------------------------------------------------

@then('I should receive {count:d} response of type "{rtype}"')
def step_receive_responses_of_type(context, count, rtype):
    matched = [r for r in context.responses if r["type"] == rtype]
    assert len(matched) == count, \
        f"Expected {count} {rtype} responses, got {len(matched)}: {context.responses}"


@then("I should receive {count:d} fill responses")
def step_receive_fills(context, count):
    filled = [r for r in context.responses if r["type"] == "FILLED"]
    assert len(filled) == count, \
        f"Expected {count} FILLED responses, got {len(filled)}: {context.responses}"


@then("client {cid:d} should be filled for {qty:d} shares with {remaining:d} remaining")
def step_client_filled(context, cid, qty, remaining):
    fills = [r for r in context.responses if r["type"] == "FILLED" and r["client_id"] == cid]
    assert len(fills) > 0, f"No fills for client {cid}"
    last_fill = fills[-1]
    assert last_fill["exec_qty"] == qty, \
        f"Expected exec_qty={qty}, got {last_fill['exec_qty']}"
    assert last_fill["leaves_qty"] == remaining, \
        f"Expected leaves_qty={remaining}, got {last_fill['leaves_qty']}"


@then("client {cid:d} total filled quantity should be {total:d}")
def step_client_total_filled(context, cid, total):
    fills = [r for r in context.responses if r["type"] == "FILLED" and r["client_id"] == cid]
    actual = sum(r["exec_qty"] for r in fills)
    assert actual == total, f"Expected total fill qty={total}, got {actual}"


@then("the first resting order should be matched")
def step_first_resting_matched(context):
    first_moid = getattr(context, "_first_resting_market_oid", None)
    assert first_moid is not None, "No first resting order market_order_id recorded"
    passive_fills = [r for r in context.responses
                     if r["type"] == "FILLED" and r["client_id"] == 1]
    assert len(passive_fills) == 1
    assert passive_fills[0]["market_order_id"] == first_moid


# ---------------------------------------------------------------------------
# Then — book state assertions
# ---------------------------------------------------------------------------

@then("the bid side should have {count:d} level")
@then("the bid side should have {count:d} levels")
def step_bid_levels(context, count):
    bids = context.engine.get_bid_levels(0)
    assert len(bids) == count, f"Expected {count} bid levels, got {len(bids)}"


@then("the ask side should have {count:d} level")
@then("the ask side should have {count:d} levels")
def step_ask_levels(context, count):
    asks = context.engine.get_ask_levels(0)
    assert len(asks) == count, f"Expected {count} ask levels, got {len(asks)}"


@then("the best bid should be {price:d}")
def step_best_bid(context, price):
    actual = context.engine.get_best_bid(0)
    assert actual == price, f"Expected best bid={price}, got {actual}"


@then("the best ask should be {price:d}")
def step_best_ask(context, price):
    actual = context.engine.get_best_ask(0)
    assert actual == price, f"Expected best ask={price}, got {actual}"


@then("the best bid should be empty")
def step_best_bid_empty(context):
    assert context.engine.get_best_bid(0) is None


@then("the best ask should be empty")
def step_best_ask_empty(context):
    assert context.engine.get_best_ask(0) is None


@then("the spread should be {spread:d}")
def step_spread(context, spread):
    actual = context.engine.get_spread(0)
    assert actual == spread, f"Expected spread={spread}, got {actual}"


@then("the spread should be empty")
def step_spread_empty(context):
    assert context.engine.get_spread(0) is None


@then("the book should be empty")
def step_book_empty(context):
    assert len(context.engine.get_bid_levels(0)) == 0, "Bids not empty"
    assert len(context.engine.get_ask_levels(0)) == 0, "Asks not empty"


@then("the bid level at price {price:d} should have total quantity {qty:d}")
def step_bid_level_qty(context, price, qty):
    bids = context.engine.get_bid_levels(0)
    level = next((b for b in bids if b.price == price), None)
    assert level is not None, f"No bid level at price {price}"
    assert level.total_qty == qty, \
        f"Expected total_qty={qty} at price {price}, got {level.total_qty}"


@then("the ask level at price {price:d} should have total quantity {qty:d}")
def step_ask_level_qty(context, price, qty):
    asks = context.engine.get_ask_levels(0)
    level = next((a for a in asks if a.price == price), None)
    assert level is not None, f"No ask level at price {price}"
    assert level.total_qty == qty, \
        f"Expected total_qty={qty} at price {price}, got {level.total_qty}"


@then("the bid level at price {price:d} should have {count:d} orders")
@then("the bid level at price {price:d} should have {count:d} order")
def step_bid_level_count(context, price, count):
    bids = context.engine.get_bid_levels(0)
    level = next((b for b in bids if b.price == price), None)
    assert level is not None, f"No bid level at price {price}"
    assert level.order_count == count, \
        f"Expected {count} orders at price {price}, got {level.order_count}"


@then("bid level {idx:d} should have price {price:d}")
def step_bid_level_n_price(context, idx, price):
    bids = context.engine.get_bid_levels(0)
    assert len(bids) >= idx, f"Only {len(bids)} bid levels, wanted index {idx}"
    assert bids[idx - 1].price == price, \
        f"Bid level {idx} price: expected {price}, got {bids[idx - 1].price}"


# ---------------------------------------------------------------------------
# Then — market data assertions
# ---------------------------------------------------------------------------

@then('I should receive {count:d} market update of type "{mtype}"')
@then('I should receive {count:d} market updates of type "{mtype}"')
def step_market_update_count(context, count, mtype):
    matched = [u for u in context.market_updates if u["type"] == mtype]
    assert len(matched) == count, \
        f"Expected {count} {mtype} updates, got {len(matched)}: {context.market_updates}"


@then('I should receive at least {count:d} market update of type "{mtype}"')
@then('I should receive at least {count:d} market updates of type "{mtype}"')
def step_market_update_at_least(context, count, mtype):
    matched = [u for u in context.market_updates if u["type"] == mtype]
    assert len(matched) >= count, \
        f"Expected at least {count} {mtype} updates, got {len(matched)}: {context.market_updates}"


@then("I should receive {count:d} market updates")
def step_total_market_updates(context, count):
    assert len(context.market_updates) == count, \
        f"Expected {count} market updates, got {len(context.market_updates)}"


@then("the market update should have price {price:d} and quantity {qty:d}")
def step_market_update_price_qty(context, price, qty):
    assert len(context.market_updates) > 0, "No market updates"
    u = context.market_updates[0]
    assert u["price"] == price, f"Expected price={price}, got {u['price']}"
    assert u["qty"] == qty, f"Expected qty={qty}, got {u['qty']}"


@then("the cancel update should have quantity {qty:d}")
def step_cancel_update_qty(context, qty):
    cancels = [u for u in context.market_updates if u["type"] == "CANCEL"]
    assert len(cancels) > 0, "No CANCEL updates"
    assert cancels[0]["qty"] == qty, \
        f"Expected cancel qty={qty}, got {cancels[0]['qty']}"


@then('I should receive {count:d} market update of type "{mtype}" with quantity {qty:d}')
def step_market_update_type_qty(context, count, mtype, qty):
    matched = [u for u in context.market_updates if u["type"] == mtype]
    assert len(matched) == count, \
        f"Expected {count} {mtype} updates, got {len(matched)}"
    assert matched[0]["qty"] == qty, \
        f"Expected qty={qty}, got {matched[0]['qty']}"
