//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright 2022, Nikolaos D. Bougalis <nikb@bougalis.net>

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_BASICS_SLABALLOCATOR_H_INCLUDED
#define RIPPLE_BASICS_SLABALLOCATOR_H_INCLUDED

#include <ripple/beast/type_name.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ripple {

template <typename Type, std::size_t ExtraSize = 0>
class SlabAllocator
{
public:
    struct Stats
    {
        // The name of the allocator
        std::string name;

        // The size of an individual item
        std::size_t size = 0;

        // The number of items the allocator can support
        std::size_t count = 0;

        // How many allocation requests were made to the allocator
        std::uint64_t alloc_count = 0;

        // How many of allocations have been serviced by the allocator
        std::uint64_t alloc_fast_count = 0;

        // How many of deallocations have been serviced by the allocator
        std::uint64_t dealloc_fast_count = 0;
    };

private:
    // The name of this allocator, used for debugging purposes:
    std::string const name_;

    // The number of items to allocate:
    std::size_t const count_;

    // The underlying memory block:
    std::uint8_t* p_;

    // A linked list of free buffers:
    std::uint8_t* l_ = nullptr;

    // A mutex to protect the linked list:
    std::mutex mutable m_;

    // How many allocations we've serviced:
    std::atomic<std::uint64_t> alloc_count_ = 0;

    // How many of those were from our internal buffer:
    std::atomic<std::uint64_t> alloc_fast_count_ = 0;

    // How many of those went to our internal buffer:
    std::atomic<std::uint64_t> dealloc_fast_count_ = 0;

    // Scrubs the memory range [ptr, ptr+Size)
    std::uint8_t*
    scrub(std::uint8_t* ptr, [[maybe_unused]] std::uint8_t value)
    {
        assert(ptr != nullptr);

#ifdef SLAB_SCRUB_MEMORY
        std::memset(ptr, value, Size);
#endif

        return ptr;
    }

public:
    // We may need to adjust the size of an individual item to account for
    // alignment issues:
    constexpr static std::size_t const size = []() constexpr {
        std::size_t ret = sizeof(Type) + ExtraSize;

        if (auto const mod = (sizeof(Type) + ExtraSize) % alignof(Type))
            ret += (alignof(Type) - mod);

        return ret;
    }();

    /** Constructs a slab allocator able to allocate up to count items

        @param count the number of items the slab allocator can allocate; note
                     that a count of 0 is valid and means that the allocator
                     is, effectively, disabled. This can be very useful in some
                     contexts (e.g. when mimimal memory usage is needed) and
                     allows for graceful failure.
     */
    SlabAllocator(
        std::size_t count,
        std::string name = "Slab: " + beast::type_name<Type>())
        : name_(name), count_(count)
    {
        if (count != 0)
        {
            p_ = reinterpret_cast<std::uint8_t*>(
                std::aligned_alloc(alignof(Type), size * count_));
            assert(p_ != nullptr);
        }

        // We don't treat a memory allocation
        if (p_ != nullptr)
        {
            for (std::size_t i = 0; i != count_; ++i)
                free(p_ + (i * size));

            // Ignore the use of free above; we just use the API to easily
            // "build" the free list, but it's not an actual deallocation.
            dealloc_fast_count_ = 0;
        }
    }

    ~SlabAllocator()
    {
        if (p_ != nullptr)
            std::free(p_);
    }

    /** Returns the number of items that the allocator can accomodate. */
    std::size_t
    count() const
    {
        return count_;
    }

    /** Determines whether the given pointer belongs to this allocator */
    bool
    own(std::uint8_t const* p) const
    {
        return (p >= p_) && (p < p_ + (size * count_));
    }

    /** Returns a suitably aligned pointer, if one is available.

        @return a pointer to a block of memory from the allocator, or
                nullptr if the allocator can't satisfy this request.
     */
    std::uint8_t*
    alloc()
    {
        alloc_count_++;

        if (l_ != nullptr)
        {
            std::uint8_t* ret;
            {
                std::lock_guard lock(m_);
                ret = l_;
                if (ret)
                    l_ = *reinterpret_cast<std::uint8_t**>(ret);
            }
            if (ret)
            {
                alloc_fast_count_++;
                return scrub(ret, 0xCC);
            }
        }

        return nullptr;
    }

    /** Returns the memory block to the allocator.

        @param ptr A pointer to a memory block.

        @return true if this memory block belonged to the allocator and has
                     been released; false otherwise.
     */
    bool
    free(std::uint8_t const* ptr)
    {
        assert(ptr);

        auto ptr2 = const_cast<std::uint8_t*>(ptr);

        if (!own(ptr2))
            return false;

        dealloc_fast_count_++;

        scrub(ptr2, 0x5A);

        std::lock_guard lock(m_);
        *reinterpret_cast<std::uint8_t**>(ptr2) = l_;
        l_ = ptr2;

        return true;
    }

    /** Returns statistical information about this allocator. */
    Stats
    stats() const
    {
        std::lock_guard lock(m_);
        Stats stats;
        stats.name = name_;
        stats.size = size;
        stats.count = count_;
        stats.alloc_count = alloc_count_;
        stats.alloc_fast_count = alloc_fast_count_;
        stats.dealloc_fast_count = dealloc_fast_count_;
        return stats;
    }
};

}  // namespace ripple

#endif  // RIPPLE_BASICS_SLABALLOCATOR_H_INCLUDED
