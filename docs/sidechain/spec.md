# Sidechains

## Overview

This document defines the ledger objects, transactions, RPC commands, and
additional servers that are used to support sidechains on the XRP ledger. A
sidechain is an independent ledger. It has its own validators and can have its
own set of custom transactions. Importantly, there is a way to move assets from
the mainchain to the sidechain and there is a way to return those assets from
the sidechain to the mainchain. This key operation is called a cross-chain
transfer. A cross-chain transfer is not a single transaction. It happens on two
chains, requires multiple transactions and involves an additional server type
called a "witness". This key operation is described in a section below.

After the mechanics of a cross-chain transfer are understood, an overview of
some supporting transactions are described. This includes transactions to create
sidechains, add signatures, create accounts across chains, and change sidechain
parameters.

The next sections describe the witness server, how to set-up a sidechain, error
handling, and trust assumptions.

Finally, a detailed reference for the new ledger objects and transactions are
presented as a reference.

## Nomenclature

Cross-chain transfer: A transaction that moves assets from the mainchain to the
sidechain, or returns those assets from the sidechain to the mainchain

Cross-chain sequence number: A ledger object used to prove ownership of the
funds moved in a cross-chain transfer.

Door accounts: The account on the mainchain that is used to put assets into
trust, or the account on the sidechain used to issue wrapped assets. The name
comes from the idea that a door is used to move from one room to another and a
door account is used to move from one chain to another.

Mainchain: Where the assets originate and are put into trust.

Sidechain: Where the assets from the mainchain are wrapped.

Witness server: A server that listens for transactions on the main and side
chains and signs attestations used to prove that certain events happened on each
chain.

## List of new transactions

SidechainCreate
SidechainXChainSeqNumCreate
SidechainXChainTransfer
SidechainXChainClaim
SidechainXChainAccountCreate
SidechainAddWitness
SidechainModify

## List of new ledger objects

Sidechain
CrosschainSeqNum
XChainAccountCreateState
XChainTransferRewardState

## Cross-chain transfers overview

A cross-chain transfer moves assets from the mainchain to the sidechain or
returns those assets from the sidechain back to the mainchain. Cross-chain
transfers need a couple primitives:

1) Put assets into trust on the mainchain.

2) Issue wrapped assets on the sidechain.

3) Return or destroy the wrapped assets on the sidechain.

4) On the sidechain, prove that assets were put into trust on the mainchain.

5) On the mainchain, prove that assets were returned or destroyed.

6) A way to prevent assets from being wrapped multiple times (prevent
transaction replay). The proofs that certain events happened on the different
chains are public and can be submitted multiple times. This can only wrap (or
unlock) assets once.

In this implementation, a regular XRP ledger account is used to put assets into
trust on the mainchain, and a regular XRP ledger account is used to issue assets
on the sidechain. These accounts will have their master keys disabled and funds
will move from these accounts through a new set of transactions specifically
meant for cross-chain transfers. A special set of "witness servers" are used to
prove that assets were put into trust on the mainchain or returned on the
sidechain. A new ledger object called a "cross-chain sequence number" is used to
prevent transaction replay.

A source chain is the chain where the cross-chain transfer start - by either
putting assets into trust on the mainchain or returning wrapped assets on the
sidechain. The steps for moving funds from a source chain to a destination chain
are:

1) On the destination chain, an account submits a transaction that adds a ledger
   object that will be used to identify the initiating transaction and prevent
   the initiating transaction from being claimed on the destination chain more
   than once. The door account will keep a new ledger object - a sidechain. This
   sidechain ledger object will keep a counter that is used for "cross chain
   sequence numbers". A "cross chain sequence number" will be checked out from
   this counter and the counter will be incremented. Once checked out, the
   sequence number would be owned by the account that submitted the transaction.
   See the section below for what fields are present in the new sidechain ledger
   object and cross chain sequence number ledger object. The actual number must
   be retrieved from the transaction metadata on a validated ledger.
   
2) On the source chain, an initiating transaction is sent from a source account.
   This transaction will include the sidechain, "cross chain sequence number"
   from step (1), a signature reward amount, in XRP, and an optional destination
   account on the destination chain. Rewards amounts much match the amount
   specified by the sidechain ledger object. Both the asset being transferred
   cross-chain and the reward amount will be transferred from the source account
   to the door account. Collecting rewards is discussed below. This transaction
   will create a `XChainTransferRewardState` ledger object for this transaction.
   It will be owned by the door account.
   
