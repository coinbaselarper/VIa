#include "internal.h"

#include <namedpipeapi.h>

#include <algorithm>
#include <cerrno>
#include <copyfile.h>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

using namespace gta6rt_os;

namespace {
struct IoPort;
}

struct gta6rt_file {
    std::atomic<int> refs{1};
    int fd = -1;
    bool readable = false, writable = false;
    bool overlapped = false, append = false, seekable = true, is_dir = false;
    bool delete_on_close = false, std_handle = false;
    std::string path;
    pthread_mutex_t pos_mx = PTHREAD_MUTEX_INITIALIZER;
    uint64_t pos = 0;
    gta6rt_waitable* evt = nullptr;
    IoPort* port = nullptr;
    ULONG_PTR key = 0;
};

namespace {

constexpr DWORD kStatusEof = 0xC0000011;
constexpr DWORD kStatusCancelled = 0xC0000120;
inline DWORD ntstatus_from_win32(DWORD e) { return e ? (0xC0070000u | (e & 0xFFFF)) : 0; }

DWORD win32_from_ntstatus(DWORD st) {
    if (st == 0) return 0;
    if (st == kStatusEof) return ERROR_HANDLE_EOF;
    if (st == kStatusCancelled) return ERROR_OPERATION_ABORTED;
    if ((st & 0xFFFF0000u) == 0xC0070000u) return st & 0xFFFF;
    return ERROR_GEN_FAILURE;
}

// ---- I/O completion ports ----

struct Packet {
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED ovl;
    DWORD status;
};

struct IoPort : OtherObject {
    std::atomic<int> refs{1};
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    std::deque<Packet> q;
    IoPort() { kind = OtherKind::IoPort; }
};

void port_retain(void* p) { ((IoPort*)p)->refs.fetch_add(1); }
void port_release(void* p) {
    IoPort* io = (IoPort*)p;
    if (io->refs.fetch_sub(1) == 1) delete io;
}

void port_post(IoPort* p, const Packet& pk) {
    pthread_mutex_lock(&p->m);
    p->q.push_back(pk);
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->m);
}

// Waits up to ms for the first packet, then takes up to max queued packets. Returns the number taken.
ULONG port_take(IoPort* p, DWORD ms, Packet* out, ULONG max) {
    uint64_t deadline = deadline_from_timeout(ms_to_ns(ms));
    pthread_mutex_lock(&p->m);
    while (p->q.empty()) {
        uint64_t now = monotonic_ns();
        if (ms == 0 || now >= deadline) break;
        if (deadline == UINT64_MAX) {
            pthread_cond_wait(&p->cv, &p->m);
        } else {
            uint64_t d = deadline - now;
            struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
            pthread_cond_timedwait_relative_np(&p->cv, &p->m, &ts);
        }
    }
    ULONG n = 0;
    while (n < max && !p->q.empty()) {
        out[n++] = p->q.front();
        p->q.pop_front();
    }
    pthread_mutex_unlock(&p->m);
    return n;
}

IoPort* port_ref(HANDLE h) {
    void (*rel)(void*) = nullptr;
    OtherObject* o = (OtherObject*)handle_ref(H(h), GTA6RT_HANDLE_OTHER, &rel);
    if (o && o->kind != OtherKind::IoPort) {
        rel(o);
        o = nullptr;
    }
    return (IoPort*)o;
}

// ---- file objects ----

void file_unref(gta6rt_file* f) {
    if (f->refs.fetch_sub(1) != 1) return;
    if (f->fd >= 0 && !f->std_handle) close(f->fd);
    if (f->delete_on_close && !f->path.empty()) {
        if (f->is_dir) rmdir(f->path.c_str());
        else unlink(f->path.c_str());
    }
    if (f->evt) gta6rt_waitable_release(f->evt);
    if (f->port) port_release(f->port);
    delete f;
}

void f_release(void* p) { file_unref((gta6rt_file*)p); }
void f_retain(void* p) { ((gta6rt_file*)p)->refs.fetch_add(1); }

HANDLE file_handle(gta6rt_file* f) { return HV(handle_alloc_internal(GTA6RT_HANDLE_FILE, f, f_release, f_retain)); }

gta6rt_file* new_file(int fd) {
    gta6rt_file* f = new gta6rt_file();
    f->fd = fd;
    f->evt = waitable_new_event(true, true);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0) {
        f->seekable = S_ISREG(st.st_mode) || S_ISBLK(st.st_mode) || S_ISDIR(st.st_mode);
        f->is_dir = S_ISDIR(st.st_mode);
    }
    return f;
}

DWORD enoent_error(const std::string& path) {
    size_t s = path.find_last_of('/');
    if (s == std::string::npos || s == 0) return ERROR_FILE_NOT_FOUND;
    struct stat st;
    std::string parent = path.substr(0, s);
    return stat(parent.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND;
}

DWORD path_errno(int e, const std::string& path) { return e == ENOENT ? enoent_error(path) : errno_to_win32(e); }

uint64_t ft_from_ts(const struct timespec& ts) {
    return (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ull;
}

void put_ft(FILETIME* f, uint64_t v) {
    f->dwLowDateTime = (DWORD)v;
    f->dwHighDateTime = (DWORD)(v >> 32);
}

uint64_t get_ft(const FILETIME* f) { return ((uint64_t)f->dwHighDateTime << 32) | f->dwLowDateTime; }

DWORD attrs_from_stat(const struct stat& st) {
    DWORD a = 0;
    if (S_ISDIR(st.st_mode)) a |= FILE_ATTRIBUTE_DIRECTORY;
    else a |= FILE_ATTRIBUTE_ARCHIVE;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    if (S_ISLNK(st.st_mode)) a |= FILE_ATTRIBUTE_REPARSE_POINT;
    return a;
}

gta6rt_file* fref(HANDLE h) {
    void (*rel)(void*) = nullptr;
    gta6rt_file* f = (gta6rt_file*)handle_ref(H(h), GTA6RT_HANDLE_FILE, &rel);
    if (!f) set_error(ERROR_INVALID_HANDLE);
    return f;
}

// ---- overlapped I/O worker pool ----

struct IoReq {
    gta6rt_file* f;
    bool write;
    void* buf;
    size_t n;
    uint64_t off;
    LPOVERLAPPED ovl;
    gta6rt_waitable* evt;
    gta6rt_thread* apc_thread;
    gta6rt_gva routine;
    bool post;
    std::atomic<bool> cancelled{false};
};

pthread_mutex_t g_io_mx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_io_cv = PTHREAD_COND_INITIALIZER;
std::deque<IoReq*>* g_io_q;
int g_io_workers;

struct CompletionApc : Apc {
    DWORD err, bytes;
    LPOVERLAPPED ovl;
};

void complete(IoReq* r, DWORD status, size_t bytes) {
    r->ovl->InternalHigh = bytes;
    __atomic_store_n(&r->ovl->Internal, (ULONG_PTR)status, __ATOMIC_RELEASE);
    pthread_mutex_lock(&g_wait_mx);
    if (r->evt) waitable_set_locked(r->evt);
    else if (!r->routine) waitable_set_locked(r->f->evt);
    pthread_mutex_unlock(&g_wait_mx);
    if (r->post && r->f->port) port_post(r->f->port, Packet{(DWORD)bytes, r->f->key, r->ovl, status});
    if (r->routine && r->apc_thread) {
        Apc* a = new Apc();
        a->fn = r->routine;
        a->args[0] = win32_from_ntstatus(status);
        a->args[1] = bytes;
        a->args[2] = (uint64_t)(uintptr_t)r->ovl;
        a->nargs = 3;
        queue_apc(r->apc_thread, a);
    }
    if (r->evt) gta6rt_waitable_release(r->evt);
    if (r->apc_thread) gta6rt_thread_release(r->apc_thread);
    file_unref(r->f);
    delete r;
}

void run_req(IoReq* r) {
    if (r->cancelled.load()) {
        complete(r, kStatusCancelled, 0);
        return;
    }
    ssize_t n;
    size_t done = 0;
    int err = 0;
    while (done < r->n) {
        if (r->write) n = r->f->seekable ? pwrite(r->f->fd, (char*)r->buf + done, r->n - done, (off_t)(r->off + done))
                                         : write(r->f->fd, (char*)r->buf + done, r->n - done);
        else n = r->f->seekable ? pread(r->f->fd, (char*)r->buf + done, r->n - done, (off_t)(r->off + done))
                                : read(r->f->fd, (char*)r->buf + done, r->n - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = errno;
            break;
        }
        if (n == 0) break;
        done += (size_t)n;
    }
    DWORD status = 0;
    if (err) status = ntstatus_from_win32(errno_to_win32(err));
    else if (!r->write && done == 0 && r->n > 0) status = kStatusEof;
    complete(r, status, done);
}

void* io_worker(void*) {
    pthread_setname_np("gta6rt-io");
    for (;;) {
        pthread_mutex_lock(&g_io_mx);
        while (g_io_q->empty()) pthread_cond_wait(&g_io_cv, &g_io_mx);
        IoReq* r = g_io_q->front();
        g_io_q->pop_front();
        pthread_mutex_unlock(&g_io_mx);
        run_req(r);
    }
    return nullptr;
}

void submit(IoReq* r) {
    pthread_mutex_lock(&g_io_mx);
    if (!g_io_q) g_io_q = new std::deque<IoReq*>();
    int want = (int)std::min<uint32_t>(4, num_cpus());
    while (g_io_workers < want) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &at, io_worker, nullptr);
        pthread_attr_destroy(&at);
        g_io_workers++;
    }
    g_io_q->push_back(r);
    static uint64_t requests = 0;
    static size_t deepest = 0;
    const size_t depth = g_io_q->size();
    const bool first = requests++ == 0, deeper = depth > deepest && depth >= 2 * deepest && depth > (size_t)want;
    if (depth > deepest) deepest = depth;
    pthread_cond_signal(&g_io_cv);
    pthread_mutex_unlock(&g_io_mx);
    if (first) OS_LOG(GTA6RT_LOG_INFO, "overlapped I/O: first request (%d workers)", want);
    if (deeper)
        OS_LOG(GTA6RT_LOG_INFO, "overlapped I/O: %zu requests queued for %d workers (request %llu)", depth, want,
               (unsigned long long)requests);
}

