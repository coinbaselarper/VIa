// Guest-side fixture for seh_dispatch_test. Compiled by the build for x86_64-pc-windows-msvc (COFF object, MSVC C++
// EH tables for __CxxFrameHandler3, SEH scope tables for __C_specific_handler) and linked into a guest image at
// run time by tests/coff_link.h. No headers: the few Windows/CRT declarations it needs are spelled out here.

extern "C" {
void host_note(int what, long long value);
__declspec(dllimport) void __stdcall RaiseException(unsigned long code, unsigned long flags, unsigned long n,
                                                    const unsigned long long* args);
unsigned long _exception_code(void);
}

namespace std {
// Same decorated name as the real std::bad_alloc (.?AVbad_alloc@std@@), so catch clauses in MSVC-built code match it.
class bad_alloc {
public:
    bad_alloc() {}
    virtual ~bad_alloc() {}
};
}  // namespace std

namespace {

struct tracker {
    int id;
    explicit tracker(int i) : id(i) { host_note(1, i); }
    ~tracker() { host_note(2, id); }
};

struct my_error {
    int code;
    explicit my_error(int c) : code(c) {}
    my_error(const my_error& o) : code(o.code) { host_note(6, code); }
    virtual ~my_error() { host_note(7, code); }
};

struct derived_error : my_error {
    explicit derived_error(int c) : my_error(c) {}
};

}  // namespace

__declspec(noinline) void fx_thrower(int kind) {
    tracker t(100 + kind);
    switch (kind) {
    case 0: throw 42;
    case 1: throw my_error(7);
    case 2: throw derived_error(9);
    case 3: throw std::bad_alloc();
    case 4: throw static_cast<long>(0x80001234);
    default: break;
    }
    host_note(3, kind);
}

__declspec(noinline) void fx_middle(int kind) {
    tracker t(200 + kind);
    fx_thrower(kind);
    host_note(3, 200 + kind);
}

__declspec(noinline) int fx_deref(volatile int* p) { return *p; }

__declspec(noinline) void fx_noexcept(int kind) noexcept { fx_middle(kind); }

namespace {
struct bad_dtor {
    ~bad_dtor() noexcept(false) {
        host_note(11, 0);
        throw 1;
    }
};
}  // namespace

__declspec(noinline) void fx_throw_with_bad_dtor() {
    bad_dtor b;
    fx_thrower(0);
}

extern "C" {

long long fx_catch_int() {
    try {
        fx_middle(0);
    } catch (int v) {
        return v;
    }
    return -1;
}

long long fx_catch_base_by_ref() {
    try {
        fx_middle(2);
    } catch (my_error& e) {
        return e.code;
    }
    return -1;
}

long long fx_catch_by_value() {
    try {
        fx_middle(1);
    } catch (my_error e) {
        return e.code + 10;
    }
    return -1;
}

long long fx_catch_all() {
    try {
        fx_middle(1);
    } catch (...) {
        return 1;
    }
    return -1;
}

long long fx_rethrow() {
    try {
        try {
            fx_middle(1);
        } catch (my_error& e) {
            host_note(4, e.code);
            throw;
        }
    } catch (my_error& e) {
        return e.code + 1000;
    }
    return -1;
}

long long fx_throw_in_catch() {
    try {
        fx_middle(0);
    } catch (int v) {
        try {
            fx_middle(1);
        } catch (my_error& e) {
            return v * 100 + e.code;
        }
    }
    return -1;
}

long long fx_outer_catches() {
    try {
        try {
            fx_middle(0);
        } catch (my_error&) {
            return -2;
        }
    } catch (int v) {
        return v + 1;
    }
    return -1;
}

long long fx_loop(int n) {
    long long sum = 0;
    for (int i = 0; i < n; ++i) {
        try {
            fx_thrower(i & 1);
        } catch (int v) {
            sum += v;
        } catch (my_error& e) {
            sum += e.code;
        }
    }
    return sum;
}

long long fx_uncaught(int kind) {
    fx_middle(kind);
    return -1;
}

long long fx_terminate() {
    try {
        fx_noexcept(0);
    } catch (...) {
        return -3;
    }
    return -1;
}

long long fx_dtor_throws() {
    try {
        fx_throw_with_bad_dtor();
    } catch (...) {
        return -4;
    }
    return -1;
}

typedef long (*translator_fn)();

long long fx_exe_translate(translator_fn t, int kind) {
    try {
        fx_thrower(kind);
    } catch (...) {
        return t();
    }
    return -1;
}

static int seh_filter(unsigned long code, unsigned long want) {
    host_note(8, static_cast<long long>(code));
    return code == want ? 1 : 0;
}

long long fx_seh_raise() {
    __try {
        RaiseException(0xE0000042u, 0, 0, nullptr);
    } __except (seh_filter(_exception_code(), 0xE0000042u)) {
        return 5;
    }
    return -1;
}

long long fx_seh_continue() {
    __try {
        RaiseException(0xE0000044u, 0, 0, nullptr);
        host_note(10, 1);
    } __except (seh_filter(_exception_code(), 0xE0000045u) ? 1 : -1) {
        return -2;
    }
    return 11;
}

long long fx_seh_finally() {
    __try {
        __try {
            RaiseException(0xE0000043u, 0, 0, nullptr);
        } __finally {
            host_note(5, 1);
        }
    } __except (1) {
        return 7;
    }
    return -1;
}

long long fx_seh_av(volatile int* p) {
    __try {
        return fx_deref(p);
    } __except (seh_filter(_exception_code(), 0xC0000005u)) {
        return 6;
    }
}

long long fx_av_through_cxx(volatile int* p) {
    tracker t(300);
    return fx_deref(p);
}

long long fx_seh_av_unwinds_cxx(volatile int* p) {
    __try {
        return fx_av_through_cxx(p);
    } __except (1) {
        return 8;
    }
}

}  // extern "C"
