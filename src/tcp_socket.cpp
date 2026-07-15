#include "tcp_socket.h"

namespace Common
{
    auto TCPSocket::destroy() noexcept -> void
    {
        close(fd);
        fd = -1;
    }

    auto TCPSocket::connect(const std::string &ip, const std::string &iface, int port, bool is_listening) -> int
    {
        destroy();
        fd = createSocket(logger, ip, iface, port, false, false, is_listening, 0, true);

        inInAddr.sin_addr.s_addr = INADDR_ANY;
        inInAddr.sin_port = htons(static_cast<uint16_t>(port));
        inInAddr.sin_family = AF_INET;

        return fd;
    }


    auto TCPSocket::send(const void *data, std::size_t len) noexcept -> void
    {
        if (len > 0) {
            ASSERT(next_send_valid_index + len <= TCPBufferSize,
                   "TCPSocket send buffer overflow on fd:" + std::to_string(fd));
            memcpy(send_buffer + next_send_valid_index, data, len);
            next_send_valid_index += len;
        }
    }

    auto TCPSocket::sendAndRecv() noexcept -> bool
    {
        char ctrl[CMSG_SPACE(sizeof(struct timeval))] = {};
        struct iovec iov;
        iov.iov_base = rcv_buffer + next_rcv_valid_index;
        iov.iov_len = TCPBufferSize - next_rcv_valid_index;

        msghdr msg{};
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);
        msg.msg_name = &inInAddr;
        msg.msg_namelen = sizeof(inInAddr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        const auto recv_len = recvmsg(fd, &msg, MSG_DONTWAIT);
        if (recv_len > 0)
        {
            next_rcv_valid_index += recv_len;
            Nanos kernel_time = 0;

            // Only trust the control buffer if the kernel actually delivered a
            // timestamp cmsg. The previous code dereferenced ctrl
            // unconditionally — an uninitialised read whenever no control
            // message was attached, feeding garbage rx timestamps into the
            // FIFO sequencer's fairness sort.
            if (const cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                cmsg != nullptr &&
                cmsg->cmsg_level == SOL_SOCKET &&
                cmsg->cmsg_type == SCM_TIMESTAMP &&
                cmsg->cmsg_len == CMSG_LEN(sizeof(struct timeval)))
            {
                struct timeval time_kernel;
                memcpy(&time_kernel, CMSG_DATA(cmsg), sizeof(time_kernel));
                kernel_time = time_kernel.tv_sec * NANOS_TO_SECS + time_kernel.tv_usec * NANOS_TO_MICROS;
            }
            else
            {
                kernel_time = getCurrentNanos(); // fallback: user-space rx time
            }
            const auto user_time = getCurrentNanos();

            logger.log("%:% %() % read socket:% len:% utime:% ktime:% diff:%\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str), fd, next_rcv_valid_index, user_time, kernel_time, (user_time - kernel_time));
            rcv_callback(this, kernel_time);
        }
        else if (UNLIKELY(recv_len == 0 && iov.iov_len > 0))
        {
            // Orderly EOF — the peer closed its end. Flag for the owner
            // (TCPServer / OrderGateway) to reap; previously this case was
            // ignored and the dead socket busy-spun forever.
            rcv_disconnected = true;
        }
        else if (UNLIKELY(recv_len < 0 && !wouldBlock()))
        {
            rcv_disconnected = true;
        }

        // Drain the send buffer. EWOULDBLOCK and partial sends keep the
        // unsent tail for the next call — the previous version zeroed
        // next_send_valid_index unconditionally (silent data loss) and
        // ASSERT-exited the whole process on a partial send.
        std::size_t sent_total = 0;
        while (sent_total < next_send_valid_index)
        {
            const auto sent_len = ::send(fd, send_buffer + sent_total,
                                         next_send_valid_index - sent_total,
                                         MSG_DONTWAIT | MSG_NOSIGNAL);
            if (UNLIKELY(sent_len < 0))
            {
                if (!wouldBlock())
                {
                    send_disconnected = true;
                }
                break;
            }
            sent_total += static_cast<std::size_t>(sent_len);
        }

        if (UNLIKELY(send_disconnected))
        {
            next_send_valid_index = 0; // connection gone — nothing to retry against
        }
        else if (UNLIKELY(sent_total < next_send_valid_index))
        {
            // Kernel socket buffer full — keep the unsent remainder at the
            // front of the buffer and retry on the next call.
            memmove(send_buffer, send_buffer + sent_total, next_send_valid_index - sent_total);
            next_send_valid_index -= sent_total;
        }
        else
        {
            next_send_valid_index = 0;
        }

        if (sent_total > 0)
        {
            logger.log("%:% %() % send socket:% len:%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str), fd, sent_total);
        }
        return (recv_len > 0);
    }

}
