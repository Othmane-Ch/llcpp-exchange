# llcpp — Low Latency C++ Trading Exchange

A real-time electronic trading exchange with a matching engine, order gateway, and market
data publisher. Built in C++23 with a focus on deterministic, low-latency execution.

## Project Status

| Component | Status |
|-----------|--------|
| Matching Engine (price-time priority order book) | Done |
| Order Gateway Server (receives client orders) | Done |
| Market Data Publisher (multicast updates) | Done |
| Python test bindings (pybind11) | Done |
| C++ performance instrumentation (LatencyTracker) | Done |
| RDTSC + TTT hot-path instrumentation (`perf_utils.h`) | Done |
| Streamlit metrics dashboard | Done |
| BDD tests (behave / Gherkin .feature files) | Done |
| Client-side market data consumer (UDP mcast + gap recovery) | Done |
| Client-side market order book (reconstructed from incrementals) | Done |
| Client-side order gateway (TCP client with SBE two-send) | Done |
| FeatureEngine (qty-weighted mid + aggressive trade ratio) | Done |
| PositionKeeper (VWAP-based PnL accounting) | Done |
| OrderManager (per-side state machine + risk-checked order entry) | Done |
| RiskManager (3-gate pre-trade checks: size / position / loss) | Done |
| TradeEngine (LFQueue drain loop + strategy callback hooks) | Done |
| MarketMaker strategy (passive quoting around fair value) | Done |
| LiquidityTaker strategy (aggressive follow-the-aggressor) | Done |
| trading_main (client executable) | Done |

## Prerequisites

- **Linux** (tested on Ubuntu 24.04)
- **GCC 13+** or **Clang 17+** with C++23 support
- **CMake 3.20+**
- **Python 3.11+** (for tests and dashboard)
- **Git** (for FetchContent dependency downloads)

### System packages (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    python3-dev \
    python3-pytest \
    python3-pip
```

## Building

### Core exchange only

```bash
cmake -B build
cmake --build build -j$(nproc)
```

This produces:
- `build/libllcore.a` — core infrastructure library
- `build/Exchange/exchange_main` — exchange executable
- `build/Exchange/me_order_book_tests` — matching engine C++ unit tests
- `build/Exchange/communication_layer_tests` — communication layer C++ unit tests
- `build/trading/trading_client_tests` — client-side (market data consumer, market order book, order gateway) C++ unit tests
- `build/trading/trading_main` — trading client executable (MAKER / TAKER / RANDOM modes)

### With Python bindings and performance tracking

```bash
cmake -B build \
    -DBUILD_PYTHON_BINDINGS=ON \
    -DENABLE_PERF_TRACKING=ON

cmake --build build -j$(nproc)
```

This additionally produces:
- `build/bindings/python/exchange_core.cpython-*.so` — Python extension module (exchange)
- `build/bindings/python/trading_core.cpython-*.so` — Python extension module (client-side MarketOrderBook)

### CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_PYTHON_BINDINGS` | `OFF` | Build the pybind11 `exchange_core` module |
| `ENABLE_PERF_TRACKING` | `OFF` | Enable `LatencyTracker` instrumentation on the hot path |
| `ENABLE_LTO` | `ON` | Enable link-time optimization |

## Quick Start (scripts)

The `scripts/` directory provides convenience scripts that handle building, testing,
and launching from any working directory:

```bash
# Build everything (Python bindings + perf tracking)
./scripts/build.sh --python --perf

# Run all tests (C++ and Python)
./scripts/run_tests.sh

# Launch the Streamlit dashboard
./scripts/run_dashboard.sh
```

See `./scripts/build.sh --help` for all build options.

## Running (manual)

### Exchange executable

```bash
./build/Exchange/exchange_main
# Ctrl+C to stop (graceful shutdown via SIGINT handler)
```

### Trading client executable

```bash
# CLIENT_ID ALGO_TYPE [CLIP THRESH MAX_ORDER_SIZE MAX_POS MAX_LOSS] ...
# One 5-tuple per ticker (up to ME_MAX_TICKERS).

# Market maker (passive quoting) for one ticker:
./build/trading/trading_main 1 MAKER 10 1.0 100 500 -1000

# Liquidity taker (trade-driven aggression):
./build/trading/trading_main 2 TAKER 10 0.5 100 500 -1000

# Random-order smoke test:
./build/trading/trading_main 3 RANDOM 10 0 100 500 -1000
# Ctrl+C to stop (graceful shutdown via SIGINT handler).
```

### C++ tests

```bash
# Matching engine / order book tests
./build/Exchange/me_order_book_tests

# Communication layer tests (FIFO sequencer, snapshot synthesizer, protocol structs, integration)
./build/Exchange/communication_layer_tests

# Client-side trading tests (MarketOrderBook, MarketDataConsumer, OrderGateway)
./build/trading/trading_client_tests
```

Expected output:
```
[doctest] test cases:  6 |  6 passed | 0 failed | 0 skipped  (me_order_book_tests)
[doctest] test cases: 16 | 16 passed | 0 failed | 0 skipped  (communication_layer_tests)
[doctest] test cases: 58 | 58 passed | 0 failed | 0 skipped  (trading_client_tests)
```

