#include "internal.h"

#include <vector>

using namespace gta6rt_os;

namespace {

constexpr uint64_t kBase = 0x1000;

// Lookups are lock-free: slots live in fixed chunks that never move, and an Object is never freed (closed ones are
// recycled), so a reader may load a stale pointer, take a reference only if the count is nonzero and then confirm
// the slot still holds it. Allocation, close and recycling take g_h_mx.
struct Object {
    std::atomic<int> refs{0};
    gta6rt_handle_kind kind;
    void* object;
    void (*close)(void*);
    void (*retain)(void*);
    void (*release)(void*);
    bool os_other;
    Object* next_free;
};

struct Slot {
    std::atomic<Object*> obj{nullptr};
    std::atomic<uint32_t> flags{0};
    uint32_t next_free = UINT32_MAX;
};

constexpr size_t kChunkBits = 12, kChunkSlots = size_t{1} << kChunkBits, kMaxChunks = 1024;

pthread_mutex_t g_h_mx = PTHREAD_MUTEX_INITIALIZER;
std::atomic<Slot*> g_chunks[kMaxChunks];
std::atomic<size_t> g_nslots{0};
uint32_t g_free_head = UINT32_MAX;
Object* g_free_objs;

Object* new_object() {
    pthread_mutex_lock(&g_h_mx);
    Object* o = g_free_objs;
    if (o) g_free_objs = o->next_free;
    pthread_mutex_unlock(&g_h_mx);
    return o ? o : new Object();
}

void obj_unref(Object* o) {
    if (o->refs.fetch_sub(1) == 1) {
        if (o->close) o->close(o->object);
        pthread_mutex_lock(&g_h_mx);
        o->next_free = g_free_objs;
        g_free_objs = o;
        pthread_mutex_unlock(&g_h_mx);
    }
}

Slot* slot_at(size_t i) {
    if (i >= g_nslots.load(std::memory_order_acquire)) return nullptr;
    Slot* c = g_chunks[i >> kChunkBits].load(std::memory_order_acquire);
    return c ? &c[i & (kChunkSlots - 1)] : nullptr;
}

Slot* slot_of(uint64_t h) {
    if (h < kBase || (h - kBase) & 3) return nullptr;
    return slot_at((size_t)((h - kBase) >> 2));
}

// The handle's Object with a reference taken (release with obj_unref), or null.
Object* acquire(uint64_t h) {
    Slot* s = slot_of(h);
    if (!s) return nullptr;
    for (;;) {
        Object* o = s->obj.load(std::memory_order_acquire);
        if (!o) return nullptr;
        int r = o->refs.load();
        while (r > 0 && !o->refs.compare_exchange_weak(r, r + 1)) {
        }
        if (r > 0) {
            if (s->obj.load(std::memory_order_acquire) == o) return o;
            obj_unref(o);
        }
    }
}

uint64_t insert(Object* o, uint32_t flags) {
    pthread_mutex_lock(&g_h_mx);
    size_t i;
    Slot* s;
    if (g_free_head != UINT32_MAX) {
        i = g_free_head;
        s = slot_at(i);
        g_free_head = s->next_free;
    } else {
        i = g_nslots.load();
        if ((i >> kChunkBits) >= kMaxChunks) {
            pthread_mutex_unlock(&g_h_mx);
            OS_LOG(GTA6RT_LOG_ERROR, "handle table full (%zu handles)", i);
            obj_unref(o);
            return 0;
        }
        if (!g_chunks[i >> kChunkBits].load()) g_chunks[i >> kChunkBits].store(new Slot[kChunkSlots]);
        g_nslots.store(i + 1, std::memory_order_release);
        s = slot_at(i);
    }
    s->next_free = UINT32_MAX;
    s->flags.store(flags);
    s->obj.store(o, std::memory_order_release);
    pthread_mutex_unlock(&g_h_mx);
    return kBase + ((uint64_t)i << 2);
}

Object* make_object(gta6rt_handle_kind kind, void* object, void (*close)(void*), void (*retain)(void*),
                    void (*release)(void*), bool os_other) {
    Object* o = new_object();
    o->kind = kind;
    o->object = object;
    o->close = close;
    o->retain = retain;
    o->release = release;
    o->os_other = os_other;
    o->next_free = nullptr;
    o->refs.store(1, std::memory_order_release);
    return o;
}

} // namespace