3) When a witness servers sees a new cross-chain transaction, it submits a
   transaction on the destination chain that adds a signature witnessing the
   cross-chain transaction. This will include the amount being transferred
   cross-chain, the reward amount, the account to send the reward to, and the
   optional destination account. These signatures will be accumulated on the
   cross-chain sequence number object. The keys used in these signatures must
   match the keys on the multi-signers list on the door account.

4) When a quorum of signatures have been collected, the cross-chain funds can be
   claimed on the destination chain. If a destination account is specified, the
   funds will move when the transaction that adds the last signature is executed
   (the signature that made the quorum). On success, the cross-chain sequence
   number object is removed. If there is an error (for example, the destination
   has deposit auth set) or the optional destination account was not specified,
   a "cross-chain claim" transaction must be use. Note, if the signers list on
   the door account changes while the signatures are collected, the signers list
   at the time the quorum is reached is controlling. When the quorum is reach,
   the signing keys will be checked against the current signers list, and if a
   collected signature's key is no longer on that list it is removed and
   signatures will continue to be collected.
   
5) On the destination chain, the the owner of the "cross chain sequence number"
   (see 1) can submit a "cross chain claim" transaction that includes the "cross
   chain sequence number", the sidechain, and the destination. The "cross chain
   sequence number" object must exist and must have already collected enough
   signatures from the witness servers for this to succeed. On success, a
   payment is made from the door account to the destination and the "cross chain
   sequence number" is deleted. A "cross chain claim" transaction can only
   succeed once, as the "cross chain sequence number" for that transaction can
   only be created once. In case of error, the funds can be sent to an alternate
   account and eventually returned to the initiating account. Note that this
   transaction is only used if the optional destination account is not specified
   in step (2) or there is an error when sending funds to that destination
   account.

6) When a witness server sees funds being claimed on the destination chain, it
   submits a transaction on the source chain allowing the signature rewards to
   be collected. It collects these signatures on the `XChainTransferRewardState`
   created in step (2). The number of rewards will always equal the number of
   signatures needed to reach a quorum. When a quorum of these signatures have
   been collected, the rewards will be transferred to the destination addresses
   specified in the signatures. (Note: the witness servers specify where the rewards
   for its signature goes, this is not specified on the sidechain ledger
   object). The `XChainTransferRewardState` is destroyed when the reward pool is
   distributed.
   
The cross-chain transfer is now complete. Note that the transactions sent by the
witness servers that add their signatures may send the signatures in a batch.

## Supporting transactions overview

In addition to the transactions used in a cross-chain transfer (described
above), there are new transactions for creating a sidechain, changing sidechain
parameters, and for using a cross-chain transfer to create a new account on the
destination chain.

The `SidechainCreate` transaction adds a sidechain ledger object to the account.
This contains the two door accounts, the asset type that will be put into trust
on the mainchain, the wrapped asset type on the sidechain, and the reward amount
in XRP per signature. Optional, the minimum amount of XRP needed for an account
create transaction for the mainchain, the minimum amount of XRP needed for an
account create transaction for the sidechain may be specified. Currently, this
ledger object can never be deleted (tho this my change) and adding this ledger
object means the signatures specified in this object may move funds from this
account. If this amount is not specified, the `SidechainXChainAccountCreate`
(see below) will be disabled for this sidechain.

A cross-chain transfer, as described in the section above, requires an account
on the destination chain to checkout a "cross-chain sequence number". This makes
it difficult to create new accounts using cross-chain transfers. A dedicated
transaction is used to create accounts: `SidechainXChainAccountCreate`. This
specifies the same information as a `SidechainXChainTransfer`, but the
destination account is not longer optional, and the amount is in XRP. The XRP
amount must be greater than or equal to the min creation amount specified in the
sidechain ledger object. If this optional amount is not present, the transaction
will fail. Once this transaction is submitted, it works similarly to a
cross-chain transfer, except the signatures are collected on a
`XChainAccountCreateState` ledger object on the door account. If the
account already exists, the transaction will fail. Accounts created this must be
undeletable or the the signatures could be resubmitted to collect the funds more
than once.

The `SidechainAddWitness` transaction is used to by witness servers (or accounts
that use witness servers) to add a witness's attestation that some event
happened.

