"""Market data output correctness tests."""

from conftest import drain_responses, drain_updates


class TestMarketDataAdd:
    def test_add_order_publishes_add_update(self, engine, order_factory):
        """Adding a passive order produces an ADD market update."""
        engine.add_order(**order_factory(side="BUY", price=100, qty=10))
        drain_responses(engine)

        upds = drain_updates(engine)
        assert len(upds) == 1
        assert upds[0]["type"] == "ADD"
        assert upds[0]["side"] == "BUY"
        assert upds[0]["price"] == 100
        assert upds[0]["qty"] == 10

    def test_add_order_has_valid_order_id_and_priority(self, engine, order_factory):
        """ADD update carries a valid market order ID and priority."""
        engine.add_order(**order_factory(side="SELL", price=200, qty=5))
        drain_responses(engine)

        upds = drain_updates(engine)
        assert len(upds) == 1
        assert upds[0]["order_id"] != 2**64 - 1  # not INVALID
        assert upds[0]["priority"] != 2**64 - 1


class TestMarketDataTrade:
    def test_match_publishes_trade(self, engine, order_factory):
        """A match produces a TRADE update."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=100, qty=10))
        drain_responses(engine)

        upds = drain_updates(engine)
        trade_upds = [u for u in upds if u["type"] == "TRADE"]
        assert len(trade_upds) == 1
        assert trade_upds[0]["price"] == 100
        assert trade_upds[0]["qty"] == 10


class TestMarketDataModify:
    def test_partial_fill_publishes_modify(self, engine, order_factory):
        """Partial fill produces a MODIFY update with remaining qty."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=30))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=100, qty=10))
        drain_responses(engine)

        upds = drain_updates(engine)
        modify_upds = [u for u in upds if u["type"] == "MODIFY"]
        assert len(modify_upds) == 1
        assert modify_upds[0]["qty"] == 20  # 30 - 10


class TestMarketDataCancel:
    def test_full_fill_publishes_cancel(self, engine, order_factory):
        """Full fill of passive order produces a CANCEL update."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=100, qty=10))
        drain_responses(engine)

        upds = drain_updates(engine)
        cancel_upds = [u for u in upds if u["type"] == "CANCEL"]
        assert len(cancel_upds) == 1

    def test_manual_cancel_publishes_cancel(self, engine, order_factory):
        """Explicit cancel produces a CANCEL update."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="BUY", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        engine.cancel_order(client_id=1, order_id=1, ticker_id=0)
        drain_responses(engine)

        upds = drain_updates(engine)
        assert len(upds) == 1
        assert upds[0]["type"] == "CANCEL"
        assert upds[0]["qty"] == 10


class TestMarketDataSequence:
    def test_full_match_update_sequence(self, engine, order_factory):
        """Full match produces TRADE then CANCEL (passive removed)."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=100, qty=10))
        drain_responses(engine)

        upds = drain_updates(engine)
        types = [u["type"] for u in upds]
        assert types == ["TRADE", "CANCEL"]

    def test_no_add_on_fully_consumed_aggressive(self, engine, order_factory):
        """Fully consumed aggressive order should NOT produce an ADD update."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="SELL", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        engine.add_order(**order_factory(client_id=2, order_id=2, side="BUY", price=100, qty=10))
        drain_responses(engine)

        upds = drain_updates(engine)
        add_upds = [u for u in upds if u["type"] == "ADD"]
        assert len(add_upds) == 0