namespace gta6rt_os {

uint64_t handle_alloc_internal(gta6rt_handle_kind kind, void* object, void (*close)(void*), void (*retain)(void*)) {
    return insert(make_object(kind, object, close, retain, close, kind == GTA6RT_HANDLE_OTHER), 0);
}

void* handle_ref(uint64_t h, gta6rt_handle_kind kind, void (**release)(void*)) {
    Object* o = acquire(h);
    if (!o) return nullptr;
    void* r = nullptr;
    if ((kind == GTA6RT_HANDLE_NONE || o->kind == kind) && o->retain) {
        o->retain(o->object);
        r = o->object;
        if (release) *release = o->release;
    }
    obj_unref(o);
    return r;
}

void* handle_ref_kind(uint64_t h, gta6rt_handle_kind* kind, void (**release)(void*)) {
    *kind = GTA6RT_HANDLE_NONE;
    Object* o = acquire(h);
    if (!o) return nullptr;
    void* r = nullptr;
    *kind = o->kind;
    if (o->retain) {
        o->retain(o->object);
        r = o->object;
        if (release) *release = o->release;
    }
    obj_unref(o);
    return r;
}

void* handle_pin(uint64_t h, gta6rt_handle_kind* kind, void** object) {
    Object* o = acquire(h);
    if (!o) return nullptr;
    *kind = o->kind;
    *object = o->object;
    return o;
}

void handle_unpin(void* pin) { obj_unref(static_cast<Object*>(pin)); }

gta6rt_handle_kind handle_kind(uint64_t h) {
    Object* o = acquire(h);
    if (!o) return GTA6RT_HANDLE_NONE;
    const gta6rt_handle_kind k = o->kind;
    obj_unref(o);
    return k;
}

OtherObject* handle_other(uint64_t h, OtherKind kind) {
    Object* o = acquire(h);
    if (!o) return nullptr;
    OtherObject* r = nullptr;
    if (o->os_other) {
        OtherObject* oo = (OtherObject*)o->object;
        if (oo && oo->kind == kind) r = oo;
    }
    obj_unref(o);
    return r;
}

} // namespace gta6rt_os

extern "C" {

gta6rt_status gta6rt_handle_alloc(gta6rt_handle_kind kind, void* object, void (*close)(void*), uint64_t* out_handle) {
    if (!out_handle) return GTA6RT_E_INVALID_ARG;
    *out_handle = insert(make_object(kind, object, close, nullptr, nullptr, false), 0);
    return *out_handle ? GTA6RT_OK : GTA6RT_E_NO_MEMORY;
}

void* gta6rt_handle_lookup(uint64_t handle, gta6rt_handle_kind kind) {
    Object* o = acquire(handle);
    if (!o) return nullptr;
    void* r = (kind == GTA6RT_HANDLE_NONE || o->kind == kind) ? o->object : nullptr;
    obj_unref(o);
    return r;
}

gta6rt_status gta6rt_handle_close(uint64_t handle) {
    pthread_mutex_lock(&g_h_mx);
    Slot* s = slot_of(handle);
    Object* o = s ? s->obj.load() : nullptr;
    if (!o) {
        pthread_mutex_unlock(&g_h_mx);
        return GTA6RT_E_INVALID_ARG;
    }
    if (s->flags.load() & HANDLE_FLAG_PROTECT_FROM_CLOSE) {
        pthread_mutex_unlock(&g_h_mx);
        return GTA6RT_E_ACCESS;
    }
    s->obj.store(nullptr);
    s->next_free = g_free_head;
    g_free_head = (uint32_t)((handle - kBase) >> 2);
    pthread_mutex_unlock(&g_h_mx);
    obj_unref(o);
    return GTA6RT_OK;
}

BOOL WINAPI CloseHandle(HANDLE hObject) {
    uint64_t h = H(hObject);
    if (h == kPseudoProcess || h == kPseudoThread) return TRUE;
    gta6rt_status st = gta6rt_handle_close(h);
    if (st == GTA6RT_OK) return TRUE;
    set_error(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
                            LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions) {
    (void)hSourceProcessHandle, (void)hTargetProcessHandle, (void)dwDesiredAccess, (void)bInheritHandle;
    uint64_t h = H(hSourceHandle);
    if (h == kPseudoThread) {
        gta6rt_thread* t = gta6rt_thread_current();
        thread_retain(t);
        uint64_t nh = handle_alloc_internal(
            GTA6RT_HANDLE_THREAD, t, [](void* p) { gta6rt_thread_release((gta6rt_thread*)p); },
            [](void* p) { thread_retain((gta6rt_thread*)p); });
        if (lpTargetHandle) *lpTargetHandle = HV(nh);
        return TRUE;
    }
    if (h == kPseudoProcess) {
        if (lpTargetHandle) *lpTargetHandle = hSourceHandle;
        return TRUE;
    }
    Object* o = acquire(h);
    if (!o) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    uint64_t nh = insert(o, 0);
    if (lpTargetHandle) *lpTargetHandle = HV(nh);
    if (dwOptions & 1 /* DUPLICATE_CLOSE_SOURCE */) gta6rt_handle_close(h);
    return TRUE;
}

BOOL WINAPI GetHandleInformation(HANDLE hObject, LPDWORD lpdwFlags) {
    Slot* s = slot_of(H(hObject));
    bool ok = s && s->obj.load();
    if (ok && lpdwFlags) *lpdwFlags = s->flags.load();
    if (!ok) set_error(ERROR_INVALID_HANDLE);
    return ok;
}

BOOL WINAPI SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags) {
    pthread_mutex_lock(&g_h_mx);
    Slot* s = slot_of(H(hObject));
    bool ok = s && s->obj.load();
    if (ok) s->flags.store((s->flags.load() & ~dwMask) | (dwFlags & dwMask));
    pthread_mutex_unlock(&g_h_mx);
    if (!ok) set_error(ERROR_INVALID_HANDLE);
    return ok;
}

BOOL WINAPI CompareObjectHandles(HANDLE a, HANDLE b) {
    if (a == b) return TRUE;
    Object* x = acquire(H(a));
    Object* y = acquire(H(b));
    bool same = x && x == y;
    if (x) obj_unref(x);
    if (y) obj_unref(y);
    return same;
}

} // extern "C"
