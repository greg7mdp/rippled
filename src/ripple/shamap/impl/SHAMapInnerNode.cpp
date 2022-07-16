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

#include <ripple/shamap/SHAMapInnerNode.h>

#include <ripple/basics/Log.h>
#include <ripple/basics/Slice.h>
#include <ripple/basics/contract.h>
#include <ripple/beast/core/LexicalCast.h>
#include <ripple/protocol/HashPrefix.h>
#include <ripple/protocol/digest.h>
#include <ripple/shamap/SHAMapTreeNode.h>
#include <ripple/shamap/impl/TaggedPointer.ipp>

#include <openssl/sha.h>

#include <algorithm>
#include <iterator>
#include <utility>

#ifndef __aarch64__
// This is used for the _mm_pause instruction:
#include <immintrin.h>
#endif

namespace ripple {

/** A specialized 16-way spinlock used to protect inner node branches.

    This class packs 16 separate spinlocks into a single 16-bit value. It makes
    it possible to lock any one lock at once or, alternatively, all together.

    The implementation tries to use portable constructs but has to be low-level
    for performance.
 */
class SpinBitlock
{
private:
    std::atomic<std::uint16_t>& bits_;
    std::uint16_t mask_;

public:
    SpinBitlock(std::atomic<std::uint16_t>& lock) : bits_(lock), mask_(0xFFFF)
    {
    }

    SpinBitlock(std::atomic<std::uint16_t>& lock, int index)
        : bits_(lock), mask_(1 << index)
    {
        assert(index >= 0 && index < 16);
    }

    [[nodiscard]] bool
    try_lock()
    {
        // If we want to grab all the individual bitlocks at once we cannot
        // use `fetch_or`! To see why, imagine that `lock_ == 0x0020` which
        // means that the `fetch_or` would return `0x0020` but all the bits
        // would already be (incorrectly!) set. Oops!
        std::uint16_t expected = 0;

        if (mask_ != 0xFFFF)
            return (bits_.fetch_or(mask_, std::memory_order_acquire) & mask_) ==
                expected;

        return bits_.compare_exchange_weak(
            expected,
            mask_,
            std::memory_order_acquire,
            std::memory_order_relaxed);
    }

    void
    lock()
    {
        // Testing suggests that 99.9999% of the time this will succeed, so
        // we try to optimize the fast path.
        if (try_lock())
            return;

        do
        {
            // We try to spin for a few times:
            for (int i = 0; i != 100; ++i)
            {
                if (try_lock())
                    return;

#ifndef __aarch64__
                _mm_pause();
#endif
            }

            std::this_thread::yield();
        } while ((bits_.load(std::memory_order_relaxed) & mask_) == 0);
    }

