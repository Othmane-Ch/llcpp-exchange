#include "tcp_server.h"

namespace Common
{

    auto TCPServer::destroy() noexcept -> void
    {
        close(server_fd);
        server_fd = -1;
        listener_socket.destroy();
    }

    auto TCPServer::listen(const std::string &iface, int port) -> void
    {
        destroy();
        server_fd = epoll_create(1);
        ASSERT(server_fd >= 0, "epoll_create() failed with error: " + std::string(std::strerror(errno)));
        ASSERT(listener_socket.connect("", iface, port, true) >= 0, "Listener socket failed to connect. iface:" + iface + " port:" + std::to_string(port)
            + " error:" + std::string(std::strerror(errno)));

        ASSERT(epoll_add(&listener_socket), "epoll_ctl() failed. error:" + std::string(std::strerror(errno)));
    }

    auto TCPServer::del(TCPSocket *socket) -> void
    {
        epoll_del(socket);
        sockets.erase(std::remove(sockets.begin(), sockets.end(), socket), sockets.end());
        rcv_sockets.erase(std::remove(rcv_sockets.begin(), rcv_sockets.end(), socket), rcv_sockets.end());
        send_sockets.erase(std::remove(send_sockets.begin(), send_sockets.end(), socket), send_sockets.end());
    }

    auto TCPServer::poll() -> void
    {
        const int MAX_EVENTS = 1 + static_cast<int>(sockets.size());

        // Reap sockets that disconnected since the last poll: notify the
        // owner (so it can drop its pointers), remove from epoll and all
        // containers, then free the socket. The previous version never
        // deleted the TCPSocket (128MB of buffers leaked per disconnect) and
        // never cleared disconnected_sockets.
        if (UNLIKELY(!disconnected_sockets.empty()))
        {
            for (auto *socket: disconnected_sockets)
            {
                if (socket == &listener_socket) // member, not heap-owned
                    continue;
                logger.log("%:% %() % reaping disconnected socket:%\n", __FILE__, __LINE__, __FUNCTION__,
                            Common::getCurrentTimeStr(&time_str), socket->fd);
                del(socket);
                if (disconnect_callback)
                    disconnect_callback(socket);
                delete socket;
            }
            disconnected_sockets.clear();
        }

        const int n = epoll_wait(server_fd, events, MAX_EVENTS, 0);
        bool have_new_connection = false;
        for (int i = 0; i < n; ++i)
        {
            auto *socket = reinterpret_cast<TCPSocket *>(events[i].data.ptr);
            if (events[i].events & EPOLLIN)
            {
                if (socket == &listener_socket)
                {
                    logger.log("%:% %() % EPOLLIN listener_socket:%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str), socket->fd);
                    have_new_connection = true;
                    continue;
                }
                logger.log("%:% %() % EPOLLIN socket:%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str), socket->fd);
                if (std::find(rcv_sockets.begin(), rcv_sockets.end(), socket) == rcv_sockets.end())
                {
                    rcv_sockets.push_back(socket);
                }
            }
            if (events[i].events & EPOLLOUT)
            {
                logger.log("%:% %() % EPOLLOUT socket:%\n", __FILE__, __LINE__, __FUNCTION__,
                            Common::getCurrentTimeStr(&time_str), socket->fd);
                if (std::find(send_sockets.begin(), send_sockets.end(), socket) == send_sockets.end())
                {
                    send_sockets.push_back(socket);
                }
            }
            if (events[i].events & (EPOLLERR | EPOLLHUP))
            {
                logger.log("%:% %() % EPOLLERR socket:%\n", __FILE__, __LINE__, __FUNCTION__,
                            Common::getCurrentTimeStr(&time_str), socket->fd);
                if (std::find(disconnected_sockets.begin(), disconnected_sockets.end(), socket) == disconnected_sockets.end())
                {
                    disconnected_sockets.push_back(socket);
                }
            }
        }

        // Accept OUTSIDE the event-classification loop.
        //
        // The accept block used to live inside the for-loop body, after a
        // `continue` for the listener event. When the only ready event was
        // the listener itself (i.e. the FIRST client connecting), the
        // `continue` ended the final loop iteration and accept() never ran.
        // Because the listener is registered edge-triggered (EPOLLET), the
        // notification never re-fired, the connection sat in the kernel
        // backlog forever (the client saw a completed TCP handshake!), and
        // no order ever reached the matching engine over real TCP.
        while (have_new_connection) {
            logger.log("%:% %() % have_new_connection\n", __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str));
            sockaddr_storage addr;
            socklen_t addr_len = sizeof(addr);
            int fd = accept(listener_socket.fd, reinterpret_cast<sockaddr *>(&addr), &addr_len);
            if (fd == -1) // EAGAIN: backlog fully drained
                break;

            ASSERT(setNonBlocking(fd) && setNoDelay(fd), "Failed to set non-blocking or no-delay on socket:" + std::to_string(fd));

            logger.log("%:% %() % accepted socket:%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str), fd);
            auto *new_socket = new TCPSocket(logger);
            new_socket->fd = fd;
            new_socket->rcv_callback = rcv_callback;
            ASSERT(epoll_add(new_socket), "Failed to add new socket to epoll. error:" + std::string(std::strerror(errno)));
            if (std::find(sockets.begin(), sockets.end(), new_socket) == sockets.end())
            {
                sockets.push_back(new_socket);
            }
            if (std::find(rcv_sockets.begin(), rcv_sockets.end(), new_socket) == rcv_sockets.end())
            {
                rcv_sockets.push_back(new_socket);
            }
        }
    }

    /// Publish outgoing data from the send buffer and read incoming data from the receive buffer.
    auto TCPServer::sendAndRecv() noexcept -> void {
        auto recv = false;

        for (auto *socket: rcv_sockets)
        {
            recv |= socket->sendAndRecv();
            // sendAndRecv() flags EOF / hard errors; queue the socket for
            // reaping on the next poll() pass.
            if (UNLIKELY(socket->rcv_disconnected || socket->send_disconnected))
            {
                if (std::find(disconnected_sockets.begin(), disconnected_sockets.end(), socket) == disconnected_sockets.end())
                {
                    disconnected_sockets.push_back(socket);
                }
            }
        }

        if (recv) // There were some events and they have all been dispatched, inform listener.
            rcv_finished_callback();

        std::for_each(send_sockets.begin(), send_sockets.end(), [](auto socket) {
          socket->sendAndRecv();
        });
    }
}
