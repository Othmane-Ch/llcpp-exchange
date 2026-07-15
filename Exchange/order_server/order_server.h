//
// OrderServer — the exchange's front door for order flow.
//
// Architecture:
//   - Runs on its own thread, separate from the matching engine.
//   - Accepts TCP connections from market participants.
//   - Receives OMClientRequest messages, validates per-client sequence
//     numbers and socket identity, then buffers into the FIFO sequencer.
//   - After all sockets are drained (recvFinishedCallback), the sequencer
//     sorts by kernel rx timestamp and publishes to the matching engine
//     in true arrival order — fairness across connections.
//   - Reads MEClientResponse from the matching engine's outgoing queue,
//     wraps them in OMClientResponse (SBE two-send: seq_num then payload),
//     and sends them back to the appropriate client over TCP.
//
// Low-latency design:
//   - Busy-spin loop polling both TCP (epoll) and the response queue.
//   - Per-client arrays indexed by ClientId — O(1) lookup, no hashing.
//   - Pre-allocated buffers — no heap allocation on hot path.
//   - noexcept on all hot path functions.
//   - std::atomic<bool> for run flag — never volatile.
//   - FIFO sequencer ensures fairness without locks.
//

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <string>

#include "fifo_sequencer.h"
#include "client_request.h"
#include "client_response.h"
#include "tcp_server.h"
#include "thread_utils.h"
#include "logging.h"
#include "time_utils.h"
#include "macros.h"

namespace Exchange {

class OrderServer final {
public:
    OrderServer(ClientRequestLFQueue  *client_requests,
                ClientResponseLFQueue *client_responses,
                const std::string     &iface,
                int                    port,
                const std::string     &log_file = "exchange_order_server.log");

    ~OrderServer();

    auto start() -> void;
    auto stop()  -> void;

    OrderServer() = delete;
    OrderServer(const OrderServer &)             = delete;
    OrderServer(OrderServer &&)                  = delete;
    OrderServer &operator=(const OrderServer &)  = delete;
    OrderServer &operator=(OrderServer &&)       = delete;

private:
    /// Hot path: called by TCPServer when data arrives on a client socket.
    auto recvCallback(Common::TCPSocket *socket, Common::Nanos rx_time) noexcept -> void;

    /// Hot path: called after all pending TCP receives are dispatched.
    /// Triggers FIFO sequencer to sort-and-publish.
    auto recvFinishedCallback() noexcept -> void;

    /// The main busy-spin loop (runs on its own thread).
    auto run() noexcept -> void;

    // ── Data members ─────────────────────────────────────────────────

    ClientRequestLFQueue  *outgoing_requests_  = nullptr;   // gateway → matching engine
    ClientResponseLFQueue *incoming_responses_ = nullptr;   // matching engine → gateway

    // logger_ must be declared BEFORE tcp_server_ because TCPServer takes
    // a Logger& in its constructor, so logger_ must be fully constructed first.
    Common::Logger logger_;
    Common::TCPServer tcp_server_;

    FIFOSequencer fifo_sequencer_;

    /// Per-client outgoing sequence numbers (1-indexed, 0 = invalid).
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_outgoing_seq_num_{};

    /// Per-client expected incoming sequence numbers.
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_exp_seq_num_{};

    /// Per-client TCP socket mapping for response routing + identity checks.
    std::array<Common::TCPSocket *, ME_MAX_NUM_CLIENTS> cid_tcp_socket_{};

    std::atomic<bool> run_ = {false};
    std::string time_str_;
};

} // namespace Exchange
