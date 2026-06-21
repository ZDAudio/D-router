#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace dcr
{

    // Persistent fork/join worker pool used by MatrixProcessor to parallelize the
    // per-channel plugin chains.  One published job at a time; workers grab work
    // items via atomic fetch_add and the calling thread participates too so the
    // caller's core isn't idle.
    //
    // Design notes:
    //   - N persistent worker threads (chosen at construction).  They sleep on
    //     std::atomic::wait when no job is published -- no busy-spin at idle.
    //   - Each parallelFor() bumps jobGen, atomically publishes (fn, totalCount),
    //     and notifies all workers.  Workers + the caller race to drain the
    //     [0, totalCount) index range.
    //   - The caller blocks on completedWorkers reaching numWorkers, again via
    //     atomic::wait -- no spin.
    //   - Lambda is captured into a std::function once per parallelFor().  Small
    //     captures (e.g. just `this`) fit the std::function SBO so no heap
    //     allocation in the hot path.
    //
    // Constraints:
    //   - SINGLE producer.  Only the matrix thread is allowed to call
    //     parallelFor().  Two concurrent submitters would corrupt the published
    //     job state.
    //   - Worker functions MUST be thread-safe vs. each other for their assigned
    //     indices.  In MatrixProcessor's case each per-channel plugin chain has
    //     its own buffer slice + its own slot lock -- no shared mutable state.
    class WorkerPool
    {
    public:
        explicit WorkerPool (int numWorkers);
        ~WorkerPool();

        int workerCount() const noexcept { return numWorkers; }

        // Runs fn(i) for every i in [0, count).  Distributes work across worker
        // threads + the calling thread.  Blocks until every index has been
        // processed.  Re-entrant only from the same calling thread.
        void parallelFor (int count, std::function<void (int)> fn);

    private:
        void workerLoop();

        int numWorkers = 0;
        std::vector<std::thread> threads;
        std::atomic<bool> stopFlag { false };

        // Job state -- published by parallelFor(), consumed by workers.
        std::function<void (int)> jobFn;
        std::atomic<int> totalCount { 0 };
        std::atomic<int> nextIdx { 0 };
        std::atomic<int> completedWorkers { 0 };
        std::atomic<uint64_t> jobGen { 0 }; // bump = "new job"
    };

} // namespace dcr
