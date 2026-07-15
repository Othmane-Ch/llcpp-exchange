// protocol_structs_test.cpp — compile-time and runtime validation that
// #pragma pack(push,1) produces the expected struct sizes. Any size
// change means the wire format has broken.

#include "doctest/doctest.h"

#include "market_data/market_update.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"

using namespace Exchange;

TEST_CASE("MEMarketUpdate is exactly 34 bytes (no padding)") {
    CHECK(sizeof(MEMarketUpdate) == 34);
}

TEST_CASE("MDPMarketUpdate is sizeof(size_t) + 34") {
    CHECK(sizeof(MDPMarketUpdate) == sizeof(size_t) + 34);
}

TEST_CASE("MEClientRequest has no unexpected padding") {
    // type(1) + client_id(4) + ticker_id(4) + order_id(8) + side(1) + price(8) + qty(4) = 30
    CHECK(sizeof(MEClientRequest) == 30);
}

TEST_CASE("OMClientRequest is sizeof(size_t) + sizeof(MEClientRequest)") {
    CHECK(sizeof(OMClientRequest) == sizeof(size_t) + sizeof(MEClientRequest));
}

TEST_CASE("MEClientResponse has no unexpected padding") {
    // type(1) + client_id(4) + ticker_id(4) + client_oid(8) + market_oid(8) +
    // side(1) + price(8) + exec_qty(4) + leaves_qty(4) = 42
    CHECK(sizeof(MEClientResponse) == 42);
}

TEST_CASE("OMClientResponse is sizeof(size_t) + sizeof(MEClientResponse)") {
    CHECK(sizeof(OMClientResponse) == sizeof(size_t) + sizeof(MEClientResponse));
}
