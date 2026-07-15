//
// OrderServer implementation.
//

#include "order_server.h"

namespace Exchange {

OrderServer::OrderServer(ClientRequestLFQueue  *client_requests,
                         ClientResponseLFQueue *client_responses,
                         const std::string     &iface,
                         int                    port,
                         const std::string     &log_file)
    : outgoing_requests_(client_requests),
      incoming_responses_(client_responses),
      logger_(log_file),
      tcp_server_(logger_),
      fifo_sequencer_(client_requests, &logger_) {

    cid_next_outgoing_seq_num_.fill(1);
    cid_next_exp_seq_num_.fill(1);
    cid_tcp_socket_.fill(nullptr);

    // Wire TCP callbacks — these fire from inside sendAndRecv() on the
    // OrderServer thread, so no cross-thread sharing.
    tcp_server_.rcv_callback = [this](auto *socket, auto rx_time) {
        recvCallback(socket, rx_time);
    };
    tcp_server_.rcv_finished_callback = [this]() {
        recvFinishedCallback();
    };
    // A client socket died: drop its registration so (a) we never route
    // responses into a dangling socket and (b) the client can reconnect
    // with a fresh session (sequence numbers restart at 1).
    tcp_server_.disconnect_callback = [this](Common::TCPSocket *socket) {
        for (size_t cid = 0; cid < cid_tcp_socket_.size(); ++cid) {
            if (cid_tcp_socket_[cid] == socket) {
                cid_tcp_socket_[cid] = nullptr;
                cid_next_outgoing_seq_num_[cid] = 1;
                cid_next_exp_seq_num_[cid]      = 1;
                logger_.log("%:% %() % client:% disconnected — session reset\n",
                            __FILE__, __LINE__, __FUNCTION__,
                            Common::getCurrentTimeStr(&time_str_), cid);
            }
        }
    };

    tcp_server_.listen(iface, port);

    logger_.log("%:% %() % OrderServer listening on %:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                iface, port);
}

OrderServer::~OrderServer() {
    stop();
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);
}

auto OrderServer::start() -> void {
    run_ = true;
    auto *t = Common::createAndStartThread(
        -1, "Exchange/OrderServer", [this]() { run(); });
    ASSERT(t != nullptr, "Failed to start OrderServer thread.");
    t->detach();
    delete t;
    logger_.log("%:% %() % OrderServer started.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
}

auto OrderServer::stop() -> void {
    run_ = false;
}

// ── Hot path ───────────────────────────────────────────────────────────────

auto OrderServer::recvCallback(Common::TCPSocket *socket,
                               Common::Nanos rx_time) noexcept -> void {
    TTT_MEASURE(T1_OrderServer_TCP_read, logger_);
    logger_.log("%:% %() % socket:% rx_time:% len:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                socket->fd, rx_time, socket->next_rcv_valid_index);

    // Process as many complete OMClientRequest messages as available.
    // Parse with a read offset and compact ONCE at the end — the previous
    // per-message memmove of the whole remaining buffer made a K-message
    // burst cost O(K * buffered_bytes).
    size_t offset = 0;
    while (socket->next_rcv_valid_index - offset >= sizeof(OMClientRequest)) {
        // Reinterpret directly — no copy. The struct is packed (SBE).
        auto *request = reinterpret_cast<const OMClientRequest *>(socket->rcv_buffer + offset);
        offset += sizeof(OMClientRequest);

        logger_.log("%:% %() % Received %\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    request->toString());

        // Only NEW and CANCEL may cross the wire. Anything else is dropped
        // here: SHUTDOWN is the matching engine's internal poison pill (a
        // hostile client could otherwise stop the engine remotely), and an
        // unknown type would previously reach processClientRequest()'s
        // FATAL() — a remote kill switch for the whole exchange.
        const auto type = request->me_client_request_.type_;
        if (UNLIKELY(type != ClientRequestType::NEW &&
                     type != ClientRequestType::CANCEL)) {
            logger_.log("%:% %() % INVALID request type:% — dropping\n",
                        __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_),
                        clientRequestTypeToString(type));
            continue;
        }

        const auto client_id = request->me_client_request_.client_id_.value;

        if (UNLIKELY(client_id >= ME_MAX_NUM_CLIENTS)) {
            logger_.log("%:% %() % INVALID client_id:% — skipping\n",
                        __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_),
                        client_id);
            continue;
        }

        // New client: register socket.
        if (UNLIKELY(cid_tcp_socket_[client_id] == nullptr)) {
            cid_tcp_socket_[client_id] = socket;
        }

        // Existing client: verify socket identity — reject hijack attempts.
        if (UNLIKELY(cid_tcp_socket_[client_id] != socket)) {
            logger_.log("%:% %() % SOCKET MISMATCH client_id:% expected_fd:% got_fd:% — skipping\n",
                        __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_),
                        client_id,
                        cid_tcp_socket_[client_id]->fd,
                        socket->fd);
            continue;
        }

        // Sequence validation — drop out-of-order messages.
        if (UNLIKELY(request->seq_num_ != cid_next_exp_seq_num_[client_id])) {
            logger_.log("%:% %() % SEQUENCE GAP client_id:% expected:% got:%\n",
                        __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_),
                        client_id,
                        cid_next_exp_seq_num_[client_id],
                        request->seq_num_);
            continue;
        }
        ++cid_next_exp_seq_num_[client_id];

        // Buffer into FIFO sequencer — will be sorted and published in
        // recvFinishedCallback() after all sockets are drained.
        START_MEASURE(Exchange_FIFOSequencer_addClientRequest);
        fifo_sequencer_.addClientRequest(rx_time, request->me_client_request_);
        END_MEASURE(Exchange_FIFOSequencer_addClientRequest, logger_);
    }

