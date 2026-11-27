#include <cstdint>
#include <cstring>
#include <string>

#include "shim_call.h"
#include "test.h"

using tcall::call;

namespace {

constexpr const char* kW = "ws2_32.dll";
constexpr uint64_t kBadSocket = ~0ull;

struct WinSockaddrIn {
    int16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
};

struct WinFdSet {
    uint32_t count;
    uint64_t fds[64];
};

uint32_t last() { return static_cast<uint32_t>(call<int>(kW, "WSAGetLastError")); }

WinSockaddrIn make_addr(const char* ip, uint16_t port) {
    WinSockaddrIn a{};
    a.family = 2;
    a.port = call<uint16_t>(kW, "htons", port);
    a.addr = call<uint32_t>(kW, "inet_addr", ip);
    return a;
}

}  // namespace

TEST(ws2_startup_and_byte_order) {
    uint8_t data[408];
    std::memset(data, 0xAA, sizeof data);
    CHECK_EQ(call<int>(kW, "WSAStartup", uint16_t{0x0202}, data), 0);
    uint16_t version;
    std::memcpy(&version, data, 2);
    CHECK_EQ(version, 0x0202);
    CHECK(std::strstr(reinterpret_cast<char*>(data + 16), "offline") != nullptr);
    CHECK_EQ(call<int>(kW, "WSAStartup", uint16_t{0x0003}, data), 10092);
    CHECK_EQ(call<uint16_t>(kW, "htons", uint16_t{0x1234}), 0x3412);
    CHECK_EQ(call<uint16_t>(kW, "ntohs", uint16_t{0x1234}), 0x3412);
    CHECK_EQ(call<uint32_t>(kW, "htonl", 0x01020304u), 0x04030201u);
    CHECK_EQ(call<uint32_t>(kW, "ntohl", 0x01020304u), 0x04030201u);
    CHECK_EQ(call<uint32_t>(kW, "inet_addr", "127.0.0.1"), 0x0100007Fu);
    char* text = call<char*>(kW, "inet_ntoa", 0x0100007Fu);
    CHECK(std::string(text) == "127.0.0.1");
    CHECK_EQ(call<uint32_t>(kW, "inet_addr", "not-an-ip"), 0xFFFFFFFFu);
}

TEST(ws2_ordinals_resolve_to_the_named_functions) {
    for (auto [ord, name] : {std::pair{"#2", "bind"}, std::pair{"#4", "connect"}, std::pair{"#20", "sendto"}, std::pair{"#23", "socket"},
                             std::pair{"#111", "WSAGetLastError"}, std::pair{"#115", "WSAStartup"}, std::pair{"#151", "__WSAFDIsSet"}}) {
        const auto a = rt::shims::find_entry("ws2_32.dll", ord), b = rt::shims::find_entry("ws2_32.dll", name);
        CHECK(a.fn == b.fn && a.frame == b.frame && (a.fn || a.frame));
    }
}

TEST(ws2_loopback_tcp_roundtrip_with_windows_layouts) {
    const uint64_t server = call<uint64_t>(kW, "socket", 2, 1, 6);
    CHECK(server != kBadSocket);
    WinSockaddrIn addr = make_addr("127.0.0.1", 0);
    CHECK_EQ(call<int>(kW, "bind", server, &addr, 16), 0);
    WinSockaddrIn bound{};
    int len = 16;
    CHECK_EQ(call<int>(kW, "getsockname", server, &bound, &len), 0);
    CHECK_EQ(len, 16);
    CHECK_EQ(bound.family, 2);
    CHECK(bound.port != 0 && bound.addr == 0x0100007Fu);
    CHECK_EQ(call<int>(kW, "listen", server, 4), 0);

    const uint64_t client = call<uint64_t>(kW, "socket", 2, 1, 6);
    CHECK_EQ(call<int>(kW, "connect", client, &bound, 16), 0);
    WinSockaddrIn peer{};
    len = 16;
    const uint64_t conn = call<uint64_t>(kW, "accept", server, &peer, &len);
    CHECK(conn != kBadSocket);
    CHECK(peer.family == 2 && peer.addr == 0x0100007Fu);

    CHECK_EQ(call<int>(kW, "send", client, "ping", 4, 0), 4);
    WinFdSet readable{};
    readable.count = 1;
    readable.fds[0] = conn;
    int32_t timeout[2] = {1, 0};
    CHECK_EQ(call<int>(kW, "select", 0, &readable, static_cast<WinFdSet*>(nullptr), static_cast<WinFdSet*>(nullptr), timeout), 1);
    CHECK_EQ(readable.count, 1u);
    CHECK_EQ(call<int>(kW, "__WSAFDIsSet", conn, &readable), 1);
    char buf[16] = {};
    CHECK_EQ(call<int>(kW, "recv", conn, buf, 16, 0), 4);
    CHECK(std::string(buf, 4) == "ping");
    CHECK_EQ(call<int>(kW, "send", conn, "pong!", 5, 0), 5);
    CHECK_EQ(call<int>(kW, "recv", client, buf, 16, 0x2), 5);
    CHECK_EQ(call<int>(kW, "recv", client, buf, 16, 0), 5);

    uint32_t nonblocking = 1;
    CHECK_EQ(call<int>(kW, "ioctlsocket", client, static_cast<int32_t>(0x8004667E), &nonblocking), 0);
    CHECK_EQ(call<int>(kW, "recv", client, buf, 16, 0), -1);
    CHECK_EQ(last(), 10035u);
    int one = 1;
    CHECK_EQ(call<int>(kW, "setsockopt", client, 6, 1, reinterpret_cast<const char*>(&one), 4), 0);
    uint32_t timeout_ms = 250;
    CHECK_EQ(call<int>(kW, "setsockopt", client, 0xFFFF, 0x1006, reinterpret_cast<const char*>(&timeout_ms), 4), 0);
    uint32_t got = 0;
    int optlen = 4;
    CHECK_EQ(call<int>(kW, "getsockopt", client, 0xFFFF, 0x1006, reinterpret_cast<char*>(&got), &optlen), 0);
    CHECK_EQ(got, 250u);
    CHECK_EQ(call<int>(kW, "setsockopt", client, 0xFFFF, 0x9999, reinterpret_cast<const char*>(&one), 4), -1);
    CHECK_EQ(last(), 10042u);
    CHECK_EQ(call<int>(kW, "shutdown", client, 2), 0);
    CHECK_EQ(call<int>(kW, "closesocket", client), 0);
    CHECK_EQ(call<int>(kW, "closesocket", conn), 0);
    CHECK_EQ(call<int>(kW, "closesocket", server), 0);
    CHECK_EQ(call<int>(kW, "closesocket", server), -1);
    CHECK_EQ(last(), 10009u);
}

