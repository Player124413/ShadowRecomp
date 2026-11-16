// Test of ShadowRecomp/gpu/background_work_queue.h on the build machine: the queue that
// creates pipelines on background threads while the game is already running. What matters
// is that every item is processed exactly once, and that the "caught up" notice (which
// writes the driver's pipeline cache to disk) comes once per batch and never too early.
#include <gpu/background_work_queue.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

static int g_failures = 0, g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

using namespace std::chrono_literals;

// What a queue did, as seen through its callbacks.
struct Record
{
    std::mutex mutex;
    std::vector<int> processed;                 // in the order processing finished
    std::vector<uint32_t> notices;              // count of every caughtUp call
    std::vector<double> noticeMilliseconds;
    std::set<std::thread::id> processingThreads;
    std::vector<uint32_t> startedThreads;
    std::atomic<int> inProcess{ 0 };
    std::atomic<int> mostInProcess{ 0 };
    std::atomic<int> inNotice{ 0 };
    std::atomic<bool> noticesOverlapped{ false };
    std::atomic<bool> processedBeforeThreadStarted{ false };
    std::atomic<int> processMilliseconds{ 0 };
    std::atomic<int> noticeDelayMilliseconds{ 0 };

    size_t ProcessedCount() { std::lock_guard lock(mutex); return processed.size(); }
    size_t NoticeCount() { std::lock_guard lock(mutex); return notices.size(); }
    std::vector<uint32_t> Notices() { std::lock_guard lock(mutex); return notices; }
    std::vector<int> Processed() { std::lock_guard lock(mutex); return processed; }
};

using Queue = BackgroundWorkQueue<int>;

// Queues are never destroyed (their threads outlive everything), so neither are the records.
static Queue* MakeQueue(uint32_t threads, Record*& record)
{
    record = new Record();
    Record* r = record;

    Queue::Callbacks callbacks;
    callbacks.threadStarted = [r](uint32_t index)
    {
        std::lock_guard lock(r->mutex);
        r->startedThreads.push_back(index);
    };
    callbacks.process = [r](int& item)
    {
        const int now = ++r->inProcess;
        int most = r->mostInProcess.load();
        while (now > most && !r->mostInProcess.compare_exchange_weak(most, now)) {}

        if (r->processMilliseconds > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(r->processMilliseconds.load()));

        {
            std::lock_guard lock(r->mutex);
            if (r->startedThreads.empty())
                r->processedBeforeThreadStarted = true;

            r->processed.push_back(item);
            r->processingThreads.insert(std::this_thread::get_id());
        }

        --r->inProcess;
    };
    callbacks.caughtUp = [r](uint32_t count, double milliseconds)
    {
        if (++r->inNotice > 1)
            r->noticesOverlapped = true;

        if (r->noticeDelayMilliseconds > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(r->noticeDelayMilliseconds.load()));

        {
            std::lock_guard lock(r->mutex);
            r->notices.push_back(count);
            r->noticeMilliseconds.push_back(milliseconds);
        }

        --r->inNotice;
    };

    return new Queue(threads, std::move(callbacks));
}

// Waits until the condition holds; false after five seconds.
template<typename Condition>
static bool Eventually(Condition condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;

        std::this_thread::sleep_for(1ms);
    }

    return true;
}

// Long enough for anything that is going to happen by mistake to have happened.
static void Settle()
{
    std::this_thread::sleep_for(150ms);
}

static uint32_t Sum(const std::vector<uint32_t>& values)
{
    uint32_t sum = 0;
    for (uint32_t value : values)
        sum += value;

    return sum;
}

