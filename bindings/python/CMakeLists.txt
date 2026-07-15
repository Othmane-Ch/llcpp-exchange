# ── exchange_core Python module ──────────────────────────────────────────────

pybind11_add_module(exchange_core
    exchange_core.cpp
    ${CMAKE_SOURCE_DIR}/Exchange/matcher/matching_engine.cpp
    ${CMAKE_SOURCE_DIR}/Exchange/matcher/me_order_book.cpp
)

target_include_directories(exchange_core PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/Exchange
    ${CMAKE_SOURCE_DIR}/Exchange/matcher
    ${CMAKE_SOURCE_DIR}/Exchange/order_server
    ${CMAKE_SOURCE_DIR}/Exchange/market_data
)

target_link_libraries(exchange_core PRIVATE llcore)
target_compile_options(exchange_core PRIVATE -O3 -march=native -Wall -Wextra)

# Always enable perf tracking in the Python bindings module so tests can read stats.
target_compile_definitions(exchange_core PRIVATE ENABLE_PERF_TRACKING)

# ── trading_core Python module ───────────────────────────────────────────────
# Exposes Trading::MarketOrderBook so Python can exercise client-side book
# reconstruction from synthetic MEMarketUpdate inputs.

pybind11_add_module(trading_core
    trading_core.cpp
    ${CMAKE_SOURCE_DIR}/trading/strategy/market_order_book.cpp
)

target_include_directories(trading_core PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/Exchange
    ${CMAKE_SOURCE_DIR}/Exchange/matcher
    ${CMAKE_SOURCE_DIR}/Exchange/order_server
    ${CMAKE_SOURCE_DIR}/Exchange/market_data
    ${CMAKE_SOURCE_DIR}/trading
    ${CMAKE_SOURCE_DIR}/trading/strategy
)

target_link_libraries(trading_core PRIVATE llcore)
target_compile_options(trading_core PRIVATE -O3 -march=native -Wall -Wextra)
