//
// opt_mem_pool.h — release-build optimized variant of Common::MemPool.
//
// Identical to common/mem_pool.h except that the ASSERT() calls on the
// hot path (allocate / deallocate / updateNextFreeIndex) are guarded
// by `#if !defined(NDEBUG)`. The originals are unchanged; this file is
// side-by-side evidence for the report's §5 NDEBUG-stripping benchmark.
//
// Mechanism: ASSERT in macros.h is an `inline` function that conditionally
// runs `std::cerr <<` + `exit(1)` — but the compiler must still evaluate
// the `cond` expression and construct the `std::string &msg` argument
// because the `inline` is NOT a compile-time no-op. The `+ std::to_string`
// path in the message allocates a heap string on every call. Even when the
// branch is rarely taken, the message construction is on the hot path.
//
// Wrapping the ASSERT call site in `#if !defined(NDEBUG)` removes both
// the comparison and the message construction in release builds.
//

#pragma once
#include <string>
#include <vector>
#include "macros.h"

namespace OptCommon
{
    template<typename T>
    class OptMemPool final
    {
    public:
        explicit OptMemPool(std::size_t num_elems) :
            store(num_elems, {T(), true})
        {
#if !defined(NDEBUG)
            ASSERT(reinterpret_cast<const ObjectBlock *>(&(store[0].object)) == &(store[0]),
                   "T object should be first member of ObjectBlock.");
#endif
        }

        template<typename... Args>
        T * allocate(Args&&... args) noexcept
        {
            auto obj_block = &store[next_free_index];
#if !defined(NDEBUG)
            ASSERT(obj_block->is_free,
                   "Expected free ObjectBlock at index : " + std::to_string(next_free_index));
#endif
            T * ret = &(obj_block->object);
            ret = new (ret) T(args...); // placement new.
            obj_block->is_free = false;
            updateNextFreeIndex();
            return ret;
        }

        auto deallocate(const T* elem) noexcept
        {
            const auto elem_index = reinterpret_cast<const ObjectBlock *>(elem) - &(store[0]);
#if !defined(NDEBUG)
            ASSERT(elem_index >= 0 && static_cast<size_t>(elem_index) < store.size(),
                   "Element being deallocated does not belong to this memory pool.");
            ASSERT(!store[elem_index].is_free,
                   "Expected allocated ObjectBlock at index : " + std::to_string(elem_index));
#endif
            store[elem_index].is_free = true;
        }

        OptMemPool() = delete;
        OptMemPool(const OptMemPool &) = delete;
        OptMemPool &operator=(const OptMemPool &) = delete;
        OptMemPool(OptMemPool &&) = delete;
        OptMemPool &operator=(OptMemPool &&) = delete;
    private:
        auto updateNextFreeIndex() noexcept
        {
            const auto initial_free_index = next_free_index;
            while (!store[next_free_index].is_free)
            {
                ++next_free_index;
                if (UNLIKELY(next_free_index == store.size()))
                {
                    next_free_index = 0;
                }
#if !defined(NDEBUG)
                if (UNLIKELY(initial_free_index == next_free_index))
                {
                    ASSERT(initial_free_index == next_free_index,
                           "Memory Pool exhausted. No free objects available for allocation.");
                }
#else
                (void) initial_free_index;
#endif
            }
        }

        struct ObjectBlock
        {
            T object;
            bool is_free = true;
        };
        std::vector<ObjectBlock> store;
        size_t next_free_index = 0;
    };
}