int cancel_reqs(gta6rt_file* f, LPOVERLAPPED ovl) {
    int n = 0;
    std::vector<IoReq*> hit;
    pthread_mutex_lock(&g_io_mx);
    if (g_io_q)
        for (auto it = g_io_q->begin(); it != g_io_q->end();) {
            if ((*it)->f == f && (!ovl || (*it)->ovl == ovl)) {
                hit.push_back(*it);
                it = g_io_q->erase(it);
            } else {
                ++it;
            }
        }
    pthread_mutex_unlock(&g_io_mx);
    for (IoReq* r : hit) {
        complete(r, kStatusCancelled, 0);
        n++;
    }
    return n;
}

BOOL do_io(HANDLE hFile, bool write, void* buf, DWORD n, LPDWORD out_n, LPOVERLAPPED ovl, gta6rt_gva routine) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    if ((write && !f->writable) || (!write && !f->readable)) {
        file_unref(f);
        set_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (ovl && (f->overlapped || routine)) {
        uint64_t off = ((uint64_t)ovl->OffsetHigh << 32) | ovl->Offset;
        if (write && f->append) off = (uint64_t)lseek(f->fd, 0, SEEK_END);
        IoReq* r = new IoReq();
        r->f = f;
        r->write = write;
        r->buf = buf;
        r->n = n;
        r->off = off;
        r->ovl = ovl;
        r->routine = routine;
        r->post = !routine && !((uintptr_t)ovl->hEvent & 1);
        r->evt = nullptr;
        r->apc_thread = nullptr;
        HANDLE ev = HV(H(ovl->hEvent) & ~1ull);
        if (!routine && ev) {
            r->evt = handle_to_waitable(ev);
            if (r->evt) ResetEvent(ev);
        } else if (!routine) {
            gta6rt_event_reset(f->evt);
        }
        if (routine) {
            r->apc_thread = gta6rt_thread_current();
            thread_retain(r->apc_thread);
        }
        __atomic_store_n(&ovl->Internal, (ULONG_PTR)STATUS_PENDING, __ATOMIC_RELEASE);
        ovl->InternalHigh = 0;
        if (out_n) *out_n = 0;
        submit(r);
        set_error(ERROR_IO_PENDING);
        return routine ? TRUE : FALSE;
    }
    size_t done = 0;
    int err = 0;
    pthread_mutex_lock(&f->pos_mx);
    uint64_t off = ovl ? (((uint64_t)ovl->OffsetHigh << 32) | ovl->Offset) : f->pos;
    if (write && f->append && f->seekable) off = (uint64_t)lseek(f->fd, 0, SEEK_END);
    while (done < n) {
        ssize_t k;
        if (write) k = f->seekable ? pwrite(f->fd, (char*)buf + done, n - done, (off_t)(off + done))
                                   : ::write(f->fd, (char*)buf + done, n - done);
        else k = f->seekable ? pread(f->fd, (char*)buf + done, n - done, (off_t)(off + done))
                             : read(f->fd, (char*)buf + done, n - done);
        if (k < 0) {
            if (errno == EINTR) continue;
            err = errno;
            break;
        }
        if (k == 0) break;
        done += (size_t)k;
        if (!f->seekable && !write) break;
    }
    if (f->seekable) f->pos = off + done;
    bool seekable = f->seekable;
    pthread_mutex_unlock(&f->pos_mx);
    if (out_n) *out_n = (DWORD)done;
    if (ovl) {
        ovl->InternalHigh = done;
        ovl->Internal = err ? ntstatus_from_win32(errno_to_win32(err)) : 0;
        if (ovl->hEvent) SetEvent(HV(H(ovl->hEvent) & ~1ull));
    }
    file_unref(f);
    if (err) {
        set_error(errno_to_win32(err));
        return FALSE;
    }
    if (ovl && !write && done == 0 && n > 0 && seekable) {
        set_error(ERROR_HANDLE_EOF);
        return FALSE;
    }
    return TRUE;
}

// ---- find ----

struct FindState {
    std::vector<std::string> names;
    std::vector<struct stat> stats;
    size_t next = 0;
};

bool wild_match(const char* p, const char* s) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return true;
            for (; *s; s++)
                if (wild_match(p, s)) return true;
            return wild_match(p, s);
        }
        if (!*s) return false;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false;
        p++;
        s++;
    }
    return !*s;
}

void find_close(void* p) { delete (FindState*)p; }

void fill_find(const std::string& name, const struct stat& st, LPWIN32_FIND_DATAW d) {
    memset(d, 0, sizeof *d);
    d->dwFileAttributes = attrs_from_stat(st);
    put_ft(&d->ftCreationTime, ft_from_ts(st.st_birthtimespec));
    put_ft(&d->ftLastAccessTime, ft_from_ts(st.st_atimespec));
    put_ft(&d->ftLastWriteTime, ft_from_ts(st.st_mtimespec));
    uint64_t sz = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    d->nFileSizeHigh = (DWORD)(sz >> 32);
    d->nFileSizeLow = (DWORD)sz;
    std::u16string w = utf8_to_u16(name.c_str());
    if (w.size() > MAX_PATH - 1) w.resize(MAX_PATH - 1);
    memcpy(d->cFileName, w.data(), w.size() * 2);
}

void w_to_a(const WIN32_FIND_DATAW* w, LPWIN32_FIND_DATAA a) {
    memset(a, 0, sizeof *a);
    a->dwFileAttributes = w->dwFileAttributes;
    a->ftCreationTime = w->ftCreationTime;
    a->ftLastAccessTime = w->ftLastAccessTime;
    a->ftLastWriteTime = w->ftLastWriteTime;
    a->nFileSizeHigh = w->nFileSizeHigh;
    a->nFileSizeLow = w->nFileSizeLow;
    std::string n = u16_to_utf8(w->cFileName);
    strlcpy(a->cFileName, n.c_str(), MAX_PATH);
}

