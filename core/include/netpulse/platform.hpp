// Guaranteed, link-independent Winsock initialization.
//
// WHY THIS FILE EXISTS: on Windows, WSAStartup() must run before ANY other
// Winsock call (getaddrinfo, socket, select, ...). It used to live only as a
// namespace-scope static object inside transport.cpp. That is NOT reliable:
// when netpulse_core is linked as a static library, the linker only pulls in
// .obj files needed to resolve a symbol some other .obj references. A binary
// that never calls into transport.cpp (e.g. a unit test that only exercises
// Session::resolve(), which calls getaddrinfo() directly) will silently never
// link transport.obj — so WSAStartup() never runs — and the first Winsock
// call crashes. This bit us in exactly that way: netpulse_tests.exe crashed
// instantly under the debugger because it calls getaddrinfo() via
// Session::resolve() but never references Prober/list_interfaces.
//
// Fix: make initialization a function, called explicitly at the top of every
// place that touches a socket API (Session::resolve, Prober::Prober). A
// function-local static runs exactly once, on first call, regardless of which
// .obj files the linker decided to include.
#pragma once

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <timeapi.h>
#  pragma comment(lib, "Ws2_32.lib")
#  pragma comment(lib, "Winmm.lib")
#endif

namespace netpulse {

inline void ensure_winsock_ready() {
#ifdef _WIN32
    struct WsaInit {
        WsaInit() {
            WSADATA d;
            WSAStartup(MAKEWORD(2, 2), &d);
            timeBeginPeriod(1); // 1ms timer resolution for accurate RTTs
        }
        ~WsaInit() {
            timeEndPeriod(1);
            WSACleanup();
        }
    };
    static WsaInit g_wsa; // constructed on first call, from whichever TU calls first
#endif
}

} // namespace netpulse
