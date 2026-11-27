// ws2_offline: Winsock2 (ws2_32.dll) for a mapped title, over BSD sockets, restricted to loopback.
//
// Nothing in Leonida goes online. macOS has no Winsock, so ws2_32 is implemented here on top of the host's sockets, with
// Windows layouts (sockaddr_in has no length byte, AF_INET6 is 23, fd_set is a counted array of 64-bit SOCKETs, timeval
// is two 32-bit longs, ADDRINFOA has its fields in another order, WSADATA is the Win64 layout) and Winsock error codes.
// A SOCKET is the host file descriptor. The title imports these by name and by ordinal; both are registered.
//   - connect (#4) and sendto (#20) permit only IPv4 loopback (127.0.0.0/8), IPv6 loopback (::1) and IPv4-mapped
//     IPv6 loopback. Every other destination fails with WSAENETUNREACH (10051).
//   - bind (#2) rewrites wildcard binds (0.0.0.0 or ::) to 127.0.0.1 or ::1 so no external peer can connect in, allows
//     explicit loopback binds, and refuses any other address with WSAEADDRNOTAVAIL (10049).
//   - getaddrinfo, gethostbyname (#52) and gethostbyaddr (#51) permit only null/empty nodes, "localhost" and loopback
//     IP literals; everything else fails with WSAHOST_NOT_FOUND without sending DNS packets.
// Each refused destination is logged once. Not served (they resolve to named logging stubs): WSAConnect*, WSAIoctl,
// ConnectEx, WSASend*, GetAddrInfoW/ExW, getnameinfo, WSALookupService*, WSAAsync*.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <gta6rt/os.h>

#include "shims.h"