### Python tests

```bash
python3 -m pytest tests/ -v
```

This runs 39 tests across 6 test files:

| File | Tests | Covers |
|------|-------|--------|
| `test_order_book.py` | 10 | Book construction, levels, FIFO, empty book |
| `test_matching.py` | 6 | Full match, partial fill, multi-fill, sweep, FIFO priority |
| `test_cancellation.py` | 5 | Cancel resting, cancel filled, cancel partial, cancel non-existent |
| `test_market_data.py` | 8 | ADD/TRADE/MODIFY/CANCEL updates, sequencing |
| `test_edge_cases.py` | 7 | Zero qty, aggressive limit, multi-ticker, price improvement |
| `test_performance.py` | 4 | Latency benchmarks, throughput, stats reset |

### BDD tests (behave)

```bash
# Install behave (one-time)
pip install behave

# Run all BDD scenarios
behave

# Run a single feature
behave features/order_book.feature
```

This runs 28 scenarios across 5 feature files:

| Feature file | Scenarios | Covers |
|---|---|---|
| `order_book.feature` | 6 | Single orders, both sides, aggregation, price sorting, empty book |
| `matching.feature` | 6 | Full match, partial fill, multi-order sweep, multi-level sweep, FIFO priority |
| `cancellation.feature` | 5 | Cancel resting, cancel one of many, cancel filled (rejected), cancel partial, cancel non-existent |
| `market_data.feature` | 4 | ADD on new order, TRADE on match, CANCEL on cancel, multi-level trade updates |
| `trading_book.feature` | 7 | Client-side book: ADD / MODIFY / CANCEL / CLEAR / TRADE, level ordering, BBO invariants |

### Streamlit dashboard

```bash
# Install streamlit (one-time)
pip install streamlit

# Run the dashboard
streamlit run dashboard/app.py
```

Opens a browser dashboard at `http://localhost:8501` with:
- Interactive order submission
- Real-time order book visualization (bid/ask depth)
- Execution report log
- Market data feed
- Performance metrics (latency histograms, throughput)

## Project Structure

