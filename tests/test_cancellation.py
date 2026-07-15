"""Cancel and cancel-reject scenarios."""

from conftest import drain_responses, drain_updates


class TestCancelResting:
    def test_cancel_resting_order(self, engine, order_factory):
        """Cancel resting order → order removed, book updated."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="BUY", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        engine.cancel_order(client_id=1, order_id=1, ticker_id=0)

        resp = drain_responses(engine)
        assert len(resp) == 1
        assert resp[0]["type"] == "CANCELED"
        assert resp[0]["client_id"] == 1

        upds = drain_updates(engine)
        assert len(upds) == 1
        assert upds[0]["type"] == "CANCEL"
        assert upds[0]["qty"] == 10

        assert len(engine.get_bid_levels(0)) == 0

    def test_cancel_removes_correct_order(self, engine, order_factory):
        """Cancel one of multiple orders at same price → only that one removed."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="BUY", price=100, qty=10))
        engine.add_order(**order_factory(client_id=1, order_id=2, side="BUY", price=100, qty=20))
        drain_responses(engine)
        drain_updates(engine)

        engine.cancel_order(client_id=1, order_id=1, ticker_id=0)
        drain_responses(engine)
        drain_updates(engine)

        bids = engine.get_bid_levels(0)
        assert len(bids) == 1
        assert bids[0].total_qty == 20
        assert bids[0].order_count == 1


class TestCancelAlreadyFilled:
    def test_cancel_fully_filled_order(self, engine, order_factory):
        """Cancel already-filled order → rejection."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        # Fully fill it.
        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        # Try to cancel the fully filled sell.
        engine.cancel_order(client_id=1, order_id=1, ticker_id=0)

        resp = drain_responses(engine)
        assert len(resp) == 1
        assert resp[0]["type"] == "CANCEL_REJECTED"


class TestCancelPartiallyFilled:
    def test_cancel_partially_filled(self, engine, order_factory):
        """Cancel partially filled order → only remaining qty removed."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=30))
        drain_responses(engine)
        drain_updates(engine)

        # Partially fill: buy 10 of 30.
        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        # 20 remaining. Cancel it.
        engine.cancel_order(client_id=1, order_id=1, ticker_id=0)

        resp = drain_responses(engine)
        assert len(resp) == 1
        assert resp[0]["type"] == "CANCELED"

        upds = drain_updates(engine)
        assert len(upds) == 1
        assert upds[0]["type"] == "CANCEL"
        assert upds[0]["qty"] == 20  # remaining qty

        assert len(engine.get_ask_levels(0)) == 0


class TestCancelNonExistent:
    def test_cancel_non_existent_order(self, engine):
        """Cancel non-existent order ID → CANCEL_REJECTED, no market update."""
        engine.cancel_order(client_id=5, order_id=9999, ticker_id=0)

        resp = drain_responses(engine)
        assert len(resp) == 1
        assert resp[0]["type"] == "CANCEL_REJECTED"

        upds = drain_updates(engine)
        assert len(upds) == 0
