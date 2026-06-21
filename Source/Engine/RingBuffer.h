#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace dcr
{

    // Single-producer single-consumer lock-free float ring buffer.
    // Size is rounded up to the next power of two; capacity is size-1 usable samples.
    class FloatRingBuffer
    {
    public:
        explicit FloatRingBuffer (size_t requestedSize) { resize (requestedSize); }
        FloatRingBuffer() = default;

        // Allow move so std::vector<FloatRingBuffer> works. Only safe to move
        // when no other thread is accessing this instance (true at construction).
        FloatRingBuffer (FloatRingBuffer&& other) noexcept
            : buffer (std::move (other.buffer)),
              mask (other.mask),
              writePos (other.writePos.load (std::memory_order_relaxed)),
              readPos (other.readPos.load (std::memory_order_relaxed))
        {
        }
        FloatRingBuffer& operator= (FloatRingBuffer&& other) noexcept
        {
            buffer = std::move (other.buffer);
            mask = other.mask;
            writePos.store (other.writePos.load (std::memory_order_relaxed), std::memory_order_relaxed);
            readPos.store (other.readPos.load (std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }
        FloatRingBuffer (const FloatRingBuffer&) = delete;
        FloatRingBuffer& operator= (const FloatRingBuffer&) = delete;

        void resize (size_t requestedSize)
        {
            size_t s = 1;
            while (s < requestedSize + 1)
                s <<= 1;
            buffer.assign (s, 0.0f);
            mask = s - 1;
            writePos.store (0, std::memory_order_relaxed);
            readPos.store (0, std::memory_order_relaxed);
        }

        size_t capacity() const noexcept { return mask; } // usable
        size_t bufferSize() const noexcept { return mask + 1; }

        size_t writeAvailable() const noexcept
        {
            const auto w = writePos.load (std::memory_order_relaxed);
            const auto r = readPos.load (std::memory_order_acquire);
            return (r + mask - w) & mask;
        }

        size_t readAvailable() const noexcept
        {
            const auto w = writePos.load (std::memory_order_acquire);
            const auto r = readPos.load (std::memory_order_relaxed);
            return (w - r) & mask;
        }

        // Write up to numSamples; returns how many were actually written.
        size_t write (const float* src, size_t numSamples) noexcept
        {
            const auto w = writePos.load (std::memory_order_relaxed);
            const auto r = readPos.load (std::memory_order_acquire);
            const auto free = (r + mask - w) & mask;
            const auto toWrite = numSamples < free ? numSamples : free;
            if (toWrite == 0)
                return 0;

            const auto bufSize = mask + 1;
            const auto first = std::min (toWrite, bufSize - w);
            std::memcpy (buffer.data() + w, src, first * sizeof (float));
            if (toWrite > first)
                std::memcpy (buffer.data(), src + first, (toWrite - first) * sizeof (float));

            writePos.store ((w + toWrite) & mask, std::memory_order_release);
            return toWrite;
        }

        // Read up to numSamples; returns how many were actually read.
        size_t read (float* dst, size_t numSamples) noexcept
        {
            const auto r = readPos.load (std::memory_order_relaxed);
            const auto w = writePos.load (std::memory_order_acquire);
            const auto avail = (w - r) & mask;
            const auto toRead = numSamples < avail ? numSamples : avail;
            if (toRead == 0)
                return 0;

            const auto bufSize = mask + 1;
            const auto first = std::min (toRead, bufSize - r);
            std::memcpy (dst, buffer.data() + r, first * sizeof (float));
            if (toRead > first)
                std::memcpy (dst + first, buffer.data(), (toRead - first) * sizeof (float));

            readPos.store ((r + toRead) & mask, std::memory_order_release);
            return toRead;
        }

        void clear() noexcept
        {
            std::fill (buffer.begin(), buffer.end(), 0.0f);
            writePos.store (0, std::memory_order_relaxed);
            readPos.store (0, std::memory_order_relaxed);
        }

    private:
        std::vector<float> buffer;
        size_t mask = 0;
        std::atomic<size_t> writePos { 0 };
        std::atomic<size_t> readPos { 0 };
    };

} // namespace dcr
