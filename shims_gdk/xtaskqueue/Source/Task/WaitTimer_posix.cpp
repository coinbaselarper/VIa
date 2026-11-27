#include "pch.h"
#include "WaitTimer.h"

#include "gdk_thread.h"

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
using TimerDuration = std::chrono::nanoseconds;

namespace
{
    Deadline DeadlineFromDueTime(uint64_t dueTime) noexcept
    {
        return Deadline(std::chrono::duration_cast<Clock::duration>(TimerDuration(dueTime)));
    }

    uint64_t DueTimeFromDeadline(Deadline deadline) noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<TimerDuration>(deadline.time_since_epoch()).count());
    }

    thread_local void* t_callbackTimer = nullptr;
}

namespace OS
{
    class WaitTimerImpl
    {
    public:
        void AddRef()
        {
            ++m_refs;
        }

        void Release()
        {
            if (--m_refs == 0)
            {
                delete this;
            }
        }

        HRESULT Initialize(_In_opt_ void* context, _In_ WaitTimerCallback* callback)
        {
            m_context = context;
            m_callback = callback;
            return S_OK;
        }

        // A callback that tears down its own timer does not wait for itself; the callback must not touch the owner
        // after Terminate returns.
        void Terminate()
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_terminated = true;
            m_armed = false;
            m_wake.notify_all();
            if (t_callbackTimer != this)
            {
                m_idle.wait(lock, [this] { return !m_running; });
            }
        }

        void Start(_In_ uint64_t dueTime)
        {
            bool spawn = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_terminated)
                {
                    return;
                }
                m_due = DeadlineFromDueTime(dueTime);
                m_armed = true;
                ++m_generation;
                if (!m_threadStarted)
                {
                    m_threadStarted = true;
                    spawn = true;
                    AddRef();
                }
            }
            if (spawn && !rt::gdk::spawn_detached([this] { Run(); }, "xtaskqueue timer"))
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_threadStarted = false;
                Release();
                return;
            }
            m_wake.notify_all();
        }

        void Cancel()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_armed = false;
            ++m_generation;
            m_wake.notify_all();
        }

    private:
        void Run() noexcept
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            while (!m_terminated)
            {
                if (!m_armed)
                {
                    m_wake.wait(lock);
                    continue;
                }
                const uint64_t generation = m_generation;
                const Deadline due = m_due;
                if (Clock::now() < due)
                {
                    m_wake.wait_until(lock, due, [&] { return m_terminated || m_generation != generation; });
                    continue;
                }
                m_armed = false;
                m_running = true;
                lock.unlock();
                t_callbackTimer = this;
                m_callback(m_context);
                t_callbackTimer = nullptr;
                lock.lock();
                m_running = false;
                m_idle.notify_all();
            }
            lock.unlock();
            Release();
        }

        std::atomic<uint32_t> m_refs{ 1 };
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::condition_variable m_idle;
        void* m_context = nullptr;
        WaitTimerCallback* m_callback = nullptr;
        Deadline m_due{};
        uint64_t m_generation = 0;
        bool m_armed = false;
        bool m_running = false;
        bool m_terminated = false;
        bool m_threadStarted = false;
    };

    WaitTimer::WaitTimer() noexcept
        : m_impl(nullptr)
    {}

    WaitTimer::~WaitTimer() noexcept
    {
        Terminate();
    }

    HRESULT WaitTimer::Initialize(_In_opt_ void* context, _In_ WaitTimerCallback* callback) noexcept
    {
        std::lock_guard<DefaultUnnamedMutex> lock{ m_mutex };
        if (m_impl.load() != nullptr || callback == nullptr)
        {
            ASSERT(FALSE);
            return E_UNEXPECTED;
        }

        std::unique_ptr<WaitTimerImpl> timer(new (std::nothrow) WaitTimerImpl);
        RETURN_IF_NULL_ALLOC(timer.get());
        RETURN_IF_FAILED(timer->Initialize(context, callback));

        m_impl = timer.release();

        return S_OK;
    }

    void WaitTimer::Terminate() noexcept
    {
        WaitTimerImpl* timer;
        {
            std::lock_guard<DefaultUnnamedMutex> lock{ m_mutex };
            timer = m_impl.exchange(nullptr);
        }
        if (timer != nullptr)
        {
            timer->Terminate();
            timer->Release();
        }
    }

    void WaitTimer::Start(_In_ uint64_t dueTime) noexcept
    {
        std::lock_guard<DefaultUnnamedMutex> lock{ m_mutex };
        if (WaitTimerImpl* timer = m_impl.load())
        {
            timer->Start(dueTime);
        }
    }

    void WaitTimer::Cancel() noexcept
    {
        std::lock_guard<DefaultUnnamedMutex> lock{ m_mutex };
        if (WaitTimerImpl* timer = m_impl.load())
        {
            timer->Cancel();
        }
    }

    uint64_t WaitTimer::GetCurrentTime() noexcept
    {
        return DueTimeFromDeadline(Clock::now());
    }

    uint64_t WaitTimer::GetDueTime(_In_ uint32_t msFromNow) noexcept
    {
        Deadline deadline = Clock::now() + std::chrono::milliseconds(msFromNow);
        return DueTimeFromDeadline(deadline);
    }

    void WaitTimerSetTestHooks(_In_opt_ WaitTimerTestHooks*) noexcept
    {
    }
}
