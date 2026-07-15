"""Performance benchmarks — reads C++ LatencyTracker stats.

These tests do NOT measure latency through Python. They submit orders via
the binding, then READ the C++ LatencyTracker aggregated results.
"""

import time

import pytest
from conftest import drain_responses, drain_updates

# The engine's async Logger drains through a flush thread that naps whenever
# its queue goes empty; the fixed log queue holds 64K-1 elements under
# LLCPP_SMALL_FOOTPRINT. Every add/match emits measure-macro log lines, so an
# unpaced 100k-order loop can outrun the flusher across one long nap and
# FATAL the engine. A short sleep every PACE_EVERY orders gives the flusher
# guaranteed catch-up windows; it does not affect the C++-side LatencyTracker
# numbers (they measure inside each call, and throughput is derived from the
# sum of per-call latencies).
PACE_EVERY = 100


def _pace(i):
    if i % PACE_EVERY == PACE_EVERY - 1:
        time.sleep(0.001)


class TestMatchingLatency:
    def test_add_order_latency(self, engine, order_factory):
        """Verify hot-path latency stays within bounds for add_order."""
        # Warm up: create orders to populate internal pools.
        for i in range(1000):
            engine.add_order(**order_factory(
                order_id=i + 1, side="BUY", price=50 + (i % 50), qty=1))
        drain_responses(engine)
        drain_updates(engine)
        engine.reset_perf_stats(0)

        # Measured run. Drain periodically: the response/update LFQueues hold
        # capacity-1 messages (64K-1 under LLCPP_SMALL_FOOTPRINT), and an
        # undrained 100k-order run overflows them (FATAL). Draining between
        # adds does not touch the C++-side LatencyTracker measurements.
        for i in range(100_000):
            engine.add_order(**order_factory(
                client_id=1, order_id=1001 + i,
                side="BUY", price=100 + (i % 100), qty=1))
            _pace(i)
            if i % 20_000 == 19_999:
                drain_responses(engine)
                drain_updates(engine)
        drain_responses(engine)
        drain_updates(engine)

        stats = engine.get_perf_stats(0)
        print(f"\nadd_order latency: count={stats['count']} "
              f"p50={stats['p50_ns']:.0f}ns "
              f"p99={stats['p99_ns']:.0f}ns "
              f"max={stats['max_ns']:.0f}ns")

        assert stats["count"] > 0
        # Soft assertion: p99 should be under 5us for passive adds.
        assert stats["p99_ns"] < 50_000, \
            f"p99 latency regression: {stats['p99_ns']:.0f}ns"

    def test_matching_latency(self, engine, order_factory):
        """Verify matching (cross) latency stays reasonable."""
        engine.reset_perf_stats(0)

        # Alternate SELL then BUY to force matches. Each pair produces ~4
        # responses and ~3 market updates, so drain periodically to keep the
        # LFQueues (capacity-1, 64K-1 under LLCPP_SMALL_FOOTPRINT) from
        # overflowing; draining does not touch the LatencyTracker stats.
        for i in range(50_000):
            engine.add_order(**order_factory(
                client_id=1, order_id=2 * i + 1,
                side="SELL", price=100, qty=1))
            engine.add_order(**order_factory(
                client_id=2, order_id=2 * i + 2,
                side="BUY", price=100, qty=1))
            _pace(i)
            if i % 10_000 == 9_999:
                drain_responses(engine)
                drain_updates(engine)
        drain_responses(engine)
        drain_updates(engine)

        stats = engine.get_perf_stats(0)
        print(f"\nmatching latency: count={stats['count']} "
              f"p50={stats['p50_ns']:.0f}ns "
              f"p99={stats['p99_ns']:.0f}ns "
              f"max={stats['max_ns']:.0f}ns")

        assert stats["count"] > 0
        assert stats["p99_ns"] < 50_000, \
            f"p99 matching latency regression: {stats['p99_ns']:.0f}ns"


class TestThroughput:
    def test_order_throughput(self, engine, order_factory):
        """Submit many orders and check throughput from C++ stats."""
        engine.reset_perf_stats(0)

        # Drain periodically — see test_add_order_latency: an undrained run
        # this size overflows the fixed-capacity LFQueues.
        n = 200_000
        for i in range(n):
            engine.add_order(**order_factory(
                client_id=1, order_id=i + 1,
                side="BUY", price=100 + (i % 100), qty=1))
            _pace(i)
            if i % 20_000 == 19_999:
                drain_responses(engine)
                drain_updates(engine)
        drain_responses(engine)
        drain_updates(engine)

        stats = engine.get_perf_stats(0)
        print(f"\nthroughput: {stats['throughput_per_sec']:.0f} orders/sec "
              f"mean={stats['mean_ns']:.0f}ns")

        assert stats["count"] == n
        # Throughput should be reasonably high (at least 100K/sec even under Python overhead).
        assert stats["throughput_per_sec"] > 100_000, \
            f"Throughput too low: {stats['throughput_per_sec']:.0f}/sec"


class TestPerfStatsReset:
    def test_reset_clears_stats(self, engine, order_factory):
        """reset_perf_stats clears accumulated data."""
        engine.add_order(**order_factory(side="BUY", price=100, qty=10))
        drain_responses(engine)

        stats_before = engine.get_perf_stats(0)
        assert stats_before["count"] > 0

        engine.reset_perf_stats(0)
        stats_after = engine.get_perf_stats(0)
        assert stats_after["count"] == 0
