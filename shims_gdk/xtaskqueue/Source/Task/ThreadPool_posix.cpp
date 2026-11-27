#include "pch.h"
#include "ThreadPool.h"

#include "gdk_thread.h"

#include <algorithm>
#include <deque>

namespace OS
{
    namespace
    {
        constexpr auto kIdleExit = std::chrono::seconds(2);
    }

    class ThreadPoolImpl
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

        HRESULT Initialize(_In_opt_ void* context, _In_ ThreadPoolCallback* callback) noexcept
        {
            m_context = context;
            m_callback = callback;
            m_limit = (std::max)(4u, std::thread::hardware_concurrency());
            return S_OK;
        }

        // Pending and running callbacks that have not called Complete drain before this returns, as
        // WaitForThreadpoolWorkCallbacks(work, FALSE) does. A callback that already called Complete (a task queue
        // closing itself from inside a callback) is not waited for.
        void Terminate() noexcept
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_terminated = true;
            m_drained.wait(lock, [this] { return m_outstanding == 0; });
            m_wake.notify_all();
        }

        void Submit() noexcept
        {
            bool spawn = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_terminated)
                {
                    return;
                }
                ++m_outstanding;
                ++m_pending;
                if (m_idle < m_pending && m_threads < m_limit + m_long)
                {
                    ++m_threads;
                    spawn = true;
                }
            }
            if (spawn)
            {
                AddRef();
                if (!rt::gdk::spawn_detached([this] { Worker(); }, "xtaskqueue"))
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    --m_threads;
                    Release();
                    if (m_threads == 0 && m_pending != 0)
                    {
                        --m_pending;
                        --m_outstanding;
                        m_drained.notify_all();
                    }
                    return;
                }
            }
            m_wake.notify_one();
        }

    private:
        struct ActionStatusImpl : ThreadPoolActionStatus
        {
            explicit ActionStatusImpl(ThreadPoolImpl* owner) : m_owner(owner) {}

            bool IsComplete = false;

            void Complete() override
            {
                if (IsComplete)
                {
                    return;
                }
                IsComplete = true;
                std::lock_guard<std::mutex> lock(m_owner->m_mutex);
                if (--m_owner->m_outstanding == 0)
                {
                    m_owner->m_drained.notify_all();
                }
            }

            void MayRunLong() override
            {
                if (!m_longRunning)
                {
                    m_longRunning = true;
                    std::lock_guard<std::mutex> lock(m_owner->m_mutex);
                    ++m_owner->m_long;
                }
            }

            bool m_longRunning = false;

        private:
            ThreadPoolImpl* m_owner;
        };

        void Worker() noexcept
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            for (;;)
            {
                ++m_idle;
                const bool woke = m_wake.wait_for(lock, kIdleExit, [this] { return m_pending != 0 || m_terminated; });
                --m_idle;
                if (!woke || m_pending == 0)
                {
                    break;
                }
                --m_pending;
                lock.unlock();

                AddRef();
                ActionStatusImpl status(this);
                m_callback(m_context, status);
                if (!status.IsComplete)
                {
                    status.Complete();
                }
                const bool wasLong = status.m_longRunning;
                lock.lock();
                if (wasLong)
                {
                    --m_long;
                }
                lock.unlock();
                Release();
                lock.lock();
            }
            --m_threads;
            lock.unlock();
            Release();
        }

        std::atomic<uint32_t> m_refs{ 1 };
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::condition_variable m_drained;
        void* m_context = nullptr;
        ThreadPoolCallback* m_callback = nullptr;
        uint32_t m_outstanding = 0;
        uint32_t m_pending = 0;
        uint32_t m_idle = 0;
        uint32_t m_threads = 0;
        uint32_t m_long = 0;
        uint32_t m_limit = 4;
        bool m_terminated = false;
    };

    ThreadPool::ThreadPool() noexcept :
        m_impl(nullptr)
    {
    }

    ThreadPool::~ThreadPool() noexcept
    {
        Terminate();
    }

    HRESULT ThreadPool::Initialize(_In_opt_ void* context, _In_ ThreadPoolCallback* callback) noexcept
    {
        RETURN_HR_IF(E_UNEXPECTED, m_impl != nullptr);

        std::unique_ptr<ThreadPoolImpl> impl(new (std::nothrow) ThreadPoolImpl);
        RETURN_IF_NULL_ALLOC(impl);
        RETURN_IF_FAILED(impl->Initialize(context, callback));

        m_impl = impl.release();
        return S_OK;
    }

    void ThreadPool::Terminate() noexcept
    {
        if (m_impl != nullptr)
        {
            m_impl->Terminate();
            m_impl->Release();
            m_impl = nullptr;
        }
    }

    void ThreadPool::Submit()
    {
        m_impl->Submit();
    }
}
