#include "netpulse/transport.hpp"
#include "netpulse/stats.hpp" // now_secs

#include <cstring>
#include <chrono>
#include <thread>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  include <timeapi.h>
#  pragma comment(lib, "Ws2_32.lib")
#  pragma comment(lib, "Iphlpapi.lib")
#  pragma comment(lib, "Winmm.lib")
using socklen_t = int;
#  define CLOSESOCK closesocket
#  define LASTERR WSAGetLastError()
#  define WOULDBLOCK WSAEWOULDBLOCK
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <cerrno>
#  include <ifaddrs.h>
#  include <net/if.h>
#  define CLOSESOCK ::close
#  define LASTERR errno
#  define WOULDBLOCK EWOULDBLOCK
#endif

namespace netpulse {

#ifdef _WIN32
namespace {
struct WsaInit {
    WsaInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        timeBeginPeriod(1); // 1ms timer resolution for accurate sleeps/timing
    }
    ~WsaInit() {
        timeEndPeriod(1);
        WSACleanup();
    }
};
static WsaInit g_wsa;
} // namespace
#endif

Prober::Prober(Family family, bool privileged, const std::string& source) : family_(family) {
    int domain = (family == Family::V4) ? AF_INET : AF_INET6;
    int type = privileged ? SOCK_RAW : SOCK_DGRAM;
    int proto = (family == Family::V4) ? static_cast<int>(IPPROTO_ICMP)
                                       : static_cast<int>(IPPROTO_ICMPV6);
    long long s = static_cast<long long>(::socket(domain, type, proto));
    if (s < 0) {
        error_ = "socket() failed (need admin/root for raw ICMP)";
        fd_ = -1;
        return;
    }
    fd_ = s;

    // Optionally bind to a chosen local address so probes egress a specific NIC.
    if (!source.empty()) {
        int fd = static_cast<int>(fd_);
        if (family == Family::V4) {
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            if (inet_pton(AF_INET, source.c_str(), &sa.sin_addr) == 1)
                bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        } else {
            sockaddr_in6 sa{};
            sa.sin6_family = AF_INET6;
            if (inet_pton(AF_INET6, source.c_str(), &sa.sin6_addr) == 1)
                bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        }
    }
    set_nonblocking();
}

Prober::~Prober() {
    if (fd_ >= 0) CLOSESOCK(static_cast<int>(fd_));
}

void Prober::set_nonblocking() {
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(static_cast<int>(fd_), FIONBIO, &nb);
#else
    int fl = fcntl(static_cast<int>(fd_), F_GETFL, 0);
    fcntl(static_cast<int>(fd_), F_SETFL, fl | O_NONBLOCK);
#endif
}

bool Prober::send(const std::string& ip, uint8_t ttl, uint16_t id, uint16_t seq, size_t payload) {
    if (fd_ < 0) return false;
    int fd = static_cast<int>(fd_);

    if (family_ == Family::V4) {
        int t = ttl;
        setsockopt(fd, IPPROTO_IP, IP_TTL, reinterpret_cast<const char*>(&t), sizeof(t));
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        if (inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) != 1) return false;
        auto pkt = build_echo(Family::V4, id, seq, payload);
        return sendto(fd, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
                      reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) >= 0;
    } else {
        int t = ttl;
        setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, reinterpret_cast<const char*>(&t), sizeof(t));
        sockaddr_in6 dst{};
        dst.sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, ip.c_str(), &dst.sin6_addr) != 1) return false;
        auto pkt = build_echo(Family::V6, id, seq, payload);
        return sendto(fd, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
                      reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) >= 0;
    }
}

std::vector<Incoming> Prober::drain(double deadline_secs) {
    std::vector<Incoming> out;
    if (fd_ < 0) return out;
    int fd = static_cast<int>(fd_);
    uint8_t buf[1500];

    for (;;) {
        double remaining = deadline_secs - now_secs();
        if (remaining <= 0) break;

        // Block on the socket until a packet is ready or the deadline passes.
        // select() wakes the instant data arrives (interrupt-driven), so we are
        // not subject to the OS sleep-timer granularity (~15.6 ms on Windows)
        // that would otherwise inflate every measured RTT.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(static_cast<unsigned>(fd), &rfds);
        struct timeval tv;
        tv.tv_sec = static_cast<long>(remaining);
        tv.tv_usec = static_cast<long>((remaining - tv.tv_sec) * 1e6);
        int sel = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) break; // 0 = deadline reached, <0 = error

        // Read every datagram currently queued, timestamping each on arrival.
        for (;;) {
            sockaddr_storage src{};
            socklen_t slen = sizeof(src);
            long n = recvfrom(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                              reinterpret_cast<sockaddr*>(&src), &slen);
            if (n <= 0) break;
            double at = now_secs();
            std::optional<ParsedReply> r = (family_ == Family::V4)
                                               ? parse_v4(buf, static_cast<size_t>(n))
                                               : parse_v6(buf, static_cast<size_t>(n));
            if (r) {
                char ipstr[INET6_ADDRSTRLEN] = {0};
                if (src.ss_family == AF_INET) {
                    auto* s4 = reinterpret_cast<sockaddr_in*>(&src);
                    inet_ntop(AF_INET, &s4->sin_addr, ipstr, sizeof(ipstr));
                } else {
                    auto* s6 = reinterpret_cast<sockaddr_in6*>(&src);
                    inet_ntop(AF_INET6, &s6->sin6_addr, ipstr, sizeof(ipstr));
                }
                out.push_back(Incoming{std::string(ipstr), *r, at});
            }
        }
    }
    return out;
}

std::vector<NetInterface> list_interfaces() {
    std::vector<NetInterface> out;
#ifdef _WIN32
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15000;
    std::vector<uint8_t> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size) == NO_ERROR) {
        for (auto* a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            char name[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name), nullptr, nullptr);
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                char ip[INET6_ADDRSTRLEN] = {0};
                auto* sa = u->Address.lpSockaddr;
                if (sa->sa_family == AF_INET) {
                    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(sa)->sin_addr, ip, sizeof(ip));
                    out.push_back({name, ip, false});
                } else if (sa->sa_family == AF_INET6) {
                    inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(sa)->sin6_addr, ip, sizeof(ip));
                    out.push_back({name, ip, true});
                }
            }
        }
    }
#else
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) == 0) {
        for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            char ip[INET6_ADDRSTRLEN] = {0};
            if (ifa->ifa_addr->sa_family == AF_INET) {
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(ifa->ifa_addr)->sin_addr, ip, sizeof(ip));
                out.push_back({ifa->ifa_name ? ifa->ifa_name : "", ip, false});
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr)->sin6_addr, ip, sizeof(ip));
                out.push_back({ifa->ifa_name ? ifa->ifa_name : "", ip, true});
            }
        }
        freeifaddrs(ifap);
    }
#endif
    return out;
}

} // namespace netpulse
