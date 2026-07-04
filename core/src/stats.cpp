#include "netpulse/stats.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace netpulse {

double now_secs() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

void HopStats::set_address(const std::string& addr) {
    if (!address_ || *address_ != addr) {
        address_ = addr;
        hostname_.reset();
        samples_.clear();
    }
}

void HopStats::push(double ts, std::optional<double> rtt) {
    if (samples_.size() >= cap_) samples_.pop_front();
    samples_.emplace_back(ts, rtt);
}

HopStat HopStats::compute(std::optional<double> focus_secs) const {
    std::optional<double> cutoff;
    if (focus_secs) cutoff = now_secs() - *focus_secs;

    HopStat s;
    s.hop = hop_;
    s.address = address_;
    s.hostname = hostname_;
    s.is_dest = is_dest_;

    std::vector<double> rtts;
    size_t sent = 0;
    std::optional<double> cur;
    for (const auto& [ts, rtt] : samples_) {
        if (cutoff && ts < *cutoff) continue;
        ++sent;
        cur = rtt; // latest (possibly nullopt = lost)
        if (rtt) rtts.push_back(*rtt);
    }
    s.sent = sent;
    s.recv = rtts.size();
    s.loss = sent > 0 ? (1.0 - static_cast<double>(s.recv) / sent) * 100.0 : 0.0;
    s.cur = cur;
    if (!rtts.empty()) {
        double mn = rtts[0], mx = rtts[0], sum = 0;
        for (double r : rtts) {
            mn = std::min(mn, r);
            mx = std::max(mx, r);
            sum += r;
        }
        double avg = sum / rtts.size();
        double var = 0;
        for (double r : rtts) var += (r - avg) * (r - avg);
        var /= rtts.size();
        s.min = mn;
        s.max = mx;
        s.avg = avg;
        s.std = std::sqrt(var);
    }
    return s;
}

} // namespace netpulse
