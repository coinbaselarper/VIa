// winhttp_offline: WinHTTP for a mapped title, with no network.
//
// The package's XCurl.dll and libHttpClient.GDK.dll do their HTTP through WinHTTP: between them they import 16 functions
// from winhttp.dll. macOS has no WinHTTP and nothing here goes online, so every function a title can reach is answered
// in-process:
//   - WinHttpOpen, WinHttpConnect and WinHttpOpenRequest return local handles. WinHttpConnect resolves no name and
//     opens no socket.
//   - WinHttpSendRequest fails with ERROR_WINHTTP_CANNOT_CONNECT. On a session opened with WINHTTP_FLAG_ASYNC it
//     returns TRUE and reports the failure the way WinHTTP does, through the status callback:
//     WINHTTP_CALLBACK_STATUS_REQUEST_ERROR with WINHTTP_ASYNC_RESULT { API_SEND_REQUEST, ERROR_WINHTTP_CANNOT_CONNECT },
//     from a host thread, with the context the request was sent with (a context of 0 keeps the one set with
//     WINHTTP_OPTION_CONTEXT_VALUE).
//   - Reading a response (WinHttpReceiveResponse, WinHttpQueryHeaders, WinHttpQueryDataAvailable, WinHttpReadData) and
//     WinHttpWriteData fail with ERROR_WINHTTP_INCORRECT_HANDLE_STATE: no request ever got that far.
//   - Options, timeouts and request headers are accepted. WINHTTP_OPTION_CONTEXT_VALUE is kept per handle and
//     WINHTTP_OPTION_HANDLE_TYPE answered; WinHttpQueryOption reports ERROR_WINHTTP_INVALID_OPTION for the rest.
//   - A status callback set on a handle is inherited by the handles created under it, as in WinHTTP, and so is the
//     handle's context. WINHTTP_CALLBACK_STATUS_HANDLE_CREATED is reported when a child handle is made, and
//     WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING once per handle, last.
//   - WinHttpCrackUrl is a string parser, written here.
// Each refused server is logged once, which says what a title tried to reach. Status callbacks are guest functions,
// called through gta6rt_isa_call_guest.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include <gta6rt/isa.h>
#include <gta6rt/os.h>

#include "shims.h"
#include "util.h"

namespace rt::winhttp {
namespace {

constexpr uint32_t kErrorInvalidParameter = 87, kErrorInvalidHandle = 6, kErrorInsufficientBuffer = 122,
                   kErrorNotEnoughMemory = 8;
constexpr uint32_t kErrCannotConnect = 12029, kErrIncorrectHandleState = 12019, kErrIncorrectHandleType = 12018,
                   kErrInvalidOption = 12009, kErrOperationCancelled = 12017, kErrInvalidUrl = 12005,
                   kErrUnrecognizedScheme = 12006;
constexpr uint32_t kStatusRequestError = 0x00200000, kStatusHandleCreated = 0x00000400, kStatusHandleClosing = 0x00000800;
constexpr uint32_t kOptionContextValue = 45, kOptionHandleType = 9;
constexpr uint32_t kHandleTypeSession = 1, kHandleTypeConnect = 2, kHandleTypeRequest = 3;
constexpr uint32_t kFlagAsync = 0x10000000, kApiSendRequest = 5;
constexpr uint64_t kInvalidStatusCallback = ~0ull;

enum class Kind { Session, Connect, Request };

struct Handle {
    Kind kind = Kind::Session;
    Handle* parent = nullptr;
    bool async = false;
    std::atomic<long> refs{1};
    std::atomic<bool> closed{false};

    std::mutex mutex;
    uint64_t callback = 0;
    uint32_t notifications = 0;
    uint64_t context = 0;

