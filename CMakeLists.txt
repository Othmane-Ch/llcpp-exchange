cmake_minimum_required(VERSION 3.20)
project(llcpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_LTO "Enable link-time optimization" ON)

add_library(llcore
        src/threading.cpp
        src/mempool.cpp
        src/lf_queue.cpp
        src/logger.cpp
        src/socket_utils.cpp
        src/tcp_socket.cpp
        src/tcp_server.cpp
)
target_include_directories(llcore PUBLIC include)

target_compile_options(llcore PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_compile_options(llcore PRIVATE -O3 -march=native)
target_compile_options(llcore PRIVATE $<$<CONFIG:Release>:-DNDEBUG>)
target_link_options(llcore PRIVATE $<$<BOOL:${ENABLE_LTO}>:-flto>)

add_executable(demo
        src/main.cpp
)
target_link_libraries(demo PRIVATE llcore)

add_subdirectory(Exchange)
add_subdirectory(trading)

# ── Optional: pybind11 Python bindings ──────────────────────────────────────
option(BUILD_PYTHON_BINDINGS "Build Python pybind11 bindings" OFF)
option(ENABLE_PERF_TRACKING  "Enable hot-path latency instrumentation" OFF)
option(LLCPP_SMALL_FOOTPRINT "Right-size capacity constants for small VMs (<1GB RSS)" OFF)

if(ENABLE_PERF_TRACKING)
    target_compile_definitions(llcore PUBLIC ENABLE_PERF_TRACKING)
endif()

if(LLCPP_SMALL_FOOTPRINT)
    # PUBLIC: propagates to every target that links llcore (exchange, trading,
    # tests, bindings) so all translation units agree on the constants.
    target_compile_definitions(llcore PUBLIC LLCPP_SMALL_FOOTPRINT)
endif()

if(BUILD_PYTHON_BINDINGS)
    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.13.6
    )
    FetchContent_MakeAvailable(pybind11)

    add_subdirectory(bindings/python)
endif()

# ── Benchmarks (Chapter 12 -- report evidence) ──────────────────────────────
# All three benchmarks are built with -O3 -march=native -DNDEBUG -Wall
# -Wextra -Wpedantic. release_benchmark in particular MUST have NDEBUG;
# the ASSERT path costs we are measuring evaporate without it.

add_executable(logger_benchmark
        benchmarks/logger_benchmark.cpp
)
target_link_libraries(logger_benchmark PRIVATE llcore)
target_compile_options(logger_benchmark PRIVATE -O3 -march=native -DNDEBUG -Wall -Wextra -Wpedantic)

add_executable(release_benchmark
        benchmarks/release_benchmark.cpp
)
target_include_directories(release_benchmark PRIVATE
        Exchange Exchange/market_data Exchange/order_server Exchange/matcher)
target_link_libraries(release_benchmark PRIVATE llcore)
target_compile_options(release_benchmark PRIVATE -O3 -march=native -DNDEBUG -Wall -Wextra -Wpedantic)

add_executable(hash_benchmark
        benchmarks/hash_benchmark.cpp
        Exchange/matcher/me_order_book.cpp
        Exchange/matcher/matching_engine.cpp
        Exchange/matcher/unordered_map_me_order_book.cpp
)
target_include_directories(hash_benchmark PRIVATE
        Exchange Exchange/market_data Exchange/order_server Exchange/matcher)
target_link_libraries(hash_benchmark PRIVATE llcore)
target_compile_options(hash_benchmark PRIVATE -O3 -march=native -DNDEBUG -Wall -Wextra -Wpedantic)