There is also a `SidechainModify` transaction used to change new account
creation amounts and the reward amounts. Note: if the reward amount changes
between the time a transaction is initiated the time the reward is collected,
the old amount is used (as that is the amount the source account paid).


## Witness Server

A witness server is an independent server that helps provide proof that some
event happened on either the mainchain or the sidechain. When they detect an
event of interest, they use the `SidechainAddWitness` transaction to add their
attestation that the event happened. When a quorum on signatures are collected on
the ledger, the transaction predicated on that event happening is unlocked (and
may be triggered automatically when the quorum of signatures is reached).
Witness servers are independent from the servers that run the chains themselves.

It is possible for a witness server to provide attestations for one chain only -
and it is possible for the door account on the mainchain to have a different
signer's list than the door account on the sidechain. The initial implementation
of the witness server assumes it is providing attestation for both chains,
however it is desirable to allow witness servers that only know about one of
the chains.

The current design envisions two models for how witness servers are used. In the
first model, the servers are completely private. They submit transactions to the
chains themselves and collect the rewards themselves. Allowing the servers to be
private had the advantage of greatly reducing the attack surface on these
servers. They won't have to deal with adversarial input to their RPC commands,
and since their ip address will be unknown, it will be hard to mount an DOS
attack.

In the second model, the witness server monitors events on a chain, but does not
submit their signatures themselves. Instead, another party will pay the
attestation server for their signature (for example, through a subscription
fee), and the attestation server allows that party to collect the signer's
reward. The account that the signer's reward goes to is part of the message that
the witness server signs. In this model, it is likely that the witness server
only listens to events on one chain. This model allows for a single witness
server to act as a witness for multiple sidechains.

As a side note, since submitting a signature requires submitting a transaction
and paying a fee, supporting rewards for signatures is an important requirement.
Of course, the reward can be higher than the fee, providing an incentive to
running a witness server.

## Why use the signer's list on the account

The signatures that the witness servers use must match the signatures on that
door's signer's list. But this isn't the only way to implement this. A sidechain
ledger object could contain a signer's list that's independent from the door
account. The reasons for using the door account signer's list are:

1) The sidechain signer's list can be used to move funds from the account.
   Putting this list on the door account emphasizes this trust model.
   
2) It allows for emergency action. If something goes very, very wrong, funds
   could still be moved if the entities on the signer's list sign a regular
   transaction.

3) If the door account has multiple sidechains, a strange use-case, but one that
   is currently supported. If the sidechain share a common asset type, the trust
   model is the union of the signer's list. Keeping the signer's list on the
   account makes this explicit.

4) It's a more natural way to modify sidechain parameters.

## Setting up a sidechain

Setting up a sidechain requires the following:

1) Create a new account for the door account (or use the root account - this can
   be useful for sidechains). Note, that while more than one sidechain on a door account
   is supported, is it discouraged.

2) If necessary, setup the trust lines needed for IOUs.

3) Create the sidechain ledger object on each door account.

4) Enable multi-signatures on the two door accounts. These keys much match the
   keys used by the witness servers. Note that the two door accounts may have
   different multi-signature lists.

5) Disable the master key, so only the keys on the multi-signature list can
   control the account.

## Distributing Signature Rewards

When funds are claimed on the destination chain, signatures will be collected on
the source chain so the signature rewards can be distributed. These rewards will
be distributed equally between the "reward accounts" for the attestations that
provided the quorum of signatures on the destination chain that unlocked the
claim, and the "reward accounts" for the attestations that provided the quorum
of signatures on the source chain that unlocked the reward pool. If the reward
amount is not evenly dividable among st the signers, the Mainer is kept by the
door account.

## Preventing Transaction Replay

Normally, sequence number prevent transaction replay in the XRP ledger. However,
this sidechain design allows moving funds from an account from transactions not
sent by that account. All the information to replay these transactions are
publicly available. This section describes how the different transaction
prevent certain attacks - including transaction replay attacks.

To successfully run a `SidechainXChainClaim` transaction, the account sending
the transaction must own the `CrossChainSeqNum` ledger object referenced in the
witness server's attestation. Since this sequence number is destroyed when the
funds are successfully moved, the transaction cannot be replayed.

To successfully add witnesses to a `XChainTransferRewardState` and claim the
reward pool, the reward state ledger object for that transaction must already
exist. Since that ledger object is destroyed when the reward pool is claimed,
the transaction cannot be replayed.

