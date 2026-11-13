#include "net_udp.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>

namespace hm::net {

namespace {

std::atomic<int> g_wsa_refcount{0};

bool acquire_wsa() {
    if (g_wsa_refcount.fetch_add(1) == 0) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            g_wsa_refcount.fetch_sub(1);
            return false;
        }
    }
    return true;
}

void release_wsa() {
    if (g_wsa_refcount.fetch_sub(1) == 1) {
        WSACleanup();
    }
}

} // namespace

std::string UdpEndpoint::to_string() const {
    in_addr addr{};
    addr.s_addr = ip_be;
    char ip_str[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%s:%u", ip_str, ntohs(port_be));
    return std::string(buf);
}

bool UdpEndpoint::parse(const std::string& host_port, hal::u16 default_port, UdpEndpoint& out) {
    if (host_port.empty()) return false;
    std::string host = host_port;
    hal::u16 port = default_port;

    const auto colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        const std::string port_str = host_port.substr(colon + 1);
        if (!port_str.empty()) {
            const long p = std::strtol(port_str.c_str(), nullptr, 10);
            if (p <= 0 || p > 65535) return false;
            port = static_cast<hal::u16>(p);
        }
    }
    if (host == "localhost") host = "127.0.0.1";

    in_addr addr{};
    if (inet_pton(AF_INET, host.c_str(), &addr) != 1) {
        return false;
    }

    out.ip_be = addr.s_addr;
    out.port_be = htons(port);
    return true;
}

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open(hal::u16 bind_port) {
    close();
    if (!acquire_wsa()) return false;
    wsa_acquired_ = true;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        close();
        return false;
    }

    u_long nonblock = 1;
    if (ioctlsocket(s, FIONBIO, &nonblock) != 0) {
        closesocket(s);
        close();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(bind_port);

    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        close();
        return false;
    }

    handle_ = static_cast<std::uintptr_t>(s);
    return true;
}

void UdpSocket::close() {
    if (handle_ != INVALID_HANDLE) {
        closesocket(static_cast<SOCKET>(handle_));
        handle_ = INVALID_HANDLE;
    }
    if (wsa_acquired_) {
        release_wsa();
        wsa_acquired_ = false;
    }
}

hal::u16 UdpSocket::bound_port() const {
    if (!is_open()) return 0;
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getsockname(static_cast<SOCKET>(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

bool UdpSocket::send_to(const UdpEndpoint& dst, const void* data, std::size_t size) {
    if (!is_open() || !dst.is_valid() || !data || size == 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = dst.ip_be;
    addr.sin_port = dst.port_be;

    const int sent = sendto(
        static_cast<SOCKET>(handle_),
        reinterpret_cast<const char*>(data),
        static_cast<int>(size),
        0,
        reinterpret_cast<const sockaddr*>(&addr),
        sizeof(addr));
    return sent == static_cast<int>(size);
}

int UdpSocket::recv_from(UdpEndpoint& out_src, void* buf, std::size_t max_len) {
    if (!is_open() || !buf || max_len == 0) return -1;
    sockaddr_in src{};
    int src_len = sizeof(src);
    const int n = recvfrom(
        static_cast<SOCKET>(handle_),
        reinterpret_cast<char*>(buf),
        static_cast<int>(max_len),
        0,
        reinterpret_cast<sockaddr*>(&src),
        &src_len);
    if (n >= 0) {
        out_src.ip_be = src.sin_addr.s_addr;
        out_src.port_be = src.sin_port;
        return n;
    }

    const int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAECONNRESET) {
        return 0;
    }
    return -1;
}

} // namespace hm::net
