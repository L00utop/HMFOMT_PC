#pragma once

#include "hal/gba_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hm::net {

struct UdpEndpoint {
    hal::u32 ip_be = 0;   // Network byte order IPv4
    hal::u16 port_be = 0; // Network byte order port

    bool operator==(const UdpEndpoint& o) const {
        return ip_be == o.ip_be && port_be == o.port_be;
    }
    bool is_valid() const { return ip_be != 0 && port_be != 0; }
    std::string to_string() const;
    static bool parse(const std::string& host_port, hal::u16 default_port, UdpEndpoint& out);
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open(hal::u16 bind_port = 0);
    void close();
    bool is_open() const { return handle_ != INVALID_HANDLE; }
    hal::u16 bound_port() const;

    bool send_to(const UdpEndpoint& dst, const void* data, std::size_t size);
    // Returns byte count > 0 if packet received, 0 if would-block, -1 on fatal error
    int recv_from(UdpEndpoint& out_src, void* buf, std::size_t max_len);

private:
    static constexpr std::uintptr_t INVALID_HANDLE = ~static_cast<std::uintptr_t>(0);
    std::uintptr_t handle_ = INVALID_HANDLE;
    bool wsa_acquired_ = false;
};

} // namespace hm::net
