// trading/order_gw/order_gateway.h
//
// OrderGateway -- the client's TCP front door to the exchange's OrderServer.
//
// Responsibilities:
//   - Connect to the exchange over TCP (client side: tcp_socket_.connect).
//   - Send MEClientRequest messages wrapped in OMClientRequest via the SBE
//     two-send pattern (seq_num then payload).
//   - Receive OMClientResponse messages, validate per-client seq numbers,
//     and publish MEClientResponse into the client response queue for
//     downstream consumers (strategy / order manager).
//
// Low-latency design:
//   - Busy-spin loop over both queues + TCP socket.
//   - Pre-allocated 64MB socket buffers from TCPSocket (no heap).
//   - std::atomic<bool> for run flag -- never volatile (codebase convention).
//   - noexcept on all hot-path methods.
//
// Symmetry with the exchange side: see Exchange::OrderServer for the mirror.
//

#pragma once

#include <atomic>
#include <string>

#include "tcp_socket.h"
#include "logging.h"
#include "thread_utils.h"
#include "time_utils.h"
#include "macros.h"

#include "client_request.h"   // Exchange::MEClientRequest / OMClientRequest
#include "client_response.h"  // Exchange::MEClientResponse / OMClientResponse

namespace Trading {

class OrderGateway final {
public:
    OrderGateway(Common::ClientId                    client_id,
                 Exchange::ClientRequestLFQueue     *client_requests,
                 Exchange::ClientResponseLFQueue    *client_responses,
                 const std::string                  &ip,
                 const std::string                  &iface,
                 int                                 port,
                 const std::string                  &log_file = "trading_order_gateway.log");

    ~OrderGateway();

    auto start() -> void;
    auto stop()  -> void;

    // ── Accessors for tests ─────────────────────────────────────────────

    auto nextOutgoingSeqNum() const noexcept -> size_t { return next_outgoing_seq_num_; }
    auto nextExpectedIncomingSeqNum() const noexcept -> size_t { return next_exp_incoming_seq_num_; }

    /// Test hook: parse a raw byte stream as if it had arrived on the TCP
    /// socket. Used by unit tests to drive the recv path without real TCP.
    auto onBytesForTest(const char *data, size_t len) noexcept -> void;

    OrderGateway() = delete;
    OrderGateway(const OrderGateway &) = delete;
    OrderGateway(OrderGateway &&) = delete;
    OrderGateway &operator=(const OrderGateway &) = delete;
    OrderGateway &operator=(OrderGateway &&) = delete;

private:
    /// Busy-spin loop: TCP sendAndRecv + drain outgoing request queue.
    auto run() noexcept -> void;

    /// TCP rcv callback: consume whole OMClientResponse frames from socket buf.
    auto recvCallback(Common::TCPSocket *socket, Common::Nanos rx_time) noexcept -> void;

    /// Validate + publish a decoded response to the downstream queue.
    auto processResponse(const Exchange::OMClientResponse &msg) noexcept -> void;

    /// Send a decoded request to the exchange via TCP (SBE two-send).
    auto sendRequest(const Exchange::MEClientRequest &req) noexcept -> void;

    // ── Data members ────────────────────────────────────────────────────

    /// Outgoing: strategy → exchange (our producer, strategy is consumer elsewhere).
    Exchange::ClientRequestLFQueue  *outgoing_requests_  = nullptr;

    /// Incoming: exchange → strategy (we write to this queue).
    Exchange::ClientResponseLFQueue *incoming_responses_ = nullptr;

    Common::ClientId client_id_;

    // logger_ declared BEFORE tcp_socket_ (constructor dep).
    Common::Logger    logger_;
    Common::TCPSocket tcp_socket_;

    /// Next outgoing seq num we'll stamp onto an OMClientRequest (1-indexed).
    size_t next_outgoing_seq_num_ = 1;

    /// Next incoming seq num we expect on the OMClientResponse stream.
    size_t next_exp_incoming_seq_num_ = 1;

    std::atomic<bool> run_ = {false};
    std::string time_str_;
};

} // namespace Trading