namespace rt::ws2 {
namespace {

constexpr uint64_t kInvalidSocket = ~0ull;
constexpr int kSocketError = -1;

enum Wsa : uint32_t {
    WSAEINTR = 10004, WSAEBADF = 10009, WSAEACCES = 10013, WSAEFAULT = 10014, WSAEINVAL = 10022, WSAEMFILE = 10024,
    WSAEWOULDBLOCK = 10035, WSAEINPROGRESS = 10036, WSAEALREADY = 10037, WSAENOTSOCK = 10038, WSAEDESTADDRREQ = 10039,
    WSAEMSGSIZE = 10040, WSAEPROTOTYPE = 10041, WSAENOPROTOOPT = 10042, WSAEPROTONOSUPPORT = 10043,
    WSAESOCKTNOSUPPORT = 10044, WSAEOPNOTSUPP = 10045, WSAEAFNOSUPPORT = 10047, WSAEADDRINUSE = 10048,
    WSAEADDRNOTAVAIL = 10049, WSAENETDOWN = 10050, WSAENETUNREACH = 10051, WSAECONNABORTED = 10053,
    WSAECONNRESET = 10054, WSAENOBUFS = 10055, WSAEISCONN = 10056, WSAENOTCONN = 10057, WSAESHUTDOWN = 10058,
    WSAETIMEDOUT = 10060, WSAECONNREFUSED = 10061, WSAEHOSTUNREACH = 10065, WSATYPE_NOT_FOUND = 10109,
    WSAHOST_NOT_FOUND = 11001, WSATRY_AGAIN = 11002, WSANO_RECOVERY = 11003, WSA_NOT_ENOUGH_MEMORY = 8,
};

constexpr int kWinAfInet = 2, kWinAfInet6 = 23;

uint32_t wsa_from_errno(int e) {
    switch (e) {
    case EINTR: return WSAEINTR;
    case EBADF: return WSAEBADF;
    case EACCES: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: case ENFILE: return WSAEMFILE;
    case EWOULDBLOCK: return WSAEWOULDBLOCK;
    case EINPROGRESS: return WSAEINPROGRESS;
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EPROTOTYPE: return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: return WSAECONNRESET;
    case ENOBUFS: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ESHUTDOWN: return WSAESHUTDOWN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    case EPIPE: return WSAECONNRESET;
    default: return WSAEINVAL;
    }
}

int fail_errno() {
    gta6rt_last_error_set(wsa_from_errno(errno));
    return kSocketError;
}

int fail_wsa(uint32_t code) {
    gta6rt_last_error_set(code);
    return kSocketError;
}

void log_refused(const char* op, const std::string& target) {
    static auto* mutex = new std::mutex;
    static auto* logged = new std::set<std::string>;
    std::lock_guard lock(*mutex);
    if (logged->insert(std::string(op) + " " + target).second)
        gta6rt_log(GTA6RT_LOG_INFO, "ws2", "offline, refused %s %s", op, target.c_str());
}

// ---- Windows layouts

struct WinSockaddrIn {
    int16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t zero[8];
};
struct WinSockaddrIn6 {
    int16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    uint8_t sin6_addr[16];
    uint32_t sin6_scope_id;
};
static_assert(sizeof(WinSockaddrIn) == 16 && sizeof(WinSockaddrIn6) == 28);

struct WinFdSet {
    uint32_t fd_count;
    uint64_t fd_array[64];
};
static_assert(sizeof(WinFdSet) == 520);

struct WinTimeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

struct WinAddrInfo {
    int32_t ai_flags, ai_family, ai_socktype, ai_protocol;
    uint64_t ai_addrlen;
    char* ai_canonname;
    void* ai_addr;
    WinAddrInfo* ai_next;
};
static_assert(sizeof(WinAddrInfo) == 48 && offsetof(WinAddrInfo, ai_addr) == 32);

struct WinHostent {
    char* h_name;
    char** h_aliases;
    int16_t h_addrtype;
    int16_t h_length;
    char** h_addr_list;
};
static_assert(sizeof(WinHostent) == 32);

struct WinWsaData {
    uint16_t wVersion, wHighVersion;
    uint16_t iMaxSockets, iMaxUdpDg;
    char* lpVendorInfo;
    char szDescription[257];
    char szSystemStatus[129];
};
static_assert(offsetof(WinWsaData, szDescription) == 16);

int host_family(int win_af) {
    switch (win_af) {
    case kWinAfInet: return AF_INET;
    case kWinAfInet6: return AF_INET6;
    case 0: return AF_UNSPEC;
    default: return -1;
    }
}

int win_family(int host_af) { return host_af == AF_INET ? kWinAfInet : host_af == AF_INET6 ? kWinAfInet6 : host_af; }

bool win_to_host(const void* sa, int len, sockaddr_storage& out, socklen_t& out_len) {
    std::memset(&out, 0, sizeof out);
    if (!sa || len < 2) return false;
    int16_t family;
    std::memcpy(&family, sa, 2);
    if (family == kWinAfInet && len >= static_cast<int>(sizeof(WinSockaddrIn))) {
        const auto* w = static_cast<const WinSockaddrIn*>(sa);
        auto* h = reinterpret_cast<sockaddr_in*>(&out);
        h->sin_len = sizeof *h;
        h->sin_family = AF_INET;
        h->sin_port = w->sin_port;
        h->sin_addr.s_addr = w->sin_addr;
        out_len = sizeof *h;
        return true;
    }
    if (family == kWinAfInet6 && len >= static_cast<int>(sizeof(WinSockaddrIn6))) {
        const auto* w = static_cast<const WinSockaddrIn6*>(sa);
        auto* h = reinterpret_cast<sockaddr_in6*>(&out);
        h->sin6_len = sizeof *h;
        h->sin6_family = AF_INET6;
        h->sin6_port = w->sin6_port;
        h->sin6_flowinfo = w->sin6_flowinfo;
        std::memcpy(&h->sin6_addr, w->sin6_addr, 16);
        h->sin6_scope_id = w->sin6_scope_id;
        out_len = sizeof *h;
        return true;
    }
    return false;
}

// Copies a host address into a guest buffer, honouring the in/out length of getsockname and friends.
void host_to_win(const sockaddr* h, void* out, int* inout_len) {
    if (!out || !inout_len) return;
    uint8_t buf[28] = {};
    int size = 0;
    if (h->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(h);
        WinSockaddrIn w{};
        w.sin_family = kWinAfInet;
        w.sin_port = in->sin_port;
        w.sin_addr = in->sin_addr.s_addr;
        std::memcpy(buf, &w, sizeof w);
        size = sizeof w;
    } else if (h->sa_family == AF_INET6) {
        const auto* in = reinterpret_cast<const sockaddr_in6*>(h);
        WinSockaddrIn6 w{};
        w.sin6_family = kWinAfInet6;
        w.sin6_port = in->sin6_port;
        w.sin6_flowinfo = in->sin6_flowinfo;
        std::memcpy(w.sin6_addr, &in->sin6_addr, 16);
        w.sin6_scope_id = in->sin6_scope_id;
        std::memcpy(buf, &w, sizeof w);
        size = sizeof w;
    }
    const int n = *inout_len < size ? *inout_len : size;
    if (n > 0) std::memcpy(out, buf, static_cast<size_t>(n));
    *inout_len = size;
}

bool is_loopback(const sockaddr_storage& a) {
    if (a.ss_family == AF_INET) {
        const uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(&a)->sin_addr.s_addr);
        return (ip >> 24) == 127;
    }
    if (a.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&a);
        if (IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)) return true;
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) return in6->sin6_addr.s6_addr[12] == 127;
    }
    return false;
}

