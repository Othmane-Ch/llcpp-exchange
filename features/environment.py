"""Behave environment hooks for the llcpp exchange BDD tests."""

import sys
from pathlib import Path

# Ensure the built C++ module is importable.
_build_dir = Path(__file__).resolve().parent.parent / "build" / "bindings" / "python"
if str(_build_dir) not in sys.path:
    sys.path.insert(0, str(_build_dir))

import exchange_core  # noqa: E402


def before_scenario(context, scenario):
    """Create a fresh exchange engine for every scenario."""
    context.engine = exchange_core.TestExchange()
    context.responses = []
    context.market_updates = []
    context.next_order_id = 1


def drain_responses(context):
    """Drain all pending responses and store them."""
    context.responses = [dict(r) for r in context.engine.get_responses()]
    return context.responses


def drain_updates(context):
    """Drain all pending market updates and store them."""
    context.market_updates = [dict(u) for u in context.engine.get_market_updates()]
    return context.market_updates
