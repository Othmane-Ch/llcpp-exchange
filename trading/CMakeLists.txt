cmake_minimum_required(VERSION 3.20)

# ── trading interface library (headers only for dependants) ──────────────────
add_library(trading INTERFACE)
target_include_directories(trading INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/strategy
        ${CMAKE_CURRENT_SOURCE_DIR}/market_data
        ${CMAKE_CURRENT_SOURCE_DIR}/order_gw
)
target_link_libraries(trading INTERFACE llcore exchange)

# ── trading_client_tests executable ──────────────────────────────────────────
add_executable(trading_client_tests
        tests/market_order_book_test.cpp
        tests/market_data_consumer_test.cpp
        tests/order_gateway_test.cpp
        tests/position_keeper_test.cpp
        tests/risk_manager_test.cpp
        tests/order_manager_test.cpp
        tests/feature_engine_test.cpp
        tests/trade_engine_test.cpp
        strategy/market_order_book.cpp
        strategy/order_manager.cpp
        strategy/risk_manager.cpp
        strategy/trade_engine.cpp
        strategy/market_maker.cpp
        strategy/liquidity_taker.cpp
        market_data/market_data_consumer.cpp
        order_gw/order_gateway.cpp
)
target_include_directories(trading_client_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/strategy
        ${CMAKE_CURRENT_SOURCE_DIR}/market_data
        ${CMAKE_CURRENT_SOURCE_DIR}/order_gw
        ${CMAKE_SOURCE_DIR}/Exchange
        ${CMAKE_SOURCE_DIR}/Exchange/market_data
        ${CMAKE_SOURCE_DIR}/Exchange/order_server
        ${CMAKE_SOURCE_DIR}/Exchange/matcher
)
target_compile_options(trading_client_tests PRIVATE -O3 -march=native -Wall -Wextra)
target_link_libraries(trading_client_tests PRIVATE trading exchange llcore doctest::doctest)

add_test(NAME TradingClientTests COMMAND trading_client_tests)

# ── trading_main executable ──────────────────────────────────────────────────
add_executable(trading_main
        trading_main.cpp
        strategy/market_order_book.cpp
        strategy/order_manager.cpp
        strategy/risk_manager.cpp
        strategy/trade_engine.cpp
        strategy/market_maker.cpp
        strategy/liquidity_taker.cpp
        market_data/market_data_consumer.cpp
        order_gw/order_gateway.cpp
)
target_include_directories(trading_main PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/strategy
        ${CMAKE_CURRENT_SOURCE_DIR}/market_data
        ${CMAKE_CURRENT_SOURCE_DIR}/order_gw
        ${CMAKE_SOURCE_DIR}/Exchange
        ${CMAKE_SOURCE_DIR}/Exchange/market_data
        ${CMAKE_SOURCE_DIR}/Exchange/order_server
        ${CMAKE_SOURCE_DIR}/Exchange/matcher
)
target_compile_options(trading_main PRIVATE -O3 -march=native -Wall -Wextra)
target_link_libraries(trading_main PRIVATE trading exchange llcore)