To successfully create an account with the `SidechainXChainAccountCreate`
transaction, the account to be created must not already exist. If the account
were deletable, this opens up an attack where the account could be created
multiple times. To prevent this, accounts created with
`SidechainXChainAccountCreate` are not deletable. Since the account is not
deletable, the transaction cannot be replayed.

Since the `SidechainXChainTransfer` can contain an optional destination account
on the destination chain, and the funds will move when the destination chain
collects enough signatures, on attack would be for an account to watch for a
`SidechainXChainTransfer` to be sent and then send their own
`SidechainXChainTransfer` for a smaller amount. This attack doesn't steal funds,
but it does result in the original sender losing their funds. To prevent this,
when a `CrossChainSeqNum` is created on the destination chain, the account that
will send the `SidechainXChainTransfer` on the source chain must be specified.
Only the witnesses from this transaction will be accepted on the
`CrossChainSeqNum`.

## Error Handling

Error handling cross-chain transfers is straight forward. The "cross-chain
sequence number" is only destroyed when a claim succeeds. If it fails for any
reason - for example the destination account doesn't exist or has deposit auth
set - then an explicit `SidechainXChainClaim` transaction may be submitted to
redirect the funds.

If a cross-chain account create fails, recovering the funds are outside the
rules of the sidechain system. Assume the funds are lost (the only way to
recover them would be if the witness servers created a transaction themselves.
But this is unlikely to happen and should not be relied upon.) The "Minimum
account create" amount is meant to prevent these transactions from failing. If
this transaction is used to try to create an already existing account, the funds
are lost.

## Trust Assumptions

The witness servers are trusted, and if a quorum of them collude they can steal
funds from the door account.

## STSidechain

## Ledger Objects

Many of the ledger objects and transactions contain a `STSidechain` object. These are the parameters that
define a sidechain. It contains the following fields:

* srcChainDoor: `AccountID` of the door account on the mainchain. This account
  will hold assets in trust while they are used on the sidechain.
    
* srcChainIssue: `Issue` of the asset put into trust on the mainchain.

* dstChainDoor: `AccountID` of the door account on the sidechain. This account
  will issue wrapped assets representing assets put into trust on the mainchain.
    
* dstChainIssue: `Issue` of the asset used to represent from the mainchain.

Note: `srcChainDoor` and `dstChainDoor` must be distinct accounts. This is done
    to help prevent transaction replay attacks.

Note: `srcChainIssue` and `dstChainIssue` must both XRP or both be IOUs. This is
    done because the exchange rate is fixed at 1:1, and IOUs and XRP have a
    different numeric range and precision. This requirement may be relaxed in
    the future.


A snippet of the data for C++ class for an `STSidechain` is:

```c++
class STSidechain final : public STBase
{
    AccountID srcChainDoor_{};
    Issue srcChainIssue_{};
    AccountID dstChainDoor_{};
    Issue dstChainIssue_{};
}
```

### Sidechain

The sidechain ledger object is owned by the door account and defines the
sidechain parameters. Note, the signatures used to attest to chain events are on
the door account, not on this ledger object. It is created with a
`SidechainCreate` transaction, modified with a `SidechainModify` transaction
(only the `MinAccountCreateAmount` and `SignaturesReward` may be changed). It
can not be deleted.

#### Fields

The ledger object contains the following fields:

* Account: The account that owns this object. The door account. Required.

* Balance: If positive, the funds in held in trust on this door account. If
  negative, the wrapped funds issued from this door account. Required.

* SignaturesReward: Total amount, in XRP, to be rewarded for providing a
  signatures for a cross-chain transfer or for signing for the cross-chain
  reward. This will be split among the signers. Required.

* SignatureRewardsBalance: Amount, in XRP, currently lock for rewarding signers.
  This is paid by the accounts sending `SidechainXChainTransfer` transactions.
  Required.

* MinAccountCreateAmount: Minimum Amount, in XRP, required for an
  `SidechainXChainAccountCreate` transaction. If this is not present, the
  `SidechainXChainAccountCreate` will fail. Optional

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.

* XChainSequence: A counter used to assign unique cross-chain sequence numbers
  in the `SidechainXChainSeqNumCreate` transaction. Required.

