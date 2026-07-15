#pragma once
#include <atomic>
#include <vector>

#include "macros.h"

namespace Common
{
    /// Single-producer / single-consumer lock-free FIFO queue.
    ///
    /// Contract:
    ///   - Exactly one thread produces (getNextToWriteTo + updateWriteIndex).
    ///   - Exactly one thread consumes (getNextToRead + updateReadIndex).
    ///   - One slot is sacrificed to distinguish "full" from "empty", so a
    ///     queue constructed with N slots holds at most N-1 elements.
    ///   - Writing into a full queue is FATAL. Queues are sized so this never
    ///     happens in normal operation; silently overwriting unread slots
    ///     (the previous behaviour) corrupts data and is strictly worse.
    ///
    /// Performance notes:
    ///   - Producer-owned and consumer-owned state live on separate cache
    ///     lines (alignas(64)) so the two threads never false-share.
    ///   - Each side keeps a cached copy of the other side's index and only
    ///     re-reads the shared atomic when the cached value suggests
    ///     full/empty — the common-case push/pop touches no foreign line.
    ///   - Index wrap uses a compare instead of modulo: '% capacity' is an
    ///     integer division (~20-40 cycles) on the hot path.
    ///   - release/acquire pairs replace the default seq_cst: the release
    ///     store on next_write_index_ publishes the slot contents written
    ///     via getNextToWriteTo(); the consumer's acquire load synchronises
    ///     with it (and symmetrically for the read index).
    template<typename T>
    class LFQueue final
    {
    public:
        explicit LFQueue(size_t num_elems) :
            store(num_elems, T())
        {
            ASSERT(num_elems >= 2, "LFQueue requires at least 2 slots (one is reserved).");
        }

        auto getNextToWriteTo() noexcept
        {
            return &store[next_write_index.load(std::memory_order_relaxed)];
        }

        auto updateWriteIndex() noexcept
        {
            const auto write = next_write_index.load(std::memory_order_relaxed);
            auto next = write + 1;
            if (UNLIKELY(next == store.size()))
                next = 0; // wrap around.
            if (UNLIKELY(next == cached_read_index)) {
                cached_read_index = next_read_index.load(std::memory_order_acquire);
                if (UNLIKELY(next == cached_read_index)) {
                    FATAL("LFQueue overflow — producer caught up with consumer; "
                          "size the queue larger or drain faster. capacity=" +
                          std::to_string(store.size()));
                }
            }
            next_write_index.store(next, std::memory_order_release);
        }

        auto getNextToRead() const noexcept -> const T*
        {
            const auto read = next_read_index.load(std::memory_order_relaxed);
            if (read == cached_write_index) {
                cached_write_index = next_write_index.load(std::memory_order_acquire);
                if (read == cached_write_index)
                    return nullptr; // queue is empty.
            }
            return &store[read];
        }

        auto updateReadIndex() noexcept
        {
            const auto read = next_read_index.load(std::memory_order_relaxed);
            if (UNLIKELY(read == cached_write_index)) {
                cached_write_index = next_write_index.load(std::memory_order_acquire);
                ASSERT(read != cached_write_index,
                       "LFQueue underflow — updateReadIndex() on an empty queue.");
            }
            auto next = read + 1;
            if (UNLIKELY(next == store.size()))
                next = 0; // wrap around.
            next_read_index.store(next, std::memory_order_release);
        }

        auto size() const noexcept -> size_t
        {
            const auto write = next_write_index.load(std::memory_order_acquire);
            const auto read  = next_read_index.load(std::memory_order_acquire);
            return (write >= read) ? (write - read) : (store.size() - read + write);
        }


        LFQueue() = delete; // prohibit default constructor.
        LFQueue(const LFQueue &) = delete; // prohibit copy constructor.
        LFQueue &operator=(const LFQueue &) = delete; // prohibit copy assignment.
        LFQueue(LFQueue &&) = delete; // prohibit move constructor.
        LFQueue &operator=(LFQueue &&) = delete; // prohibit move assignment.

    private:
        std::vector<T> store;

        // Producer-owned cache line: its index + cached view of the reader.
        alignas(64) std::atomic<size_t> next_write_index = {0};
        size_t cached_read_index = 0;

        // Consumer-owned cache line: its index + cached view of the writer.
        // mutable: getNextToRead() is const but may refresh the cache.
        alignas(64) std::atomic<size_t> next_read_index = {0};
        mutable size_t cached_write_index = 0;
    };
}