```
llcpp/
├── README.md                          # This file
├── CMakeLists.txt                     # Root build configuration
│
├── include/                           # Core infrastructure headers (Common namespace)
│   ├── types.h                        #   Strong types, Side enum, constants
│   ├── lq_free.h                      #   Lock-free SPSC queue
│   ├── mem_pool.h                     #   Fixed-size object pool
│   ├── logging.h                      #   Async logger
│   ├── latency_tracker.h             #   Hot-path performance instrumentation
│   ├── macros.h                       #   LIKELY/UNLIKELY, ASSERT, FATAL
│   ├── time_utils.h                   #   Nanosecond timestamps
│   ├── thread_utils.h                #   Thread creation with CPU affinity
│   ├── socket_utils.h                #   Socket helpers (TCP + UDP + multicast)
│   ├── tcp_server.h                  #   TCP server (epoll-based)
│   ├── tcp_socket.h                  #   TCP socket wrapper
│   └── mcast_socket.h                #   UDP multicast socket wrapper
│
├── src/                               # Core infrastructure implementations
│   ├── main.cpp                       #   Demo executable
│   ├── logger.cpp, mempool.cpp, ...   #   .cpp files for headers above
│
├── Exchange/                          # Exchange-side logic (Exchange namespace)
│   ├── CMakeLists.txt                 #   Exchange build config
│   ├── exchange_main.cpp              #   Exchange entry point
│   ├── matcher/
│   │   ├── matching_engine.h/.cpp     #   MatchingEngine — event loop + dispatch
│   │   ├── me_order_book.h/.cpp       #   MEOrderBook — price-time priority book
│   │   ├── me_order.h                 #   MEOrder, MEOrdersAtPrice structs
│   │   └── me_order_book_test.cpp     #   C++ unit tests (doctest)
│   ├── order_server/
│   │   ├── order_server.h/.cpp        #   OrderServer — TCP front door + FIFO fairness
│   │   ├── fifo_sequencer.h           #   FIFOSequencer — sorts requests by rx_time across connections
│   │   ├── client_request.h           #   MEClientRequest (internal) + OMClientRequest (wire)
│   │   └── client_response.h          #   MEClientResponse (internal) + OMClientResponse (wire)
│   ├── market_data/
│   │   ├── market_data_publisher.h/.cpp #  MarketDataPublisher — UDP multicast incremental stream
│   │   ├── snapshot_synthesizer.h/.cpp #  SnapshotSynthesizer — reconstructs book, publishes snapshots
│   │   └── market_update.h            #   MEMarketUpdate (34 bytes, internal) + MDPMarketUpdate (wire)
│   └── tests/
│       ├── fifo_sequencer_test.cpp    #   FIFO sequencer ordering tests
│       ├── snapshot_synthesizer_test.cpp # Snapshot book reconstruction tests
│       ├── protocol_structs_test.cpp  #   Wire format size validation
│       └── exchange_integration_test.cpp # End-to-end matching engine smoke tests
│
├── trading/                           # Client-side market participant (Trading namespace)
│   ├── CMakeLists.txt                 #   Trading build config
│   ├── strategy/
│   │   ├── market_order_book.h/.cpp   #   MarketOrderBook — client-side book reconstructed from MEMarketUpdate
│   │   ├── market_order.h             #   MarketOrder, MarketOrdersAtPrice structs
│   │   ├── feature_engine.h           #   FeatureEngine — qty-weighted mid + aggressive trade qty ratio
│   │   ├── position_keeper.h          #   PositionInfo / PositionKeeper — VWAP-based PnL accounting
│   │   ├── om_order.h                 #   OMOrder, OMOrderState, ticker×side hash map
│   │   ├── order_manager.h/.cpp       #   OrderManager — per-side state machine, risk-gated order entry
│   │   ├── risk_manager.h/.cpp        #   RiskManager — pre-trade size/position/loss checks
│   │   └── trade_engine.h/.cpp        #   TradeEngine — LFQueue drain loop, owns sub-components, strategy callback hooks
│   ├── market_data/
│   │   └── market_data_consumer.h/.cpp #  MarketDataConsumer — UDP mcast subscriber, gap detect + snapshot recovery
│   ├── order_gw/
│   │   └── order_gateway.h/.cpp       #   OrderGateway — TCP client, SBE two-send, per-client seq_num tracking
│   └── tests/
│       ├── market_order_book_test.cpp #   Client-side book reconstruction tests
│       ├── market_data_consumer_test.cpp # Gap detect + snapshot recovery tests
│       └── order_gateway_test.cpp     #   Inbound/outbound seq_num + drop tests
│
├── bindings/python/                   # pybind11 Python bindings
│   ├── CMakeLists.txt
│   ├── exchange_core.cpp              #   TestExchange wrapper + module definition
│   └── trading_core.cpp               #   TestMarketOrderBook wrapper (client-side book)
│
├── tests/                             # Python test suite (pytest)
│   ├── conftest.py                    #   Shared fixtures
│   ├── test_order_book.py
│   ├── test_matching.py
│   ├── test_cancellation.py
│   ├── test_market_data.py
│   ├── test_edge_cases.py
│   └── test_performance.py
│
├── features/                          # BDD tests (behave / Gherkin)
│   ├── environment.py                 #   Behave hooks (fresh engine per scenario)
│   ├── order_book.feature             #   Order book scenarios
│   ├── matching.feature               #   Matching / trade scenarios
│   ├── cancellation.feature           #   Cancel scenarios
│   ├── market_data.feature            #   Market data update scenarios
│   ├── trading_book.feature           #   Client-side MarketOrderBook scenarios
│   └── steps/
│       ├── exchange_steps.py          #   Exchange step definitions (Given/When/Then)
│       └── trading_steps.py           #   Client-side MarketOrderBook step definitions
│
├── dashboard/                         # Streamlit metrics dashboard
│   └── app.py
│
└── scripts/                           # Convenience run scripts
    ├── build.sh                       #   Build with optional flags
    ├── run_tests.sh                   #   Run all C++ and Python tests
    └── run_dashboard.sh               #   Launch Streamlit dashboard
```

## Design Principles

- **Zero allocation on the hot path** — orders and price levels use pre-allocated pools; socket buffers are pre-allocated at startup
- **No virtual functions** — all hot-path types are `final`, dispatch via `switch`
- **Lock-free IPC** — four SPSC queues connect four threads; no mutex anywhere
- **Busy-spin everywhere** — matching engine, order server, market data publisher, and snapshot synthesizer all busy-spin; no `sleep()`, no `condition_variable`
- **FIFO fairness** — the `FIFOSequencer` sorts incoming requests by kernel receive timestamp across all TCP connections, ensuring arrival-time ordering regardless of epoll polling order
- **Two-tier wire format** — internal `ME*` structs cross LFQueues with no serialization; public `OM*`/`MDP*` structs add `seq_num_` and are memcpy'd directly onto the wire (`#pragma pack(push,1)`, SBE two-send pattern)
- **UDP multicast for market data, TCP for orders** — market data is one-to-many (multicast), orders require reliability (TCP); gap recovery via separate snapshot stream published by the `SnapshotSynthesizer`
- **Per-client sequence tracking** — `OrderServer` maintains independent sequence counters per client for both incoming and outgoing messages, with socket identity validation
- **Strong types** — `OrderId`, `Price`, `Qty`, etc. prevent accidental type mixing at zero runtime cost
- **Compile-time instrumentation** — `LatencyTracker` compiles to no-ops when disabled
- **RDTSC + TTT macros** — `START_MEASURE`/`END_MEASURE` log cycle counts around named hot-path methods; `TTT_MEASURE` stamps tick-to-trade markers (T1..T12 / T1t..T12t) at LFQueue/network boundaries (see `include/perf_utils.h`). Time format is `HH:MM:SS.nnnnnnnnn`
- **Separation of measurement** — Python tests validate correctness; C++ measures performance internally

## License

All rights reserved. Shared for review purposes; not licensed for redistribution or reuse.