bool is_unspecified(const sockaddr_storage& a) {
    if (a.ss_family == AF_INET) return reinterpret_cast<const sockaddr_in*>(&a)->sin_addr.s_addr == 0;
    if (a.ss_family == AF_INET6) return IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<const sockaddr_in6*>(&a)->sin6_addr);
    return false;
}

std::string describe(const sockaddr_storage& a) {
    char host[64] = "?";
    uint16_t port = 0;
    if (a.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&a);
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
        port = ntohs(in->sin_port);
    } else if (a.ss_family == AF_INET6) {
        const auto* in = reinterpret_cast<const sockaddr_in6*>(&a);
        inet_ntop(AF_INET6, &in->sin6_addr, host, sizeof host);
        port = ntohs(in->sin6_port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

// ---- startup and byte order

int WINAPI w_WSAStartup(uint16_t version, WinWsaData* data) {
    if (!data) return static_cast<int>(WSAEFAULT);
    const uint8_t major = static_cast<uint8_t>(version & 0xff), minor = static_cast<uint8_t>(version >> 8);
    if (major == 0 || major > 2 || (major == 2 && minor > 2)) return 10092;
    std::memset(data, 0, sizeof *data);
    data->wVersion = major >= 2 ? 0x0202 : version;
    data->wHighVersion = 0x0202;
    data->iMaxSockets = 0;
    data->iMaxUdpDg = 0;
    std::snprintf(data->szDescription, sizeof data->szDescription, "gta6rt offline Winsock (loopback only)");
    std::snprintf(data->szSystemStatus, sizeof data->szSystemStatus, "Running");
    return 0;
}
int WINAPI w_WSACleanup() { return 0; }
int WINAPI w_WSAGetLastError() { return static_cast<int>(gta6rt_last_error_get()); }
void WINAPI w_WSASetLastError(int e) { gta6rt_last_error_set(static_cast<uint32_t>(e)); }

uint16_t WINAPI w_htons(uint16_t v) { return htons(v); }
uint16_t WINAPI w_ntohs(uint16_t v) { return ntohs(v); }
uint32_t WINAPI w_htonl(uint32_t v) { return htonl(v); }
uint32_t WINAPI w_ntohl(uint32_t v) { return ntohl(v); }

uint32_t WINAPI w_inet_addr(const char* s) {
    if (!s) return 0xFFFFFFFFu;
    return ::inet_addr(s);
}

char* WINAPI w_inet_ntoa(uint32_t addr) {
    static thread_local char buf[16];
    in_addr a;
    a.s_addr = addr;
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

// ---- sockets

uint64_t WINAPI w_socket(int af, int type, int protocol) {
    const int hf = host_family(af);
    if (hf < 0) {
        fail_wsa(WSAEAFNOSUPPORT);
        return kInvalidSocket;
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != 0) {
        fail_wsa(WSAESOCKTNOSUPPORT);
        return kInvalidSocket;
    }
    const int fd = ::socket(hf == AF_UNSPEC ? AF_INET : hf, type ? type : SOCK_STREAM, protocol);
    if (fd < 0) {
        fail_errno();
        return kInvalidSocket;
    }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    return static_cast<uint64_t>(fd);
}

int WINAPI w_closesocket(uint64_t s) {
    if (::close(static_cast<int>(s)) != 0) return fail_errno();
    return 0;
}

int WINAPI w_shutdown(uint64_t s, int how) {
    if (::shutdown(static_cast<int>(s), how) != 0) return fail_errno();
    return 0;
}

int WINAPI w_bind(uint64_t s, const void* name, int len) {
    sockaddr_storage a;
    socklen_t hl = 0;
    if (!win_to_host(name, len, a, hl)) return fail_wsa(WSAEFAULT);
    if (is_unspecified(a)) {
        if (a.ss_family == AF_INET) reinterpret_cast<sockaddr_in*>(&a)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        else reinterpret_cast<sockaddr_in6*>(&a)->sin6_addr = in6addr_loopback;
    } else if (!is_loopback(a)) {
        log_refused("bind", describe(a));
        return fail_wsa(WSAEADDRNOTAVAIL);
    }
    if (::bind(static_cast<int>(s), reinterpret_cast<sockaddr*>(&a), hl) != 0) return fail_errno();
    return 0;
}

int WINAPI w_connect(uint64_t s, const void* name, int len) {
    sockaddr_storage a;
    socklen_t hl = 0;
    if (!win_to_host(name, len, a, hl)) return fail_wsa(WSAEFAULT);
    if (!is_loopback(a)) {
        log_refused("connect", describe(a));
        return fail_wsa(WSAENETUNREACH);
    }
    if (::connect(static_cast<int>(s), reinterpret_cast<sockaddr*>(&a), hl) != 0) return fail_errno();
    return 0;
}

int WINAPI w_listen(uint64_t s, int backlog) {
    if (::listen(static_cast<int>(s), backlog) != 0) return fail_errno();
    return 0;
}

uint64_t WINAPI w_accept(uint64_t s, void* addr, int* len) {
    sockaddr_storage a;
    socklen_t hl = sizeof a;
    const int fd = ::accept(static_cast<int>(s), reinterpret_cast<sockaddr*>(&a), &hl);
    if (fd < 0) {
        fail_errno();
        return kInvalidSocket;
    }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    if (addr && len) host_to_win(reinterpret_cast<sockaddr*>(&a), addr, len);
    return static_cast<uint64_t>(fd);
}

int WINAPI w_getsockname(uint64_t s, void* name, int* len) {
    sockaddr_storage a;
    socklen_t hl = sizeof a;
    if (::getsockname(static_cast<int>(s), reinterpret_cast<sockaddr*>(&a), &hl) != 0) return fail_errno();
    host_to_win(reinterpret_cast<sockaddr*>(&a), name, len);
    return 0;
}

int WINAPI w_getpeername(uint64_t s, void* name, int* len) {
    sockaddr_storage a;
    socklen_t hl = sizeof a;
    if (::getpeername(static_cast<int>(s), reinterpret_cast<sockaddr*>(&a), &hl) != 0) return fail_errno();
    host_to_win(reinterpret_cast<sockaddr*>(&a), name, len);
    return 0;
}

int host_recv_flags(int win) {
    int f = 0;
    if (win & 0x1) f |= MSG_OOB;
    if (win & 0x2) f |= MSG_PEEK;
    if (win & 0x8) f |= MSG_WAITALL;
    return f;
}

int WINAPI w_recv(uint64_t s, char* buf, int len, int flags) {
    if (len < 0) return fail_wsa(WSAEFAULT);
    const ssize_t n = ::recv(static_cast<int>(s), buf, static_cast<size_t>(len), host_recv_flags(flags));
    if (n < 0) return fail_errno();
    return static_cast<int>(n);
}

int WINAPI w_recvfrom(uint64_t s, char* buf, int len, int flags, void* from, int* fromlen) {
    if (len < 0) return fail_wsa(WSAEFAULT);
    sockaddr_storage a;
    socklen_t hl = sizeof a;
    const ssize_t n = ::recvfrom(static_cast<int>(s), buf, static_cast<size_t>(len), host_recv_flags(flags),
                                 reinterpret_cast<sockaddr*>(&a), &hl);
    if (n < 0) return fail_errno();
    if (from && fromlen && hl > 0) host_to_win(reinterpret_cast<sockaddr*>(&a), from, fromlen);
    return static_cast<int>(n);
}

int WINAPI w_send(uint64_t s, const char* buf, int len, int flags) {
    if (len < 0) return fail_wsa(WSAEFAULT);
    int hf = 0;
    if (flags & 0x1) hf |= MSG_OOB;
    if (flags & 0x4) hf |= MSG_DONTROUTE;
    const ssize_t n = ::send(static_cast<int>(s), buf, static_cast<size_t>(len), hf);
    if (n < 0) return fail_errno();
    return static_cast<int>(n);
}

int WINAPI w_sendto(uint64_t s, const char* buf, int len, int flags, const void* to, int tolen) {
    if (len < 0) return fail_wsa(WSAEFAULT);
    int hf = 0;
    if (flags & 0x1) hf |= MSG_OOB;
    if (flags & 0x4) hf |= MSG_DONTROUTE;
    sockaddr_storage a;
    socklen_t hl = 0;
    if (to) {
        if (!win_to_host(to, tolen, a, hl)) return fail_wsa(WSAEFAULT);
        if (!is_loopback(a)) {
            log_refused("sendto", describe(a));
            return fail_wsa(WSAENETUNREACH);
        }
    }
    const ssize_t n = to ? ::sendto(static_cast<int>(s), buf, static_cast<size_t>(len), hf, reinterpret_cast<sockaddr*>(&a), hl)
                         : ::send(static_cast<int>(s), buf, static_cast<size_t>(len), hf);
    if (n < 0) return fail_errno();
    return static_cast<int>(n);
}

constexpr int kWinSolSocket = 0xFFFF;
constexpr int kWinSoReuseAddr = 0x4, kWinSoKeepAlive = 0x8, kWinSoBroadcast = 0x20, kWinSoLinger = 0x80,
              kWinSoSndBuf = 0x1001, kWinSoRcvBuf = 0x1002, kWinSoSndTimeo = 0x1005, kWinSoRcvTimeo = 0x1006,
              kWinSoError = 0x1007, kWinSoType = 0x1008, kWinSoDontLinger = ~0x80;

bool map_sockopt(int level, int name, int& host_level, int& host_name) {
    if (level == kWinSolSocket) {
        host_level = SOL_SOCKET;
        switch (name) {
        case kWinSoReuseAddr: host_name = SO_REUSEADDR; return true;
        case kWinSoKeepAlive: host_name = SO_KEEPALIVE; return true;
        case kWinSoBroadcast: host_name = SO_BROADCAST; return true;
        case kWinSoSndBuf: host_name = SO_SNDBUF; return true;
        case kWinSoRcvBuf: host_name = SO_RCVBUF; return true;
        case kWinSoError: host_name = SO_ERROR; return true;
        case kWinSoType: host_name = SO_TYPE; return true;
        default: return false;
        }
    }
    if (level == IPPROTO_TCP && name == TCP_NODELAY) {
        host_level = IPPROTO_TCP;
        host_name = TCP_NODELAY;
        return true;
    }
    if (level == IPPROTO_IPV6 && name == IPV6_V6ONLY) {
        host_level = IPPROTO_IPV6;
        host_name = IPV6_V6ONLY;
        return true;
    }
    return false;
}

int WINAPI w_setsockopt(uint64_t s, int level, int name, const char* val, int len) {
    const int fd = static_cast<int>(s);
    if (level == kWinSolSocket && (name == kWinSoRcvTimeo || name == kWinSoSndTimeo)) {
        if (!val || len < 4) return fail_wsa(WSAEFAULT);
        uint32_t ms;
        std::memcpy(&ms, val, 4);
        timeval tv{static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
        if (::setsockopt(fd, SOL_SOCKET, name == kWinSoRcvTimeo ? SO_RCVTIMEO : SO_SNDTIMEO, &tv, sizeof tv) != 0) return fail_errno();
        return 0;
    }
    if (level == kWinSolSocket && name == kWinSoLinger) {
        if (!val || len < 4) return fail_wsa(WSAEFAULT);
        uint16_t w[2];
        std::memcpy(w, val, 4);
        linger l{w[0], w[1]};
        if (::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l) != 0) return fail_errno();
        return 0;
    }
    if (level == kWinSolSocket && name == kWinSoDontLinger) {
        linger l{0, 0};
        if (::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l) != 0) return fail_errno();
        return 0;
    }
    int hl, hn;
    if (!map_sockopt(level, name, hl, hn)) return fail_wsa(WSAENOPROTOOPT);
    if (!val || len < 1) return fail_wsa(WSAEFAULT);
    int v = 0;
    std::memcpy(&v, val, len < 4 ? static_cast<size_t>(len) : 4);
    if (::setsockopt(fd, hl, hn, &v, sizeof v) != 0) return fail_errno();
    return 0;
}

int WINAPI w_getsockopt(uint64_t s, int level, int name, char* val, int* len) {
    const int fd = static_cast<int>(s);
    if (!val || !len) return fail_wsa(WSAEFAULT);
    if (level == kWinSolSocket && (name == kWinSoRcvTimeo || name == kWinSoSndTimeo)) {
        timeval tv;
        socklen_t tl = sizeof tv;
        if (::getsockopt(fd, SOL_SOCKET, name == kWinSoRcvTimeo ? SO_RCVTIMEO : SO_SNDTIMEO, &tv, &tl) != 0) return fail_errno();
        const uint32_t ms = static_cast<uint32_t>(tv.tv_sec * 1000 + tv.tv_usec / 1000);
        if (*len < 4) return fail_wsa(WSAEFAULT);
        std::memcpy(val, &ms, 4);
        *len = 4;
        return 0;
    }
    int hl, hn;
    if (!map_sockopt(level, name, hl, hn)) return fail_wsa(WSAENOPROTOOPT);
    int v = 0;
    socklen_t vl = sizeof v;
    if (::getsockopt(fd, hl, hn, &v, &vl) != 0) return fail_errno();
    if (name == kWinSoError && level == kWinSolSocket) v = static_cast<int>(wsa_from_errno(v) * (v != 0));
    const int n = *len < 4 ? *len : 4;
    std::memcpy(val, &v, static_cast<size_t>(n));
    *len = 4;
    return 0;
}

constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;

int WINAPI w_ioctlsocket(uint64_t s, int32_t cmd, uint32_t* arg) {
    const int fd = static_cast<int>(s);
    if (!arg) return fail_wsa(WSAEFAULT);
    if (static_cast<uint32_t>(cmd) == kFionbio) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0) return fail_errno();
        if (::fcntl(fd, F_SETFL, *arg ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) != 0) return fail_errno();
        return 0;
    }
    if (static_cast<uint32_t>(cmd) == kFionread) {
        int n = 0;
        if (::ioctl(fd, FIONREAD, &n) != 0) return fail_errno();
        *arg = static_cast<uint32_t>(n);
        return 0;
    }
    return fail_wsa(WSAEINVAL);
}

