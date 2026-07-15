"""Order book construction and state tests."""

from conftest import drain_responses, drain_updates


class TestSingleOrder:
    def test_single_buy_rests_in_book(self, engine, order_factory):
        """Adding a single BUY → book shows 1 bid level, empty ask."""
        engine.add_order(**order_factory(side="BUY", price=100, qty=10))

        resp = drain_responses(engine)
        assert len(resp) == 1
        assert resp[0]["type"] == "ACCEPTED"
        assert resp[0]["leaves_qty"] == 10

        bids = engine.get_bid_levels(0)
        asks = engine.get_ask_levels(0)
        assert len(bids) == 1
        assert bids[0].price == 100
        assert bids[0].total_qty == 10
        assert len(asks) == 0

    def test_single_sell_rests_in_book(self, engine, order_factory):
        """Adding a single SELL → book shows 1 ask level, empty bid."""
        engine.add_order(**order_factory(side="SELL", price=200, qty=5))

        resp = drain_responses(engine)
        assert len(resp) == 1
        assert resp[0]["type"] == "ACCEPTED"

        bids = engine.get_bid_levels(0)
        asks = engine.get_ask_levels(0)
        assert len(bids) == 0
        assert len(asks) == 1
        assert asks[0].price == 200
        assert asks[0].total_qty == 5


class TestBothSides:
    def test_non_crossing_orders_both_sides(self, engine, order_factory):
        """Buy and sell at different prices (no cross) → both sides populated."""
        engine.add_order(**order_factory(side="BUY", price=99, qty=10))
        engine.add_order(**order_factory(side="SELL", price=101, qty=5))
        drain_responses(engine)

        bids = engine.get_bid_levels(0)
        asks = engine.get_ask_levels(0)
        assert len(bids) == 1
        assert len(asks) == 1
        assert bids[0].price == 99
        assert asks[0].price == 101

    def test_best_bid_ask_spread(self, engine, order_factory):
        """Best bid/ask and spread are correct."""
        engine.add_order(**order_factory(side="BUY", price=99, qty=10))
        engine.add_order(**order_factory(side="SELL", price=101, qty=5))
        drain_responses(engine)

        assert engine.get_best_bid(0) == 99
        assert engine.get_best_ask(0) == 101
        assert engine.get_spread(0) == 2


class TestMultipleOrdersSamePrice:
    def test_fifo_ordering_preserved(self, engine, order_factory):
        """Multiple orders at same price → FIFO ordering preserved."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="BUY", price=100, qty=5))
        engine.add_order(**order_factory(client_id=1, order_id=2, side="BUY", price=100, qty=7))
        engine.add_order(**order_factory(client_id=1, order_id=3, side="BUY", price=100, qty=3))
        drain_responses(engine)

        bids = engine.get_bid_levels(0)
        assert len(bids) == 1
        assert bids[0].total_qty == 15
        assert bids[0].order_count == 3

    def test_multiple_price_levels_sorted(self, engine, order_factory):
        """Multiple bid levels are sorted descending."""
        engine.add_order(**order_factory(order_id=1, side="BUY", price=98, qty=5))
        engine.add_order(**order_factory(order_id=2, side="BUY", price=100, qty=5))
        engine.add_order(**order_factory(order_id=3, side="BUY", price=99, qty=5))
        drain_responses(engine)

        bids = engine.get_bid_levels(0)
        assert len(bids) == 3
        assert bids[0].price == 100
        assert bids[1].price == 99
        assert bids[2].price == 98


class TestEmptyBook:
    def test_empty_book_no_levels(self, engine):
        """Empty book has no bid or ask levels."""
        bids = engine.get_bid_levels(0)
        asks = engine.get_ask_levels(0)
        assert len(bids) == 0
        assert len(asks) == 0

    def test_empty_book_no_best(self, engine):
        """Empty book returns None for best bid/ask/spread."""
        assert engine.get_best_bid(0) is None
        assert engine.get_best_ask(0) is None
        assert engine.get_spread(0) is None

    def test_book_empty_after_removing_all_orders(self, engine, order_factory):
        """Book state after removing all orders → empty book."""
        engine.add_order(**order_factory(client_id=1, order_id=1, side="BUY", price=100, qty=10))
        drain_responses(engine)
        drain_updates(engine)

        engine.cancel_order(client_id=1, order_id=1, ticker_id=0)
        drain_responses(engine)

        bids = engine.get_bid_levels(0)
        assert len(bids) == 0
