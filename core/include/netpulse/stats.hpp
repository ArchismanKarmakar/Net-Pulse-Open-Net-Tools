// Per-hop rolling samples and focus-window statistics.
#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace netpulse {

double now_secs();

// (epoch seconds, rtt in ms or nullopt for a lost probe)
using Sample = std::pair<double, std::optional<double>>;

struct HopStat {
    uint8_t hop = 0;
    std::optional<std::string> address;
    std::optional<std::string> hostname;
    bool is_dest = false;
    size_t sent = 0;
    size_t recv = 0;
    double loss = 0.0;
    std::optional<double> cur, min, max, avg;
    double std = 0.0;
};

class HopStats {
public:
    explicit HopStats(uint8_t hop) : hop_(hop) {}

    uint8_t hop() const { return hop_; }
    const std::optional<std::string>& address() const { return address_; }
    bool is_dest() const { return is_dest_; }
    void set_is_dest(bool v) { is_dest_ = v; }
    const std::optional<std::string>& hostname() const { return hostname_; }
    void set_hostname(std::string h) { hostname_ = std::move(h); }

    // Assign / change the hop's IP. A real change (route flap) clears history.
    void set_address(const std::string& addr);
    void push(double ts, std::optional<double> rtt);
    HopStat compute(std::optional<double> focus_secs) const;

private:
    uint8_t hop_;
    std::optional<std::string> address_;
    std::optional<std::string> hostname_;
    bool is_dest_ = false;
    std::deque<Sample> samples_;
    size_t cap_ = 200000;
};

} // namespace netpulse
