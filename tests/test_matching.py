"""Matching logic tests: crossing, partial fills, multi-level sweeps."""

from conftest import drain_responses, drain_updates


class TestFullMatch:
    def test_buy_crosses_resting_sell(self, engine, order_factory):
        """Buy limit crosses resting sell → trade at passive (resting) price."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=20))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=100, qty=20))

        resp = drain_responses(engine)
        # ACCEPTED + 2x FILLED (aggressive + passive)
        assert len(resp) == 3
        assert resp[0]["type"] == "ACCEPTED"
        filled = [r for r in resp if r["type"] == "FILLED"]
        assert len(filled) == 2

        agg = [r for r in filled if r["client_id"] == 2][0]
        pas = [r for r in filled if r["client_id"] == 1][0]
        assert agg["exec_qty"] == 20
        assert agg["leaves_qty"] == 0
        assert pas["exec_qty"] == 20
        assert pas["leaves_qty"] == 0
        assert pas["price"] == 100  # trade at passive price

        # Book should be empty after full match.
        assert len(engine.get_bid_levels(0)) == 0
        assert len(engine.get_ask_levels(0)) == 0

    def test_sell_crosses_resting_buy(self, engine, order_factory):
        """Sell limit crosses resting buy → trade at passive price."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="BUY", price=100, qty=15))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=2, side="SELL", price=100, qty=15))

        resp = drain_responses(engine)
        filled = [r for r in resp if r["type"] == "FILLED"]
        assert len(filled) == 2
        assert all(r["exec_qty"] == 15 for r in filled)

        assert len(engine.get_bid_levels(0)) == 0
        assert len(engine.get_ask_levels(0)) == 0


class TestPartialFill:
    def test_partial_fill_buy_crosses_smaller_sell(self, engine, order_factory):
        """Partial fill: buy 100 crosses sell 60 → fill 60, buy has 40 remaining."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=60))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=100, qty=100))

        resp = drain_responses(engine)
        filled = [r for r in resp if r["type"] == "FILLED"]
        assert len(filled) == 2

        agg = [r for r in filled if r["client_id"] == 2][0]
        assert agg["exec_qty"] == 60
        assert agg["leaves_qty"] == 40

        # Buy should rest in book with remaining 40.
        bids = engine.get_bid_levels(0)
        assert len(bids) == 1
        assert bids[0].total_qty == 40

        # Sell side empty.
        assert len(engine.get_ask_levels(0)) == 0


class TestMultipleFills:
    def test_multiple_fills_sweep(self, engine, order_factory):
        """Buy 100 crosses sell 30 + sell 30 + sell 30 → three fills, 10 remaining."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=30))
        engine.add_order(**order_factory(client_id=1, order_id=2, side="SELL", price=100, qty=30))
        engine.add_order(**order_factory(client_id=1, order_id=3, side="SELL", price=100, qty=30))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=4, side="BUY", price=100, qty=100))

        resp = drain_responses(engine)
        upds = drain_updates(engine)

        # 1 ACCEPTED + fills for each passive + aggressive fill per match
        filled = [r for r in resp if r["type"] == "FILLED"]
        # 3 passive fills + 3 aggressive fills = 6
        assert len(filled) == 6

        agg_fills = [r for r in filled if r["client_id"] == 2]
        # Each match produces one fill for the aggressive side
        assert len(agg_fills) == 3
        total_filled = sum(r["exec_qty"] for r in agg_fills)
        assert total_filled == 90

        # Last aggressive fill should show 10 remaining.
        last_agg = agg_fills[-1]
        assert last_agg["leaves_qty"] == 10

        # Buy rests with remaining 10.
        bids = engine.get_bid_levels(0)
        assert len(bids) == 1
        assert bids[0].total_qty == 10

    def test_sweep_multiple_price_levels(self, engine, order_factory):
        """Market-like order eats through multiple price levels."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=10))
        engine.add_order(**order_factory(client_id=1, order_id=2, side="SELL", price=101, qty=10))
        engine.add_order(**order_factory(client_id=1, order_id=3, side="SELL", price=102, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        # Aggressive buy at 102 sweeps all three levels.
        engine.add_order(**order_factory(client_id=2, order_id=4, side="BUY", price=102, qty=30))

        resp = drain_responses(engine)
        filled = [r for r in resp if r["type"] == "FILLED"]
        agg_fills = [r for r in filled if r["client_id"] == 2]
        total = sum(r["exec_qty"] for r in agg_fills)
        assert total == 30

        # Book should be empty.
        assert len(engine.get_bid_levels(0)) == 0
        assert len(engine.get_ask_levels(0)) == 0


class TestFIFOPriority:
    def test_fifo_matching_order(self, engine, order_factory):
        """First-in order at same price is matched first."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=5))
        engine.add_order(**order_factory(client_id=1, order_id=2, side="SELL", price=100, qty=5))
        drain_responses(engine)

        upds = drain_updates(engine)
        moid_first = upds[0]["order_id"]

        # Buy 5 — should match the first SELL only.
        engine.add_order(**order_factory(client_id=2, order_id=3, side="BUY", price=100, qty=5))

        resp = drain_responses(engine)
        filled_passive = [r for r in resp if r["type"] == "FILLED" and r["client_id"] == 1]
        assert len(filled_passive) == 1
        assert filled_passive[0]["market_order_id"] == moid_first

        # Second SELL remains.
        asks = engine.get_ask_levels(0)
        assert len(asks) == 1
        assert asks[0].total_qty == 5
