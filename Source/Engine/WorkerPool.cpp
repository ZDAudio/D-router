#include "Engine/WorkerPool.h"

#include <pthread.h>
#include <sys/qos.h>

namespace dcr
{

    WorkerPool::WorkerPool (int n)
        : numWorkers (n)
    {
        threads.reserve ((size_t) numWorkers);
        for (int i = 0; i < numWorkers; ++i)
            threads.emplace_back ([this] { workerLoop(); });
    }

    WorkerPool::~WorkerPool()
    {
        stopFlag.store (true, std::memory_order_release);
        // Wake everyone -- workers re-check stopFlag right after wait() returns.
        jobGen.fetch_add (1, std::memory_order_acq_rel);
        jobGen.notify_all();

        for (auto& t : threads)
            if (t.joinable())
                t.join();
    }

    void WorkerPool::workerLoop()
    {
        // Audio-rate worker -- same QoS bump the matrix thread uses, otherwise
        // the workers would be background threads while the matrix thread runs
        // at USER_INTERACTIVE and the pool would be a net loss.
        pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);

        uint64_t lastSeenGen = 0;
        while (!stopFlag.load (std::memory_order_acquire))
        {
            // Wait until a new job is published.
            uint64_t cur = jobGen.load (std::memory_order_acquire);
            while (cur == lastSeenGen && !stopFlag.load (std::memory_order_acquire))
            {
                jobGen.wait (cur, std::memory_order_acquire);
                cur = jobGen.load (std::memory_order_acquire);
            }
            if (stopFlag.load (std::memory_order_acquire))
                break;
            lastSeenGen = cur;

            // Drain the index range.  Both workers and the caller race on
            // nextIdx, so distribution is automatically load-balanced -- a slow
            // plugin on one core doesn't leave other cores idle.
            const int total = totalCount.load (std::memory_order_acquire);
            while (true)
            {
                const int idx = nextIdx.fetch_add (1, std::memory_order_acq_rel);
                if (idx >= total)
                    break;
                jobFn (idx);
            }

            // Signal completion -- caller blocks until completedWorkers == N.
            if (completedWorkers.fetch_add (1, std::memory_order_acq_rel) + 1 == numWorkers)
                completedWorkers.notify_all();
        }
    }

    void WorkerPool::parallelFor (int count, std::function<void (int)> fn)
    {
        if (count <= 0)
            return;

        // No pool / trivial size: inline on the calling thread, skip the
        // synchronization cost entirely.  Threshold 2 because dispatching one
        // item to a worker is always slower than just running it ourselves.
        if (numWorkers <= 0 || count <= 2)
        {
            for (int i = 0; i < count; ++i)
                fn (i);
            return;
        }

        jobFn = std::move (fn);
        totalCount.store (count, std::memory_order_relaxed);
        nextIdx.store (0, std::memory_order_relaxed);
        completedWorkers.store (0, std::memory_order_relaxed);

        // Publish the job and wake every worker.
        jobGen.fetch_add (1, std::memory_order_acq_rel);
        jobGen.notify_all();

        // Caller participates in the work too -- N workers + 1 caller share the
        // load, so we don't waste this core.
        while (true)
        {
            const int idx = nextIdx.fetch_add (1, std::memory_order_acq_rel);
            if (idx >= count)
                break;
            jobFn (idx);
        }

        // Wait for every worker to finish.  All workers report done; once
        // completedWorkers == numWorkers we know nothing is still touching jobFn
        // and it's safe to return / overwrite for the next call.
        int seen = completedWorkers.load (std::memory_order_acquire);
        while (seen < numWorkers)
        {
            completedWorkers.wait (seen, std::memory_order_acquire);
            seen = completedWorkers.load (std::memory_order_acquire);
        }
    }

} // namespace dcr
