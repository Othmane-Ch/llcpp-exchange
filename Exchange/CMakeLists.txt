cmake_minimum_required(VERSION 3.20)

include(FetchContent)

# ── doctest (single-header test framework) ────────────────────────────────────
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)

# ── Exchange interface library ────────────────────────────────────────────────
add_library(exchange INTERFACE)
target_include_directories(exchange INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(exchange INTERFACE llcore)

# ── exchange_main executable ──────────────────────────────────────────────────
add_executable(exchange_main
        exchange_main.cpp
        matcher/matching_engine.cpp
        matcher/me_order_book.cpp
        order_server/order_server.cpp
        market_data/market_data_publisher.cpp
        market_data/snapshot_synthesizer.cpp
)
target_include_directories(exchange_main PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/order_server
        ${CMAKE_CURRENT_SOURCE_DIR}/market_data
        ${CMAKE_CURRENT_SOURCE_DIR}/matcher
)
target_compile_options(exchange_main PRIVATE -O3 -march=native -Wall -Wextra)
target_link_libraries(exchange_main PRIVATE exchange llcore)

# ── me_order_book_tests executable ───────────────────────────────────────────
add_executable(me_order_book_tests
        matcher/me_order_book_test.cpp
        matcher/matching_engine.cpp
        matcher/me_order_book.cpp
)
target_include_directories(me_order_book_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/order_server
        ${CMAKE_CURRENT_SOURCE_DIR}/market_data
        ${CMAKE_CURRENT_SOURCE_DIR}/matcher
)
target_compile_options(me_order_book_tests PRIVATE -O3 -march=native -Wall -Wextra)
target_link_libraries(me_order_book_tests PRIVATE exchange llcore doctest::doctest)

# ── communication_layer_tests executable ─────────────────────────────────────
add_executable(communication_layer_tests
        tests/fifo_sequencer_test.cpp
        tests/snapshot_synthesizer_test.cpp
        tests/protocol_structs_test.cpp
        tests/market_data_publisher_test.cpp
        tests/exchange_integration_test.cpp
        matcher/matching_engine.cpp
        matcher/me_order_book.cpp
        order_server/order_server.cpp
        market_data/market_data_publisher.cpp
        market_data/snapshot_synthesizer.cpp
)
target_include_directories(communication_layer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/order_server
        ${CMAKE_CURRENT_SOURCE_DIR}/market_data
        ${CMAKE_CURRENT_SOURCE_DIR}/matcher
)
target_compile_options(communication_layer_tests PRIVATE -O3 -march=native -Wall -Wextra)
target_link_libraries(communication_layer_tests PRIVATE exchange llcore doctest::doctest)

enable_testing()
add_test(NAME OrderBookTests COMMAND me_order_book_tests)
add_test(NAME CommunicationLayerTests COMMAND communication_layer_tests)