The c++ code for this ledger object format is:
```c++
    add(jss::Sidechain,
        ltSIDECHAIN,
        {
            {sfAccount,                soeREQUIRED},
            {sfBalance,                soeREQUIRED},
            {sfSignaturesRewardBalance,soeREQUIRED},
            {sfSignaturesReward,       soeREQUIRED},
            {sfMinAccountCreateAmount, soeOPTIONAL},
            {sfSidechain,              soeREQUIRED},
            {sfXChainSequence,         soeREQUIRED},
            {sfOwnerNode,              soeREQUIRED},
            {sfPreviousTxnID,          soeREQUIRED},
            {sfPreviousTxnLgrSeq,      soeREQUIRED}
        },
        commonFields);
```

#### Ledger ID

The ledger id is a hash of a unique prefix for sidechain object, and the fields
in `STSidechain`. The C++ code for this is:

```c++
Keylet
sidechain(STSidechain const& sidechain)
{
    return {
        ltSIDECHAIN,
        indexHash(
            LedgerNameSpace::SIDECHAIN,
            sidechain.srcChainDoor(),
            sidechain.srcChainIssue(),
            sidechain.dstChainDoor(),
            sidechain.dstChainIssue())};
}
```

### CrosschainSeqNum

The cross-chain sequence number ledger object must be acquired on the
destination before submitting a `SidechainXChainSeqNumCreate` on the source
chain. A `SidechainXChainSeqNumCreate` transaction is used for this. It's
purpose is to prevent transaction replay attacks and is also used as a place to
collect signatures from witness servers. It is destroyed when the funds are
successfully claimed on the destination chain.

#### Fields

* Account: The account that owns this object. Required.

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.

* XChainSequence: Integer unique sequence number for a cross-chain transfer.
  Required.

* SourceAccount: Account that must send the `SidechainXChainTransfer` on the
  other chain. Required. Since the destination may be specified in the
  `SidechainXChainTransfer` transaction, if the `SourceAccount` wasn't specified
  another account to try to specify a different destination and steal the funds.
  This also allows tracking only a single set of signatures, since we know which
  account will send the `SidechainXChainTransfer` transaction. Required.

* Signatures: Signatures collected from the witness servers. This includes the
  parameters needed to recreate the message that was signed, including the
  amount, optional destination, and reward account for that signature. Required.

The c++ code for this ledger object format is:
```c++
    add(jss::CrosschainSeqNum,
        ltCROSSCHAIN_SEQUENCE_NUMBER,
        {
            {sfAccount,              soeREQUIRED},
            {sfSidechain,            soeREQUIRED},
            {sfXChainSequence,       soeREQUIRED},
            {sfSourceAccount,        soeREQUIRED},
            {sfSignatures,           soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfPreviousTxnID,        soeREQUIRED},
            {sfPreviousTxnLgrSeq,    soeREQUIRED}
        },
        commonFields);
```
#### Ledger ID

The ledger id is a hash of a unique prefix for cross-chain sequence numbers, the
sequence number, and the fields in `STSidechain`. The C++ code for this is:

```c++
Keylet
xChainSeqNum(STSidechain const& sidechain, std::uint32_t seq)
{
    return {
        ltCROSSCHAIN_SEQUENCE_NUMBER,
        indexHash(
            LedgerNameSpace::XCHAIN_SEQ,
            sidechain.srcChainDoor(),
            sidechain.srcChainIssue(),
            sidechain.dstChainDoor(),
            sidechain.dstChainIssue(),
            seq)};
}

```

### XChainAccountCreateState

This ledger object is used to collect signatures for creating an account using a
cross-chain transfer. It is created when an `SidechainAddWitness` transaction
adds a signature attesting to a `XChainAccountCreate` transaction and the
destination account does not already exist.

#### Fields

* Account: Owner of this object. The door account. Required.

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.

* TxnID: The transaction ID of the initiating transaction on the other chain.

* Amount: Amount, in XRP, to be used for account creation.

* Signatures: Signatures collected from the witness servers. This includes the
  parameters needed to recreate the message that was signed, including the
  amount, destination, and reward account for that signature.

TBD: C++ code.

#### LedgerID

The ledger id is a hash of a unique prefix for cross-chain account create
signatures, the sidechain, and the transaction ID.

### XChainTransferRewardState

This ledger object is used to collect signatures for distributing signature
rewards. It will be created on the door account whenever a
`SidechainXChainTransfer` or `SidechainXChainAccountCreate` successfully runs.


#### Fields

* Account: Owner of this object. The door account. Required.

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.

* TxnID: The transaction ID of the initiating transaction on this chain.

