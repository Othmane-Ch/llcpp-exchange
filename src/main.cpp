#include "time_utils.h"
#include "logging.h"
#include "tcp_server.h"

struct MyStruct {
    int d_[3];
};


int main(int, char **) {
    using namespace Common;
    std::string time_str;
    Logger logger("tcp_server_example.log");
    auto tcpServerRcvCallback = [&](TCPSocket *socket, Nanos rx_time) noexcept
    {
        logger.log("TCPServer::defaultRcvCallback()  socket:%  len:%  rx:%\n", socket->fd, socket->next_rcv_valid_index, rx_time);
        const std::string reply = "TCPServer received msg: " + std::string(socket->rcv_buffer, socket->next_rcv_valid_index);
        socket->next_rcv_valid_index = 0;
        socket->send(reply.data(), reply.length());
    };

    auto tcpServerRecvFinishedCallback = [&]() noexcept {
        logger.log("TCPServer::defaultRecvFinishedCallback()\n");
    };

    auto tcpClientRecvCallback = [&](TCPSocket *socket, Nanos rx_time) noexcept {
        const std::string recv_msg = std::string(socket->rcv_buffer, socket->next_rcv_valid_index);
        socket->next_rcv_valid_index = 0;

        logger.log("TCPSocket::defaultRecvCallback() socket:% len:% rx:% msg:%\n",
                    socket->fd, socket->next_rcv_valid_index, rx_time, recv_msg);
    };

    const std::string iface = "lo";
    const std::string ip = "127.0.0.1";
    const int port = 12345;

    logger.log("Creating TCPServer on iface:% ip:% port:%\n", iface, ip, port);
    TCPServer server(logger);
    server.rcv_callback = tcpServerRcvCallback;
    server.rcv_finished_callback = tcpServerRecvFinishedCallback;
    server.listen(iface, port);

    std::vector<TCPSocket *> clients(5);
    for (int i = 0; i < clients.size(); ++i)
    {
        clients[i] = new TCPSocket(logger);
        clients[i]->rcv_callback = tcpClientRecvCallback;
        const std::string client_ip = ip;
        logger.log("Connecting TCP client % on iface:% ip:% port:%\n", i, iface, client_ip, port);
        if (clients[i]->connect(client_ip, iface, port, false) < 0)
        {
            logger.log("Failed to connect TCP client %. iface:% ip:% port:% error:%\n", i, iface, client_ip, port, strerror(errno));
            return -1;
        }
        server.poll();
    }

    using namespace std::literals::chrono_literals;

    for (auto itr = 0; itr < 5; ++itr)
    {
        for (size_t i = 0; i < clients.size(); ++i) {
            const std::string client_msg = "CLIENT-[" + std::to_string(i) + "] : Sending " + std::to_string(itr * 100 + i);
            logger.log("Sending TCPClient-[%] %\n", i, client_msg);
            clients[i]->send(client_msg.data(), client_msg.length());
            clients[i]->sendAndRecv();

            std::this_thread::sleep_for(500ms);
            server.poll();
            server.sendAndRecv();
        }
    }

    return 0;
}