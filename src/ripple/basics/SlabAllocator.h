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
#include <vector>
#include <mutex>

namespace ripple {

namespace detail {

    
template <typename Type, std::size_t ExtraSize = 0>
class SlabAllocator
{
public:
    struct Stats
    {
        // The name of the allocator
        std::string name;

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
    std::vector<std::uint8_t*> p_;

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

    // Scrubs the memory range [ptr, ptr+size)
    std::uint8_t*
    scrub(std::uint8_t* ptr, [[maybe_unused]] std::uint8_t value)
    {
        assert(ptr != nullptr);

#ifdef SLAB_SCRUB_MEMORY
        std::memset(ptr, value, size);
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

    SlabAllocator(
        std::size_t count,
        std::string name = "Slab: " + beast::type_name<Type>())
        : name_(name), count_(count)
    {
        assert(count_);
    }

    SlabAllocator(const SlabAllocator&) = delete;

    SlabAllocator&
    operator=(const SlabAllocator&) = delete;

    ~SlabAllocator()
    {
        for (auto p : p_)
            delete [] p;
    }

    /** Returns the number of items that the allocator can accomodate. */
    std::size_t
    count() const
    {
        std::lock_guard lock(m_);
        return count_ * p_.size();
    }

    /** Returns a suitably aligned pointer, if one is available.

        @return a pointer to a block of memory from the allocator, or
                nullptr if the allocator can't satisfy this request.
     */
    std::uint8_t*
    alloc()
    {
        alloc_count_++;
        std::uint8_t* ret;
        {
            std::lock_guard lock(m_);
        
            if (l_ == nullptr)
                add_block();
        
            assert (l_ != nullptr);
        
            ret = l_;
            l_ = *reinterpret_cast<std::uint8_t**>(ret);
        }
        alloc_fast_count_++;
        
        return scrub(ret, 0xCC);
    }

    /** Returns the memory block to the allocator.

        @param ptr A pointer to a memory block.

        @return true if this memory block belonged to the allocator and has
                     been released; false otherwise.
     */
    bool
    dealloc(std::uint8_t const* ptr)
    {
        assert(ptr);

        auto p = const_cast<std::uint8_t*>(ptr);

        dealloc_fast_count_++;
        scrub(p, 0x5A);

        std::lock_guard lock(m_);
        
        assert(own(p));
        *reinterpret_cast<std::uint8_t**>(p) = l_;
        l_ = p;

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
        stats.count = count();
        stats.alloc_count = alloc_count_;
        stats.alloc_fast_count = alloc_fast_count_;
        stats.dealloc_fast_count = dealloc_fast_count_;
        return stats;
    }

private:
    void
    add_block()
    {
        auto block = new std::uint8_t [size * count_];
        assert(((uintptr_t)block & 0x7) == 0); // check alignment
        assert(block != nullptr);

        p_.push_back(block);

        // add block buckets to linked list (in increasing address order)
        assert(l_ == nullptr);
        std::uint8_t** tail = &l_;
        for (std::size_t i = 0; i != count_; ++i)
        {
            std::uint8_t* p = block + (i * size);
            scrub(p, 0x5A);
            *tail = p;
            tail = reinterpret_cast<std::uint8_t**>(p);
        }
        *tail = nullptr;
    }

    bool
    own(std::uint8_t const* ptr) const
    {
        for (auto p : p_)
            if ((ptr >= p) && (ptr < p + (size * count_)))
                return true;
        return false;
    }
};

}  // namespace detail

template <class T, size_t num_slabs = 64, size_t slab_increment = 8>
class SlabAllocators
{
    template <std::size_t... I>
    static constexpr auto
    make_slab_helper(size_t slab_block_size, std::index_sequence<I...>)
    {
        return std::tuple{
            new detail::SlabAllocator<SHAMapItem, (I + 1) * slab_increment>(
                slab_block_size)...};
    }

private:
    decltype(make_slab_helper(1, std::make_index_sequence<num_slabs>{})) slabs_;
    std::array<std::function<std::uint8_t*()>, num_slabs> allocators_;
    std::array<std::function<void(std::uint8_t const*)>, num_slabs>
        deallocators_;

public:
    static constexpr size_t max_slab_size = num_slabs * slab_increment;

    SlabAllocators(size_t slab_block_size = 4096)
        : slabs_(make_slab_helper(
              slab_block_size,
              std::make_index_sequence<num_slabs>{}))
        , allocators_(
              make_allocators_helper(std::make_index_sequence<num_slabs>{}))
        , deallocators_(
              make_deallocators_helper(std::make_index_sequence<num_slabs>{}))

    {
    }

    void
    deallocate(std::size_t sz, std::uint8_t const* p)
    {
        assert(sz <= max_slab_size);
        deallocators_[allocator_index(sz)](p);
    }

    std::uint8_t*
    allocate(std::size_t sz)
    {
        assert(sz <= max_slab_size);
        return allocators_[allocator_index(sz)]();
    }

private:
    template <std::size_t... I>
    constexpr auto
    make_allocators_helper(std::index_sequence<I...>)
    {
        return std::array{
            std::function{[this] { return std::get<I>(slabs_)->alloc(); }}...};
    }

    template <std::size_t... I>
    constexpr auto
    make_deallocators_helper(std::index_sequence<I...>)
    {
        return std::array{std::function{[this](std::uint8_t const* p) {
            std::get<I>(slabs_)->dealloc(p);
        }}...};
    }

    static constexpr size_t
    allocator_index(size_t sz)
    {
        return sz ? (sz - 1) / slab_increment : 0;
    }
};

}  // namespace ripple

#endif  // RIPPLE_BASICS_SLABALLOCATOR_H_INCLUDED
