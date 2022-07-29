//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2022 Ripple Labs Inc.

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

#ifndef RIPPLE_PROTOCOL_STXATTESTATIONS_H_INCLUDED
#define RIPPLE_PROTOCOL_STXATTESTATIONS_H_INCLUDED

#include <ripple/basics/Buffer.h>
#include <ripple/basics/Expected.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/TER.h>

#include "ripple/protocol/STXChainAttestationBatch.h"
#include <cstddef>
#include <vector>

namespace ripple {

namespace AttestationBatch {
struct AttestationClaim;
struct AttestationCreateAccount;
}  // namespace AttestationBatch

// Result when checking when two attestation match.
enum class AttestationMatch {
    // One of the fields doesn't match, and it isn't the dst field
    non_dst_mismatch,
    // all of the fields match, except the dst field
    match_except_dst,
    // all of the fields match
    match
};

struct XChainClaimAttestation
{
    using TBatchAttestation = AttestationBatch::AttestationClaim;
    static SField const& ArrayFieldName;

    AccountID keyAccount;
    STAmount amount;
    AccountID rewardAccount;
    bool wasLockingChainSend;
    std::optional<AccountID> dst;

    struct MatchFields
    {
        STAmount amount;
        bool wasLockingChainSend;
        std::optional<AccountID> dst;
        MatchFields(TBatchAttestation const& att);
        MatchFields(
            STAmount const& a,
            bool b,
            std::optional<AccountID> const& d)
            : amount{a}, wasLockingChainSend{b}, dst{d}
        {
        }
    };

    explicit XChainClaimAttestation(
        AccountID const& keyAccount_,
        STAmount const& amount_,
        AccountID const& rewardAccount_,
        bool wasLockingChainSend_,
        std::optional<AccountID> const& dst);

    explicit XChainClaimAttestation(
        STAccount const& keyAccount_,
        STAmount const& amount_,
        STAccount const& rewardAccount_,
        bool wasLockingChainSend_,
        std::optional<STAccount> const& dst);

    explicit XChainClaimAttestation(TBatchAttestation const& claimAtt);

    explicit XChainClaimAttestation(STObject const& o);

    explicit XChainClaimAttestation(Json::Value const& v);

    AttestationMatch
    match(MatchFields const& rhs) const;

    STObject
    toSTObject() const;

    friend bool
    operator==(
        XChainClaimAttestation const& lhs,
        XChainClaimAttestation const& rhs);
    friend bool
    operator!=(
        XChainClaimAttestation const& lhs,
        XChainClaimAttestation const& rhs);
};

struct XChainCreateAccountAttestation
{
    using TBatchAttestation = AttestationBatch::AttestationCreateAccount;
    static SField const& ArrayFieldName;

    AccountID keyAccount;
    // TODO: Remove createCount: it's on the ledger object
    std::uint64_t createCount;
    STAmount amount;
    STAmount rewardAmount;
    AccountID rewardAccount;
    bool wasLockingChainSend;
    AccountID dst;

    struct MatchFields
    {
        std::uint64_t createCount;
        STAmount amount;
        STAmount rewardAmount;
        bool wasLockingChainSend;
        AccountID dst;

        MatchFields(TBatchAttestation const& att);
    };

    explicit XChainCreateAccountAttestation(
        AccountID const& keyAccount_,
        std::uint64_t createCount_,
        STAmount const& amount_,
        STAmount const& rewardAmount_,
        AccountID const& rewardAccount_,
        bool wasLockingChainSend_,
        AccountID const& dst_);

    explicit XChainCreateAccountAttestation(TBatchAttestation const& claimAtt);

    explicit XChainCreateAccountAttestation(STObject const& o);

    explicit XChainCreateAccountAttestation(Json::Value const& v);

    STObject
    toSTObject() const;

    AttestationMatch
    match(MatchFields const& rhs) const;

    friend bool
    operator==(
        XChainCreateAccountAttestation const& lhs,
        XChainCreateAccountAttestation const& rhs);
    friend bool
    operator!=(
        XChainCreateAccountAttestation const& lhs,
        XChainCreateAccountAttestation const& rhs);
};

// Attestations from witness servers for a particular claimid and bridge.
// Only one attestation per signature is allowed. If more than one is added, the
// attestation with the larger amount is kept.
template <class TAttestation>
class XChainAttestationsBase
{
public:
    using AttCollection = std::vector<TAttestation>;

private:
    AttCollection attestations_;

protected:
    // Prevent slicing to the base class
    ~XChainAttestationsBase() = default;

public:
    XChainAttestationsBase() = default;
    XChainAttestationsBase(XChainAttestationsBase const& rhs) = default;
    XChainAttestationsBase&
    operator=(XChainAttestationsBase const& rhs) = default;