static void TestSingleItem()
{
    Record* r;
    Queue* queue = MakeQueue(3, r);
    CHECK(queue->GetThreadCount() == 3);

    Settle();
    {
        std::lock_guard lock(r->mutex);
        CHECK(r->startedThreads.empty()); // no threads before there is work
    }
    CHECK(r->NoticeCount() == 0);

    queue->Add(7);
    CHECK(Eventually([&] { return r->NoticeCount() == 1; }));
    CHECK(r->Processed() == std::vector<int>{ 7 });
    CHECK(r->Notices() == std::vector<uint32_t>{ 1 });

    Settle();
    CHECK(r->NoticeCount() == 1); // one notice, not one per idle thread
    {
        std::lock_guard lock(r->mutex);
        std::vector<uint32_t> started = r->startedThreads;
        std::sort(started.begin(), started.end());
        CHECK((started == std::vector<uint32_t>{ 0, 1, 2 })); // each thread announced itself once
        CHECK(r->noticeMilliseconds[0] >= 0.0 && r->noticeMilliseconds[0] < 5000.0);
    }
    CHECK(!r->processedBeforeThreadStarted);

    // The threads stay: a later item is processed and announced again.
    queue->Add(8);
    CHECK(Eventually([&] { return r->NoticeCount() == 2; }));
    CHECK((r->Processed() == std::vector<int>{ 7, 8 }));
    CHECK((r->Notices() == std::vector<uint32_t>{ 1, 1 }));
    {
        std::lock_guard lock(r->mutex);
        CHECK(r->startedThreads.size() == 3);
    }
}

static void TestZeroThreadsMeansOne()
{
    Record* r;
    Queue* queue = MakeQueue(0, r);
    CHECK(queue->GetThreadCount() == 1);
    queue->Add(1);
    CHECK(Eventually([&] { return r->NoticeCount() == 1; }));
}

static void TestOrderOnOneThread()
{
    Record* r;
    Queue* queue = MakeQueue(1, r);
    {
        Queue::Hold hold(*queue);
        for (int i = 0; i < 500; i++)
            queue->Add(i);
    }

    CHECK(Eventually([&] { return r->NoticeCount() == 1; }));
    std::vector<int> expected(500);
    for (int i = 0; i < 500; i++)
        expected[i] = i;

    CHECK(r->Processed() == expected);          // first in, first out
    CHECK(r->Notices() == std::vector<uint32_t>{ 500 });
    CHECK(r->mostInProcess == 1);
    {
        std::lock_guard lock(r->mutex);
        CHECK(r->processingThreads.size() == 1);
    }
}

static void TestHoldDelaysEverything()
{
    Record* r;
    Queue* queue = MakeQueue(4, r);
    {
        Queue::Hold hold(*queue);
        for (int i = 0; i < 100; i++)
            queue->Add(i);

        Settle();
        CHECK(r->ProcessedCount() == 0);        // nothing starts under a Hold
        CHECK(r->NoticeCount() == 0);
    }

    CHECK(Eventually([&] { return r->NoticeCount() == 1; }));
    CHECK(r->ProcessedCount() == 100);
    CHECK(r->Notices() == std::vector<uint32_t>{ 100 }); // one notice for the whole batch

    std::vector<int> processed = r->Processed();
    std::sort(processed.begin(), processed.end());
    for (size_t i = 0; i < processed.size(); i++)
        CHECK(processed[i] == int(i));          // each item exactly once

    Settle();
    CHECK(r->NoticeCount() == 1);
}

static void TestWorkSpreadsOverThreads()
{
    Record* r;
    Queue* queue = MakeQueue(4, r);
    r->processMilliseconds = 20;
    {
        Queue::Hold hold(*queue);
        for (int i = 0; i < 40; i++)
            queue->Add(i);
    }

    CHECK(Eventually([&] { return r->NoticeCount() == 1; }));
    CHECK(r->mostInProcess == 4);               // all threads take part, and no more than there are
    {
        std::lock_guard lock(r->mutex);
        CHECK(r->processingThreads.size() == 4);
        // 40 items of 20 ms on 4 threads: 200 ms, not the 800 ms one thread would need.
        CHECK(r->noticeMilliseconds[0] >= 190.0 && r->noticeMilliseconds[0] < 790.0);
    }
}

static void TestTimeCountsFromTheFirstItem()
{
    Record* r;
    Queue* queue = MakeQueue(2, r);
    {
        Queue::Hold hold(*queue);
        queue->Add(1);
        std::this_thread::sleep_for(250ms);
        queue->Add(2);
    }

    CHECK(Eventually([&] { return r->NoticeCount() == 1; }));
    {
        std::lock_guard lock(r->mutex);
        CHECK(r->noticeMilliseconds[0] >= 250.0);   // not from the last item added
    }

    // The next batch is timed by itself.
    queue->Add(3);
    CHECK(Eventually([&] { return r->NoticeCount() == 2; }));
    {
        std::lock_guard lock(r->mutex);
        CHECK(r->noticeMilliseconds[1] < 240.0);
    }
}

