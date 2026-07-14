#include "netpulse/transport.hpp"
#include "netpulse/stats.hpp" // now_secs
#include "netpulse/platform.hpp"

#include <cstring>
#include <chrono>
#include <map>
#include <memory>
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

// NOTE: Winsock init used to be a namespace-scope static object right here.
// That is unreliable in a static-lib build: a binary that links netpulse_core
// but never calls anything in *this* .obj (e.g. a test that only calls
// Session::resolve()) would never link this file in, so WSAStartup() would
// never run. Initialization now happens via ensure_winsock_ready() (see
// platform.hpp), called explicitly at the top of every function here that
// touches a socket API — see platform.hpp for the full explanation.

Prober::Prober(Family family, bool privileged, const std::string& source) : family_(family) {
    ensure_winsock_ready();
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

bool Prober::send(const std::string& ip, uint8_t ttl, uint16_t id, uint16_t seq, size_t payload,
                  std::optional<uint16_t> pin_checksum) {
    if (fd_ < 0) return false;
    int fd = static_cast<int>(fd_);
    // Serializes {setsockopt TTL, sendto} — this socket may be shared by many
    // target threads at once (see the class comment in transport.hpp); TTL is
    // socket-level state, not a per-packet parameter, so two concurrent
    // sends with different TTLs could otherwise interleave and one probe
    // would silently go out with the WRONG TTL.
    std::lock_guard<std::mutex> lk(send_mtx_);

    if (family_ == Family::V4) {
        int t = ttl;
        setsockopt(fd, IPPROTO_IP, IP_TTL, reinterpret_cast<const char*>(&t), sizeof(t));
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        if (inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) != 1) return false;
        auto pkt = build_echo(Family::V4, id, seq, payload, pin_checksum);
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

std::vector<Incoming> Prober::drain_ready() {
    std::vector<Incoming> out;
    if (fd_ < 0) return out;
    int fd = static_cast<int>(fd_);
    uint8_t buf[1500];

    // Non-blocking: the shared RX dispatcher already knows (via its own
    // select() over every pooled socket) that this fd is readable RIGHT NOW —
    // this just drains whatever's queued and returns, no waiting here.
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
    return out;
}

// ---------------------------------------------------------------------------
// Socket pool (see the doc comment on acquire_pooled_socket in transport.hpp).
namespace {
struct SocketPoolKey {
    Family family;
    bool privileged;
    std::string source;
    bool operator<(const SocketPoolKey& o) const {
        if (family != o.family) return family < o.family;
        if (privileged != o.privileged) return privileged < o.privileged;
        return source < o.source;
    }
};
struct SocketPool {
    std::mutex mtx;
    std::map<SocketPoolKey, std::weak_ptr<Prober>> table;
};
inline SocketPool& g_socket_pool() { static SocketPool* p = new SocketPool(); return *p; } // leaked singleton (process lifetime, never destroyed — same pattern as the other g_* singletons in session.cpp)
} // namespace

std::shared_ptr<Prober> acquire_pooled_socket(Family family, bool privileged, const std::string& source) {
    SocketPoolKey key{family, privileged, source};
    SocketPool& pool = g_socket_pool();
    std::lock_guard<std::mutex> lk(pool.mtx);
    std::weak_ptr<Prober>& slot = pool.table[key]; // default-constructs an already-expired weak_ptr if new
    if (auto existing = slot.lock()) return existing;
    auto sock = std::make_shared<Prober>(family, privileged, source);
    slot = sock; // overwrite whatever was there (nothing, or an expired weak_ptr) — always safe under this same lock
    return sock;
}

std::vector<std::shared_ptr<Prober>> list_active_pooled_sockets() {
    std::vector<std::shared_ptr<Prober>> out;
    SocketPool& pool = g_socket_pool();
    std::lock_guard<std::mutex> lk(pool.mtx);
    out.reserve(pool.table.size());
    for (auto& [key, weak] : pool.table) {
        if (auto s = weak.lock()) out.push_back(std::move(s));
    }
    return out;
}

std::vector<NetInterface> list_interfaces() {
    ensure_winsock_ready();
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
