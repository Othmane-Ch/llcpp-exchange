// trading/order_gw/order_gateway.cpp

#include "order_gateway.h"

#include <cstring>

namespace Trading {

using namespace Common;
using Exchange::MEClientRequest;
using Exchange::MEClientResponse;
using Exchange::OMClientRequest;
using Exchange::OMClientResponse;

// ── Construction ─────────────────────────────────────────────────────────────

OrderGateway::OrderGateway(ClientId                            client_id,
                           Exchange::ClientRequestLFQueue     *client_requests,
                           Exchange::ClientResponseLFQueue    *client_responses,
                           const std::string                  &ip,
                           const std::string                  &iface,
                           int                                 port,
                           const std::string                  &log_file)
    : outgoing_requests_(client_requests),
      incoming_responses_(client_responses),
      client_id_(client_id),
      logger_(log_file),
      tcp_socket_(logger_) {

    tcp_socket_.rcv_callback = [this](auto *s, auto rx_time) { recvCallback(s, rx_time); };

    // Client mode connect: TCPSocket::connect(ip, iface, port, is_listening=false).
    ASSERT(tcp_socket_.connect(ip, iface, port, /*is_listening=*/false) >= 0,
           "OrderGateway connect failed to " + ip + ":" + std::to_string(port));

    logger_.log("%:% %() % OrderGateway connected to %:% as client:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                ip, port, client_id_.value);
}

OrderGateway::~OrderGateway() {
    stop();
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);
}

auto OrderGateway::start() -> void {
    run_.store(true);
    auto *t = Common::createAndStartThread(
        -1, "Trading/OrderGateway", [this]() { run(); });
    ASSERT(t != nullptr, "Failed to start OrderGateway thread.");
    t->detach();
    delete t;
    logger_.log("%:% %() % OrderGateway started.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
}

auto OrderGateway::stop() -> void {
    run_.store(false);
}

// ── Busy-spin loop ───────────────────────────────────────────────────────────

auto OrderGateway::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));

    while (run_.load()) {
        // 1. Drain outgoing requests from the strategy -> serialize onto TCP.
        for (const auto *req = outgoing_requests_->getNextToRead();
             req != nullptr;
             req = outgoing_requests_->getNextToRead()) {
            TTT_MEASURE(T11_OrderGateway_LFQueue_read, logger_);
            sendRequest(*req);
            outgoing_requests_->updateReadIndex();
            TTT_MEASURE(T12_OrderGateway_TCP_write, logger_);
        }

        // 2. Flush + receive on the TCP socket. recvCallback fires on data.
        tcp_socket_.sendAndRecv();
    }
}

// ── Send path ────────────────────────────────────────────────────────────────

auto OrderGateway::sendRequest(const MEClientRequest &req) noexcept -> void {
    // SBE two-send: seq_num first, then the MEClientRequest payload.
    // Both land in the TCP send buffer contiguously -> forms OMClientRequest
    // on the wire without constructing a temp struct.
    START_MEASURE(Trading_TCPSocket_send);
    tcp_socket_.send(&next_outgoing_seq_num_, sizeof(next_outgoing_seq_num_));
    tcp_socket_.send(&req,                    sizeof(MEClientRequest));
    END_MEASURE(Trading_TCPSocket_send, logger_);

    logger_.log("%:% %() % Sent seq:% %\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                next_outgoing_seq_num_,
                req.toString());

    ++next_outgoing_seq_num_;
}

// ── Receive path ─────────────────────────────────────────────────────────────

auto OrderGateway::recvCallback(TCPSocket *socket, Nanos rx_time) noexcept -> void {
    TTT_MEASURE(T7t_OrderGateway_TCP_read, logger_);
    START_MEASURE(Trading_OrderGateway_recvCallback);
    logger_.log("%:% %() % fd:% rx_time:% len:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                socket->fd, rx_time, socket->next_rcv_valid_index);

    // Offset-based parse + single compaction (was per-frame memmove).
    size_t offset = 0;
    while (socket->next_rcv_valid_index - offset >= sizeof(OMClientResponse)) {
        OMClientResponse msg;
        std::memcpy(&msg, socket->rcv_buffer + offset, sizeof(OMClientResponse));
        offset += sizeof(OMClientResponse);

        processResponse(msg);
    }
    if (offset > 0) {
        std::memmove(socket->rcv_buffer,
                     socket->rcv_buffer + offset,
                     socket->next_rcv_valid_index - offset);
        socket->next_rcv_valid_index -= offset;
    }
    END_MEASURE(Trading_OrderGateway_recvCallback, logger_);
}

auto OrderGateway::onBytesForTest(const char *data, size_t len) noexcept -> void {
    // Iterate in fixed-size OMClientResponse frames.
    size_t off = 0;
    while (off + sizeof(OMClientResponse) <= len) {
        OMClientResponse msg;
        std::memcpy(&msg, data + off, sizeof(OMClientResponse));
        processResponse(msg);
        off += sizeof(OMClientResponse);
    }
}

auto OrderGateway::processResponse(const OMClientResponse &msg) noexcept -> void {
    // Identity check: responses must be addressed to us.
    if (UNLIKELY(msg.me_client_response_.client_id_ != client_id_)) {
        logger_.log("%:% %() % WRONG CLIENT expected:% got:% -- dropping\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    client_id_.value,
                    msg.me_client_response_.client_id_.value);
        return;
    }

    // Sequence check: drop out-of-order or duplicate responses.
    if (UNLIKELY(msg.seq_num_ != next_exp_incoming_seq_num_)) {
        logger_.log("%:% %() % SEQUENCE GAP expected:% got:% -- dropping\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    next_exp_incoming_seq_num_,
                    msg.seq_num_);
        return;
    }
    ++next_exp_incoming_seq_num_;

    // Publish MEClientResponse into the incoming-response queue for downstream.
    auto *slot = incoming_responses_->getNextToWriteTo();
    *slot = msg.me_client_response_;
    incoming_responses_->updateWriteIndex();
    TTT_MEASURE(T8t_OrderGateway_LFQueue_write, logger_);
}

} // namespace Trading