* Amount: Amount, in XRP, in the signer's reward pool.

* Signatures: Signatures collected from the witness servers. This includes the
  parameters needed to recreate the message that was signed.

TBD: C++ code.

#### LedgerID

The ledger id is a hash of a unique prefix for cross-chain transfer reward state,
the sidechain, and the transaction ID.

## Transactions

### SidechainCreate

Attach a new sidechain to a door account. Once this is done, the cross-chain
transfer transactions may be used to transfer funds from this account.

#### Fields

The transaction contains the following fields:

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.

* SignaturesReward: Total amount, in XRP, to be rewarded for providing a
  signatures for a cross-chain transfer or for signing for the cross-chain
  reward. This will be split among the signers. Required.

* MinAccountCreateAmount: Minimum Amount, in XRP, required for an
  `SidechainXChainAccountCreate` transaction. If this is not present, the
  `SidechainXChainAccountCreate` will fail. Optional

See notes in the `STSidechain` section and the `Sidechain` ledger object section
for restrictions on these fields (i.e. door account must be unique, assets must
both be XRP or both be IOU).

```c++
    add(jss::SidechainCreate,
        ttSIDECHAIN_CREATE,
        {
            {sfSidechain,              soeREQUIRED},
            {sfSignaturesReward,       soeREQUIRED},
            {sfMinAccountCreateAmount, soeOPTIONAL},
        },
        commonFields);
```


### SidechainXChainSeqNumCreate

The first step in a cross-chain transfer. The sidechain sequence number must be
created on the destination chain before the `SidechainXChainTransfer`
transaction (which must reference this number) can be sent on the source chain.
The account that will send the `SidechainXChainTransfer` on the source chain
must be specified in this transaction (see note on the `SourceAccount` field in
the `CrosschainSeqNum` ledger object for justification). The actual sequence
number must be retrieved from a validated ledger.

#### Fields

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.

* XChainSequence: Integer unique sequence number for a cross-chain transfer. Required.

* SourceAccount: Account that must send the `SidechainXChainTransfer` on the
  other chain. Required. Since the destination may be specified in the
  `SidechainXChainTransfer` transaction, if the `SourceAccount` wasn't specified
  another account to try to specify a different destination and steal the funds.
  This also allows tracking only a single set of signatures, since we know which
  account will send the `SidechainXChainTransfer` transaction. Required.

```c++
    add(jss::SidechainXChainSeqNumCreate,
        ttSIDECHAIN_XCHAIN_SEQNUM_CREATE,
        {
            {sfSidechain,     soeREQUIRED},
            {sfSourceAccount, soeREQUIRED},
        },
        commonFields);
```

### SidechainXChainTransfer

Put assets into trust on the mainchain so they may be wrapped on the sidechain,
or return wrapped assets on the sidechain so they can be unlocked on the
mainchain. The second step in a cross-chain transfer.

#### Fields

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.

* XChainSequence: Integer unique sequence number for a cross-chain transfer.
  Must be acquired on the destination chain and checked from a validated ledger
  before submitting this transaction. If an incorrect sequence number is
  specified, the funds will be lost. Required.
  
* OtherChainDestination: Destination account on the other chain. Must exist on the other
  chain or the transaction will fail. However, if the transaction fails in this
  case, the funds can be recovered with a `SidechainXChainClaim` transaction. Optional.

* SignaturesReward: Amount, in XRP, to be used to reward the witness servers for
  providing signatures. Must match the amount on the sidechain ledger object.
  This could be optional, but it is required so the sender can be made
  positively aware that these funds will be deducted from their account.
  Required.

Note: Only account specified in the `SourceAccount` field of the
`SidechainXChainSeqNumCreate` transaction should send this transaction. If it is
sent from another account the funds will be lost.

```c++
    add(jss::SidechainXChainTransfer,
        ttSIDECHAIN_XCHAIN_TRANSFER,
        {
            {sfSidechain, soeREQUIRED},
            {sfXChainSequence, soeREQUIRED},
            {sfAmount, soeREQUIRED},
            {sfOtherChainDestination, soeOptional},
            {sfSignaturesReward, soeREQUIRED},
        },
        commonFields);
```

### SidechainXChainClaim

Claim funds from a `SidechainXChainTransfer` transaction. This is normally not
needed, but may be used to handle transaction failures or if the destination
account was not specified in the `SidechainXChainTransfer` transaction. It may
only be used after a quorum of signatures have been sent from the witness
servers.

