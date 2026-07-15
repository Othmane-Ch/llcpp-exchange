// fifo_sequencer_test.cpp — unit tests for FIFOSequencer fairness ordering.

#include "doctest/doctest.h"

#include <algorithm>
#include <vector>

#include "order_server/fifo_sequencer.h"
#include "time_utils.h"

using namespace Exchange;
using namespace Common;

TEST_CASE("FIFOSequencer orders requests by rx_time, not insertion order") {
    ClientRequestLFQueue queue(1024);
    FIFOSequencer seq(&queue);

    // Client 2's request arrived first (earlier timestamp) but is added second.
    MEClientRequest req_c1{};
    req_c1.client_id_ = ClientId{1};
    req_c1.order_id_ = OrderId{100};
    req_c1.type_ = ClientRequestType::NEW;

    MEClientRequest req_c2{};
    req_c2.client_id_ = ClientId{2};
    req_c2.order_id_ = OrderId{200};
    req_c2.type_ = ClientRequestType::NEW;

    // Add in reverse timestamp order: c1 at t=2000, c2 at t=1000.
    seq.addClientRequest(2000, req_c1);
    seq.addClientRequest(1000, req_c2);

    seq.sequenceAndPublish();

    // Queue should contain c2 first (earlier rx_time), then c1.
    const auto *first = queue.getNextToRead();
    REQUIRE(first != nullptr);
    CHECK(first->client_id_.value == 2);
    CHECK(first->order_id_.value == 200);
    queue.updateReadIndex();

    const auto *second = queue.getNextToRead();
    REQUIRE(second != nullptr);
    CHECK(second->client_id_.value == 1);
    CHECK(second->order_id_.value == 100);
    queue.updateReadIndex();

    CHECK(queue.getNextToRead() == nullptr);
}

TEST_CASE("FIFOSequencer handles empty pending set gracefully") {
    ClientRequestLFQueue queue(1024);
    FIFOSequencer seq(&queue);

    seq.sequenceAndPublish();
    CHECK(queue.getNextToRead() == nullptr);
}

TEST_CASE("FIFOSequencer preserves all requests with identical timestamps") {
    ClientRequestLFQueue queue(1024);
    FIFOSequencer seq(&queue);

    for (uint64_t i = 0; i < 3; ++i) {
        MEClientRequest req{};
        req.client_id_ = ClientId{static_cast<uint32_t>(i)};
        req.order_id_ = OrderId{i};
        req.type_ = ClientRequestType::NEW;
        seq.addClientRequest(5000, req);
    }

    seq.sequenceAndPublish();

    std::vector<uint32_t> client_ids;
    for (int i = 0; i < 3; ++i) {
        const auto *r = queue.getNextToRead();
        REQUIRE(r != nullptr);
        client_ids.push_back(r->client_id_.value);
        queue.updateReadIndex();
    }
    CHECK(queue.getNextToRead() == nullptr);

    std::sort(client_ids.begin(), client_ids.end());
    CHECK(client_ids == std::vector<uint32_t>{0, 1, 2});
}

TEST_CASE("FIFOSequencer resets pending buffer after sequenceAndPublish") {
    ClientRequestLFQueue queue(1024);
    FIFOSequencer seq(&queue);

    MEClientRequest req{};
    req.client_id_ = ClientId{1};
    req.type_ = ClientRequestType::NEW;
    seq.addClientRequest(1000, req);
    seq.sequenceAndPublish();

    queue.getNextToRead();
    queue.updateReadIndex();

    // Second cycle — should only contain the new request.
    MEClientRequest req2{};
    req2.client_id_ = ClientId{2};
    req2.type_ = ClientRequestType::CANCEL;
    seq.addClientRequest(2000, req2);
    seq.sequenceAndPublish();

    const auto *r = queue.getNextToRead();
    REQUIRE(r != nullptr);
    CHECK(r->client_id_.value == 2);
    CHECK(r->type_ == ClientRequestType::CANCEL);
    queue.updateReadIndex();
    CHECK(queue.getNextToRead() == nullptr);
}

TEST_CASE("RecvTimeClientRequest sorts correctly by recv_time") {
    std::array<RecvTimeClientRequest, 4> entries{};

    entries[0].recv_time_ = 5000;
    entries[0].request_.client_id_ = ClientId{0};
    entries[1].recv_time_ = 1000;
    entries[1].request_.client_id_ = ClientId{1};
    entries[2].recv_time_ = 3000;
    entries[2].request_.client_id_ = ClientId{2};
    entries[3].recv_time_ = 2000;
    entries[3].request_.client_id_ = ClientId{3};

    std::sort(entries.begin(), entries.end());

    CHECK(entries[0].recv_time_ == 1000);
    CHECK(entries[0].request_.client_id_.value == 1);
    CHECK(entries[1].recv_time_ == 2000);
    CHECK(entries[1].request_.client_id_.value == 3);
    CHECK(entries[2].recv_time_ == 3000);
    CHECK(entries[2].request_.client_id_.value == 2);
    CHECK(entries[3].recv_time_ == 5000);
    CHECK(entries[3].request_.client_id_.value == 0);
}
