// Raw-socket transport for ICMPv4 / ICMPv6. One Prober owns one socket for the
// chosen family. Cross-platform (POSIX + Winsock); the POSIX path is compiled
// and the codec it relies on is unit-tested. Real probing needs privileges.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "netpulse/icmp.hpp"

namespace netpulse {

struct Incoming {
    std::string from; // responder IP (textual)
    ParsedReply reply;
    double at; // epoch seconds
};

// A usable local network interface (one entry per address).
struct NetInterface {
    std::string name;    // adapter name / friendly name
    std::string address; // local IP (textual)
    bool v6;             // false = IPv4, true = IPv6
};

// Enumerate local interfaces with usable unicast addresses.
std::vector<NetInterface> list_interfaces();

class Prober {
public:
    // `source` optionally binds the socket to a local address so probes leave a
    // specific interface (multi-homed hosts, VPNs, etc.).
    Prober(Family family, bool privileged, const std::string& source = std::string());
    ~Prober();
    Prober(const Prober&) = delete;
    Prober& operator=(const Prober&) = delete;

    bool ok() const { return fd_ >= 0; }
    const std::string& error() const { return error_; }
    Family family() const { return family_; }

    // Send one echo request to a numeric IP with the given TTL / hop limit.
    bool send(const std::string& ip, uint8_t ttl, uint16_t id, uint16_t seq, size_t payload);

    // Drain replies until the deadline (epoch seconds).
    std::vector<Incoming> drain(double deadline_secs);

private:
    Family family_;
    long long fd_ = -1;
    std::string error_;
    void set_nonblocking();
};

} // namespace netpulse
