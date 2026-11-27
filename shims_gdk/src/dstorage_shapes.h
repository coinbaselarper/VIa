#pragma once
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "gdk_env.h"

// Request-shape log, off by default. LEONIDA_DSTORAGE_SHAPES=<path> appends tab-separated records to that file,
// =1 writes them to stderr behind "[rt] ds-shape ". Record kinds (first column; time is microseconds since the first record):
//   Q  t queue source(file|mem) capacity priority autosubmit_disabled name tid      CreateQueue
//   F  fileid path                                                                   first use of a file
//   R  t queue seq tid options source fileid offset src mid dst dest(buf|pages) tag outstanding   EnqueueRequest
//   S  t queue requests entries src_bytes dst_bytes                                  Submit (auto or explicit)
//   C  t queue seq hr latency_us read_us decode_us via(pool|inline) window cancelled   request published
//   X  t queue mask value matched                                                    CancelRequestsWithTag
//   Z  t queue requests src_bytes max_window                                         queue closed
namespace leonida::dstorage::shapes {

struct Sink {
    int fd = -1;
    bool prefix = false;
    std::mutex mutex;
    std::map<std::string, uint32_t> files;
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
};

inline Sink* sink() {
    static Sink* const s = [] () -> Sink* {
        const std::string v = rt::gdk::env_or("LEONIDA_DSTORAGE_SHAPES");
        if (v.empty() || v == "0") return nullptr;
        auto* out = new Sink;
        if (v == "1") {
            out->fd = 2;
            out->prefix = true;
        } else {
            out->fd = ::open(v.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            if (out->fd < 0) {
                std::fprintf(stderr, "[rt] dstorage: LEONIDA_DSTORAGE_SHAPES=%s cannot be opened; request shapes are not logged\n", v.c_str());
                delete out;
                return nullptr;
            }
        }
        char head[256];
        const auto wall = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const int n = std::snprintf(head, sizeof head, "%s# ds-shapes v1 pid=%d unix=%lld\n", out->prefix ? "[rt] ds-shape " : "", int(::getpid()),
                                    static_cast<long long>(wall));
        if (n > 0) (void)!::write(out->fd, head, static_cast<size_t>(n));
        std::fprintf(stderr, "[rt] dstorage: logging request shapes to %s\n", out->prefix ? "stderr" : v.c_str());
        return out;
    }();
    return s;
}

inline bool enabled() { return sink() != nullptr; }

inline uint64_t now_us() {
    Sink* s = sink();
    return s ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - s->origin).count()) : 0;
}

inline uint64_t thread_id() {
    uint64_t t = 0;
    pthread_threadid_np(nullptr, &t);
    return t;
}

inline void record(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void record(const char* fmt, ...) {
    Sink* s = sink();
    if (!s) return;
    char buf[1024];
    size_t at = 0;
    if (s->prefix) at = static_cast<size_t>(std::snprintf(buf, sizeof buf, "[rt] ds-shape "));
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf + at, sizeof buf - at - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    at += std::min<size_t>(static_cast<size_t>(n), sizeof buf - at - 2);
    buf[at++] = '\n';
    (void)!::write(s->fd, buf, at);
}

inline uint32_t file_id(const std::string& path) {
    Sink* s = sink();
    if (!s) return 0;
    uint32_t id;
    bool fresh = false;
    {
        std::lock_guard lock(s->mutex);
        auto it = s->files.find(path);
        fresh = it == s->files.end();
        if (fresh) it = s->files.emplace(path, static_cast<uint32_t>(s->files.size() + 1)).first;
        id = it->second;
    }
    if (fresh) record("F\t%u\t%s", id, path.c_str());
    return id;
}

}  // namespace leonida::dstorage::shapes