int fill_host_set(const WinFdSet* w, fd_set* h, int& max_fd) {
    FD_ZERO(h);
    if (!w) return 0;
    const uint32_t n = w->fd_count > 64 ? 64 : w->fd_count;
    for (uint32_t i = 0; i < n; ++i) {
        const int fd = static_cast<int>(w->fd_array[i]);
        if (fd < 0 || fd >= FD_SETSIZE) return -1;
        FD_SET(fd, h);
        if (fd > max_fd) max_fd = fd;
    }
    return 0;
}

int WINAPI w_select(int, WinFdSet* r, WinFdSet* w, WinFdSet* e, const WinTimeval* timeout) {
    fd_set hr, hw, he;
    int max_fd = -1;
    if (fill_host_set(r, &hr, max_fd) || fill_host_set(w, &hw, max_fd) || fill_host_set(e, &he, max_fd)) return fail_wsa(WSAENOTSOCK);
    if (!r && !w && !e) return fail_wsa(WSAEINVAL);
    timeval tv{};
    if (timeout) {
        tv.tv_sec = timeout->tv_sec;
        tv.tv_usec = timeout->tv_usec;
    }
    const int rc = ::select(max_fd + 1, r ? &hr : nullptr, w ? &hw : nullptr, e ? &he : nullptr, timeout ? &tv : nullptr);
    if (rc < 0) return fail_errno();
    auto copy_back = [](WinFdSet* out, fd_set* in) {
        if (!out) return;
        uint32_t k = 0;
        const uint32_t n = out->fd_count > 64 ? 64 : out->fd_count;
        for (uint32_t i = 0; i < n; ++i)
            if (FD_ISSET(static_cast<int>(out->fd_array[i]), in)) out->fd_array[k++] = out->fd_array[i];
        out->fd_count = k;
    };
    copy_back(r, &hr);
    copy_back(w, &hw);
    copy_back(e, &he);
    return rc;
}

