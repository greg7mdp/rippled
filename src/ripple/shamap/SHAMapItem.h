//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#ifndef RIPPLE_SHAMAP_SHAMAPITEM_H_INCLUDED
#define RIPPLE_SHAMAP_SHAMAPITEM_H_INCLUDED

#include <ripple/basics/ByteUtilities.h>
#include <ripple/basics/CountedObject.h>
#include <ripple/basics/SlabAllocator.h>
#include <ripple/basics/Slice.h>
#include <ripple/basics/base_uint.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cassert>

namespace ripple {

// an item stored in a SHAMap
class SHAMapItem : public CountedObject<SHAMapItem>
{
    // These are used to support boost::intrusive_ptr reference counting
    // These functions are used internally by boost::intrusive_ptr to handle
    // lifetime management.
    friend void
    intrusive_ptr_add_ref(SHAMapItem const* x);

    friend void
    intrusive_ptr_release(SHAMapItem const* x);

    // This is the interface for creating new instances of this class.
    friend boost::intrusive_ptr<SHAMapItem>
    make_shamapitem(uint256 const& tag, Slice data);

private:
    uint256 const tag_;

    // We use std::uint32_t to minimize the size; there's no SHAMapItem whose
    // size exceeds 4GB and there won't ever be (famous last words?), so this
    // is safe.
    std::uint32_t const size_;

    // This is the reference count used to support boost::intrusive_ptr
    mutable std::atomic<std::uint32_t> refcount_ = 1;

    // Because of the unusual way in which SHAMapItem objects are constructed
    // the only way to properly create one is to first allocate enough memory
    // so we limit this constructor to codepaths that do this right and limit
    // arbitrary construction.
    SHAMapItem(uint256 const& tag, Slice data)
        : tag_(tag), size_(static_cast<std::uint32_t>(data.size()))
    {
        std::memcpy(
            reinterpret_cast<std::uint8_t*>(this) + sizeof(*this),
            data.data(),
            data.size());
    }

public:
    SHAMapItem() = delete;

    SHAMapItem(SHAMapItem const& other) = delete;

    SHAMapItem&
    operator=(SHAMapItem const& other) = delete;

    SHAMapItem(SHAMapItem&& other) = delete;

    SHAMapItem&
    operator=(SHAMapItem&&) = delete;

    uint256 const&
    key() const
    {
        return tag_;
    }

    std::size_t
    size() const
    {
        return size_;
    }

    void const*
    data() const
    {
        return reinterpret_cast<std::uint8_t const*>(this) + sizeof(*this);
    }

    Slice
    slice() const
    {
        return {data(), size()};
    }
};

using shamapitem_ptr = boost::intrusive_ptr<SHAMapItem const>;
    

namespace detail {

template<std::size_t... I>
constexpr auto
make_slab_helper(std::index_sequence<I...>) {
    return std::tuple { new SlabAllocator<SHAMapItem, (I + 1) * 64>(16384)...};
}

inline auto slabs = make_slab_helper(std::make_index_sequence<16>{});

template<std::size_t... I>
constexpr auto
make_allocators_helper(std::index_sequence<I...>) {
    return std::array{std::function{[] { std::get<I>(slabs)->alloc(); }}...};
}

template<std::size_t... I>
constexpr auto
make_deallocators_helper(std::index_sequence<I...>) {
    return std::array{std::function{
            [](std::uint8_t const* p) { std::get<I>(slabs)->dealloc(p); }}...};
}


inline auto allocators = make_allocators_helper(std::make_index_sequence<16>{});
inline auto deallocators = make_deallocators_helper(std::make_index_sequence<16>{});
    

// clang-format off
inline SlabAllocator<SHAMapItem,  128> slab128 ( 7000000);
inline SlabAllocator<SHAMapItem,  192> slab192 ( 1000000);
inline SlabAllocator<SHAMapItem,  256> slab256 (10000000);
inline SlabAllocator<SHAMapItem,  512> slab512 (  750000);
inline SlabAllocator<SHAMapItem, 1024> slab1024(  450000);
// clang-format on

inline std::atomic<std::uint64_t> cnt64 = 0;

}  // namespace detail

inline void
intrusive_ptr_add_ref(SHAMapItem const* x)
{
    // This can only happen if someone releases the last reference to the
    // item while we were trying to increment the refcount.
    if (x->refcount_++ == 0)
        LogicError("SHAMapItem: the reference count is 0!");
}

inline void
intrusive_ptr_release(SHAMapItem const* x)
{
    if (--x->refcount_ == 0)
    {
        auto p = reinterpret_cast<std::uint8_t const*>(x);

        // The SHAMapItem constuctor isn't trivial (because the destructor
        // for CountedObject isn't) so we can't avoid calling it here, but
        // plan for a future where we might not need to.
        if constexpr (!std::is_trivially_destructible_v<SHAMapItem>)
            std::destroy_at(x);

        // At most one slab will claim this pointer; if none do, it was
        // allocated manually, so we free it manually.
        if (!detail::slab128.dealloc(p) && !detail::slab192.dealloc(p) &&
            !detail::slab256.dealloc(p) && !detail::slab512.dealloc(p) &&
            !detail::slab1024.dealloc(p))
        {
            delete [] p;
        }
    }
}

inline boost::intrusive_ptr<SHAMapItem>
make_shamapitem(uint256 const& tag, Slice data)
{
    assert(data.size() <= megabytes<std::size_t>(64));

    std::uint8_t* raw = [&data]() -> std::uint8_t* {
        if (data.size() < 128)
            return detail::slab128.alloc();

        if (data.size() < 192)
            return detail::slab192.alloc();

        if (data.size() < 256)
            return detail::slab256.alloc();

        if (data.size() < 512)
            return detail::slab512.alloc();

        if (data.size() < 1024)
            return detail::slab1024.alloc();

        return nullptr;
    }();

    if (raw == nullptr)
    {
        // If we can't grab memory from the slab allocators, we fall back to
        // the standard library and try to grab a precisely-sized memory block:
        raw = new std::uint8_t [sizeof(SHAMapItem) + data.size()];

        if (raw == nullptr)
            throw std::bad_alloc();
    }

    // We do not increment the reference count here on purpose: the
    // constructor of SHAMapItem explicitly sets it to 1. We use the fact
    // that the refcount can never be zero before incrementing as an
    // invariant.
    return {new (raw) SHAMapItem{tag, data}, false};
}

inline boost::intrusive_ptr<SHAMapItem>
make_shamapitem(SHAMapItem const& other)
{
    return make_shamapitem(other.key(), other.slice());
}

}  // namespace ripple

#endif