static void TestNoticeWaitsForTheHold()
{
    Record* r;
    Queue* queue = MakeQueue(2, r);

    // A second producer takes a Hold while the first batch is still being processed...
    r->processMilliseconds = 30;
    {
        Queue::Hold first(*queue);
        for (int i = 0; i < 40; i++)
            queue->Add(i);
    }

    CHECK(Eventually([&] { return r->inProcess.load() > 0; }));
    {
        Queue::Hold second(*queue);
        CHECK(Eventually([&] { return r->inProcess.load() == 0; }));

        // ...so what was in progress finishes, nothing new starts, and no notice is given.
        const size_t finished = r->ProcessedCount();
        CHECK(finished >= 1 && finished < 40);
        Settle();
        CHECK(r->ProcessedCount() == finished);
        CHECK(r->inProcess.load() == 0);
        CHECK(r->NoticeCount() == 0);

        queue->Add(100);
        queue->Add(101);
    }

    CHECK(Eventually([&] { return r->NoticeCount() == 1; }));
    CHECK(r->Notices() == std::vector<uint32_t>{ 42 }); // both batches in one notice
    CHECK(r->ProcessedCount() == 42);
}

static void TestHoldThatAddsNothing()
{
    Record* r;
    Queue* queue = MakeQueue(2, r);
    r->processMilliseconds = 400;
    queue->Add(1);
    CHECK(Eventually([&] { return r->inProcess.load() == 1; }));
    {
        // The only item finishes under a Hold whose owner then has nothing to add.
        Queue::Hold hold(*queue);
        CHECK(Eventually([&] { return r->ProcessedCount() == 1; }));
        Settle();
        CHECK(r->NoticeCount() == 0);
    }

    // The notice for it must still come, although no thread is processing anything.
    CHECK(Eventually([&] { return r->NoticeCount() == 1; }));
    CHECK(r->Notices() == std::vector<uint32_t>{ 1 });

    // A Hold over an idle queue is not a batch.
    { Queue::Hold hold(*queue); }
    { Queue::Hold outer(*queue); Queue::Hold inner(*queue); }
    Settle();
    CHECK(r->NoticeCount() == 1);
}

static void TestNestedHolds()
{
    Record* r;
    Queue* queue = MakeQueue(3, r);
    {
        Queue::Hold outer(*queue);
        queue->Add(1);
        {
            Queue::Hold inner(*queue);
            queue->Add(2);
        }

        Settle();
        CHECK(r->ProcessedCount() == 0);        // the outer Hold still counts
        queue->Add(3);
    }

    CHECK(Eventually([&] { return r->NoticeCount() == 1; }));
    CHECK(r->Notices() == std::vector<uint32_t>{ 3 });
}

static void TestItemsAddedWithoutHold()
{
    // Without a Hold, items added one at a time may be announced in several notices; what
    // must hold is that each is processed once and that the notices add up.
    Record* r;
    Queue* queue = MakeQueue(4, r);
    for (int i = 0; i < 300; i++)
        queue->Add(i);

    CHECK(Eventually([&] { return r->ProcessedCount() == 300; }));
    CHECK(Eventually([&] { return Sum(r->Notices()) == 300; }));
    Settle();
    CHECK(Sum(r->Notices()) == 300);

    std::vector<int> processed = r->Processed();
    std::sort(processed.begin(), processed.end());
    CHECK(std::adjacent_find(processed.begin(), processed.end()) == processed.end());
}

static void TestAddedWhileNoticeRuns()
{
    Record* r;
    Queue* queue = MakeQueue(3, r);
    r->noticeDelayMilliseconds = 1000; // the notice of the real queue writes a file
    queue->Add(1);
    CHECK(Eventually([&] { return r->inNotice.load() == 1; }));

    // Work that arrives while one thread is busy with the notice is not kept waiting for it...
    queue->Add(2);
    CHECK(Eventually([&] { return r->ProcessedCount() == 2; }));
    CHECK(r->inNotice.load() == 1 && r->NoticeCount() == 0); // processed while the first notice still runs

    // ...and gets a notice of its own, after the first one has finished.
    CHECK(Eventually([&] { return r->NoticeCount() == 2; }));
    CHECK((r->Notices() == std::vector<uint32_t>{ 1, 1 }));
    CHECK(!r->noticesOverlapped);
}

