// NetPulse web backend — a tiny dependency-free HTTP server that drives the
// verified C++ core and exposes it as JSON for the React frontend.
//
//   GET  /api/state?focus=SECS   -> all targets, hop stats, and recent series
//   POST /api/add?target=..&family=auto|v4|v6&probe=..&trace=..&timeout=..&
//                 payload=..&maxhops=..&raw=0|1   -> { "id": N }
//   POST /api/remove?id=N        -> {}
//   POST /api/stop?id=N          -> {}
//   GET  /<static>               -> serves web/dist if present
//
// Listens on 127.0.0.1:8787. CORS is open so the Vite dev server (:5173) works.

#include "netpulse/session.hpp"
#include "netpulse/stats.hpp"
#include "netpulse/transport.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <tuple>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socklen_t = int;
#  define CLOSESOCK closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define CLOSESOCK ::close
#endif

using namespace netpulse;

// ----------------------------------------------------------------------- utils
static std::string urldecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::map<std::string, std::string> parse_query(const std::string& q) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        std::string kv = q.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        size_t eq = kv.find('=');
        if (eq != std::string::npos)
            m[kv.substr(0, eq)] = urldecode(kv.substr(eq + 1));
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return m;
}

#include "netpulse/manager.hpp"

// ------------------------------------------------------------------ http layer
static std::string content_type(const std::string& path) {
    auto ends = [&](const char* e) {
        size_t n = std::strlen(e);
        return path.size() >= n && path.compare(path.size() - n, n, e) == 0;
    };
    if (ends(".html")) return "text/html";
    if (ends(".js")) return "text/javascript";
    if (ends(".css")) return "text/css";
    if (ends(".json")) return "application/json";
    if (ends(".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

static void send_response(int fd, const std::string& status, const std::string& ctype,
                          const std::string& body) {
    std::ostringstream o;
    o << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: " << ctype << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
      << "Connection: close\r\n\r\n"
      << body;
    std::string s = o.str();
    size_t off = 0;
    while (off < s.size()) {
        long n = ::send(fd, s.data() + off, static_cast<int>(s.size() - off), 0);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

static Settings settings_from_query(const std::map<std::string, std::string>& q) {
    Settings st;
    auto getd = [&](const char* k, double def) {
        auto it = q.find(k);
        return it != q.end() ? std::stod(it->second) : def;
    };
    st.probe_interval = getd("probe", 1.0);
    st.trace_interval = getd("trace", 30.0);
    st.timeout = getd("timeout", 0.0);
    st.payload_size = static_cast<size_t>(getd("payload", 56));
    st.max_hops = static_cast<uint8_t>(getd("maxhops", 30));
    st.privileged = q.count("raw") ? q.at("raw") != "0" : true;
    st.focus_secs = std::nullopt; // focus computed per request
    if (q.count("src")) st.source_addr = q.at("src");
    std::string fam = q.count("family") ? q.at("family") : "auto";
    st.family = fam == "v4" ? FamilyPref::V4 : fam == "v6" ? FamilyPref::V6 : FamilyPref::Auto;
    return st;
}

static void handle(int fd, Manager& mgr, const std::string& dist) {
    char buf[8192];
    long n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { CLOSESOCK(fd); return; }
    buf[n] = 0;
    std::istringstream req(buf);
    std::string method, target;
    req >> method >> target;

    std::string path = target, query;
    size_t qpos = target.find('?');
    if (qpos != std::string::npos) { path = target.substr(0, qpos); query = target.substr(qpos + 1); }
    auto q = parse_query(query);

    if (method == "OPTIONS") { send_response(fd, "204 No Content", "text/plain", ""); CLOSESOCK(fd); return; }

    if (path == "/api/state") {
        std::optional<double> focus;
        if (q.count("focus") && q["focus"] != "all") focus = std::stod(q["focus"]);
        send_response(fd, "200 OK", "application/json", mgr.state_json(focus));
    } else if (path == "/api/interfaces") {
        std::ostringstream o;
        o << "[";
        auto ifs = list_interfaces();
        for (size_t i = 0; i < ifs.size(); ++i) {
            o << "{\"name\":\"" << esc(ifs[i].name) << "\",\"address\":\"" << esc(ifs[i].address)
              << "\",\"v6\":" << (ifs[i].v6 ? "true" : "false") << "}";
            if (i + 1 < ifs.size()) o << ",";
        }
        o << "]";
        send_response(fd, "200 OK", "application/json", o.str());
    } else if (path == "/api/add") {
        std::string tgt = q.count("target") ? q["target"] : "";
        if (tgt.empty()) { send_response(fd, "400 Bad Request", "application/json", "{\"error\":\"target required\"}"); }
        else {
            uint64_t id = mgr.add(tgt, settings_from_query(q));
            send_response(fd, "200 OK", "application/json", "{\"id\":" + std::to_string(id) + "}");
        }
    } else if (path == "/api/update") {
        if (!q.count("id")) {
            send_response(fd, "400 Bad Request", "application/json", "{\"error\":\"id required\"}");
        } else {
            uint64_t id = std::stoull(q["id"]);
            auto cur = mgr.settings_of(id);
            if (!cur) {
                send_response(fd, "404 Not Found", "application/json", "{\"error\":\"unknown id\"}");
            } else {
                // Start from the target's current config and apply only the
                // fields present in the query, so partial edits are fine.
                Settings s = *cur;
                auto getd = [&](const char* k, double def) {
                    auto it = q.find(k); return it != q.end() ? std::stod(it->second) : def;
                };
                if (q.count("probe")) s.probe_interval = getd("probe", s.probe_interval);
                if (q.count("timeout")) s.timeout = getd("timeout", s.timeout);
                if (q.count("payload")) s.payload_size = static_cast<size_t>(getd("payload", s.payload_size));
                if (q.count("maxhops")) s.max_hops = static_cast<uint8_t>(getd("maxhops", s.max_hops));
                if (q.count("raw")) s.privileged = q.at("raw") != "0";
                if (q.count("src")) s.source_addr = q.at("src");
                if (q.count("family")) {
                    std::string fam = q.at("family");
                    s.family = fam == "v4" ? FamilyPref::V4 : fam == "v6" ? FamilyPref::V6 : FamilyPref::Auto;
                }
                mgr.update(id, s);
                send_response(fd, "200 OK", "application/json", "{\"ok\":true}");
            }
        }
    } else if (path == "/api/remove") {
        if (q.count("id")) mgr.remove(std::stoull(q["id"]));
        send_response(fd, "200 OK", "application/json", "{}");
    } else if (path == "/api/stop") {
        if (q.count("id")) mgr.stop(std::stoull(q["id"]));
        send_response(fd, "200 OK", "application/json", "{}");
    } else if (path == "/api/pause") {
        if (q.count("id")) mgr.pause(std::stoull(q["id"]), !q.count("on") || q["on"] != "0");
        send_response(fd, "200 OK", "application/json", "{}");
    } else {
        // static files from the built frontend
        std::string rel = (path == "/") ? "/index.html" : path;
        std::string full = dist + rel;
        std::ifstream f(full, std::ios::binary);
        if (f) {
            std::ostringstream ss; ss << f.rdbuf();
            send_response(fd, "200 OK", content_type(full), ss.str());
        } else {
            send_response(fd, "404 Not Found", "text/plain",
                          "NetPulse backend is running. Build the frontend (web/) or run Vite dev server.");
        }
    }
    CLOSESOCK(fd);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int port = (argc > 1) ? std::atoi(argv[1]) : 8787;
    std::string dist = (argc > 2) ? argv[2] : "web/dist";

    Manager mgr;
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "bind failed on port %d\n", port);
        return 1;
    }
    listen(srv, 64);
    std::printf("NetPulse backend on http://127.0.0.1:%d  (serving '%s' if built)\n", port, dist.c_str());
    std::fflush(stdout);

    for (;;) {
        sockaddr_in cli{};
        socklen_t cl = sizeof(cli);
        int fd = static_cast<int>(::accept(srv, reinterpret_cast<sockaddr*>(&cli), &cl));
        if (fd < 0) continue;
        std::thread(handle, fd, std::ref(mgr), dist).detach();
    }
}