If the transaction succeeds in moving funds, the referenced `CrosschainSeqNum`
ledger object will be destroyed. This prevents transaction replay. If the
transaction fails, the `CrosschainSeqNum` will not be destroyed and the
transaction may be re-run with different parameters.

#### Fields

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.

* XChainSequence: Integer unique sequence number that identifies the claim and
  was referenced in the `SidechainXChainTransfer` transaction.

* Destination: Destination account on this chain. Must exist on the other chain
  or the transaction will fail. However, if the transaction fails in this case,
  the sequence number and collected signatures will not be destroyed and the
  transaction may be rerun with a different destination address.
  
```c++
    add(jss::SidechainXChainAccountClaim,
        ttSIDECHAIN_XCHAIN_ACCOUNT_CLAIM,
        {
            {sfSidechain, soeREQUIRED},
            {sfXChainSequence, soeREQUIRED},
            {sfDestination, soeREQUIRED},
            {sfAmount, soeOPTIONAL},
        },
        commonFields);
```

### SidechainXChainAccountCreate

This is a special transaction used for creating accounts through a cross-chain
transfer. A normal cross-chain transfer requires a cross-chain sequence number
(which requires an existing account on the destination chain). One purpose of
the cross-chain sequence number is to prevent transaction replay. For this
transaction, the existence or non-existence of the destination account plays
this role. If the account exists, the transaction fails. If it doesn't exist, it
is allowed to try to move funds.

