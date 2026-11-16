#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

// Work that is handed to a few background threads, with a notice for when they have
// caught up.
//
// Items are started in the order they were added, each by one of the worker threads. The
// threads are created along with the first item and then stay for as long as the process
// does, asleep while there is nothing to do. Nothing ever waits for them, so the process
// can end, or be killed the way Android ends processes, with work still queued. For that
// reason a queue is never destroyed either: create it with `new` and keep it.
//
// "Caught up" means that every item added so far has been processed. A producer that adds
// several items in a row keeps a Hold while it does: no item is started and no notice is
// given while a Hold exists, so the notice cannot come in the middle of what is one batch
// to the producer.
template<typename T>
class BackgroundWorkQueue
{
public:
    struct Callbacks
    {
        // Processes one item. Called on the worker threads, by several of them at a time.
        std::function<void(T&)> process;

        // Optional. Called on a worker thread when the last item there was has been
        // processed: with the number of items processed since the previous call, and the
        // time that took, counted from when the first of them was added. Calls never overlap.
        std::function<void(uint32_t count, double milliseconds)> caughtUp;

        // Optional. Called once on every worker thread before it takes its first item, with
        // the index of the thread: the place to name the thread and set its priority.
        std::function<void(uint32_t threadIndex)> threadStarted;
    };

    // Keeps a queue from starting items, and from giving notice, for as long as it exists.
    class Hold
    {
    public:
        explicit Hold(BackgroundWorkQueue& queue) : m_queue(queue)
        {
            m_queue.BeginHold();
        }

        ~Hold()
        {
            m_queue.EndHold();
        }

        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;

    private:
        BackgroundWorkQueue& m_queue;
    };

    BackgroundWorkQueue(uint32_t threadCount, Callbacks callbacks)
        : m_callbacks(std::move(callbacks)), m_threadCount(threadCount > 0 ? threadCount : 1)
    {
    }

    BackgroundWorkQueue(const BackgroundWorkQueue&) = delete;
    BackgroundWorkQueue& operator=(const BackgroundWorkQueue&) = delete;

    void Add(T item)
    {
        {
            std::lock_guard lock(m_mutex);

            // The first item since the workers last caught up starts the clock of the next notice.
            if (m_queue.empty() && m_activeCount == 0 && m_processedCount == 0)
                m_batchStart = std::chrono::steady_clock::now();

            m_queue.push_back(std::move(item));

            if (m_startedThreadCount == 0)
                StartThreads();
        }

        m_wake.notify_one();
    }

    // What a Hold does, for an owner that cannot keep one as an object. Every BeginHold()
    // must be followed by its EndHold().
    void BeginHold()
    {
        std::lock_guard lock(m_mutex);
        m_holdCount++;
    }

    void EndHold()
    {
        {
            std::lock_guard lock(m_mutex);
            m_holdCount--;
        }

        m_wake.notify_all();
    }

    uint32_t GetThreadCount() const
    {
        return m_threadCount;
    }

private:
    // Called with m_mutex held.
    void StartThreads()
    {
        for (uint32_t i = 0; i < m_threadCount; i++)
        {
            try
            {
                std::thread([this, i]() { WorkerThread(i); }).detach();
                m_startedThreadCount++;
            }
            catch (const std::system_error&)
            {
                // The system has no thread to spare. Whatever was started does the work; if
                // nothing was, the next item tries again and the queued ones wait until then.
                break;
            }
        }
    }

    void WorkerThread(uint32_t threadIndex)
    {
        if (m_callbacks.threadStarted)
            m_callbacks.threadStarted(threadIndex);

        std::unique_lock lock(m_mutex);

        while (true)
        {
            // Wakes up for an item to process, or to give the notice that there is none left.
            m_wake.wait(lock, [this]()
            {
                return m_holdCount == 0 && (!m_queue.empty() || (m_activeCount == 0 && m_processedCount != 0));
            });

            if (m_queue.empty())
            {
                // Taken while the lock is held: of the threads woken for it, one gives the notice.
                const uint32_t count = std::exchange(m_processedCount, 0u);
                const double milliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - m_batchStart).count();

                if (m_callbacks.caughtUp)
                {
                    lock.unlock();
                    {
                        std::lock_guard caughtUpLock(m_caughtUpMutex);
                        m_callbacks.caughtUp(count, milliseconds);
                    }
                    lock.lock();
                }

                continue;
            }

            T item = std::move(m_queue.front());
            m_queue.pop_front();
            m_activeCount++;

            lock.unlock();
            m_callbacks.process(item);
            lock.lock();

            m_activeCount--;
            m_processedCount++;

            // If that was the last item, the wait above returns at once and this thread
            // gives the notice; under a Hold, the end of the Hold wakes a thread for it.
        }
    }

    const Callbacks m_callbacks;
    const uint32_t m_threadCount;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<T> m_queue;              // Guarded by m_mutex, as are the members below.
    uint32_t m_holdCount = 0;
    uint32_t m_activeCount = 0;         // Items being processed right now.
    uint32_t m_processedCount = 0;      // Items processed since the last notice.
    uint32_t m_startedThreadCount = 0;
    std::chrono::steady_clock::time_point m_batchStart{};

    std::mutex m_caughtUpMutex;
};
