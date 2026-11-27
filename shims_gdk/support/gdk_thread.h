#pragma once

#include <gta6rt/abi.h>
#include <gta6rt/os.h>

#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace rt::gdk {

// A thread that may run guest code: created through gta6rt_os with the guest hooks (TEB, TLS block). When the os module
// reports GTA6RT_E_NOT_IMPLEMENTED (a build without it) the thread is a plain std::thread, which is enough for tests
// that never call into a mapped image.
class Thread {
public:
    Thread() = default;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&& o) noexcept { *this = std::move(o); }
    Thread& operator=(Thread&& o) noexcept {
        if (joinable()) std::terminate();
        os_ = std::exchange(o.os_, nullptr);
        std_ = std::move(o.std_);
        return *this;
    }
    ~Thread() {
        if (joinable()) std::terminate();
    }

    template <class F>
    static bool start(Thread& out, F&& fn, const char* name = nullptr) {
        auto* boxed = new std::function<void()>(std::forward<F>(fn));
        gta6rt_thread_desc d{};
        d.fn = [](void* p) {
            std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(p));
            (*f)();
        };
        d.arg = boxed;
        d.name = name;
        d.guest = 1;
        gta6rt_thread* t = nullptr;
        const gta6rt_status s = gta6rt_thread_create(&d, &t);
        if (s == GTA6RT_OK) {
            out.os_ = t;
            return true;
        }
        if (s != GTA6RT_E_NOT_IMPLEMENTED) {
            delete boxed;
            return false;
        }
        try {
            out.std_ = std::thread([boxed] {
                std::unique_ptr<std::function<void()>> f(boxed);
                (*f)();
            });
        } catch (...) {
            delete boxed;
            return false;
        }
        return true;
    }

    bool joinable() const { return os_ != nullptr || std_.joinable(); }

    void join() {
        if (os_) {
            gta6rt_thread_join(os_, GTA6RT_WAIT_INFINITE);
            gta6rt_thread_release(os_);
            os_ = nullptr;
        } else if (std_.joinable()) {
            std_.join();
        }
    }

    void detach() {
        if (os_) {
            gta6rt_thread_release(os_);
            os_ = nullptr;
        } else if (std_.joinable()) {
            std_.detach();
        }
    }

private:
    gta6rt_thread* os_ = nullptr;
    std::thread std_;
};

template <class F>
bool spawn_detached(F&& fn, const char* name = nullptr) {
    Thread t;
    if (!Thread::start(t, std::forward<F>(fn), name)) return false;
    t.detach();
    return true;
}

}  // namespace rt::gdk