    void
    unlock()
    {
        bits_.fetch_and(~mask_, std::memory_order_release);
    }
};

SHAMapInnerNode::SHAMapInnerNode(
    std::uint32_t cowid,
    std::uint8_t numAllocatedChildren)
    : SHAMapTreeNode(cowid), hashesAndChildren_(numAllocatedChildren)
{
}

SHAMapInnerNode::~SHAMapInnerNode() = default;

template <class F>
void
SHAMapInnerNode::iterChildren(F&& f) const
{
    hashesAndChildren_.iterChildren(isBranch_, std::forward<F>(f));
}

template <class F>
void
SHAMapInnerNode::iterNonEmptyChildIndexes(F&& f) const
{
    hashesAndChildren_.iterNonEmptyChildIndexes(isBranch_, std::forward<F>(f));
}

void
SHAMapInnerNode::resizeChildArrays(std::uint8_t toAllocate)
{
    hashesAndChildren_ =
        TaggedPointer(std::move(hashesAndChildren_), isBranch_, toAllocate);
}

std::optional<int>
SHAMapInnerNode::getChildIndex(int i) const
{
    return hashesAndChildren_.getChildIndex(isBranch_, i);
}

shamaptreenode_ptr
SHAMapInnerNode::clone(std::uint32_t cowid) const
{
    auto const branchCount = getBranchCount();
    auto const thisIsSparse = !hashesAndChildren_.isDense();
    auto p = make_shamapnode<SHAMapInnerNode>(cowid, branchCount);
    p->hash_ = hash_;
    p->isBranch_ = isBranch_;
    p->fullBelowGen_ = fullBelowGen_;
    SHAMapHash *cloneHashes, *thisHashes;
    shamaptreenode_ptr*cloneChildren, *thisChildren;
    // structured bindings can't be captured in c++ 17; use tie instead
    std::tie(std::ignore, cloneHashes, cloneChildren) =
        p->hashesAndChildren_.getHashesAndChildren();
    std::tie(std::ignore, thisHashes, thisChildren) =
        hashesAndChildren_.getHashesAndChildren();

    if (thisIsSparse)
    {
        int cloneChildIndex = 0;
        iterNonEmptyChildIndexes([&](auto branchNum, auto indexNum) {
            cloneHashes[cloneChildIndex++] = thisHashes[indexNum];
        });
    }
    else
    {
        iterNonEmptyChildIndexes([&](auto branchNum, auto indexNum) {
            cloneHashes[branchNum] = thisHashes[indexNum];
        });
    }

    SpinBitlock sl(lock_);
    std::lock_guard lock(sl);

    if (thisIsSparse)
    {
        int cloneChildIndex = 0;
        iterNonEmptyChildIndexes([&](auto branchNum, auto indexNum) {
            cloneChildren[cloneChildIndex++] = thisChildren[indexNum];
        });
    }
    else
    {
        iterNonEmptyChildIndexes([&](auto branchNum, auto indexNum) {
            cloneChildren[branchNum] = thisChildren[indexNum];
        });
    }

    return p;
}

shamaptreenode_ptr
SHAMapInnerNode::makeFullInner(
    Slice data,
    SHAMapHash const& hash,
    bool hashValid)
{
    // A full inner node is serialized as 16 256-bit hashes, back to back:
    if (data.size() != branchFactor * uint256::bytes)
        Throw<std::runtime_error>("Invalid FI node");

    auto ret = make_shamapnode<SHAMapInnerNode>(0, branchFactor);

    SerialIter si(data);

    auto hashes = ret->hashesAndChildren_.getHashes();

    for (int i = 0; i < branchFactor; ++i)
    {
        hashes[i].as_uint256() = si.getBitString<256>();

        if (hashes[i].isNonZero())
            ret->isBranch_ |= (1 << i);
    }

    ret->resizeChildArrays(ret->getBranchCount());

    if (hashValid)
        ret->hash_ = hash;
    else
        ret->updateHash();

    return ret;
}

shamaptreenode_ptr
SHAMapInnerNode::makeCompressedInner(Slice data)
{
    // A compressed inner node is serialized as a series of 33 byte chunks,
    // representing a one byte "position" and a 256-bit hash:
    constexpr std::size_t chunkSize = uint256::bytes + 1;

    if (auto const s = data.size();
        (s % chunkSize != 0) || (s > chunkSize * branchFactor))
        Throw<std::runtime_error>("Invalid CI node");

    SerialIter si(data);

    auto ret = make_shamapnode<SHAMapInnerNode>(0, branchFactor);

    auto hashes = ret->hashesAndChildren_.getHashes();

    while (!si.empty())
    {
        auto const hash = si.getBitString<256>();
        auto const pos = si.get8();

        if (pos >= branchFactor)
            Throw<std::runtime_error>("invalid CI node");

        hashes[pos].as_uint256() = hash;

        if (hashes[pos].isNonZero())
            ret->isBranch_ |= (1 << pos);
    }

    ret->resizeChildArrays(ret->getBranchCount());
    ret->updateHash();
    return ret;
}

void
SHAMapInnerNode::updateHash()
{
    uint256 nh;
    if (isBranch_ != 0)
    {
        sha512_half_hasher h;
        using beast::hash_append;
        hash_append(h, HashPrefix::innerNode);
        iterChildren([&](SHAMapHash const& hh) { hash_append(h, hh); });
        nh = static_cast<typename sha512_half_hasher::result_type>(h);
    }
    hash_ = SHAMapHash{nh};
}

void
SHAMapInnerNode::updateHashDeep()
{
    SHAMapHash* hashes;
    shamaptreenode_ptr* children;
    // structured bindings can't be captured in c++ 17; use tie instead
    std::tie(std::ignore, hashes, children) =
        hashesAndChildren_.getHashesAndChildren();
    iterNonEmptyChildIndexes([&](auto branchNum, auto indexNum) {
        if (children[indexNum] != nullptr)
            hashes[indexNum] = children[indexNum]->getHash();
    });
    updateHash();
}

void
SHAMapInnerNode::serializeForWire(Serializer& s) const
{
    assert(!isEmpty());

    // If the node is sparse, then only send non-empty branches:
    if (getBranchCount() < 12)
    {
        // compressed node
        auto hashes = hashesAndChildren_.getHashes();
        iterNonEmptyChildIndexes([&](auto branchNum, auto indexNum) {
            s.addBitString(hashes[indexNum].as_uint256());
            s.add8(branchNum);
        });
        s.add8(wireTypeCompressedInner);
    }
    else
    {
        iterChildren(
            [&](SHAMapHash const& hh) { s.addBitString(hh.as_uint256()); });
        s.add8(wireTypeInner);
    }
}

void
SHAMapInnerNode::serializeWithPrefix(Serializer& s) const
{
    assert(!isEmpty());

    s.add32(HashPrefix::innerNode);
    iterChildren(
        [&](SHAMapHash const& hh) { s.addBitString(hh.as_uint256()); });
}

bool
SHAMapInnerNode::isEmpty() const
{
    return isBranch_ == 0;
}

int
SHAMapInnerNode::getBranchCount() const
{
    return popcnt16(isBranch_);
}

std::string
SHAMapInnerNode::getString(const SHAMapNodeID& id) const
{
    std::string ret = SHAMapTreeNode::getString(id);
    auto hashes = hashesAndChildren_.getHashes();
    iterNonEmptyChildIndexes([&](auto branchNum, auto indexNum) {
        ret += "\nb";
        ret += std::to_string(branchNum);
        ret += " = ";
        ret += to_string(hashes[indexNum]);
    });
    return ret;
}

// We are modifying an inner node
void
SHAMapInnerNode::setChild(int m, shamaptreenode_ptr const& child)
{
    assert((m >= 0) && (m < branchFactor));
    assert(cowid_ != 0);
    assert(child.get() != this);

    auto const dstIsBranch = [&] {
        if (child)
            return isBranch_ | (1 << m);
        else
            return isBranch_ & ~(1 << m);
    }();

    auto const dstToAllocate = popcnt16(dstIsBranch);
    // change hashesAndChildren to remove the element, or make room for the
    // added element, if necessary
    hashesAndChildren_ = TaggedPointer(
        std::move(hashesAndChildren_), isBranch_, dstIsBranch, dstToAllocate);

    isBranch_ = dstIsBranch;

    if (child)
    {
        auto const childIndex = *getChildIndex(m);
        auto [_, hashes, children] = hashesAndChildren_.getHashesAndChildren();
        hashes[childIndex].zero();
        children[childIndex] = child;
    }

    hash_.zero();

    assert(getBranchCount() <= hashesAndChildren_.capacity());
}

// finished modifying, now make shareable
void
SHAMapInnerNode::shareChild(int m, shamaptreenode_ptr const& child)
{
    assert((m >= 0) && (m < branchFactor));
    assert(cowid_ != 0);
    assert(child);
    assert(child.get() != this);

    assert(!isEmptyBranch(m));
    hashesAndChildren_.getChildren()[*getChildIndex(m)] = child;
}

SHAMapTreeNode*
SHAMapInnerNode::getChildPointer(int branch)
{
    assert(branch >= 0 && branch < branchFactor);
    assert(!isEmptyBranch(branch));

    auto const index = *getChildIndex(branch);

    SpinBitlock sl(lock_, index);
    std::lock_guard lock(sl);
    return hashesAndChildren_.getChildren()[index].get();
}

shamaptreenode_ptr
SHAMapInnerNode::getChild(int branch)
{
    assert(branch >= 0 && branch < branchFactor);
    assert(!isEmptyBranch(branch));

    auto const index = *getChildIndex(branch);

    SpinBitlock sl(lock_, index);
    std::lock_guard lock(sl);
    return hashesAndChildren_.getChildren()[index];
}

SHAMapHash const&
SHAMapInnerNode::getChildHash(int m) const
{
    assert((m >= 0) && (m < branchFactor));
    if (auto const i = getChildIndex(m))
        return hashesAndChildren_.getHashes()[*i];

    return zeroSHAMapHash;
}

shamaptreenode_ptr
SHAMapInnerNode::canonicalizeChild(
    int branch,
    shamaptreenode_ptr node)
{
    assert(branch >= 0 && branch < branchFactor);
    assert(node);
    assert(!isEmptyBranch(branch));
    auto const childIndex = *getChildIndex(branch);
    auto [_, hashes, children] = hashesAndChildren_.getHashesAndChildren();
    assert(node->getHash() == hashes[childIndex]);

    SpinBitlock sl(lock_, childIndex);
    std::lock_guard lock(sl);

    if (children[childIndex])
    {
        // There is already a node hooked up, return it
        node = children[childIndex];
    }
    else
    {
        // Hook this node up
        children[childIndex] = node;
    }
    return node;
}

void
SHAMapInnerNode::invariants(bool is_root) const
{
    unsigned count = 0;
    auto [numAllocated, hashes, children] =
        hashesAndChildren_.getHashesAndChildren();

    if (numAllocated != branchFactor)
    {
        auto const branchCount = getBranchCount();
        for (int i = 0; i < branchCount; ++i)
        {
            assert(hashes[i].isNonZero());
            if (children[i] != nullptr)
                children[i]->invariants();
            ++count;
        }
    }
    else
    {
        for (int i = 0; i < branchFactor; ++i)
        {
            if (hashes[i].isNonZero())
            {
                assert((isBranch_ & (1 << i)) != 0);
                if (children[i] != nullptr)
                    children[i]->invariants();
                ++count;
            }
            else
            {
                assert((isBranch_ & (1 << i)) == 0);
            }
        }
    }

    if (!is_root)
    {
        assert(hash_.isNonZero());
        assert(count >= 1);
    }
    assert((count == 0) ? hash_.isZero() : hash_.isNonZero());
}

}  // namespace ripple
