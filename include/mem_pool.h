#pragma once
#include <string>
#include <utility>
#include <vector>
#include "macros.h"

namespace Common
{
    template<typename T>
    class MemPool final
    {
    public:
        // explicit to prohibit implicit conversions.
        explicit MemPool(std::size_t num_elems) :
            store(num_elems, {T(), true}), /* pre-allocation of vector storage. */
            free_count(num_elems)
        {
            ASSERT(reinterpret_cast<const ObjectBlock *>(&(store[0].object)) == &(store[0]), "T object should be first member of ObjectBlock.");
        }

        template<typename... Args>
        T * allocate(Args&&... args) noexcept
        {
            // Detect exhaustion up front. The previous version scanned for a
            // free block with no termination condition and spun forever when
            // the pool was empty — fail loudly instead.
            ASSERT(free_count > 0, "MemPool exhausted — no free objects; size the pool larger.");

            // next_free_index may be stale (e.g. pointing at a block that was
            // allocated and not yet freed) — advance to the next free block.
            // free_count > 0 guarantees termination.
            while (!store[next_free_index].is_free)
            {
                ++next_free_index;
                if (UNLIKELY(next_free_index == store.size()))
                {
                    next_free_index = 0; // wrap around.
                }
            }

            auto obj_block = &store[next_free_index];
            T * ret = &(obj_block->object);
            ret = new (ret) T(std::forward<Args>(args)...); // placement new.
            obj_block->is_free = false;
            --free_count;
            return ret;
        }

        auto deallocate(const T* elem) noexcept
        {
            const auto elem_index = reinterpret_cast<const ObjectBlock *>(elem) - &(store[0]);
            ASSERT(elem_index >= 0 && static_cast<size_t>(elem_index) < store.size(), "Element being deallocated does not belong to this memory pool.");
            ASSERT(!store[elem_index].is_free, "Expected allocated ObjectBlock at index : " + std::to_string(elem_index));
            store[elem_index].is_free = true;
            ++free_count;
        }

        auto freeCount() const noexcept -> std::size_t { return free_count; }

        MemPool() = delete; // prohibit default constructor.
        MemPool(const MemPool &) = delete; // prohibit copy constructor.
        MemPool &operator=(const MemPool &) = delete; // prohibit copy assignment.
        MemPool(MemPool &&) = delete; // prohibit move constructor.
        MemPool &operator=(MemPool &&) = delete; // prohibit move assignment.
    private:
        struct ObjectBlock
        {
            T object;
            bool is_free = true;
        };
        std::vector<ObjectBlock> store;
        size_t next_free_index = 0;
        size_t free_count = 0;
    };
}
