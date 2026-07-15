#include "socket_utils.h"
#include "time_utils.h"

namespace Common
{
    auto getIfaceIP(const std::string &iface) -> std::string
    {
        char buf[NI_MAXHOST] = {'\0'};
        ifaddrs *ifaddr = nullptr;

        if (getifaddrs(&ifaddr) != -1)
        {
            for (ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
            {
                if (iface == ifa->ifa_name && ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET)
                {
                    getnameinfo(ifa->ifa_addr, sizeof(sockaddr_in), buf, sizeof(buf), nullptr, 0, NI_NUMERICHOST);
                    break;
                }
            }

            freeifaddrs(ifaddr);
        }
        return buf;
    }

    auto setNonBlocking(int fd) -> bool
    {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1)
        {
            return false;
        }
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            return false;
        }
        return true;
    }

    auto setNoDelay(int fd) -> bool
    {
        int flag = 1;
        return (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<void *>(&flag), sizeof(flag)) != -1);
    }

    auto wouldBlock() -> bool
    {
        return (errno == EINPROGRESS || errno == EWOULDBLOCK);
    }

    auto setTTL(int fd, int ttl) -> bool
    {
        return (setsockopt(fd, IPPROTO_IP, IP_TTL, reinterpret_cast<void *>(&ttl), sizeof(ttl)) != -1);
    }

    auto setMcastTTL(int fd, int mcast_ttl) -> bool
    {
        return (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<void *>(&mcast_ttl), sizeof(mcast_ttl)) != -1);
    }

    auto join(int fd, const std::string &ip, const std::string &iface, int port) -> bool {
        (void)port; // port already bound during createSocket
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(ip.c_str());
        const auto iface_ip = getIfaceIP(iface);
        mreq.imr_interface.s_addr = iface_ip.empty() ? htonl(INADDR_ANY) : inet_addr(iface_ip.c_str());
        return setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != -1;
    }

    auto setSOTimestamp(int fd) -> bool
    {
        int flag = 1;
        return (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, reinterpret_cast<void *>(&flag), sizeof(flag)) != -1);
    }

    auto createSocket(Logger &logger, const std::string &t_ip, const std::string &iface, int port, bool is_udp, bool is_blocking, bool is_listening, int ttl, bool needs_so_timestamp) -> int
    {
        std::string time_str;
        const auto ip = t_ip.empty() ? getIfaceIP(iface) : t_ip;
        logger.log("%:% %() % ip:% iface:% port:% is_udp:% is_blocking:% is_listening:% ttl:% SO_time:%\n",__FILE__, __LINE__, __FUNCTION__,
        Common::getCurrentTimeStr(&time_str), ip,
        iface, port, is_udp, is_blocking,
        is_listening, ttl, needs_so_timestamp);

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = is_udp ? SOCK_DGRAM : SOCK_STREAM;
        hints.ai_protocol = is_udp ? IPPROTO_UDP : IPPROTO_TCP;
        hints.ai_flags = is_listening ? AI_PASSIVE : 0;
        if (std::isdigit(ip.c_str()[0]))
        {
            hints.ai_flags |= AI_NUMERICHOST;
        }
        hints.ai_flags |= AI_NUMERICSERV;

        addrinfo *res = nullptr;
        const auto rc = getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &res);
        if (rc)
        {
            // NOTE: argument count must match '%' count — a mismatch used to
            // FATAL("extra arguments provided to log()") and kill the process
            // on this error path instead of returning -1.
            logger.log("getaddrinfo() failed. error:% errno:%\n", gai_strerror(rc), strerror(errno));
            return -1;
        }
        int fd = -1;
        int one = 1;
        // On any setup failure: close the fd, free the addrinfo list, and
        // bail. The previous version returned without freeaddrinfo()/close().
        const auto fail = [&](const char *what) -> int {
            logger.log("% failed. errno:%\n", what, strerror(errno));
            if (fd != -1)
            {
                close(fd);
            }
            freeaddrinfo(res);
            return -1;
        };
        for (addrinfo *rp = res; rp != nullptr; rp = rp->ai_next)
        {
            fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd == -1)
            {
                return fail("socket()");
            }
            if (!is_blocking)
            {
                if (!setNonBlocking(fd))
                {
                    return fail("setNonBlocking()");
                }
                if (!is_udp && !setNoDelay(fd))
                {
                    return fail("setNoDelay()");
                }
            }
            if (!is_listening && connect(fd, rp->ai_addr, rp->ai_addrlen) == -1 && !wouldBlock())
            {
                return fail("connect()");
            }
            if (is_listening && (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1))
            {
                return fail("setsockopt() SO_REUSEADDR");
            }
            if (is_listening && bind(fd, rp->ai_addr, rp->ai_addrlen) == -1)
            {
                return fail("bind()");
            }
            if (is_listening && is_udp == false && listen(fd, MaxTCPServerBacklog) == -1)
            {
                return fail("listen()");
            }
            if (is_udp && ttl)
            {
                // Multicast IPv4 = first octet in [224, 239]. The old check
                // `atoi(ip) && 0xe0` was a logical-AND against a non-zero
                // constant — true for ANY non-zero first octet.
                const bool is_multicast = (atoi(ip.c_str()) & 0xF0) == 0xE0;
                if (is_multicast && !setMcastTTL(fd, ttl))
                {
                    return fail("setMcastTTL()");
                }
                if (!is_multicast && !setTTL(fd, ttl)) {
                    return fail("setTTL()");
                }
            }
            if (needs_so_timestamp && !setSOTimestamp(fd))
            {
                return fail("setSOTimestamp()");
            }
        }
        freeaddrinfo(res);
        return fd;
    }
}
