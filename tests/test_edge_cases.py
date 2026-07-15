"""Edge case tests: boundary conditions and unusual inputs."""

from conftest import drain_responses, drain_updates


class TestZeroQuantity:
    def test_order_with_zero_quantity(self, engine, order_factory):
        """Order with quantity 0 is accepted but produces no resting order."""
        engine.add_order(**order_factory(side="BUY", price=100, qty=0))

        resp = drain_responses(engine)
        # Engine accepts the order (it doesn't validate qty==0).
        assert len(resp) >= 1
        assert resp[0]["type"] == "ACCEPTED"

        # With qty 0, the order should not rest (leaves_qty check in add()).
        # The engine's add() only inserts if leaves_qty > 0, so this is a no-op.
        bids = engine.get_bid_levels(0)
        # qty 0 means nothing rests.
        assert len(bids) == 0


class TestAggressiveLimitFullyFilled:
    def test_aggressive_limit_fully_consumed(self, engine, order_factory):
        """Order that would fill entirely on entry (aggressive limit) → no resting qty."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=50))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=100, qty=50))

        resp = drain_responses(engine)
        filled = [r for r in resp if r["type"] == "FILLED"]
        agg = [r for r in filled if r["client_id"] == 2]
        assert len(agg) == 1
        assert agg[0]["leaves_qty"] == 0

        assert len(engine.get_bid_levels(0)) == 0
        assert len(engine.get_ask_levels(0)) == 0


class TestSamePriceSameTime:
    def test_two_orders_same_price_fifo(self, engine, order_factory):
        """Two orders at exact same price → FIFO guaranteed."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=10))
        engine.add_order(**order_factory(client_id=1, order_id=2, side="SELL", price=100, qty=10))
        drain_responses(engine)

        upds = drain_updates(engine)
        assert len(upds) == 2
        # First order gets lower priority value.
        assert upds[0]["priority"] < upds[1]["priority"]

        # Matching a buy should hit the first order.
        engine.add_order(**order_factory(client_id=2, order_id=3, side="BUY", price=100, qty=10))
        resp = drain_responses(engine)
        filled_passive = [r for r in resp if r["type"] == "FILLED" and r["client_id"] == 1]
        assert len(filled_passive) == 1
        # The filled passive order should have the first order's market_order_id.
        assert filled_passive[0]["market_order_id"] == upds[0]["order_id"]


class TestEmptyBookOperations:
    def test_cancel_on_empty_book(self, engine):
        """Cancel on empty book → CANCEL_REJECTED."""
        engine.cancel_order(client_id=1, order_id=1, ticker_id=0)
        resp = drain_responses(engine)
        assert len(resp) == 1
        assert resp[0]["type"] == "CANCEL_REJECTED"

    def test_get_levels_empty_book(self, engine):
        """Querying levels on untouched book → empty lists."""
        assert len(engine.get_bid_levels(0)) == 0
        assert len(engine.get_ask_levels(0)) == 0
        assert engine.get_best_bid(0) is None
        assert engine.get_best_ask(0) is None
        assert engine.get_spread(0) is None


class TestMultipleTickers:
    def test_orders_on_different_tickers_independent(self, engine, order_factory):
        """Orders on different tickers don't interfere."""
        engine.add_order(**order_factory(order_id=1, ticker_id=0, side="BUY", price=100, qty=10))
        engine.add_order(**order_factory(order_id=2, ticker_id=1, side="SELL", price=200, qty=5))
        drain_responses(engine)

        bids_0 = engine.get_bid_levels(0)
        asks_0 = engine.get_ask_levels(0)
        bids_1 = engine.get_bid_levels(1)
        asks_1 = engine.get_ask_levels(1)

        assert len(bids_0) == 1
        assert bids_0[0].price == 100
        assert len(asks_0) == 0

        assert len(bids_1) == 0
        assert len(asks_1) == 1
        assert asks_1[0].price == 200


class TestPriceImprovement:
    def test_buy_at_higher_price_matches_at_passive(self, engine, order_factory):
        """Buy at 105 crosses sell at 100 → fill at passive price 100."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=105, qty=10))

        resp = drain_responses(engine)
        filled = [r for r in resp if r["type"] == "FILLED"]
        assert len(filled) == 2
        # Both fills should be at passive price 100, not 105.
        for f in filled:
            assert f["price"] == 100