int WINAPI w_wsa_fd_is_set(uint64_t s, const WinFdSet* set) {
    if (!set) return 0;
    for (uint32_t i = 0; i < set->fd_count && i < 64; ++i)
        if (set->fd_array[i] == s) return 1;
    return 0;
}

// ---- names

bool node_is_local(const char* node, bool& wildcard) {
    wildcard = false;
    if (!node || !*node) {
        wildcard = true;
        return true;
    }
    if (!strcasecmp(node, "localhost")) return true;
    in_addr a4;
    in6_addr a6;
    if (inet_pton(AF_INET, node, &a4) == 1) {
        if (std::strchr(node, '.') && node[0] == '0' && node[1] >= '0' && node[1] <= '9') return false;
        return (ntohl(a4.s_addr) >> 24) == 127;
    }
    if (inet_pton(AF_INET6, node, &a6) == 1) return IN6_IS_ADDR_LOOPBACK(&a6);
    return false;
}

int eai_to_wsa(int e) {
    switch (e) {
    case 0: return 0;
    case EAI_NONAME: return WSAHOST_NOT_FOUND;
    case EAI_SERVICE: return WSATYPE_NOT_FOUND;
    case EAI_FAMILY: return WSAEAFNOSUPPORT;
    case EAI_SOCKTYPE: return WSAESOCKTNOSUPPORT;
    case EAI_BADFLAGS: return WSAEINVAL;
    case EAI_AGAIN: return WSATRY_AGAIN;
    case EAI_MEMORY: return WSA_NOT_ENOUGH_MEMORY;
    default: return WSANO_RECOVERY;
    }
}