    std::u16string server;
    uint16_t port = 0;
};

std::mutex& live_mutex() {
    static auto* m = new std::mutex;
    return *m;
}
std::set<Handle*>& live() {
    static auto* s = new std::set<Handle*>;
    return *s;
}

uint64_t to_hinternet(Handle* h) { return reinterpret_cast<uintptr_t>(h); }

Handle* acquire(uint64_t handle, bool session, bool connect, bool request, uint32_t& error) {
    Handle* h = reinterpret_cast<Handle*>(static_cast<uintptr_t>(handle));
    std::lock_guard lock(live_mutex());
    if (!handle || live().count(h) == 0) {
        error = kErrorInvalidHandle;
        return nullptr;
    }
    const bool ok = (h->kind == Kind::Session && session) || (h->kind == Kind::Connect && connect) ||
                    (h->kind == Kind::Request && request);
    if (!ok) {
        error = kErrIncorrectHandleType;
        return nullptr;
    }
    h->refs.fetch_add(1);
    return h;
}

void notify(Handle* h, uint32_t status, void* info, uint32_t length) {
    uint64_t callback;
    uint32_t notifications;
    uint64_t context;
    {
        std::lock_guard lock(h->mutex);
        callback = h->callback;
        notifications = h->notifications;
        context = h->context;
    }
    if (callback && callback != kInvalidStatusCallback && (notifications & status) != 0) {
        gta6rt_guest_args a{};
        a.args[0] = to_hinternet(h);
        a.args[1] = context;
        a.args[2] = status;
        a.args[3] = reinterpret_cast<uintptr_t>(info);
        a.args[4] = length;
        a.count = 5;
        gta6rt_guest_ret r{};
        gta6rt_isa_call_guest(callback, &a, &r);
    }
}

void release(Handle* h) {
    while (h && h->refs.fetch_sub(1) == 1) {
        uint64_t self = to_hinternet(h);
        notify(h, kStatusHandleClosing, &self, sizeof(self));
        Handle* parent = h->parent;
        delete h;
        h = parent;
    }
}

uint64_t make_handle(Kind kind, Handle* parent, bool async, const char16_t* server = nullptr, uint16_t port = 0) {
    std::u16string host = server ? server : u"";
    auto* h = new (std::nothrow) Handle;
    if (!h) {
        if (parent) release(parent);
        gta6rt_last_error_set(kErrorNotEnoughMemory);
        return 0;
    }
    h->server = std::move(host);
    h->port = port;
    h->kind = kind;
    h->parent = parent;
    h->async = async;
    if (parent) {
        std::lock_guard lock(parent->mutex);
        h->callback = parent->callback;
        h->notifications = parent->notifications;
        h->context = parent->context;
    }
    {
        std::lock_guard lock(live_mutex());
        live().insert(h);
    }
    if (parent) {
        h->refs.fetch_add(1);
        uint64_t self = to_hinternet(h);
        notify(h, kStatusHandleCreated, &self, sizeof(self));
        const bool closed = h->closed.load();
        release(h);
        if (closed) {
            gta6rt_last_error_set(kErrOperationCancelled);
            return 0;
        }
    }
    return to_hinternet(h);
}

void log_refused(const Handle* request) {
    static auto* logged = new std::set<std::u16string>;
    static auto* mutex = new std::mutex;
    const Handle* connect = request ? request->parent : nullptr;
    const std::u16string server = connect ? connect->server + u":" + widen(std::to_string(connect->port).c_str()) : u"?";
    std::lock_guard lock(*mutex);
    if (logged->insert(server).second) RTLOG(INFO, "winhttp: offline, refused %s", narrow(server).c_str());
}

int fail(uint32_t error) {
    gta6rt_last_error_set(error);
    return 0;
}

struct AsyncResult {
    uint64_t result;
    uint32_t error;
};

void send_failed(Handle* h) {
    if (!h->closed.load()) {
        AsyncResult result{kApiSendRequest, kErrCannotConnect};
        notify(h, kStatusRequestError, &result, sizeof(result));
    }
    release(h);
}

uint64_t WINAPI open_session(const char16_t*, uint32_t, const char16_t*, const char16_t*, uint32_t flags) {
    static std::once_flag once;
    std::call_once(once, [] { RTLOG(INFO, "winhttp: offline; requests fail with ERROR_WINHTTP_CANNOT_CONNECT"); });
    return make_handle(Kind::Session, nullptr, (flags & kFlagAsync) != 0);
}

uint64_t WINAPI open_connection(uint64_t session, const char16_t* server, uint16_t port, uint32_t) {
    uint32_t error = 0;
    Handle* s = acquire(session, true, false, false, error);
    if (!s) {
        gta6rt_last_error_set(error);
        return 0;
    }
    if (!server || !*server) {
        release(s);
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    return make_handle(Kind::Connect, s, s->async, server, port);
}

uint64_t WINAPI open_request(uint64_t connection, const char16_t*, const char16_t*, const char16_t*, const char16_t*,
                             const char16_t**, uint32_t) {
    uint32_t error = 0;
    Handle* c = acquire(connection, false, true, false, error);
    if (!c) {
        gta6rt_last_error_set(error);
        return 0;
    }
    return make_handle(Kind::Request, c, c->async);
}

int WINAPI send_request(uint64_t request, const char16_t*, uint32_t, void*, uint32_t, uint32_t, uint64_t context) {
    uint32_t error = 0;
    Handle* r = acquire(request, false, false, true, error);
    if (!r) return fail(error);
    if (context) {
        std::lock_guard lock(r->mutex);
        r->context = context;
    }
    log_refused(r);
    if (!r->async) {
        release(r);
        return fail(kErrCannotConnect);
    }
    try {
        std::thread([r] { send_failed(r); }).detach();
    } catch (...) {
        release(r);
        return fail(kErrorNotEnoughMemory);
    }
    gta6rt_last_error_set(0);
    return 1;
}

int WINAPI receive_response(uint64_t request, void*) {
    uint32_t error = 0;
    Handle* r = acquire(request, false, false, true, error);
    if (!r) return fail(error);
    release(r);
    return fail(kErrIncorrectHandleState);
}

int WINAPI query_data_available(uint64_t request, uint32_t*) { return receive_response(request, nullptr); }
int WINAPI read_data(uint64_t request, void*, uint32_t, uint32_t*) { return receive_response(request, nullptr); }
int WINAPI write_data(uint64_t request, const void*, uint32_t, uint32_t*) { return receive_response(request, nullptr); }
int WINAPI query_headers(uint64_t request, uint32_t, const char16_t*, void*, uint32_t*, uint32_t*) {
    return receive_response(request, nullptr);
}

bool is_header_name_char(char16_t ch) {
    if (ch <= 0x20 || ch >= 0x7F) return false;
    switch (ch) {
    case u'(': case u')': case u'<': case u'>': case u'@': case u',': case u';': case u':': case u'\\': case u'"':
    case u'/': case u'[': case u']': case u'?': case u'=': case u'{': case u'}':
        return false;
    default:
        return true;
    }
}

// Headers WinHTTP would reject: a null or empty block, embedded NULs, a non-empty line with no ':', an empty header
// name, or any non-token character in a header name. A length of (DWORD)-1 means NUL-terminated.
bool header_block_valid(const char16_t* headers, uint32_t length) {
    if (!headers) return false;
    const size_t count = length == 0xFFFFFFFFu ? u16len(headers) : static_cast<size_t>(length);
    if (count == 0) return false;
    size_t i = 0;
    while (i < count) {
        if (headers[i] == 0) return false;
        const size_t line_start = i;
        while (i < count && headers[i] != u'\r' && headers[i] != u'\n') {
            if (headers[i] == 0) return false;
            ++i;
        }
        const size_t line_end = i;
        if (i < count && headers[i] == u'\r') ++i;
        if (i < count && headers[i] == u'\n') ++i;
        if (line_end == line_start) continue;
        size_t colon = line_start;
        while (colon < line_end && headers[colon] != u':') ++colon;
        if (colon == line_start || colon == line_end) return false;
        for (size_t k = line_start; k < colon; ++k)
            if (!is_header_name_char(headers[k])) return false;
    }
    return true;
}

int WINAPI add_request_headers(uint64_t request, const char16_t* headers, uint32_t length, uint32_t) {
    uint32_t error = 0;
    Handle* r = acquire(request, false, false, true, error);
    if (!r) return fail(error);
    release(r);
    if (!header_block_valid(headers, length)) return fail(kErrorInvalidParameter);
    return 1;
}

int WINAPI set_timeouts(uint64_t handle, int resolve, int connect, int send, int receive) {
    uint32_t error = 0;
    Handle* h = acquire(handle, true, false, true, error);
    if (!h) return fail(error);
    release(h);
    if (resolve < -1 || connect < -1 || send < -1 || receive < -1) return fail(kErrorInvalidParameter);
    return 1;
}

int WINAPI set_option(uint64_t handle, uint32_t option, void* buffer, uint32_t length) {
    if (!handle) {
        if (option == kOptionContextValue) return fail(kErrIncorrectHandleType);
        return 1;
    }
    uint32_t error = 0;
    Handle* h = acquire(handle, true, true, true, error);
    if (!h) return fail(error);
    if (option == kOptionContextValue) {
        if (!buffer) {
            release(h);
            return fail(kErrorInvalidParameter);
        }
        if (length < sizeof(uint64_t)) {
            release(h);
            return fail(kErrorInsufficientBuffer);
        }
        std::lock_guard lock(h->mutex);
        std::memcpy(&h->context, buffer, sizeof(uint64_t));
    }
    release(h);
    return 1;
}

int WINAPI query_option(uint64_t handle, uint32_t option, void* buffer, uint32_t* length) {
    if (!length) return fail(kErrorInvalidParameter);
    uint32_t error = 0;
    Handle* h = acquire(handle, true, true, true, error);
    if (!h) return fail(error);
    uint64_t value_ptr = 0;
    uint32_t value_dword = 0;
    const void* value = nullptr;
    uint32_t size = 0;
    if (option == kOptionContextValue) {
        std::lock_guard lock(h->mutex);
        value_ptr = h->context;
        value = &value_ptr;
        size = sizeof(value_ptr);
    } else if (option == kOptionHandleType) {
        value_dword = h->kind == Kind::Session ? kHandleTypeSession : h->kind == Kind::Connect ? kHandleTypeConnect : kHandleTypeRequest;
        value = &value_dword;
        size = sizeof(value_dword);
    }
    release(h);
    if (!value) return fail(kErrInvalidOption);
    if (!buffer || *length < size) {
        *length = size;
        return fail(kErrorInsufficientBuffer);
    }
    std::memcpy(buffer, value, size);
    *length = size;
    return 1;
}

uint64_t WINAPI set_status_callback(uint64_t handle, uint64_t callback, uint32_t notifications, uint64_t) {
    if (!handle) {
        gta6rt_last_error_set(kErrIncorrectHandleType);
        return kInvalidStatusCallback;
    }
    uint32_t error = 0;
    Handle* h = acquire(handle, true, true, true, error);
    if (!h) {
        gta6rt_last_error_set(error);
        return kInvalidStatusCallback;
    }
    uint64_t previous;
    {
        std::lock_guard lock(h->mutex);
        previous = h->callback;
        h->callback = callback;
        h->notifications = callback ? notifications : 0;
    }
    release(h);
    return previous;
}

int WINAPI close_handle(uint64_t handle) {
    Handle* h = reinterpret_cast<Handle*>(static_cast<uintptr_t>(handle));
    {
        std::lock_guard lock(live_mutex());
        if (!handle || live().erase(h) == 0) return fail(kErrorInvalidHandle);
    }
    h->closed.store(true);
    release(h);
    return 1;
}

// URL_COMPONENTS, Windows x64 layout (104 bytes).
struct UrlComponents {
    uint32_t dwStructSize;
    char16_t* lpszScheme;
    uint32_t dwSchemeLength;
    uint32_t nScheme;
    char16_t* lpszHostName;
    uint32_t dwHostNameLength;
    uint16_t nPort;
    char16_t* lpszUserName;
    uint32_t dwUserNameLength;
    char16_t* lpszPassword;
    uint32_t dwPasswordLength;
    char16_t* lpszUrlPath;
    uint32_t dwUrlPathLength;
    char16_t* lpszExtraInfo;
    uint32_t dwExtraInfoLength;
};
static_assert(sizeof(UrlComponents) == 104 && offsetof(UrlComponents, lpszHostName) == 24 && offsetof(UrlComponents, nPort) == 36 &&
              offsetof(UrlComponents, lpszUrlPath) == 72);

bool store_component(const char16_t* part, size_t part_len, char16_t** out_ptr, uint32_t* out_len, bool& short_buffer) {
    if (*out_ptr == nullptr && *out_len == 0) return true;
    if (*out_ptr == nullptr) {
        *out_ptr = const_cast<char16_t*>(part);
        *out_len = static_cast<uint32_t>(part_len);
        return true;
    }
    if (*out_len < part_len + 1) {
        *out_len = static_cast<uint32_t>(part_len + 1);
        short_buffer = true;
        return false;
    }
    std::memcpy(*out_ptr, part, part_len * sizeof(char16_t));
    (*out_ptr)[part_len] = 0;
    *out_len = static_cast<uint32_t>(part_len);
    return true;
}

int WINAPI crack_url(const char16_t* url, uint32_t length, uint32_t flags, UrlComponents* c) {
    (void)flags;
    if (!url || !c || c->dwStructSize != sizeof(UrlComponents)) return fail(kErrorInvalidParameter);
    const size_t n = length ? length : u16len(url);
    if (n == 0) return fail(kErrInvalidUrl);
    const std::u16string s(url, n);
    const size_t scheme_end = s.find(u"://");
    if (scheme_end == std::u16string::npos || scheme_end == 0) return fail(kErrInvalidUrl);
    const std::u16string scheme = s.substr(0, scheme_end);
    uint32_t scheme_id = 0;
    uint16_t port = 0;
    if (u16ieq(scheme.c_str(), u"http")) { scheme_id = 1; port = 80; }
    else if (u16ieq(scheme.c_str(), u"https")) { scheme_id = 2; port = 443; }
    else return fail(kErrUnrecognizedScheme);
    size_t pos = scheme_end + 3;
    const size_t auth_end = s.find_first_of(u"/?#", pos);
    const size_t end_of_auth = auth_end == std::u16string::npos ? s.size() : auth_end;
    std::u16string authority = s.substr(pos, end_of_auth - pos);
    std::u16string user, pass;
    const size_t at = authority.rfind(u'@');
    if (at != std::u16string::npos) {
        const std::u16string cred = authority.substr(0, at);
        authority = authority.substr(at + 1);
        const size_t colon = cred.find(u':');
        user = cred.substr(0, colon);
        if (colon != std::u16string::npos) pass = cred.substr(colon + 1);
    }
    std::u16string host = authority;
    const size_t host_off = pos + (at == std::u16string::npos ? 0 : at + 1);
    if (!host.empty() && host[0] == u'[') {
        const size_t close = host.find(u']');
        if (close == std::u16string::npos) return fail(kErrInvalidUrl);
        if (close + 1 < host.size() && host[close + 1] == u':') port = static_cast<uint16_t>(std::stoi(narrow(host.substr(close + 2))));
        host = host.substr(0, close + 1);
    } else {
        const size_t colon = host.rfind(u':');
        if (colon != std::u16string::npos) {
            const std::string digits = narrow(host.substr(colon + 1));
            if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return fail(kErrInvalidUrl);
            port = static_cast<uint16_t>(std::stoi(digits));
            host = host.substr(0, colon);
        }
    }
    if (host.empty()) return fail(kErrInvalidUrl);
    size_t path_end = s.find_first_of(u"?#", end_of_auth);
    if (path_end == std::u16string::npos) path_end = s.size();
    const size_t path_off = end_of_auth;
    const size_t path_len = path_end - path_off;
    const size_t extra_off = path_end;
    const size_t extra_len = s.size() - path_end;

    bool short_buffer = false;
    c->nScheme = scheme_id;
    c->nPort = port;
    const char16_t* base = url;
    store_component(base, scheme_end, &c->lpszScheme, &c->dwSchemeLength, short_buffer);
    store_component(base + host_off, host.size(), &c->lpszHostName, &c->dwHostNameLength, short_buffer);
    if (!user.empty() || c->lpszUserName) store_component(user.c_str(), user.size(), &c->lpszUserName, &c->dwUserNameLength, short_buffer);
    if (!pass.empty() || c->lpszPassword) store_component(pass.c_str(), pass.size(), &c->lpszPassword, &c->dwPasswordLength, short_buffer);
    store_component(base + path_off, path_len, &c->lpszUrlPath, &c->dwUrlPathLength, short_buffer);
    store_component(base + extra_off, extra_len, &c->lpszExtraInfo, &c->dwExtraInfoLength, short_buffer);
    if (short_buffer) return fail(kErrorInsufficientBuffer);
    return 1;
}

}  // namespace
}  // namespace rt::winhttp

namespace {
using namespace rt::winhttp;
}

SHIM_TABLE(winhttp_offline,
    SHIM("winhttp.dll", "WinHttpOpen", open_session),
    SHIM("winhttp.dll", "WinHttpConnect", open_connection),
    SHIM("winhttp.dll", "WinHttpOpenRequest", open_request),
    SHIM("winhttp.dll", "WinHttpSendRequest", send_request),
    SHIM("winhttp.dll", "WinHttpReceiveResponse", receive_response),
    SHIM("winhttp.dll", "WinHttpQueryDataAvailable", query_data_available),
    SHIM("winhttp.dll", "WinHttpReadData", read_data),
    SHIM("winhttp.dll", "WinHttpWriteData", write_data),
    SHIM("winhttp.dll", "WinHttpQueryHeaders", query_headers),
    SHIM("winhttp.dll", "WinHttpAddRequestHeaders", add_request_headers),
    SHIM("winhttp.dll", "WinHttpSetTimeouts", set_timeouts),
    SHIM("winhttp.dll", "WinHttpSetOption", set_option),
    SHIM("winhttp.dll", "WinHttpQueryOption", query_option),
    SHIM("winhttp.dll", "WinHttpSetStatusCallback", set_status_callback),
    SHIM("winhttp.dll", "WinHttpCloseHandle", close_handle),
    SHIM("winhttp.dll", "WinHttpCrackUrl", crack_url),
);