TEST(ws2_udp_loopback_sendto_recvfrom) {
    const uint64_t a = call<uint64_t>(kW, "socket", 2, 2, 17), b = call<uint64_t>(kW, "socket", 2, 2, 17);
    WinSockaddrIn any = make_addr("0.0.0.0", 0);
    CHECK_EQ(call<int>(kW, "bind", a, &any, 16), 0);
    WinSockaddrIn bound{};
    int len = 16;
    CHECK_EQ(call<int>(kW, "getsockname", a, &bound, &len), 0);
    CHECK_EQ(bound.addr, 0x0100007Fu);
    CHECK_EQ(call<int>(kW, "sendto", b, "dgram", 5, 0, &bound, 16), 5);
    char buf[16] = {};
    WinSockaddrIn from{};
    len = 16;
    CHECK_EQ(call<int>(kW, "recvfrom", a, buf, 16, 0, &from, &len), 5);
    CHECK(std::string(buf, 5) == "dgram");
    CHECK_EQ(from.family, 2);
    call<int>(kW, "closesocket", a);
    call<int>(kW, "closesocket", b);
}

TEST(ws2_refuses_everything_off_loopback) {
    const uint64_t s = call<uint64_t>(kW, "socket", 2, 1, 6);
    WinSockaddrIn remote = make_addr("8.8.8.8", 53);
    CHECK_EQ(call<int>(kW, "connect", s, &remote, 16), -1);
    CHECK_EQ(last(), 10051u);
    CHECK_EQ(call<int>(kW, "bind", s, &remote, 16), -1);
    CHECK_EQ(last(), 10049u);
    CHECK_EQ(call<int>(kW, "sendto", s, "x", 1, 0, &remote, 16), -1);
    CHECK_EQ(last(), 10051u);
    CHECK_EQ(call<int>(kW, "connect", s, static_cast<void*>(nullptr), 16), -1);
    CHECK_EQ(last(), 10014u);
    call<int>(kW, "closesocket", s);
    CHECK_EQ(call<uint64_t>(kW, "socket", 99, 1, 6), kBadSocket);
    CHECK_EQ(last(), 10047u);
}

TEST(ws2_name_resolution_is_loopback_only) {
    struct AddrInfo {
        int32_t flags, family, socktype, protocol;
        uint64_t addrlen;
        char* canon;
        WinSockaddrIn* addr;
        AddrInfo* next;
    };
    AddrInfo hints{};
    hints.family = 2;
    hints.socktype = 1;
    AddrInfo* res = nullptr;
    CHECK_EQ(call<int>(kW, "getaddrinfo", "localhost", "8080", &hints, &res), 0);
    CHECK(res != nullptr);
    CHECK(res && res->family == 2 && res->socktype == 1 && res->addrlen == 16);
    CHECK(res && res->addr && res->addr->family == 2 && res->addr->addr == 0x0100007Fu);
    CHECK(res && res->addr && call<uint16_t>(kW, "ntohs", res->addr->port) == 8080);
    call<void>(kW, "freeaddrinfo", res);
    res = nullptr;
    CHECK_EQ(call<int>(kW, "getaddrinfo", "example.com", "80", &hints, &res), 11001);
    CHECK(res == nullptr);
    CHECK_EQ(call<int>(kW, "getaddrinfo", "0127.0.0.1", "80", &hints, &res), 11001);
    CHECK_EQ(call<int>(kW, "getaddrinfo", "127.0.0.1", "80", &hints, &res), 0);
    call<void>(kW, "freeaddrinfo", res);

    struct Hostent {
        char* name;
        char** aliases;
        int16_t type, length;
        char** addrs;
    };
    auto* h = call<Hostent*>(kW, "gethostbyname", "localhost");
    CHECK(h && h->type == 2 && h->length == 4 && h->addrs && h->addrs[0]);
    CHECK(h && static_cast<uint8_t>(h->addrs[0][0]) == 127);
    CHECK(call<Hostent*>(kW, "gethostbyname", "example.com") == nullptr);
    CHECK_EQ(last(), 11001u);
    char name[64];
    CHECK_EQ(call<int>(kW, "gethostname", name, 64), 0);
    CHECK(name[0] != 0);
}