HANDLE find_first(LPCWSTR lpFileName, LPWIN32_FIND_DATAW d, bool dirs_only) {
    if (!lpFileName || !d) {
        set_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    std::string full = u16_to_utf8(lpFileName);
    for (char& c : full)
        if (c == '\\') c = '/';
    size_t slash = full.find_last_of('/');
    std::string dir_win, pat;
    if (slash == std::string::npos) {
        if (full.size() >= 2 && full[1] == ':') {
            dir_win = full.substr(0, 2) + "/";
            pat = full.substr(2);
        } else {
            dir_win = ".";
            pat = full;
        }
    } else {
        dir_win = full.substr(0, slash + 1);
        pat = full.substr(slash + 1);
    }
    if (pat.empty()) {
        set_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (pat == "*.*") pat = "*";
    uint32_t err = 0;
    std::string dir = win_to_host_path8(dir_win.c_str(), &err);
    if (err) {
        set_error(err);
        return INVALID_HANDLE_VALUE;
    }
    bool wild = pat.find_first_of("*?") != std::string::npos;
    FindState* fs = new FindState();
    if (!wild) {
        std::string p = win_to_host_path8(full.c_str(), &err);
        struct stat st;
        if (!err && lstat(p.c_str(), &st) == 0) {
            size_t s2 = p.find_last_of('/');
            std::string name = s2 == std::string::npos ? p : p.substr(s2 + 1);
            std::string parent = s2 == std::string::npos ? "." : s2 == 0 ? "/" : p.substr(0, s2);
            if (DIR* dp = opendir(parent.c_str())) {
                while (dirent* e = readdir(dp))
                    if (!strcasecmp(e->d_name, name.c_str())) {
                        name = e->d_name;
                        break;
                    }
                closedir(dp);
            }
            fs->names.push_back(name);
            fs->stats.push_back(st);
        }
    } else {
        DIR* dp = opendir(dir.c_str());
        if (!dp) {
            int e = errno;
            delete fs;
            set_error(e == ENOENT || e == ENOTDIR ? ERROR_PATH_NOT_FOUND : errno_to_win32(e));
            return INVALID_HANDLE_VALUE;
        }
        std::vector<std::pair<std::string, struct stat>> items;
        while (dirent* e = readdir(dp)) {
            if (!wild_match(pat.c_str(), e->d_name)) continue;
            struct stat st;
            std::string p = (dir == "/" ? "/" : dir + "/") + e->d_name;
            if (stat(p.c_str(), &st) != 0 && lstat(p.c_str(), &st) != 0) continue;
            if (dirs_only && !S_ISDIR(st.st_mode)) continue;
            items.emplace_back(e->d_name, st);
        }
        closedir(dp);
        std::sort(items.begin(), items.end(),
                  [](const auto& a, const auto& b) { return strcasecmp(a.first.c_str(), b.first.c_str()) < 0; });
        for (auto& it : items) {
            fs->names.push_back(it.first);
            fs->stats.push_back(it.second);
        }
    }
    if (fs->names.empty()) {
        delete fs;
        set_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    fill_find(fs->names[0], fs->stats[0], d);
    fs->next = 1;
    set_error(ERROR_SUCCESS);
    return HV(handle_alloc_internal(GTA6RT_HANDLE_FIND, fs, find_close, [](void*) {}));
}

std::string host_of(LPCWSTR p, DWORD* err) {
    uint32_t e = 0;
    std::string h = win_to_host_path(p, &e);
    *err = e;
    return h;
}

// ---- std handles ----

pthread_mutex_t g_std_mx = PTHREAD_MUTEX_INITIALIZER;
HANDLE g_std[3];

// ---- file mappings ----

struct Mapping : OtherObject {
    std::atomic<int> refs{1};
    int fd = -1;
    mach_port_t entry = MACH_PORT_NULL;
    uint64_t size = 0;
    DWORD protect = 0;
    std::u16string name;
    Mapping() { kind = OtherKind::Misc; }
    ~Mapping() {
        if (fd >= 0) close(fd);
        if (entry != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), entry);
    }
};

pthread_mutex_t g_map_mx = PTHREAD_MUTEX_INITIALIZER;
std::vector<Mapping*>* g_named_maps;

void map_retain(void* p) { ((Mapping*)p)->refs.fetch_add(1); }
void map_release(void* p) {
    Mapping* m = (Mapping*)p;
    if (m->refs.fetch_sub(1) != 1) return;
    pthread_mutex_lock(&g_map_mx);
    if (g_named_maps) g_named_maps->erase(std::remove(g_named_maps->begin(), g_named_maps->end(), m), g_named_maps->end());
    pthread_mutex_unlock(&g_map_mx);
    delete m;
}

void* map_view(Mapping* m, DWORD access, uint64_t off, size_t n, void* base, bool replace_placeholder) {
    if (off & (GTA6RT_GUEST_ALLOCATION_GRANULARITY - 1)) {
        set_error(1132 /* ERROR_MAPPED_ALIGNMENT */);
        return nullptr;
    }
    if (off > m->size) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!n) n = (size_t)(m->size - off);
    if (off + n > m->size) {
        set_error(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    bool copy = (access & FILE_MAP_COPY) && !(access & FILE_MAP_WRITE);
    bool wr = (access & FILE_MAP_WRITE) || copy || access == FILE_MAP_ALL_ACCESS;
    bool ex = (access & FILE_MAP_EXECUTE) != 0;
    DWORD mp = m->protect & 0xFF;
    if (wr && !copy && (mp == PAGE_READONLY || mp == PAGE_EXECUTE_READ)) {
        set_error(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    size_t hp = host_page();
    size_t hn = (n + hp - 1) & ~(hp - 1);
    uint32_t gprot = GTA6RT_PROT_READ | (wr ? GTA6RT_PROT_WRITE : 0) | (ex ? GTA6RT_PROT_EXEC : 0);
    int prot = PROT_READ | (wr ? PROT_WRITE : 0);
#if defined(__x86_64__)
    if (ex) prot |= PROT_EXEC;
#endif
    mach_vm_address_t a = (uintptr_t)base;
    if (base && replace_placeholder) {
        if (!vm_is_placeholder(base, (n + 0xFFF) & ~(size_t)0xFFF)) {
            set_error(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
    } else if (base) {
        kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, hn, VM_FLAGS_FIXED);
        if (kr != KERN_SUCCESS) {
            set_error(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
    } else {
        a = 0x300000000ull;
        kern_return_t kr = mach_vm_map(mach_task_self(), &a, hn, GTA6RT_GUEST_ALLOCATION_GRANULARITY - 1,
                                       VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL,
                                       VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) {
            set_error(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
    }
    if (m->entry != MACH_PORT_NULL) {
        mach_vm_address_t d = a;
        vm_prot_t vp = VM_PROT_READ | (wr ? VM_PROT_WRITE : 0);
        kern_return_t kr = mach_vm_map(mach_task_self(), &d, hn, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, m->entry, off,
                                       copy ? TRUE : FALSE, vp, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) {
            if (!replace_placeholder) munmap((void*)a, hn);
            set_error(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
    } else {
        void* p = mmap((void*)a, hn, prot, (copy ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED, m->fd, (off_t)off);
        if (p == MAP_FAILED) {
            int e = errno;
            if (!replace_placeholder) munmap((void*)a, hn);
            set_error(errno_to_win32(e));
            return nullptr;
        }
    }
    vm_register_view((void*)a, n, gprot);
    return (void*)a;
}

Mapping* mapping_ref(HANDLE h) {
    void (*rel)(void*) = nullptr;
    OtherObject* o = (OtherObject*)handle_ref(H(h), GTA6RT_HANDLE_MAPPING, &rel);
    if (!o) set_error(ERROR_INVALID_HANDLE);
    return (Mapping*)o;
}

struct BasicInfoT {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    DWORD FileAttributes;
};
static_assert(sizeof(BasicInfoT) == 40, "FILE_BASIC_INFO layout");

struct RenameInfoT {
    BOOLEAN ReplaceIfExists;
    HANDLE RootDirectory;
    DWORD FileNameLength;
    WCHAR FileName[1];
};
static_assert(offsetof(RenameInfoT, FileNameLength) == 16 && offsetof(RenameInfoT, FileName) == 20,
              "FILE_RENAME_INFO layout");

} // namespace

namespace gta6rt_os {

gta6rt_waitable* file_waitable(gta6rt_file* f) {
    gta6rt_waitable_retain(f->evt);
    return f->evt;
}
void file_retain(gta6rt_file* f) { f->refs.fetch_add(1); }
int file_fd_internal(gta6rt_file* f) { return f->fd; }
bool file_writable(gta6rt_file* f) { return f->writable; }
gta6rt_file* file_from_handle(HANDLE h, void (**release)(void*)) {
    return (gta6rt_file*)handle_ref(H(h), GTA6RT_HANDLE_FILE, release);
}

} // namespace gta6rt_os

extern "C" {

// ---- gta6rt_file_* ----

gta6rt_status gta6rt_file_open(const char* path_utf8, uint32_t flags, gta6rt_file** out) {
    if (!path_utf8 || !out) return GTA6RT_E_INVALID_ARG;
    uint32_t err = 0;
    std::string p = win_to_host_path8(path_utf8, &err);
    if (err) return GTA6RT_E_NOT_FOUND;
    int of = 0;
    bool r = flags & GTA6RT_FILE_READ, w = flags & (GTA6RT_FILE_WRITE | GTA6RT_FILE_APPEND);
    of = r && w ? O_RDWR : w ? O_WRONLY : O_RDONLY;
    if (flags & GTA6RT_FILE_CREATE) of |= O_CREAT;
    if (flags & GTA6RT_FILE_TRUNCATE) of |= O_TRUNC;
    if (flags & GTA6RT_FILE_EXCLUSIVE) of |= O_EXCL | O_CREAT;
    if (flags & GTA6RT_FILE_APPEND) of |= O_APPEND;
    int fd = open(p.c_str(), of | O_CLOEXEC, 0644);
    if (fd < 0) return errno_to_status(errno);
    gta6rt_file* f = new_file(fd);
    f->readable = r || !w;
    f->writable = w;
    f->append = (flags & GTA6RT_FILE_APPEND) != 0;
    f->path = p;
    *out = f;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_file_pread(gta6rt_file* f, void* buf, size_t size, uint64_t offset, size_t* out_read) {
    if (!f) return GTA6RT_E_INVALID_ARG;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(f->fd, (char*)buf + done, size - done, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (out_read) *out_read = done;
            return errno_to_status(errno);
        }
        if (n == 0) break;
        done += (size_t)n;
    }
    if (out_read) *out_read = done;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_file_pwrite(gta6rt_file* f, const void* buf, size_t size, uint64_t offset, size_t* out_written) {
    if (!f) return GTA6RT_E_INVALID_ARG;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pwrite(f->fd, (const char*)buf + done, size - done, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (out_written) *out_written = done;
            return errno_to_status(errno);
        }
        done += (size_t)n;
    }
    if (out_written) *out_written = done;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_file_size(gta6rt_file* f, uint64_t* out_size) {
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) return GTA6RT_E_IO;
    *out_size = (uint64_t)st.st_size;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_file_truncate(gta6rt_file* f, uint64_t size) {
    if (!f) return GTA6RT_E_INVALID_ARG;
    return ftruncate(f->fd, (off_t)size) == 0 ? GTA6RT_OK : errno_to_status(errno);
}

gta6rt_status gta6rt_file_sync(gta6rt_file* f) {
    if (!f) return GTA6RT_E_INVALID_ARG;
    return fsync(f->fd) == 0 ? GTA6RT_OK : errno_to_status(errno);
}

void gta6rt_file_close(gta6rt_file* f) {
    if (f) file_unref(f);
}

int gta6rt_file_fd(const gta6rt_file* f) { return f ? f->fd : -1; }

gta6rt_status gta6rt_file_map_readonly(const char* path_utf8, const void** out_data, size_t* out_size) {
    if (!path_utf8 || !out_data || !out_size) return GTA6RT_E_INVALID_ARG;
    uint32_t err = 0;
    std::string p = win_to_host_path8(path_utf8, &err);
    if (err) return GTA6RT_E_NOT_FOUND;
    int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno_to_status(errno);
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return GTA6RT_E_IO;
    }
    if (st.st_size == 0) {
        close(fd);
        *out_data = nullptr;
        *out_size = 0;
        return GTA6RT_OK;
    }
    void* m = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return GTA6RT_E_NO_MEMORY;
    *out_data = m;
    *out_size = (size_t)st.st_size;
    return GTA6RT_OK;
}

void gta6rt_file_unmap(const void* data, size_t size) {
    if (data && size) munmap((void*)data, size);
}

// ---- CreateFile and friends ----

HANDLE WINAPI CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES,
                          DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE) {
    (void)dwShareMode;
    DWORD err;
    std::string p = host_of(lpFileName, &err);
    if (err) {
        set_error(err);
        return INVALID_HANDLE_VALUE;
    }
    bool r = (dwDesiredAccess & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0;
    bool w = (dwDesiredAccess & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    bool append_only = (dwDesiredAccess & FILE_APPEND_DATA) && !(dwDesiredAccess & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA));
    int of = r && w ? O_RDWR : w ? O_WRONLY : O_RDONLY;
    struct stat st;
    bool existed = stat(p.c_str(), &st) == 0;
    switch (dwCreationDisposition) {
    case CREATE_NEW: of |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: of |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: of |= O_CREAT; break;
    case TRUNCATE_EXISTING:
        if (!w) {
            set_error(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
        }
        of |= O_TRUNC;
        break;
    default: set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
    }
    if (existed && S_ISDIR(st.st_mode)) {
        if (!(dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS)) {
            set_error(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        of = O_RDONLY;
    }
    mode_t mode = (dwFlagsAndAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644;
    int fd = open(p.c_str(), of | O_CLOEXEC, mode);
    if (fd < 0) {
        int e = errno;
        if (e == EEXIST) set_error(ERROR_FILE_EXISTS);
        else if (e == EISDIR) set_error(ERROR_ACCESS_DENIED);
        else set_error(path_errno(e, p));
        return INVALID_HANDLE_VALUE;
    }
    if (dwFlagsAndAttributes & FILE_FLAG_NO_BUFFERING) fcntl(fd, F_NOCACHE, 1);
    if (dwFlagsAndAttributes & FILE_FLAG_RANDOM_ACCESS) fcntl(fd, F_RDAHEAD, 0);
    gta6rt_file* f = new_file(fd);
    f->readable = r || !w;
    f->writable = w;
    f->append = append_only;
    f->overlapped = (dwFlagsAndAttributes & FILE_FLAG_OVERLAPPED) != 0;
    f->delete_on_close = (dwFlagsAndAttributes & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    f->path = p;
    set_error((existed && (dwCreationDisposition == CREATE_ALWAYS || dwCreationDisposition == OPEN_ALWAYS))
                  ? ERROR_ALREADY_EXISTS
                  : ERROR_SUCCESS);
    return file_handle(f);
}

HANDLE WINAPI CreateFileA(LPCSTR lpFileName, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    std::u16string w = utf8_to_u16(lpFileName);
    return CreateFileW(lpFileName ? w.c_str() : nullptr, a, s, sa, c, f, t);
}

HANDLE WINAPI CreateFile2(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition,
                          LPCREATEFILE2_EXTENDED_PARAMETERS p) {
    DWORD flags = p ? (p->dwFileAttributes | p->dwFileFlags) : 0;
    return CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, p ? p->lpSecurityAttributes : nullptr,
                       dwCreationDisposition, flags, nullptr);
}

BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD n, LPDWORD lpRead, LPOVERLAPPED lpOverlapped) {
    return do_io(hFile, false, lpBuffer, n, lpRead, lpOverlapped, 0);
}

BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD n, LPDWORD lpWritten, LPOVERLAPPED lpOverlapped) {
    return do_io(hFile, true, (void*)lpBuffer, n, lpWritten, lpOverlapped, 0);
}

BOOL WINAPI ReadFileEx(HANDLE hFile, LPVOID lpBuffer, DWORD n, LPOVERLAPPED lpOverlapped,
                       LPOVERLAPPED_COMPLETION_ROUTINE r) {
    if (!lpOverlapped || !r) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    BOOL ok = do_io(hFile, false, lpBuffer, n, nullptr, lpOverlapped, (gta6rt_gva)(uintptr_t)r);
    if (ok) set_error(ERROR_SUCCESS);
    return ok;
}

BOOL WINAPI WriteFileEx(HANDLE hFile, LPCVOID lpBuffer, DWORD n, LPOVERLAPPED lpOverlapped,
                        LPOVERLAPPED_COMPLETION_ROUTINE r) {
    if (!lpOverlapped || !r) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    BOOL ok = do_io(hFile, true, (void*)lpBuffer, n, nullptr, lpOverlapped, (gta6rt_gva)(uintptr_t)r);
    if (ok) set_error(ERROR_SUCCESS);
    return ok;
}

BOOL WINAPI ReadFileScatter(HANDLE, FILE_SEGMENT_ELEMENT*, DWORD, LPDWORD, LPOVERLAPPED) {
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI GetOverlappedResultEx(HANDLE hFile, LPOVERLAPPED ovl, LPDWORD lpBytes, DWORD dwMilliseconds,
                                  BOOL bAlertable) {
    if (!ovl) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    uint64_t deadline = deadline_from_timeout(ms_to_ns(dwMilliseconds));
    while (__atomic_load_n(&ovl->Internal, __ATOMIC_ACQUIRE) == STATUS_PENDING) {
        if (dwMilliseconds == 0) {
            set_error(ERROR_IO_INCOMPLETE);
            return FALSE;
        }
        HANDLE ev = HV(H(ovl->hEvent) & ~1ull);
        gta6rt_waitable* w = ev ? handle_to_waitable(ev) : handle_to_waitable(hFile);
        if (!w) {
            set_error(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        uint64_t now = monotonic_ns();
        uint64_t left = deadline == UINT64_MAX ? GTA6RT_WAIT_INFINITE : (deadline > now ? deadline - now : 0);
        DWORD r = wait_win32(&w, 1, false, left, bAlertable);
        gta6rt_waitable_release(w);
        if (r == WAIT_IO_COMPLETION) {
            set_error(WAIT_IO_COMPLETION);
            return FALSE;
        }
        if (r == WAIT_TIMEOUT && __atomic_load_n(&ovl->Internal, __ATOMIC_ACQUIRE) == STATUS_PENDING) {
            set_error(WAIT_TIMEOUT);
            return FALSE;
        }
        if (__atomic_load_n(&ovl->Internal, __ATOMIC_ACQUIRE) == STATUS_PENDING) gta6rt_thread_yield();
    }
    if (lpBytes) *lpBytes = (DWORD)ovl->InternalHigh;
    DWORD st = (DWORD)ovl->Internal;
    if (st) {
        set_error(win32_from_ntstatus(st));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI GetOverlappedResult(HANDLE hFile, LPOVERLAPPED ovl, LPDWORD lpBytes, BOOL bWait) {
    return GetOverlappedResultEx(hFile, ovl, lpBytes, bWait ? INFINITE : 0, FALSE);
}

BOOL WINAPI CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    int n = cancel_reqs(f, lpOverlapped);
    file_unref(f);
    if (!n && lpOverlapped) {
        set_error(ERROR_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI CancelIo(HANDLE hFile) { return CancelIoEx(hFile, nullptr) || GetLastError() == ERROR_NOT_FOUND; }

HANDLE WINAPI CreateIoCompletionPort(HANDLE FileHandle, HANDLE ExistingCompletionPort, ULONG_PTR CompletionKey, DWORD) {
    if (FileHandle == INVALID_HANDLE_VALUE) {
        if (ExistingCompletionPort) {
            set_error(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        IoPort* p = new IoPort();
        return HV(handle_alloc_internal(GTA6RT_HANDLE_OTHER, p, port_release, port_retain));
    }
    gta6rt_file* f = fref(FileHandle);
    if (!f) return nullptr;
    HANDLE ph = ExistingCompletionPort;
    IoPort* p = nullptr;
    if (ph) {
        p = port_ref(ph);
        if (!p) {
            file_unref(f);
            set_error(ERROR_INVALID_HANDLE);
            return nullptr;
        }
    } else {
        p = new IoPort();
        port_retain(p);
        ph = HV(handle_alloc_internal(GTA6RT_HANDLE_OTHER, p, port_release, port_retain));
    }
    if (f->port) port_release(f->port);
    f->port = p;
    f->key = CompletionKey;
    file_unref(f);
    return ph;
}

BOOL WINAPI GetQueuedCompletionStatus(HANDLE CompletionPort, LPDWORD lpBytes, PULONG_PTR lpKey, LPOVERLAPPED* lpOvl,
                                      DWORD dwMilliseconds) {
    IoPort* p = port_ref(CompletionPort);
    if (!p) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    Packet pk;
    ULONG n = port_take(p, dwMilliseconds, &pk, 1);
    port_release(p);
    if (!n) {
        if (lpOvl) *lpOvl = nullptr;
        set_error(WAIT_TIMEOUT);
        return FALSE;
    }
    if (lpBytes) *lpBytes = pk.bytes;
    if (lpKey) *lpKey = pk.key;
    if (lpOvl) *lpOvl = pk.ovl;
    if (pk.status) {
        set_error(win32_from_ntstatus(pk.status));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI GetQueuedCompletionStatusEx(HANDLE CompletionPort, LPOVERLAPPED_ENTRY entries, ULONG count,
                                        PULONG removed, DWORD dwMilliseconds, BOOL) {
    if (!entries || !count) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    IoPort* p = port_ref(CompletionPort);
    if (!p) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::vector<Packet> pk(count > 64 ? 64 : count);
    ULONG n = port_take(p, dwMilliseconds, pk.data(), (ULONG)pk.size());
    port_release(p);
    if (removed) *removed = n;
    if (!n) {
        set_error(WAIT_TIMEOUT);
        return FALSE;
    }
    for (ULONG i = 0; i < n; i++) {
        entries[i].lpCompletionKey = pk[i].key;
        entries[i].lpOverlapped = pk[i].ovl;
        entries[i].Internal = pk[i].status;
        entries[i].dwNumberOfBytesTransferred = pk[i].bytes;
    }
    set_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI PostQueuedCompletionStatus(HANDLE CompletionPort, DWORD dwBytes, ULONG_PTR dwKey, LPOVERLAPPED lpOvl) {
    IoPort* p = port_ref(CompletionPort);
    if (!p) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    port_post(p, Packet{dwBytes, dwKey, lpOvl, 0});
    port_release(p);
    return TRUE;
}

BOOL WINAPI DeviceIoControl(HANDLE, DWORD dwIoControlCode, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED) {
    OS_LOG(GTA6RT_LOG_DEBUG, "DeviceIoControl(%#x) not supported", dwIoControlCode);
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI FlushFileBuffers(HANDLE hFile) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    int r = f->std_handle ? 0 : fsync(f->fd);
    file_unref(f);
    if (r != 0 && errno != EINVAL && errno != ENOTSUP) {
        set_error(errno_to_win32(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    struct stat st;
    int r = fstat(f->fd, &st);
    file_unref(f);
    if (r != 0) {
        set_error(errno_to_win32(errno));
        return FALSE;
    }
    lpFileSize->QuadPart = st.st_size;
    return TRUE;
}

DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(hFile, &s)) return INVALID_FILE_SIZE;
    if (lpFileSizeHigh) *lpFileSizeHigh = (DWORD)(s.QuadPart >> 32);
    set_error(ERROR_SUCCESS);
    return (DWORD)s.QuadPart;
}

DWORD WINAPI GetFileType(HANDLE hFile) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FILE_TYPE_UNKNOWN;
    struct stat st;
    DWORD t = FILE_TYPE_UNKNOWN;
    if (fstat(f->fd, &st) == 0) {
        if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) t = FILE_TYPE_DISK;
        else if (S_ISCHR(st.st_mode)) t = FILE_TYPE_CHAR;
        else if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) t = FILE_TYPE_PIPE;
    }
    file_unref(f);
    set_error(ERROR_SUCCESS);
    return t;
}

BOOL WINAPI SetFilePointerEx(HANDLE hFile, LARGE_INTEGER dist, PLARGE_INTEGER lpNew, DWORD dwMoveMethod) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    pthread_mutex_lock(&f->pos_mx);
    int64_t base = 0;
    if (dwMoveMethod == FILE_CURRENT) base = (int64_t)f->pos;
    else if (dwMoveMethod == FILE_END) {
        struct stat st;
        fstat(f->fd, &st);
        base = st.st_size;
    } else if (dwMoveMethod != FILE_BEGIN) {
        pthread_mutex_unlock(&f->pos_mx);
        file_unref(f);
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    int64_t np = base + dist.QuadPart;
    if (np < 0) {
        pthread_mutex_unlock(&f->pos_mx);
        file_unref(f);
        set_error(ERROR_NEGATIVE_SEEK);
        return FALSE;
    }
    f->pos = (uint64_t)np;
    if (!f->seekable) lseek(f->fd, np, SEEK_SET);
    pthread_mutex_unlock(&f->pos_mx);
    file_unref(f);
    if (lpNew) lpNew->QuadPart = np;
    return TRUE;
}

DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod) {
    LARGE_INTEGER d, n;
    if (lpDistanceToMoveHigh) d.QuadPart = ((int64_t)*lpDistanceToMoveHigh << 32) | (uint32_t)lDistanceToMove;
    else d.QuadPart = lDistanceToMove;
    if (!SetFilePointerEx(hFile, d, &n, dwMoveMethod)) return INVALID_SET_FILE_POINTER;
    if (lpDistanceToMoveHigh) *lpDistanceToMoveHigh = (LONG)(n.QuadPart >> 32);
    set_error(ERROR_SUCCESS);
    return (DWORD)n.QuadPart;
}

BOOL WINAPI SetEndOfFile(HANDLE hFile) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    pthread_mutex_lock(&f->pos_mx);
    int r = ftruncate(f->fd, (off_t)f->pos);
    pthread_mutex_unlock(&f->pos_mx);
    int e = errno;
    file_unref(f);
    if (r != 0) {
        set_error(errno_to_win32(e));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI GetFileTime(HANDLE hFile, LPFILETIME c, LPFILETIME a, LPFILETIME w) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    struct stat st;
    int r = fstat(f->fd, &st);
    file_unref(f);
    if (r != 0) {
        set_error(errno_to_win32(errno));
        return FALSE;
    }
    if (c) put_ft(c, ft_from_ts(st.st_birthtimespec));
    if (a) put_ft(a, ft_from_ts(st.st_atimespec));
    if (w) put_ft(w, ft_from_ts(st.st_mtimespec));
    return TRUE;
}

BOOL WINAPI SetFileTime(HANDLE hFile, const FILETIME* c, const FILETIME* a, const FILETIME* w) {
    (void)c;
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    struct timespec ts[2];
    auto conv = [](const FILETIME* ft, struct timespec* t) {
        uint64_t v = ft ? get_ft(ft) : 0;
        if (!ft || v == 0 || v == UINT64_MAX) {
            t->tv_sec = 0;
            t->tv_nsec = UTIME_OMIT;
            return;
        }
        v -= 116444736000000000ull;
        t->tv_sec = (time_t)(v / 10000000ull);
        t->tv_nsec = (long)(v % 10000000ull) * 100;
    };
    conv(a, &ts[0]);
    conv(w, &ts[1]);
    int r = futimens(f->fd, ts);
    file_unref(f);
    if (r != 0) {
        set_error(errno_to_win32(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION info) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    struct stat st;
    int r = fstat(f->fd, &st);
    file_unref(f);
    if (r != 0) {
        set_error(errno_to_win32(errno));
        return FALSE;
    }
    memset(info, 0, sizeof *info);
    info->dwFileAttributes = attrs_from_stat(st);
    put_ft(&info->ftCreationTime, ft_from_ts(st.st_birthtimespec));
    put_ft(&info->ftLastAccessTime, ft_from_ts(st.st_atimespec));
    put_ft(&info->ftLastWriteTime, ft_from_ts(st.st_mtimespec));
    info->dwVolumeSerialNumber = (DWORD)st.st_dev;
    info->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    info->nFileSizeLow = (DWORD)st.st_size;
    info->nNumberOfLinks = st.st_nlink;
    info->nFileIndexHigh = (DWORD)(st.st_ino >> 32);
    info->nFileIndexLow = (DWORD)st.st_ino;
    return TRUE;
}

BOOL WINAPI GetFileInformationByHandleEx(HANDLE hFile, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID buf, DWORD size) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    struct stat st;
    int r = fstat(f->fd, &st);
    std::string path = f->path;
    file_unref(f);
    if (r != 0) {
        set_error(errno_to_win32(errno));
        return FALSE;
    }
    switch (cls) {
    case FileBasicInfo: {
        if (size < sizeof(BasicInfoT)) break;
        auto* b = (BasicInfoT*)buf;
        b->CreationTime.QuadPart = (LONGLONG)ft_from_ts(st.st_birthtimespec);
        b->LastAccessTime.QuadPart = (LONGLONG)ft_from_ts(st.st_atimespec);
        b->LastWriteTime.QuadPart = (LONGLONG)ft_from_ts(st.st_mtimespec);
        b->ChangeTime.QuadPart = (LONGLONG)ft_from_ts(st.st_ctimespec);
        b->FileAttributes = attrs_from_stat(st);
        return TRUE;
    }
    case FileStandardInfo: {
        if (size < sizeof(FILE_STANDARD_INFO)) break;
        auto* s = (FILE_STANDARD_INFO*)buf;
        memset(s, 0, sizeof *s);
        s->AllocationSize.QuadPart = (LONGLONG)st.st_blocks * 512;
        s->EndOfFile.QuadPart = st.st_size;
        s->NumberOfLinks = st.st_nlink;
        s->Directory = S_ISDIR(st.st_mode);
        return TRUE;
    }
    case FileNameInfo: {
        std::u16string w = host_to_win_path(path);
        if (w.size() >= 2 && w[1] == u':') w = w.substr(2);
        if (size < 4) break;
        DWORD bytes = (DWORD)(w.size() * 2);
        *(DWORD*)buf = bytes;
        if (size < 4 + bytes) {
            set_error(ERROR_MORE_DATA);
            return FALSE;
        }
        memcpy((char*)buf + 4, w.data(), bytes);
        return TRUE;
    }
    case FileAttributeTagInfo: {
        if (size < 8) break;
        ((DWORD*)buf)[0] = attrs_from_stat(st);
        ((DWORD*)buf)[1] = 0;
        return TRUE;
    }
    default: set_error(ERROR_INVALID_PARAMETER); return FALSE;
    }
    set_error(24 /* ERROR_BAD_LENGTH */);
    return FALSE;
}

BOOL WINAPI SetFileInformationByHandle(HANDLE hFile, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID buf, DWORD size) {
    gta6rt_file* f = fref(hFile);
    if (!f) return FALSE;
    BOOL ok = TRUE;
    switch (cls) {
    case FileEndOfFileInfo:
        if (size < 8 || ftruncate(f->fd, (off_t)((LARGE_INTEGER*)buf)->QuadPart) != 0) {
            set_error(size < 8 ? ERROR_INVALID_PARAMETER : errno_to_win32(errno));
            ok = FALSE;
        }
        break;
    case FileDispositionInfo:
        if (size < 1) ok = FALSE;
        else f->delete_on_close = *(BOOLEAN*)buf != 0;
        break;
    case FileRenameInfo: {
        auto* ri = (RenameInfoT*)buf;
        std::u16string name(ri->FileName, ri->FileNameLength / 2);
        uint32_t err = 0;
        std::string to = win_to_host_path(name.c_str(), &err);
        struct stat st;
        if (err) {
            set_error(err);
            ok = FALSE;
        } else if (!ri->ReplaceIfExists && lstat(to.c_str(), &st) == 0) {
            set_error(ERROR_ALREADY_EXISTS);
            ok = FALSE;
        } else if (rename(f->path.c_str(), to.c_str()) != 0) {
            set_error(path_errno(errno, to));
            ok = FALSE;
        } else {
            f->path = to;
        }
        break;
    }
    case FileAllocationInfo:
    case FileBasicInfo:
    case FileIoPriorityHintInfo: break;
    default: set_error(ERROR_INVALID_PARAMETER); ok = FALSE;
    }
    file_unref(f);
    return ok;
}

DWORD WINAPI GetFinalPathNameByHandleW(HANDLE hFile, LPWSTR lpszFilePath, DWORD cchFilePath, DWORD) {
    gta6rt_file* f = fref(hFile);
    if (!f) return 0;
    char buf[PATH_MAX];
    std::string p = fcntl(f->fd, F_GETPATH, buf) == 0 ? std::string(buf) : f->path;
    file_unref(f);
    std::u16string w = u"\\\\?\\" + host_to_win_path(p);
    return copy_out_w(w, lpszFilePath, cchFilePath);
}

HANDLE WINAPI FindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS, LPVOID lpFindFileData,
                               FINDEX_SEARCH_OPS fSearchOp, LPVOID, DWORD) {
    return find_first(lpFileName, (LPWIN32_FIND_DATAW)lpFindFileData, fSearchOp == FindExSearchLimitToDirectories);
}

HANDLE WINAPI FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW d) { return find_first(lpFileName, d, false); }

HANDLE WINAPI FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA d) {
    WIN32_FIND_DATAW w;
    std::u16string n = utf8_to_u16(lpFileName);
    HANDLE h = find_first(lpFileName ? n.c_str() : nullptr, &w, false);
    if (h != INVALID_HANDLE_VALUE) w_to_a(&w, d);
    return h;
}

HANDLE WINAPI FindFirstFileExA(LPCSTR lpFileName, FINDEX_INFO_LEVELS, LPVOID d, FINDEX_SEARCH_OPS op, LPVOID, DWORD) {
    WIN32_FIND_DATAW w;
    std::u16string n = utf8_to_u16(lpFileName);
    HANDLE h = find_first(lpFileName ? n.c_str() : nullptr, &w, op == FindExSearchLimitToDirectories);
    if (h != INVALID_HANDLE_VALUE) w_to_a(&w, (LPWIN32_FIND_DATAA)d);
    return h;
}

BOOL WINAPI FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW d) {
    FindState* fs = (FindState*)gta6rt_handle_lookup(H(hFindFile), GTA6RT_HANDLE_FIND);
    if (!fs) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (fs->next >= fs->names.size()) {
        set_error(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    fill_find(fs->names[fs->next], fs->stats[fs->next], d);
    fs->next++;
    return TRUE;
}

BOOL WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA d) {
    WIN32_FIND_DATAW w;
    if (!FindNextFileW(hFindFile, &w)) return FALSE;
    w_to_a(&w, d);
    return TRUE;
}

BOOL WINAPI FindClose(HANDLE hFindFile) {
    if (handle_kind(H(hFindFile)) != GTA6RT_HANDLE_FIND) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return gta6rt_handle_close(H(hFindFile)) == GTA6RT_OK;
}

DWORD WINAPI GetFileAttributesW(LPCWSTR lpFileName) {
    DWORD err;
    std::string p = host_of(lpFileName, &err);
    if (err) {
        set_error(err);
        return INVALID_FILE_ATTRIBUTES;
    }
    struct stat st;
    if (stat(p.c_str(), &st) != 0) {
        set_error(path_errno(errno, p));
        return INVALID_FILE_ATTRIBUTES;
    }
    return attrs_from_stat(st);
}

DWORD WINAPI GetFileAttributesA(LPCSTR lpFileName) {
    std::u16string w = utf8_to_u16(lpFileName);
    return GetFileAttributesW(lpFileName ? w.c_str() : nullptr);
}

BOOL WINAPI GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS, LPVOID info) {
    DWORD err;
    std::string p = host_of(lpFileName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    struct stat st;
    if (stat(p.c_str(), &st) != 0) {
        set_error(path_errno(errno, p));
        return FALSE;
    }
    auto* d = (WIN32_FILE_ATTRIBUTE_DATA*)info;
    d->dwFileAttributes = attrs_from_stat(st);
    put_ft(&d->ftCreationTime, ft_from_ts(st.st_birthtimespec));
    put_ft(&d->ftLastAccessTime, ft_from_ts(st.st_atimespec));
    put_ft(&d->ftLastWriteTime, ft_from_ts(st.st_mtimespec));
    uint64_t sz = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    d->nFileSizeHigh = (DWORD)(sz >> 32);
    d->nFileSizeLow = (DWORD)sz;
    return TRUE;
}

BOOL WINAPI SetFileAttributesW(LPCWSTR lpFileName, DWORD attrs) {
    DWORD err;
    std::string p = host_of(lpFileName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    struct stat st;
    if (stat(p.c_str(), &st) != 0) {
        set_error(path_errno(errno, p));
        return FALSE;
    }
    mode_t m = st.st_mode & 07777;
    if (attrs & FILE_ATTRIBUTE_READONLY) m &= ~(mode_t)0222;
    else m |= S_IWUSR;
    if (chmod(p.c_str(), m) != 0) {
        set_error(errno_to_win32(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI DeleteFileW(LPCWSTR lpFileName) {
    DWORD err;
    std::string p = host_of(lpFileName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    struct stat st;
    if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        set_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (unlink(p.c_str()) != 0) {
        set_error(path_errno(errno, p));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI DeleteFileA(LPCSTR lpFileName) {
    std::u16string w = utf8_to_u16(lpFileName);
    return DeleteFileW(lpFileName ? w.c_str() : nullptr);
}

BOOL WINAPI CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES) {
    DWORD err;
    std::string p = host_of(lpPathName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    if (mkdir(p.c_str(), 0755) != 0) {
        set_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : path_errno(errno, p));
        if (errno == ENOENT) set_error(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES sa) {
    std::u16string w = utf8_to_u16(lpPathName);
    return CreateDirectoryW(lpPathName ? w.c_str() : nullptr, sa);
}

BOOL WINAPI CreateDirectoryExW(LPCWSTR, LPCWSTR lpNewDirectory, LPSECURITY_ATTRIBUTES sa) {
    return CreateDirectoryW(lpNewDirectory, sa);
}

BOOL WINAPI RemoveDirectoryW(LPCWSTR lpPathName) {
    DWORD err;
    std::string p = host_of(lpPathName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    if (rmdir(p.c_str()) != 0) {
        set_error(errno == ENOTDIR ? ERROR_DIRECTORY : path_errno(errno, p));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags) {
    DWORD err;
    std::string from = host_of(lpExistingFileName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    if (!lpNewFileName) {
        if (dwFlags & MOVEFILE_DELAY_UNTIL_REBOOT) return TRUE;
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::string to = host_of(lpNewFileName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    struct stat st;
    if (lstat(from.c_str(), &st) != 0) {
        set_error(path_errno(errno, from));
        return FALSE;
    }
    struct stat st2;
    if (!(dwFlags & MOVEFILE_REPLACE_EXISTING) && lstat(to.c_str(), &st2) == 0 &&
        strcasecmp(from.c_str(), to.c_str()) != 0) {
        set_error(ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    if (rename(from.c_str(), to.c_str()) == 0) return TRUE;
    if (errno == EXDEV && (dwFlags & MOVEFILE_COPY_ALLOWED)) {
        if (copyfile(from.c_str(), to.c_str(), nullptr, COPYFILE_ALL | COPYFILE_RECURSIVE) == 0) {
            if (S_ISDIR(st.st_mode)) {
                set_error(ERROR_NOT_SAME_DEVICE);
                return FALSE;
            }
            unlink(from.c_str());
            return TRUE;
        }
    }
    set_error(path_errno(errno, to));
    return FALSE;
}

BOOL WINAPI CopyFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, LPPROGRESS_ROUTINE, LPVOID, LPBOOL,
                        DWORD dwCopyFlags) {
    DWORD err;
    std::string from = host_of(lpExistingFileName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    std::string to = host_of(lpNewFileName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    struct stat st;
    if (stat(from.c_str(), &st) != 0) {
        set_error(path_errno(errno, from));
        return FALSE;
    }
    if (S_ISDIR(st.st_mode)) {
        set_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    copyfile_flags_t fl = COPYFILE_DATA | COPYFILE_STAT;
    if (dwCopyFlags & COPY_FILE_FAIL_IF_EXISTS) fl |= COPYFILE_EXCL;
    if (copyfile(from.c_str(), to.c_str(), nullptr, fl) != 0) {
        set_error(errno == EEXIST ? ERROR_FILE_EXISTS : path_errno(errno, to));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI CopyFileW(LPCWSTR a, LPCWSTR b, BOOL bFailIfExists) {
    return CopyFileExW(a, b, nullptr, nullptr, nullptr, bFailIfExists ? COPY_FILE_FAIL_IF_EXISTS : 0);
}

HRESULT WINAPI CopyFile2(PCWSTR a, PCWSTR b, COPYFILE2_EXTENDED_PARAMETERS* p) {
    if (CopyFileExW(a, b, nullptr, nullptr, nullptr, p ? p->dwCopyFlags : 0)) return S_OK;
    return (HRESULT)(0x80070000u | (GetLastError() & 0xFFFF));
}

BOOLEAN WINAPI CreateSymbolicLinkW(LPCWSTR lpSymlinkFileName, LPCWSTR lpTargetFileName, DWORD) {
    DWORD err;
    std::string link = host_of(lpSymlinkFileName, &err);
    if (err) {
        set_error(err);
        return 0;
    }
    std::string target = u16_to_utf8(lpTargetFileName);
    bool absolute = target.size() >= 2 && (target[1] == ':' || target[0] == '\\' || target[0] == '/');
    if (absolute) target = host_of(lpTargetFileName, &err);
    else
        for (char& c : target)
            if (c == '\\') c = '/';
    if (symlink(target.c_str(), link.c_str()) != 0) {
        set_error(errno_to_win32(errno));
        return 0;
    }
    return 1;
}

BOOL WINAPI CreateHardLinkW(LPCWSTR lpFileName, LPCWSTR lpExistingFileName, LPSECURITY_ATTRIBUTES) {
    DWORD err;
    std::string a = host_of(lpFileName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    std::string b = host_of(lpExistingFileName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    if (link(b.c_str(), a.c_str()) != 0) {
        set_error(path_errno(errno, b));
        return FALSE;
    }
    return TRUE;
}

DWORD WINAPI GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR* lpFilePart) {
    if (!lpFileName || !*lpFileName) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::u16string in(lpFileName);
    for (char16_t& c : in)
        if (c == u'/') c = u'\\';
    std::u16string full;
    bool unc_ex = in.rfind(u"\\\\?\\", 0) == 0;
    if (unc_ex) {
        full = in;
    } else if (in.size() >= 2 && in[1] == u':') {
        full = in;
    } else if (!in.empty() && in[0] == u'\\') {
        full = u"Z:" + in;
    } else {
        char cwd[PATH_MAX];
        std::u16string base = getcwd(cwd, sizeof cwd) ? host_to_win_path(cwd) : u"Z:\\";
        if (base.back() != u'\\') base += u'\\';
        full = base + in;
    }
    std::u16string prefix = full.substr(0, unc_ex ? 6 : 2);
    std::u16string rest = full.substr(prefix.size());
    std::vector<std::u16string> parts;
    size_t i = 0;
    while (i <= rest.size()) {
        size_t j = rest.find(u'\\', i);
        if (j == std::u16string::npos) j = rest.size();
        std::u16string c = rest.substr(i, j - i);
        i = j + 1;
        if (c.empty() || c == u".") continue;
        if (c == u"..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(c);
    }
    std::u16string out = prefix + u"\\";
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) out += u'\\';
        out += parts[k];
    }
    if (in.back() == u'\\' && !parts.empty()) out += u'\\';
    DWORD r = copy_out_w(out, lpBuffer, nBufferLength);
    if (lpFilePart && lpBuffer && r < nBufferLength) {
        size_t s = out.find_last_of(u'\\');
        *lpFilePart = (s == std::u16string::npos || s + 1 >= out.size()) ? nullptr : lpBuffer + s + 1;
    }
    return r;
}

DWORD WINAPI GetLongPathNameW(LPCWSTR lpszShortPath, LPWSTR lpszLongPath, DWORD cchBuffer) {
    if (!lpszShortPath) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (GetFileAttributesW(lpszShortPath) == INVALID_FILE_ATTRIBUTES) return 0;
    return copy_out_w(std::u16string(lpszShortPath), lpszLongPath, cchBuffer);
}

DWORD WINAPI GetTempPathW(DWORD nBufferLength, LPWSTR lpBuffer) {
    const char* t = getenv("TMPDIR");
    std::string tmp = t && *t ? t : "/tmp/";
    std::u16string w = host_to_win_path(tmp);
    if (w.empty() || w.back() != u'\\') w += u'\\';
    return copy_out_w(w, lpBuffer, nBufferLength);
}

DWORD WINAPI GetCurrentDirectoryW(DWORD nBufferLength, LPWSTR lpBuffer) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) {
        set_error(errno_to_win32(errno));
        return 0;
    }
    return copy_out_w(host_to_win_path(cwd), lpBuffer, nBufferLength);
}

BOOL WINAPI SetCurrentDirectoryW(LPCWSTR lpPathName) {
    DWORD err;
    std::string p = host_of(lpPathName, &err);
    if (err) {
        set_error(err);
        return FALSE;
    }
    if (chdir(p.c_str()) != 0) {
        set_error(path_errno(errno, p));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName, PULARGE_INTEGER lpFree, PULARGE_INTEGER lpTotal,
                                PULARGE_INTEGER lpTotalFree) {
    std::string p = ".";
    if (lpDirectoryName) {
        DWORD err;
        p = host_of(lpDirectoryName, &err);
        if (err) {
            set_error(err);
            return FALSE;
        }
    }
    struct statfs s;
    if (statfs(p.c_str(), &s) != 0) {
        set_error(path_errno(errno, p));
        return FALSE;
    }
    if (lpFree) lpFree->QuadPart = (ULONGLONG)s.f_bavail * s.f_bsize;
    if (lpTotal) lpTotal->QuadPart = (ULONGLONG)s.f_blocks * s.f_bsize;
    if (lpTotalFree) lpTotalFree->QuadPart = (ULONGLONG)s.f_bfree * s.f_bsize;
    return TRUE;
}

BOOL WINAPI GetVolumeInformationW(LPCWSTR, LPWSTR lpVolumeNameBuffer, DWORD nVolumeNameSize, LPDWORD lpSerial,
                                  LPDWORD lpMaxComponentLength, LPDWORD lpFlags, LPWSTR lpFsName, DWORD nFsNameSize) {
    if (lpVolumeNameBuffer && nVolumeNameSize) lpVolumeNameBuffer[0] = 0;
    if (lpSerial) *lpSerial = 0x47544136;
    if (lpMaxComponentLength) *lpMaxComponentLength = 255;
    if (lpFlags) *lpFlags = 0x3 /* case preserved, case sensitive search off */;
    if (lpFsName) copy_out_w(u"NTFS", lpFsName, nFsNameSize);
    return TRUE;
}

BOOL WINAPI LockFile(HANDLE, DWORD, DWORD, DWORD, DWORD) { return TRUE; }
BOOL WINAPI UnlockFile(HANDLE, DWORD, DWORD, DWORD, DWORD) { return TRUE; }

HANDLE WINAPI GetStdHandle(DWORD nStdHandle) {
    int idx = nStdHandle == STD_INPUT_HANDLE ? 0 : nStdHandle == STD_OUTPUT_HANDLE ? 1 : nStdHandle == STD_ERROR_HANDLE ? 2 : -1;
    if (idx < 0) {
        set_error(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    pthread_mutex_lock(&g_std_mx);
    if (!g_std[idx]) {
        gta6rt_file* f = new_file(idx);
        f->std_handle = true;
        f->readable = idx == 0;
        f->writable = idx != 0;
        f->seekable = false;
        g_std[idx] = file_handle(f);
    }
    HANDLE h = g_std[idx];
    pthread_mutex_unlock(&g_std_mx);
    return h;
}

BOOL WINAPI SetStdHandle(DWORD nStdHandle, HANDLE hHandle) {
    int idx = nStdHandle == STD_INPUT_HANDLE ? 0 : nStdHandle == STD_OUTPUT_HANDLE ? 1 : nStdHandle == STD_ERROR_HANDLE ? 2 : -1;
    if (idx < 0) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&g_std_mx);
    g_std[idx] = hHandle;
    pthread_mutex_unlock(&g_std_mx);
    return TRUE;
}

// ---- file mappings ----

HANDLE WINAPI CreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES, DWORD flProtect, DWORD dwMaximumSizeHigh,
                                 DWORD dwMaximumSizeLow, LPCWSTR lpName) {
    uint64_t size = ((uint64_t)dwMaximumSizeHigh << 32) | dwMaximumSizeLow;
    if (lpName && *lpName) {
        pthread_mutex_lock(&g_map_mx);
        if (g_named_maps)
            for (Mapping* m : *g_named_maps)
                if (m->name == lpName) {
                    m->refs.fetch_add(1);
                    pthread_mutex_unlock(&g_map_mx);
                    set_error(ERROR_ALREADY_EXISTS);
                    return HV(handle_alloc_internal(GTA6RT_HANDLE_MAPPING, m, map_release, map_retain));
                }
        pthread_mutex_unlock(&g_map_mx);
    }
    Mapping* m = new Mapping();
    m->protect = flProtect;
    if (hFile == INVALID_HANDLE_VALUE || !hFile) {
        if (!size) {
            delete m;
            set_error(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        memory_object_size_t sz = (size + host_page() - 1) & ~(uint64_t)(host_page() - 1);
        kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &sz, 0,
                                                     MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &m->entry,
                                                     MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            delete m;
            set_error(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        m->size = size;
    } else {
        void (*rel)(void*) = nullptr;
        gta6rt_file* f = file_from_handle(hFile, &rel);
        if (!f) {
            delete m;
            set_error(ERROR_INVALID_HANDLE);
            return nullptr;
        }
        struct stat st;
        fstat(f->fd, &st);
        DWORD p = flProtect & 0xFF;
        bool wr = p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE;
        if (!size) size = (uint64_t)st.st_size;
        if (!size) {
            rel(f);
            delete m;
            set_error(1006 /* ERROR_FILE_INVALID */);
            return nullptr;
        }
        if (size > (uint64_t)st.st_size) {
            if (!wr || !f->writable || ftruncate(f->fd, (off_t)size) != 0) {
                rel(f);
                delete m;
                set_error(ERROR_ACCESS_DENIED);
                return nullptr;
            }
        }
        m->fd = dup(f->fd);
        m->size = size;
        rel(f);
    }
    if (lpName && *lpName) {
        m->name = lpName;
        pthread_mutex_lock(&g_map_mx);
        if (!g_named_maps) g_named_maps = new std::vector<Mapping*>();
        g_named_maps->push_back(m);
        pthread_mutex_unlock(&g_map_mx);
    }
    set_error(ERROR_SUCCESS);
    return HV(handle_alloc_internal(GTA6RT_HANDLE_MAPPING, m, map_release, map_retain));
}

HANDLE WINAPI CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES sa, DWORD p, DWORD hi, DWORD lo, LPCSTR lpName) {
    std::u16string n = utf8_to_u16(lpName);
    return CreateFileMappingW(hFile, sa, p, hi, lo, lpName ? n.c_str() : nullptr);
}

HANDLE WINAPI OpenFileMappingW(DWORD, BOOL, LPCWSTR lpName) {
    if (!lpName) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    pthread_mutex_lock(&g_map_mx);
    if (g_named_maps)
        for (Mapping* m : *g_named_maps)
            if (m->name == lpName) {
                m->refs.fetch_add(1);
                pthread_mutex_unlock(&g_map_mx);
                return HV(handle_alloc_internal(GTA6RT_HANDLE_MAPPING, m, map_release, map_retain));
            }
    pthread_mutex_unlock(&g_map_mx);
    set_error(ERROR_FILE_NOT_FOUND);
    return nullptr;
}

LPVOID WINAPI MapViewOfFileEx(HANDLE h, DWORD access, DWORD offHi, DWORD offLo, SIZE_T n, LPVOID base) {
    Mapping* m = mapping_ref(h);
    if (!m) return nullptr;
    void* r = map_view(m, access, ((uint64_t)offHi << 32) | offLo, n, base, false);
    map_release(m);
    return r;
}

LPVOID WINAPI MapViewOfFile(HANDLE h, DWORD access, DWORD offHi, DWORD offLo, SIZE_T n) {
    return MapViewOfFileEx(h, access, offHi, offLo, n, nullptr);
}

PVOID WINAPI MapViewOfFile3(HANDLE h, HANDLE, PVOID base, ULONG64 off, SIZE_T n, ULONG allocType, ULONG pageProt,
                            MEM_EXTENDED_PARAMETER*, ULONG) {
    Mapping* m = mapping_ref(h);
    if (!m) return nullptr;
    DWORD acc = FILE_MAP_READ;
    DWORD p = pageProt & 0xFF;
    if (p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE) acc |= FILE_MAP_WRITE;
    if (p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY) acc = FILE_MAP_COPY;
    if (p & 0xF0) acc |= FILE_MAP_EXECUTE;
    void* r = map_view(m, acc, off, n, base, (allocType & MEM_REPLACE_PLACEHOLDER) != 0);
    map_release(m);
    return r;
}

BOOL WINAPI UnmapViewOfFile2(HANDLE, PVOID base, ULONG flags) {
    size_t hs = 0;
    if (!vm_unregister_view(base, &hs)) {
        set_error(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    if (flags & MEM_PRESERVE_PLACEHOLDER) {
        void* nb = nullptr;
        mmap(base, hs, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED | MAP_NORESERVE, -1, 0);
        munmap(base, hs);
        if (gta6rt_vm_reserve(base, hs, GTA6RT_VM_FIXED | GTA6RT_VM_PLACEHOLDER, &nb) != GTA6RT_OK) {
            set_error(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        return TRUE;
    }
    munmap(base, hs);
    return TRUE;
}

BOOL WINAPI UnmapViewOfFile(LPCVOID base) { return UnmapViewOfFile2(GetCurrentProcess(), (PVOID)base, 0); }

BOOL WINAPI FlushViewOfFile(LPCVOID base, SIZE_T n) {
    size_t hp = host_page();
    uintptr_t a = (uintptr_t)base & ~(uintptr_t)(hp - 1);
    if (!n) {
        gta6rt_vm_region r;
        if (gta6rt_vm_query(base, &r) != GTA6RT_OK) return FALSE;
        n = r.size;
    }
    size_t len = (((uintptr_t)base + n + hp - 1) & ~(uintptr_t)(hp - 1)) - a;
    if (msync((void*)a, len, MS_SYNC) != 0) {
        set_error(errno_to_win32(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI SetNamedPipeHandleState(HANDLE hNamedPipe, LPDWORD, LPDWORD, LPDWORD) {
    OS_LOG(GTA6RT_LOG_DEBUG, "SetNamedPipeHandleState(%#llx): no named pipes", (unsigned long long)H(hNamedPipe));
    set_error(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI WaitNamedPipeW(LPCWSTR lpNamedPipeName, DWORD) {
    OS_LOG(GTA6RT_LOG_DEBUG, "WaitNamedPipeW(%s): no named pipes", u16_to_utf8(lpNamedPipeName).c_str());
    set_error(ERROR_FILE_NOT_FOUND);
    return FALSE;
}

} // extern "C"
