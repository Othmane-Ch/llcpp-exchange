"""Streamlit dashboard for the llcpp trading exchange.

Provides interactive order submission, order book visualization,
execution report logs, market data feed, and C++ performance metrics.

Usage:
    streamlit run dashboard/app.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import streamlit as st
import pandas as pd

# ---------------------------------------------------------------------------
# Module import
# ---------------------------------------------------------------------------
_build_dir = Path(__file__).resolve().parent.parent / "build" / "bindings" / "python"
if str(_build_dir) not in sys.path:
    sys.path.insert(0, str(_build_dir))

try:
    import exchange_core  # noqa: E402
except ImportError:
    st.error(
        "Could not import `exchange_core`. "
        "Build the Python module first:\n\n"
        "```\n"
        "cmake -B build -DBUILD_PYTHON_BINDINGS=ON -DENABLE_PERF_TRACKING=ON\n"
        "cmake --build build --target exchange_core -j$(nproc)\n"
        "```"
    )
    st.stop()

# ---------------------------------------------------------------------------
# Page config
# ---------------------------------------------------------------------------
st.set_page_config(
    page_title="llcpp Exchange Dashboard",
    page_icon=":chart_with_upwards_trend:",
    layout="wide",
)

st.title("llcpp Exchange Dashboard")

# ---------------------------------------------------------------------------
# Session state — one TestExchange instance per session
# ---------------------------------------------------------------------------
if "engine" not in st.session_state:
    st.session_state.engine = exchange_core.TestExchange()
    st.session_state.responses: list[dict] = []
    st.session_state.market_updates: list[dict] = []
    st.session_state.order_count = 0
    st.session_state.next_order_id = 1
    st.session_state.resp_counts: dict[str, int] = {}
    st.session_state.upd_counts: dict[str, int] = {}

engine: exchange_core.TestExchange = st.session_state.engine


def _drain() -> None:
    """Drain queues and append to session-state logs."""
    new_responses = list(engine.get_responses())
    new_updates = list(engine.get_market_updates())
    for r in new_responses:
        d = dict(r)
        st.session_state.responses.append(d)
        # Update running counters
        rtype = d["type"]
        st.session_state.resp_counts[rtype] = st.session_state.resp_counts.get(rtype, 0) + 1
    for u in new_updates:
        d = dict(u)
        st.session_state.market_updates.append(d)
        utype = d["type"]
        st.session_state.upd_counts[utype] = st.session_state.upd_counts.get(utype, 0) + 1


# ---------------------------------------------------------------------------
# Sidebar — Order Submission
# ---------------------------------------------------------------------------
with st.sidebar:
    st.header("Submit Order")

    with st.form("order_form", clear_on_submit=True):
        col1, col2 = st.columns(2)
        with col1:
            client_id = st.number_input("Client ID", min_value=0, max_value=255, value=1, step=1)
        with col2:
            ticker_id = st.number_input("Ticker ID", min_value=0, max_value=7, value=0, step=1)

        side = st.selectbox("Side", ["BUY", "SELL"])
        price = st.number_input("Price", min_value=1, max_value=10000, value=100, step=1)
        qty = st.number_input("Quantity", min_value=1, max_value=100000, value=10, step=1)

        submitted = st.form_submit_button("Add Order", use_container_width=True)

    if submitted:
        oid = st.session_state.next_order_id
        st.session_state.next_order_id += 1
        engine.add_order(
            client_id=int(client_id),
            order_id=oid,
            ticker_id=int(ticker_id),
            side=side,
            price=int(price),
            qty=int(qty),
        )
        st.session_state.order_count += 1
        _drain()
        st.toast(f"Order #{oid} submitted: {side} {qty}@{price}", icon=":white_check_mark:")

    st.divider()

    st.header("Cancel Order")
    with st.form("cancel_form", clear_on_submit=True):
        cancel_cid = st.number_input("Client ID ", min_value=0, max_value=255, value=1, step=1, key="cancel_cid")
        cancel_oid = st.number_input("Order ID", min_value=0, value=1, step=1, key="cancel_oid")
        cancel_tid = st.number_input("Ticker ID ", min_value=0, max_value=7, value=0, step=1, key="cancel_tid")
        cancel_submitted = st.form_submit_button("Cancel Order", use_container_width=True)

    if cancel_submitted:
        engine.cancel_order(
            client_id=int(cancel_cid),
            order_id=int(cancel_oid),
            ticker_id=int(cancel_tid),
        )
        _drain()
        st.toast(f"Cancel request sent for client={cancel_cid} order={cancel_oid}", icon=":x:")

    st.divider()

    # Batch order generation for perf testing
    st.header("Batch Orders")
    with st.form("batch_form"):
        batch_count = st.number_input("Number of orders", min_value=100, max_value=500000, value=10000, step=1000)
        batch_side = st.selectbox("Side ", ["BUY", "SELL"], key="batch_side")
        batch_base_price = st.number_input("Base price", min_value=1, max_value=10000, value=100, step=1)
        batch_price_spread = st.number_input("Price spread", min_value=1, max_value=200, value=50, step=1)
        batch_submitted = st.form_submit_button("Generate Batch", use_container_width=True)

    if batch_submitted:
        start_oid = st.session_state.next_order_id
        with st.spinner(f"Submitting {batch_count} orders..."):
            for i in range(int(batch_count)):
                oid = start_oid + i
                p = int(batch_base_price) + (i % int(batch_price_spread))
                engine.add_order(
                    client_id=1,
                    order_id=oid,
                    ticker_id=0,
                    side=batch_side,
                    price=p,
                    qty=1,
                )
            st.session_state.next_order_id = start_oid + int(batch_count)
            st.session_state.order_count += int(batch_count)
            _drain()
        st.toast(f"Submitted {batch_count} orders", icon=":rocket:")

    st.divider()
    if st.button("Reset Engine", use_container_width=True, type="secondary"):
        st.session_state.engine = exchange_core.TestExchange()
        st.session_state.responses = []
        st.session_state.market_updates = []
        st.session_state.order_count = 0
        st.session_state.next_order_id = 1
        st.session_state.resp_counts = {}
        st.session_state.upd_counts = {}
        st.rerun()

# ---------------------------------------------------------------------------
# Make sure queues are drained before rendering
# ---------------------------------------------------------------------------
_drain()

# ---------------------------------------------------------------------------
# Main content — tabs
# ---------------------------------------------------------------------------
tab_book, tab_responses, tab_market, tab_perf = st.tabs([
    "Order Book", "Execution Reports", "Market Data", "Performance"
])

# ── Tab 1: Order Book ──────────────────────────────────────────────────────
with tab_book:
    ticker_select = st.selectbox(
        "Ticker", list(range(8)), format_func=lambda x: f"Ticker {x}", key="book_ticker"
    )

    col_bid, col_mid, col_ask = st.columns([5, 2, 5])

    bids = engine.get_bid_levels(int(ticker_select))
    asks = engine.get_ask_levels(int(ticker_select))

    with col_mid:
        st.metric("Best Bid", engine.get_best_bid(int(ticker_select)) or "---")
        st.metric("Best Ask", engine.get_best_ask(int(ticker_select)) or "---")
        spread = engine.get_spread(int(ticker_select))
        st.metric("Spread", spread if spread is not None else "---")
        st.metric("Total Orders", st.session_state.order_count)

    with col_bid:
        st.subheader("Bids")
        if bids:
            bid_data = pd.DataFrame([
                {"Price": l.price, "Qty": l.total_qty, "Orders": l.order_count}
                for l in bids
            ])
            st.dataframe(bid_data, use_container_width=True, hide_index=True)

            # Depth chart
            bid_data_chart = bid_data.copy()
            bid_data_chart["Cumulative Qty"] = bid_data_chart["Qty"].cumsum()
            st.bar_chart(bid_data_chart, x="Price", y="Qty", color="#22c55e",
                         use_container_width=True)
        else:
            st.info("No bids")

    with col_ask:
        st.subheader("Asks")
        if asks:
            ask_data = pd.DataFrame([
                {"Price": l.price, "Qty": l.total_qty, "Orders": l.order_count}
                for l in asks
            ])
            st.dataframe(ask_data, use_container_width=True, hide_index=True)

            ask_data_chart = ask_data.copy()
            ask_data_chart["Cumulative Qty"] = ask_data_chart["Qty"].cumsum()
            st.bar_chart(ask_data_chart, x="Price", y="Qty", color="#ef4444",
                         use_container_width=True)
        else:
            st.info("No asks")

# ── Tab 2: Execution Reports ──────────────────────────────────────────────
with tab_responses:
    st.subheader("Client Responses (Execution Reports)")

    responses = st.session_state.responses
    if responses:
        # Show most recent first, cap at 1000 rows for rendering performance
        recent = responses[-1000:] if len(responses) > 1000 else responses
        df_resp = pd.DataFrame(reversed(recent))

        # Summary metrics (O(1) from pre-computed counters)
        rc = st.session_state.resp_counts
        c1, c2, c3, c4 = st.columns(4)
        c1.metric("Total", len(responses))
        c2.metric("ACCEPTED", rc.get("ACCEPTED", 0))
        c3.metric("FILLED", rc.get("FILLED", 0))
        c4.metric("CANCELED", rc.get("CANCELED", 0) + rc.get("CANCEL_REJECTED", 0))

        # Type filter
        types = sorted(df_resp["type"].unique())
        selected_types = st.multiselect("Filter by type", types, default=types, key="resp_filter")
        filtered = df_resp[df_resp["type"].isin(selected_types)]

        st.dataframe(filtered, use_container_width=True, hide_index=True, height=400)
    else:
        st.info("No execution reports yet. Submit some orders from the sidebar.")

# ── Tab 3: Market Data ────────────────────────────────────────────────────
with tab_market:
    st.subheader("Market Data Updates")

    updates = st.session_state.market_updates
    if updates:
        # Cap at 1000 rows for rendering performance
        recent_upds = updates[-1000:] if len(updates) > 1000 else updates
        df_upds = pd.DataFrame(reversed(recent_upds))

        uc = st.session_state.upd_counts
        c1, c2, c3, c4 = st.columns(4)
        c1.metric("Total", len(updates))
        c2.metric("ADD", uc.get("ADD", 0))
        c3.metric("TRADE", uc.get("TRADE", 0))
        c4.metric("CANCEL", uc.get("CANCEL", 0))

        types = sorted(df_upds["type"].unique())
        selected_types = st.multiselect("Filter by type", types, default=types, key="mkt_filter")
        filtered = df_upds[df_upds["type"].isin(selected_types)]

        st.dataframe(filtered, use_container_width=True, hide_index=True, height=400)

        # Trade chart (cap at 500 most recent for chart performance)
        trades = [u for u in recent_upds if u["type"] == "TRADE"]
        if trades:
            trades = trades[-500:] if len(trades) > 500 else trades
            st.subheader("Trade History")
            trade_df = pd.DataFrame(trades)
            trade_df["trade_num"] = range(1, len(trade_df) + 1)
            st.line_chart(trade_df, x="trade_num", y="price", use_container_width=True)
    else:
        st.info("No market data updates yet.")

# ── Tab 4: Performance ────────────────────────────────────────────────────
with tab_perf:
    st.subheader("C++ LatencyTracker Performance Metrics")
    st.caption("All latencies are measured purely in C++, not through the Python binding layer.")

    perf_ticker = st.selectbox(
        "Ticker", list(range(8)), format_func=lambda x: f"Ticker {x}", key="perf_ticker"
    )

    col_reset, _ = st.columns([1, 5])
    with col_reset:
        if st.button("Reset Stats", key="reset_perf"):
            engine.reset_perf_stats(int(perf_ticker))
            st.toast("Performance stats reset", icon=":broom:")
            st.rerun()

    stats = engine.get_perf_stats(int(perf_ticker))

    if stats["count"] == 0:
        st.info(
            "No performance data yet. Submit orders to start collecting latency samples. "
            "Use 'Batch Orders' in the sidebar for meaningful stats."
        )
    else:
        # Top-level KPIs
        k1, k2, k3, k4 = st.columns(4)
        k1.metric("Samples", f"{stats['count']:,}")
        k2.metric("Throughput", f"{stats['throughput_per_sec']:,.0f} ops/sec")
        k3.metric("Mean Latency", f"{stats['mean_ns']:,.0f} ns")
        k4.metric("P99 Latency", f"{stats['p99_ns']:,.0f} ns")

        st.divider()

        # Detailed percentile breakdown
        st.subheader("Latency Distribution")

        col_table, col_chart = st.columns([2, 3])

        with col_table:
            latency_data = {
                "Metric": ["Min", "P50 (Median)", "P99", "P99.9", "Max", "Mean"],
                "Latency (ns)": [
                    f"{stats['min_ns']:,.0f}",
                    f"{stats['p50_ns']:,.0f}",
                    f"{stats['p99_ns']:,.0f}",
                    f"{stats['p999_ns']:,.0f}",
                    f"{stats['max_ns']:,.0f}",
                    f"{stats['mean_ns']:,.0f}",
                ],
            }
            st.table(pd.DataFrame(latency_data))

        with col_chart:
            # Bar chart of percentiles
            pct_df = pd.DataFrame({
                "Percentile": ["Min", "P50", "P99", "P99.9", "Max"],
                "Latency (ns)": [
                    stats["min_ns"],
                    stats["p50_ns"],
                    stats["p99_ns"],
                    stats["p999_ns"],
                    stats["max_ns"],
                ],
            })
            st.bar_chart(pct_df, x="Percentile", y="Latency (ns)", use_container_width=True)

        st.divider()

        # Throughput section
        st.subheader("Throughput")
        tp_col1, tp_col2, tp_col3 = st.columns(3)
        tp_col1.metric("Orders Processed", f"{stats['count']:,}")
        tp_col2.metric("Ops/sec", f"{stats['throughput_per_sec']:,.0f}")
        tp_col3.metric(
            "Avg Time/Order",
            f"{stats['mean_ns']:,.0f} ns"
            + (f" ({stats['mean_ns'] / 1000:.1f} us)" if stats['mean_ns'] > 1000 else ""),
        )
