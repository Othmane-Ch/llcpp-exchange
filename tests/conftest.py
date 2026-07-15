"""Shared fixtures for exchange functional tests."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Ensure the built module is importable.
_build_dir = Path(__file__).resolve().parent.parent / "build" / "bindings" / "python"
if str(_build_dir) not in sys.path:
    sys.path.insert(0, str(_build_dir))

import exchange_core  # noqa: E402


@pytest.fixture
def engine() -> exchange_core.TestExchange:
    """Fresh matching engine instance for each test."""
    eng = exchange_core.TestExchange()
    yield eng


@pytest.fixture
def order_factory():
    """Helper to create order kwargs with sensible defaults."""

    _next_oid: int = 1

    def make_order(
        *,
        client_id: int = 1,
        order_id: int | None = None,
        ticker_id: int = 0,
        side: str = "BUY",
        price: int = 100,
        qty: int = 10,
    ) -> dict:
        nonlocal _next_oid
        if order_id is None:
            order_id = _next_oid
            _next_oid += 1
        return dict(
            client_id=client_id,
            order_id=order_id,
            ticker_id=ticker_id,
            side=side,
            price=price,
            qty=qty,
        )

    return make_order


def drain_responses(engine: exchange_core.TestExchange) -> list[dict]:
    """Return all pending responses as a list of dicts."""
    return list(engine.get_responses())


def drain_updates(engine: exchange_core.TestExchange) -> list[dict]:
    """Return all pending market updates as a list of dicts."""
    return list(engine.get_market_updates())