    XChainAttestationsBase(AttCollection&& sigs);

    explicit XChainAttestationsBase(Json::Value const& v);

    explicit XChainAttestationsBase(STArray const& arr);

    STArray
    toSTArray() const;

    /**
     Handle a new attestation event.

     Attempt to add the given attestation and reconcile with the current
     signer's list. Attestations that are not part of the current signer's
     list will be removed.

     @param claimAtt New attestation to add. It will be added if it is not
     already part of the collection, or attests to a larger value.

     @param quorum Min weight required for a quorum

     @param signersList Map from signer's account id (derived from public keys)
     to the weight of that key.

     @return optional reward accounts. If after handling the new attestation
     there is a quorum for the amount specified on the new attestation, then
     return the reward accounts for that amount, otherwise return a nullopt.
     Note that if the signer's list changes and there have been `commit`
     transactions at different amounts then there may be a different subset that
     has reached quorum. However, to "trigger" that subset would require adding
     (or re-adding) an attestation that supports that subset.

     The reason for using a nullopt instead of an empty vector when a quorum is
     not reached is to allow for an interface where a quorum is reached but no
     rewards are distributed.

     @note This function is not called `add` because it does more than just
           add the new attestation (in fact, it may not add the attestation at
           all). Instead, it handles the event of a new attestation.
     */
    std::optional<std::vector<AccountID>>
    onNewAttestation(
        typename TAttestation::TBatchAttestation const& claimAtt,
        std::uint32_t quorum,
        std::unordered_map<AccountID, std::uint32_t> const& signersList);

    typename AttCollection::const_iterator
    begin() const;

    typename AttCollection::const_iterator
    end() const;

    std::size_t
    size() const;

    bool
    empty() const;

    AttCollection const&
    attestations() const;

    // verify that all the signatures attest to transaction data.
    bool
    verify() const;

protected:
    // If there is a quorum of attestations for the given parameters, then
    // return the reward accounts, otherwise return TER for the error.
    // Also removes attestations that are no longer part of the signers list.
    //
    // Note: the dst parameter is what the attestations are attesting to, which
    // is not always used (it is used when automatically triggering a transfer
    // from an `addAttestation` transaction, it is not used in a `claim`
    // transaction). If the `checkDst` parameter is `check`, the attestations
    // must attest to this destination, if it is `ignore` then the `dst` of the
    // attestations are not checked (as for a `claim` transaction)

    enum class CheckDst { check, ignore };
    Expected<std::vector<AccountID>, TER>
    claimHelper(
        typename TAttestation::MatchFields const& toMatch,
        CheckDst checkDst,
        std::uint32_t quorum,
        std::unordered_map<AccountID, std::uint32_t> const& signersList);

    // Return the message that was expected to be signed by the attesters given
    // the data to be proved.
    std::vector<std::uint8_t>
    message() const;
};

template <class TAttestation>
inline bool
operator==(
    XChainAttestationsBase<TAttestation> const& lhs,
    XChainAttestationsBase<TAttestation> const& rhs)
{
    return lhs.attestations() == rhs.attestations();
}

template <class TAttestation>
inline bool
operator!=(
    XChainAttestationsBase<TAttestation> const& lhs,
    XChainAttestationsBase<TAttestation> const& rhs)
{
    return !(lhs == rhs);
}

template <class TAttestation>
inline typename XChainAttestationsBase<TAttestation>::AttCollection const&
XChainAttestationsBase<TAttestation>::attestations() const
{
    return attestations_;
};

template <class TAttestation>
inline std::size_t
XChainAttestationsBase<TAttestation>::size() const
{
    return attestations_.size();
}

template <class TAttestation>
inline bool
XChainAttestationsBase<TAttestation>::empty() const
{
    return attestations_.empty();
}

class XChainClaimAttestations final
    : public XChainAttestationsBase<XChainClaimAttestation>
{
    using TBase = XChainAttestationsBase<XChainClaimAttestation>;
    using TBase::TBase;

public:
    // Check if there is a quorurm of attestations for the given amount and
    // chain. If so return the reward accounts, if not return the tec code (most
    // likely tecXCHAIN_CLAIM_NO_QUORUM)
    Expected<std::vector<AccountID>, TER>
    onClaim(
        STAmount const& sendingAmount,
        bool wasLockingChainSend,
        std::uint32_t quorum,
        std::unordered_map<AccountID, std::uint32_t> const& signersList);
};

class XChainCreateAccountAttestations final
    : public XChainAttestationsBase<XChainCreateAccountAttestation>
{
    using TBase = XChainAttestationsBase<XChainCreateAccountAttestation>;
    using TBase::TBase;
};

}  // namespace ripple

#endif  // STXCHAINATTESTATIONS_H_