Note: If this account already exists, the transaction fails. This means only
amounts close to the minimum required for account creation should be used or a
malicious account can create the account for a smaller amount, causing funds to
be lost (of course, the malicious account would lose funds of their own, but
that wouldn't necessarily prevent the attack).

#### Fields

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.

* OtherChainDestination: Destination account on the other chain. Must not exist
  on the other chain or the transaction will fail. If the transaction fails in
  this case the funds are lost. Required.
  
* Amount: Amount, in XRP, to use for account creation. Must be greater than or
  equal to the amount specified in the sidechain ledger object. Required.
  
* SignaturesReward: Amount, in XRP, to be used to reward the witness servers for
  providing signatures. Must match the amount on the sidechain ledger object.
  This could be optional, but it is required so the sender can be made
  positively aware that these funds will be deducted from their account.
  Required.

```c++

    add(jss::SidechainXChainAccountCreate,
        ttSIDECHAIN_XCHAIN_ACCOUNT_CREATE,
        {
            {sfSidechain, soeREQUIRED},
            {sfOtherChainDestination, soeREQUIRED},
            {sfAmount, soeREQUIRED},
            {sfSignaturesReward, soeREQUIRED},
        },
        commonFields);
```

### SidechainAddWitness

Provide a signature attesting to some event on the other chain. The signatures
must be from one of the keys on the door's signer's list at the time the
signature was provided. However, if the signature list changes between the time
the signature was submitted and the quorum is reached, the new signature set is
used and some of the currently collected signatures may be removed. Also note
the reward is only sent to accounts that have keys on the current list.

To help with transaction throughput and to minimize fees, signatures can be
submitted in batch. If any signature is added, the transaction succeeds. The
metadata must be used to find out which specific signatures succeeded.

Note that any account can submit signatures. This is important to support
witness servers that work on the "subscription" model.

An attestation bears witness to a particular event on the other chain. It contains:

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.
* RewardAccount: Account to send this signer's share of the signer's reward. Required.
* SignatureReward: Signature reward for this event (may be different from the
  current signature reward on the ledger object, as this may have changed). 
* SigningKey: Public key used to verify the signature.
* Signature: Signature bearing witness to the event on the other chain.
* Event type: Type of event (XChainAccountCreate, XChainTransfer, SignatureProvided)
* Event data: Data needed to recreate the message signed by the witness servers.
  This will include the `SignatureReward` for this event (may be different from
  the current signature reward on this ledger object, as this may be updated).
  The other data depends on event. TBD.
  
Note a quorum of signers need to agree on the `SignatureReward`, the same way
they need to agree on the other data. A single witness server cannot provide an
incorrect value for this in an attempt to collect a larger reward.

To add a witness to a `XChainRewardState`, that ledger object must already exist.
To add a witness to a `CrosschainSeqNum`, that ledger object must already exist.
To add a witness to a `SidechainAccountCreateState`, the account to be created must
no exist. If the `SidechainAccountCreateState` does not already exist it will be created.

#### Fields

* Attestations. A collection of attestations, witnessing a particular event on
  the other chain. This collection may be contain attestations for different
  sidechain ledger objects. See section above for fields in an attestation.

### SidechainModify

Change the `SignaturesReward` field or the `MinAccountCreateAmount` on the
sidechain object. Note that this is a regular transaction that is sent by the
door account and requires the entities that control the witness servers to
co-operate and provide the signatures for this transaction. This happens outside
the ledger.

The `SignaturesReward` and `MinAccountCreateAmount` of a transaction are the
values that were in effect at the time the transaction was submitted.

Note that the signer's list for the sidechain is not modified through this
transaction. The signer's list is on the door account itself and is changed in
the same way signer's lists are changed on accounts.

#### Fields

The transaction contains the following fields:

* Sidechain: Door accounts and assets. See `STSidechain` above. Required.

* SignaturesReward: Total amount, in XRP, to be rewarded for providing a
  signatures for a cross-chain transfer or for signing for the cross-chain
  reward. This will be split among the signers. Optional.

* MinAccountCreateAmount: Minimum Amount, in XRP, required for an
  `SidechainXChainAccountCreate` transaction. If this is zero, the
  field will be removed from the ledger object. Optional
  
At least one of `SignaturesReward` and `MinAccountCreateAmount` must be present.

```c++
    add(jss::SidechainModify,
        ttSIDECHAIN_MODIFY,
        {
            {sfSidechain,              soeREQUIRED},
            {sfSignaturesReward,       soeOPTIONAL},
            {sfMinAccountCreateAmount, soeOPTIONAL},
        },
        commonFields);
```


## New RPC Commands

### Sidechain transaction history

Subscribe to transactions that effect a sidechain object, similar to subscribing
to transaction history. This would return any transaction that changes the state
of the sidechain ledger object, including:

* New sequence number checked-out
* Funds put into trust
* Funds issued
* Reward collected
* Min create fee changed

Since the `SidechainAddWitness` command can batch signatures, batching all the
signatures from a single ledger is a natural batch size. To do this, the witness
server would need to know when it has collected all the transactions from a
ledger. It would be good to add a field that either tells us how many of these
transactions are in this ledger or a field that tells us when the transaction we
receive is the last to be sent for this ledger.

### Other RPC Commands 

* Given a transaction id, get the sequence number
* Given a sidechain description, get the sidechain ledger object

## Alternate designs

One alternate design that was implemented was a set of servers similar to the
witness servers called "federators". These servers communicated among
themselves, collected signatures needed to submit transactions on behalf of the
door accounts directly, and submitted those transactions. This design is
attractive from a usability point of view. There is not "cross-chain sequence
number" that needs to be obtained on the destination chain, and creating
accounts using cross-chain transfers is straight forward. The disadvantages of
this design are the complexity in the federators. In order to submit a
transaction, the federators needed to agree on a transaction's sequence number
and fees. This required the federators to stay in "sync" with each other and
required that these federators be much more complex than the "witness" servers
proposed here. In addition, handing fee escalation, failed transactions, and
servers falling behind was much more complex. Finally, because all the
transactions were submitted from the same account (the door account) this
presented a challenge for transaction throughput as the XRP ledger limits the
number of transactions an account can submit in a single ledger.

Another minor variation on the current design involves how signature rewards are
collected. It would be nice if the rewards could be distributed on the
destination chain. However, this is challenging, since the funds for rewards are
collected on the source chain. A design was considered where the funds were
distributed on the destination chain in the form of a token that could be
redeemed on the source chain for reward funds held in trust. However, the
current scheme seems much simpler.

## Tasks (This is not part of the spec - these are just notes for tasks as I write this doc)

* Decide on nomenclature
* Create undeleteable accounts
* Modify sidechain object to contain the reward amounts
* Modify sidechain object to track rewards in trust and assets in trust
* RPC command to query sidechain object params
* RPC command to subscribe to transactions that change the sidechain object.
* Fix bug where sidechain is not present in RPC command results
* Add a reward to the initiating transaction
* Add an optional side-chain destination on the initiating transaction
* Collect signatures on the cross-chain sequence number.
* Automatically move funds when quorum threshold is reached.
* Batch send the signatures
* Add minimum account create amount to sidechain object (optional)
* Add SidechainModify transaction