int WINAPI w_getaddrinfo(const char* node, const char* service, const WinAddrInfo* hints, WinAddrInfo** result) {
    if (!result) return static_cast<int>(WSAEINVAL);
    *result = nullptr;
    bool wildcard = false;
    if (!node_is_local(node, wildcard)) {
        log_refused("getaddrinfo", node);
        gta6rt_last_error_set(WSAHOST_NOT_FOUND);
        return static_cast<int>(WSAHOST_NOT_FOUND);
    }
    addrinfo hh{};
    if (hints) {
        hh.ai_flags = hints->ai_flags & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
        hh.ai_family = host_family(hints->ai_family) < 0 ? AF_UNSPEC : host_family(hints->ai_family);
        hh.ai_socktype = hints->ai_socktype;
        hh.ai_protocol = hints->ai_protocol;
    }
    const char* host_node = wildcard ? (hh.ai_flags & AI_PASSIVE ? nullptr : "localhost") : node;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host_node, service && *service ? service : nullptr, &hh, &res);
    if (rc != 0) {
        gta6rt_last_error_set(static_cast<uint32_t>(eai_to_wsa(rc)));
        return eai_to_wsa(rc);
    }
    WinAddrInfo* head = nullptr;
    WinAddrInfo** tail = &head;
    for (addrinfo* p = res; p; p = p->ai_next) {
        if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
        if (!(hh.ai_flags & AI_PASSIVE) || !wildcard) {
            sockaddr_storage ss;
            std::memcpy(&ss, p->ai_addr, p->ai_addrlen);
            if (!is_loopback(ss) && !is_unspecified(ss)) continue;
        }
        auto* w = static_cast<WinAddrInfo*>(std::calloc(1, sizeof(WinAddrInfo) + 28));
        if (!w) break;
        w->ai_flags = p->ai_flags;
        w->ai_family = win_family(p->ai_family);
        w->ai_socktype = p->ai_socktype;
        w->ai_protocol = p->ai_protocol;
        int len = 28;
        w->ai_addr = reinterpret_cast<uint8_t*>(w) + sizeof(WinAddrInfo);
        host_to_win(p->ai_addr, w->ai_addr, &len);
        w->ai_addrlen = static_cast<uint64_t>(len);
        if (p->ai_canonname) w->ai_canonname = strdup(p->ai_canonname);
        *tail = w;
        tail = &w->ai_next;
    }
    ::freeaddrinfo(res);
    if (!head) {
        gta6rt_last_error_set(WSAHOST_NOT_FOUND);
        return static_cast<int>(WSAHOST_NOT_FOUND);
    }
    *result = head;
    return 0;
}