static void TestManyProducers()
{
    Record* r;
    Queue* queue = MakeQueue(4, r);

    std::vector<std::thread> producers;
    for (int p = 0; p < 6; p++)
    {
        producers.emplace_back([queue, p]()
        {
            for (int batch = 0; batch < 20; batch++)
            {
                Queue::Hold hold(*queue);
                for (int i = 0; i < 25; i++)
                    queue->Add(p * 100000 + batch * 100 + i);
            }
        });
    }

    for (auto& producer : producers)
        producer.join();

    CHECK(Eventually([&] { return r->ProcessedCount() == 3000 && Sum(r->Notices()) == 3000; }));
    Settle();
    CHECK(r->ProcessedCount() == 3000);
    CHECK(Sum(r->Notices()) == 3000);           // every item is accounted for by exactly one notice
    CHECK(!r->noticesOverlapped);
    CHECK(!r->processedBeforeThreadStarted);

    std::vector<int> processed = r->Processed();
    std::sort(processed.begin(), processed.end());
    CHECK(std::adjacent_find(processed.begin(), processed.end()) == processed.end());
    CHECK(r->mostInProcess <= 4);
}

static void TestItemsAddedByItems()
{
    // An item that adds further items: the notice comes when those are done as well.
    auto* total = new std::atomic<int>(0);
    auto* notices = new std::vector<uint32_t>();
    auto* mutex = new std::mutex();
    Queue** self = new Queue*(nullptr);

    Queue::Callbacks callbacks;
    callbacks.process = [total, self](int& item)
    {
        ++*total;
        if (item > 0)
        {
            Queue::Hold hold(**self);
            (*self)->Add(item - 1);
            (*self)->Add(0);
        }
    };
    callbacks.caughtUp = [notices, mutex](uint32_t count, double)
    {
        std::lock_guard lock(*mutex);
        notices->push_back(count);
    };

    *self = new Queue(3, std::move(callbacks));
    (*self)->Add(50); // 50 -> 49 -> ... -> 0, each of the 50 steps adding a 0 as well: 101 items

    CHECK(Eventually([&] { return total->load() == 101; }));
    CHECK(Eventually([&] { std::lock_guard lock(*mutex); return Sum(*notices) == 101; }));
}

static void TestCallbacksAreOptional()
{
    auto* processed = new std::atomic<int>(0);
    Queue::Callbacks callbacks;
    callbacks.process = [processed](int&) { ++*processed; };

    Queue* queue = new Queue(2, std::move(callbacks));
    for (int i = 0; i < 10; i++)
        queue->Add(i);

    CHECK(Eventually([&] { return processed->load() == 10; }));

    // And again after the point where a notice would have been given.
    Settle();
    queue->Add(11);
    CHECK(Eventually([&] { return processed->load() == 11; }));
}

static void TestMoveOnlyItems()
{
    auto* sum = new std::atomic<int>(0);
    BackgroundWorkQueue<std::unique_ptr<int>>::Callbacks callbacks;
    callbacks.process = [sum](std::unique_ptr<int>& item) { *sum += *item; };

    auto* queue = new BackgroundWorkQueue<std::unique_ptr<int>>(2, std::move(callbacks));
    for (int i = 1; i <= 10; i++)
        queue->Add(std::make_unique<int>(i));

    CHECK(Eventually([&] { return sum->load() == 55; }));
}

int main()
{
    TestSingleItem();
    TestZeroThreadsMeansOne();
    TestOrderOnOneThread();
    TestHoldDelaysEverything();
    TestWorkSpreadsOverThreads();
    TestTimeCountsFromTheFirstItem();
    TestNoticeWaitsForTheHold();
    TestHoldThatAddsNothing();
    TestNestedHolds();
    TestItemsAddedWithoutHold();
    TestAddedWhileNoticeRuns();
    TestManyProducers();
    TestItemsAddedByItems();
    TestCallbacksAreOptional();
    TestMoveOnlyItems();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    fflush(stdout);

    // The queues' threads are still there, asleep: leave the way the game does.
    _Exit(g_failures ? 1 : 0);
}