    // Single compaction: move any partial trailing message to the front.
    if (offset > 0) {
        std::memmove(socket->rcv_buffer,
                     socket->rcv_buffer + offset,
                     socket->next_rcv_valid_index - offset);
        socket->next_rcv_valid_index -= offset;
    }
}

auto OrderServer::recvFinishedCallback() noexcept -> void {
    // All sockets drained for this poll cycle — sort by rx_time and publish.
    START_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish);
    fifo_sequencer_.sequenceAndPublish();
    END_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish, logger_);
}

auto OrderServer::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));

    while (run_) {
        // Poll TCP connections — accepts new clients, receives data.
        tcp_server_.poll();
        tcp_server_.sendAndRecv();

        // Drain ALL pending matching-engine responses before the next poll
        // cycle (draining one per cycle let the response queue back up
        // behind an epoll_wait syscall per message under load).
        for (const auto *response = incoming_responses_->getNextToRead();
             response != nullptr;
             response = incoming_responses_->getNextToRead()) {
            TTT_MEASURE(T5t_OrderServer_LFQueue_read, logger_);
            logger_.log("%:% %() % Sending %\n",
                        __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_),
                        response->toString());

            const auto client_id = response->client_id_.value;
            if (LIKELY(client_id < cid_tcp_socket_.size() && cid_tcp_socket_[client_id])) {
                // SBE two-send pattern: seq_num then payload.
                // Both land in the TCP send buffer contiguously, forming
                // OMClientResponse on the wire without constructing a temp.
                auto &seq = cid_next_outgoing_seq_num_[client_id];
                START_MEASURE(Exchange_TCPSocket_send);
                cid_tcp_socket_[client_id]->send(&seq, sizeof(seq));
                cid_tcp_socket_[client_id]->send(response, sizeof(MEClientResponse));
                END_MEASURE(Exchange_TCPSocket_send, logger_);
                ++seq;
            }

            incoming_responses_->updateReadIndex();
            TTT_MEASURE(T6t_OrderServer_TCP_write, logger_);
        }
    }
}

} // namespace Exchange