void WINAPI w_freeaddrinfo(WinAddrInfo* p) {
    while (p) {
        WinAddrInfo* next = p->ai_next;
        std::free(p->ai_canonname);
        std::free(p);
        p = next;
    }
}

struct HostentStorage {
    WinHostent ent;
    char name[256];
    char* aliases[1];
    char* addrs[2];
    uint8_t addr[4];
};

WinHostent* WINAPI w_gethostbyname(const char* name) {
    bool wildcard = false;
    if (!name || !node_is_local(name, wildcard) || wildcard) {
        log_refused("gethostbyname", name ? name : "(null)");
        gta6rt_last_error_set(WSAHOST_NOT_FOUND);
        return nullptr;
    }
    static thread_local HostentStorage st;
    std::memset(&st, 0, sizeof st);
    std::snprintf(st.name, sizeof st.name, "%s", name);
    st.addr[0] = 127;
    st.addr[3] = 1;
    st.aliases[0] = nullptr;
    st.addrs[0] = reinterpret_cast<char*>(st.addr);
    st.addrs[1] = nullptr;
    st.ent.h_name = st.name;
    st.ent.h_aliases = st.aliases;
    st.ent.h_addrtype = kWinAfInet;
    st.ent.h_length = 4;
    st.ent.h_addr_list = st.addrs;
    return &st.ent;
}

