//
// FIFOSequencer — ensures fairness across TCP connections.
//
// Problem: epoll returns ready sockets in arbitrary order. Without a
// sequencer, the first polled socket's requests would always reach the
// matching engine first, even if another client's packet arrived earlier
// on the wire.
//
// Solution: buffer all requests received in a single poll cycle, sort by
// kernel-provided receive timestamp, then publish to the matching engine
// queue in true arrival order.
//
// Low-latency design:
//   - Fixed-size std::array — no heap allocation on hot path.
//   - FATAL() on overflow — crash is better than silent request loss.
//   - Inline header-only — no function call overhead.
//   - noexcept on all methods.
//

#pragma once

#include <algorithm>
#include <array>
#include <string>

#include "client_request.h"
#include "time_utils.h"
#include "logging.h"
#include "perf_utils.h"
#include "macros.h"

namespace Exchange {

/// Maximum pending requests buffered per poll cycle.
/// 1024 is generous — a single poll rarely yields more than a few dozen.
constexpr size_t ME_MAX_PENDING_REQUESTS = 1024;

/// Pairs a client request with its kernel receive timestamp for sorting.
struct RecvTimeClientRequest {
    Common::Nanos recv_time_ = 0;
    MEClientRequest request_{};

    /// Tie-break for identical timestamps. All requests parsed out of one
    /// recvmsg() share a single kernel rx_time; std::sort is NOT stable, so
    /// without this a client's NEW and CANCEL read in one TCP burst could be
    /// reordered against each other.
    size_t arrival_index_ = 0;

    /// Sort by arrival time — earliest first (FIFO fairness),
    /// then by buffering order within the poll cycle.
    auto operator<(const RecvTimeClientRequest &rhs) const noexcept -> bool {
        return (recv_time_ < rhs.recv_time_) ||
               (recv_time_ == rhs.recv_time_ && arrival_index_ < rhs.arrival_index_);
    }
};

class FIFOSequencer final {
public:
    explicit FIFOSequencer(ClientRequestLFQueue *incoming_requests,
                           Common::Logger *logger = nullptr) noexcept
        : incoming_requests_(incoming_requests), logger_(logger) {}

    /// Buffer a request received at rx_time. Called from recvCallback().
    auto addClientRequest(Common::Nanos rx_time,
                          const MEClientRequest &request) noexcept -> void {
        if (UNLIKELY(pending_size_ >= pending_client_requests_.size())) {
            FATAL("FIFOSequencer overflow — pending_size_:" +
                  std::to_string(pending_size_));
        }
        pending_client_requests_[pending_size_] = {rx_time, request, pending_size_};
        ++pending_size_;
    }

    /// Sort buffered requests by arrival time and publish to the matching
    /// engine queue. Called from recvFinishedCallback() — once per poll cycle.
    auto sequenceAndPublish() noexcept -> void {
        if (UNLIKELY(pending_size_ == 0))
            return;

        std::sort(pending_client_requests_.begin(),
                  pending_client_requests_.begin() + pending_size_);

        for (size_t i = 0; i < pending_size_; ++i) {
            const auto &entry = pending_client_requests_[i];
            auto *slot = incoming_requests_->getNextToWriteTo();
            *slot = entry.request_;
            incoming_requests_->updateWriteIndex();
            if (LIKELY(logger_)) {
                TTT_MEASURE(T2_OrderServer_LFQueue_write, (*logger_));
            }
        }

        pending_size_ = 0;
    }

    FIFOSequencer() = delete;
    FIFOSequencer(const FIFOSequencer &) = delete;
    FIFOSequencer(FIFOSequencer &&) = delete;
    FIFOSequencer &operator=(const FIFOSequencer &) = delete;
    FIFOSequencer &operator=(FIFOSequencer &&) = delete;

private:
    ClientRequestLFQueue *incoming_requests_ = nullptr;
    std::array<RecvTimeClientRequest, ME_MAX_PENDING_REQUESTS> pending_client_requests_{};
    size_t pending_size_ = 0;

    // Optional logger* — null in tests that drive the sequencer directly.
    // Used by START_MEASURE/END_MEASURE/TTT_MEASURE macros.
    Common::Logger *logger_ = nullptr;
    std::string time_str_;
};

} // namespace Exchange
