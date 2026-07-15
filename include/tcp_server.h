#pragma once
#include "tcp_socket.h"

namespace Common {

    struct  TCPServer {

        auto defaultRcvCallback(TCPSocket *socket, Nanos rx_time) noexcept -> void
        {
            logger.log("%: % % ()  % TCPServer::defaultRcvCallback()  socket:%  len:%  rx:%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str), socket->fd, socket->next_rcv_valid_index, rx_time);
        }

        auto defaultRcvFinishedCallback() noexcept -> void
        {
            logger.log("%: % % ()  % TCPServer::defaultRcvFinishedCallback()\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
        }

        explicit TCPServer(Logger &log) : listener_socket(log), logger(log)
        {
            rcv_callback = [this](auto socket, auto rx_time) {
                defaultRcvCallback(socket, rx_time);
            };
            rcv_finished_callback =  [this]() {
                defaultRcvFinishedCallback();
            };
        }

        auto destroy() noexcept -> void;

        auto epoll_add(TCPSocket *socket) -> bool
        {
            epoll_event event{};
            event.data.ptr = reinterpret_cast<void *>(socket);
            event.events = EPOLLIN | EPOLLET;
            return epoll_ctl(server_fd, EPOLL_CTL_ADD, socket->fd, &event) != -1;
        }

        auto epoll_del(TCPSocket *socket) -> bool
        {
            return epoll_ctl(server_fd, EPOLL_CTL_DEL, socket->fd, nullptr) != -1;
        }

        auto listen(const std::string &iface, int port) -> void;

        auto del(TCPSocket *socket) -> void;

        auto poll() -> void;

        auto sendAndRecv() noexcept -> void;

         ~TCPServer() noexcept
        {
            destroy();
        }

        TCPServer() = delete;
        TCPServer(const TCPServer &) = delete;
        TCPServer(const TCPServer &&) = delete;
        TCPServer &operator=(const TCPServer &) = delete;
        TCPServer &operator=(const TCPServer &&) = delete;

        int server_fd = -1;
        TCPSocket listener_socket;
        epoll_event events[1024];
        std::vector<TCPSocket *> sockets, rcv_sockets, send_sockets, disconnected_sockets;
        std::function<void(TCPSocket *s, Nanos rx_time)> rcv_callback;
        std::function<void()> rcv_finished_callback;
        /// Fired just before a disconnected socket is destroyed, so the owner
        /// can drop any pointers to it (e.g. OrderServer's per-client map).
        std::function<void(TCPSocket *s)> disconnect_callback;
        std::string time_str;
        Logger &logger;
    };

}