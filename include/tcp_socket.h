#pragma once
#include <cstddef>
#include <functional>

#include "socket_utils.h"
#include "logging.h"
#include "time_utils.h"

namespace Common
{
#if defined(LLCPP_SMALL_FOOTPRINT)
    constexpr std::size_t TCPBufferSize = 4 * 1024 * 1024;  // 4MB  (VM profile)
#else
    constexpr std::size_t TCPBufferSize = 64 * 1024 * 1024; // 64MB
#endif


    struct TCPSocket
    {
        auto defaultRcvCallback(TCPSocket *socket, Nanos rx_time) noexcept
        {
            logger.log("%: % % ()  % TCPSocket::defaultRcvCallback()  socket:%  len:%  rx:%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str), socket->fd, socket->next_rcv_valid_index, rx_time);
        }

        explicit TCPSocket(Logger &log) : logger(log)
        {
            send_buffer = new char[TCPBufferSize];
            rcv_buffer = new char[TCPBufferSize];
            rcv_callback = [this](auto socket, auto rx_time) {
                defaultRcvCallback(socket, rx_time);
            };
        }

        auto connect(const std::string &ip, const std::string &iface, int port, bool is_listening) -> int;

        auto send(const void *data, std::size_t len) noexcept -> void;

        auto sendAndRecv() noexcept -> bool;

        auto destroy() noexcept -> void;

        ~TCPSocket() noexcept
        {
            destroy();
            delete[] send_buffer;
            send_buffer = nullptr;
            delete[] rcv_buffer;
            rcv_buffer = nullptr;
        }

        TCPSocket() = delete;
        TCPSocket(const TCPSocket &) = delete;
        TCPSocket(const TCPSocket &&) = delete;
        TCPSocket &operator=(const TCPSocket &) = delete;
        TCPSocket &operator=(const TCPSocket &&) = delete;


        int fd = -1;
        char *send_buffer = nullptr;
        std::size_t next_send_valid_index = 0;
        char *rcv_buffer = nullptr;
        std::size_t next_rcv_valid_index = 0;

        bool send_disconnected = false;
        bool rcv_disconnected = false;

        struct sockaddr_in inInAddr;

        std::function<void(TCPSocket *s, Nanos rx_time)> rcv_callback;

        std::string time_str;
        Logger &logger;
    };
}