WinHostent* WINAPI w_gethostbyaddr(const char* addr, int len, int type) {
    if (!addr || len != 4 || type != kWinAfInet || static_cast<uint8_t>(addr[0]) != 127) {
        log_refused("gethostbyaddr", "non-loopback");
        gta6rt_last_error_set(WSAHOST_NOT_FOUND);
        return nullptr;
    }
    return w_gethostbyname("localhost");
}

int WINAPI w_gethostname(char* name, int len) {
    if (!name || len <= 0) return fail_wsa(WSAEFAULT);
    char host[256];
    if (::gethostname(host, sizeof host) != 0) std::snprintf(host, sizeof host, "localhost");
    if (static_cast<int>(std::strlen(host)) + 1 > len) return fail_wsa(WSAEFAULT);
    std::strcpy(name, host);
    return 0;
}

}  // namespace
}  // namespace rt::ws2

namespace {
using namespace rt::ws2;
#define WS(name, ordinal, f)                              \
    SHIM("ws2_32.dll", name, f), SHIM("ws2_32.dll", ordinal, f), SHIM("wsock32.dll", name, f), SHIM("wsock32.dll", ordinal, f)
}  // namespace

SHIM_TABLE(ws2_offline,
    WS("accept", "#1", w_accept), WS("bind", "#2", w_bind), WS("closesocket", "#3", w_closesocket),
    WS("connect", "#4", w_connect), WS("getpeername", "#5", w_getpeername), WS("getsockname", "#6", w_getsockname),
    WS("getsockopt", "#7", w_getsockopt), WS("htonl", "#8", w_htonl), WS("htons", "#9", w_htons),
    WS("ioctlsocket", "#10", w_ioctlsocket), WS("inet_addr", "#11", w_inet_addr), WS("inet_ntoa", "#12", w_inet_ntoa),
    WS("listen", "#13", w_listen), WS("ntohl", "#14", w_ntohl), WS("ntohs", "#15", w_ntohs), WS("recv", "#16", w_recv),
    WS("recvfrom", "#17", w_recvfrom), WS("select", "#18", w_select), WS("send", "#19", w_send),
    WS("sendto", "#20", w_sendto), WS("setsockopt", "#21", w_setsockopt), WS("shutdown", "#22", w_shutdown),
    WS("socket", "#23", w_socket), WS("gethostbyaddr", "#51", w_gethostbyaddr), WS("gethostbyname", "#52", w_gethostbyname),
    WS("gethostname", "#57", w_gethostname), WS("WSAGetLastError", "#111", w_WSAGetLastError),
    WS("WSASetLastError", "#112", w_WSASetLastError), WS("WSAStartup", "#115", w_WSAStartup),
    WS("WSACleanup", "#116", w_WSACleanup), WS("__WSAFDIsSet", "#151", w_wsa_fd_is_set),
    SHIM("ws2_32.dll", "getaddrinfo", w_getaddrinfo), SHIM("ws2_32.dll", "freeaddrinfo", w_freeaddrinfo),
);
