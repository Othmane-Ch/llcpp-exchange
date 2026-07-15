//
// McastSocket — UDP multicast send/receive wrapper for market data.
//
// Design rationale:
//   - Pre-allocated send/receive buffers (no heap on hot path).
//   - Non-blocking I/O so the caller can busy-spin without blocking.
//   - Thin wrapper around POSIX sendto()/recvfrom() — no abstraction tax.
//

#pragma once

#include <cstring>
#include <functional>

#include "socket_utils.h"
#include "logging.h"
#include "time_utils.h"

namespace Common {

#if defined(LLCPP_SMALL_FOOTPRINT)
    constexpr size_t McastBufferSize = 4 * 1024 * 1024;  // 4 MB  (VM profile)
#else
    constexpr size_t McastBufferSize = 64 * 1024 * 1024; // 64 MB
#endif

    struct McastSocket {

        explicit McastSocket(Logger &log) : logger_(log) {
            send_buffer_ = new char[McastBufferSize];
            rcv_buffer_  = new char[McastBufferSize];
        }

        ~McastSocket() noexcept {
            destroy();
            delete[] send_buffer_;  send_buffer_ = nullptr;
            delete[] rcv_buffer_;   rcv_buffer_  = nullptr;
        }

        /// Initialise the socket for sending or receiving.
        /// For a publisher:  is_listening = false
        /// For a subscriber: is_listening = true  (calls join() on the mcast group)
        auto init(const std::string &ip, const std::string &iface,
                  int port, bool is_listening) -> int {

            destroy();

            fd_ = createSocket(logger_, ip, iface, port,
                               /*is_udp=*/true, /*is_blocking=*/false,
                               is_listening, /*ttl=*/32,
                               /*needs_so_timestamp=*/false);

            // Store destination address for sendto().
            std::memset(&dest_addr_, 0, sizeof(dest_addr_));
            dest_addr_.sin_family      = AF_INET;
            dest_addr_.sin_port        = htons(static_cast<uint16_t>(port));
            dest_addr_.sin_addr.s_addr = inet_addr(ip.c_str());

            // If receiving, join the multicast group.
            if (is_listening) {
                ASSERT(join(fd_, ip, iface, port),
                       "McastSocket::init join() failed on " + ip + ":" + std::to_string(port));
            }

            return fd_;
        }

        auto destroy() noexcept -> void {
            if (fd_ >= 0) { close(fd_); fd_ = -1; }
            next_send_valid_index_ = 0;
            next_rcv_valid_index_  = 0;
        }

        /// Queue data into the send buffer. Actual transmission happens in
        /// sendAndRecv(). Overflow is FATAL (mirrors TCPSocket::send) — a
        /// silent overrun would smash the heap next to the buffer.
        auto send(const void *data, size_t len) noexcept -> void {
            ASSERT(next_send_valid_index_ + len <= McastBufferSize,
                   "McastSocket send buffer overflow on fd:" + std::to_string(fd_));
            std::memcpy(send_buffer_ + next_send_valid_index_, data, len);
            next_send_valid_index_ += len;
        }

        /// Flush the send buffer via sendto() and receive any pending datagrams.
        /// Returns true if data was received.
        auto sendAndRecv() noexcept -> bool {
            // --- Send ---
            if (next_send_valid_index_ > 0) {
                const auto n = ::sendto(fd_, send_buffer_,
                                        next_send_valid_index_, MSG_DONTWAIT,
                                        reinterpret_cast<const sockaddr *>(&dest_addr_),
                                        sizeof(dest_addr_));
                if (UNLIKELY(n < 0)) {
                    if (wouldBlock()) {
                        // Kernel buffer full — keep the datagram and retry on
                        // the next call (previously it was silently dropped).
                    } else {
                        send_disconnected_ = true;
                        next_send_valid_index_ = 0; // unrecoverable — drop
                    }
                } else {
                    next_send_valid_index_ = 0; // datagram handed to the kernel
                }
            }

            // --- Recv ---
            const auto n = ::recvfrom(fd_, rcv_buffer_ + next_rcv_valid_index_,
                                      McastBufferSize - next_rcv_valid_index_,
                                      MSG_DONTWAIT,
                                      nullptr, nullptr);
            if (n > 0) {
                next_rcv_valid_index_ += static_cast<size_t>(n);
                const auto rx_time = getCurrentNanos();
                logger_.log("%:% %() % McastSocket::recv fd:% len:% rx:%\n",
                            __FILE__, __LINE__, __FUNCTION__,
                            getCurrentTimeStr(&time_str_),
                            fd_, next_rcv_valid_index_, rx_time);
                if (rcv_callback_)
                    rcv_callback_(this);
                return true;
            }

            return false;
        }

        McastSocket() = delete;
        McastSocket(const McastSocket &) = delete;
        McastSocket(McastSocket &&) = delete;
        McastSocket &operator=(const McastSocket &) = delete;
        McastSocket &operator=(McastSocket &&) = delete;

        int  fd_ = -1;
        char *send_buffer_  = nullptr;
        size_t next_send_valid_index_ = 0;
        char *rcv_buffer_   = nullptr;
        size_t next_rcv_valid_index_  = 0;

        bool send_disconnected_ = false;
        bool rcv_disconnected_  = false;

        struct sockaddr_in dest_addr_ {};

        std::function<void(McastSocket *)> rcv_callback_;

        std::string time_str_;
        Logger &logger_;
    };

} // namespace Common
