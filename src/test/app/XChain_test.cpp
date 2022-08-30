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

#include <ripple/beast/unit_test/suite.hpp>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Issue.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/XChainAttestations.h>

#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <test/jtx/attester.h>
#include <test/jtx/multisign.h>
#include <test/jtx/xchain_bridge.h>

#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <fstream>
#include <iostream>

namespace ripple::test {

template <class T>
struct xEnv : public jtx::XChainBridgeObjects
{
    jtx::Env env_;

    xEnv(T& s, bool side = false)
        : env_(s, jtx::envconfig(jtx::port_increment, side ? 3 : 0), features)
    {
        using namespace jtx;
        STAmount xrp_funds{XRP(10000)};

        if (!side)
        {
            env_.fund(xrp_funds, mcDoor, mcAlice, mcBob, mcGw);

            // Signer's list must match the attestation signers
            // env_(jtx::signers(mcDoor, signers.size(), signers));
        }
        else
        {
            env_.fund(
                xrp_funds, scDoor, scAlice, scBob, scGw, scAttester, scReward);

            for (auto& ra : payees)
                env_.fund(xrp_funds, ra);

            // Signer's list must match the attestation signers
            // env_(jtx::signers(Account::master, signers.size(), signers));
        }
        env_.close();
    }

    xEnv&
    close()
    {
        env_.close();
        return *this;
    }

    template <class Arg, class... Args>
    xEnv&
    fund(STAmount const& amount, Arg const& arg, Args const&... args)
    {
        env_.fund(amount, arg, args...);
        return *this;
    }

    template <class JsonValue, class... FN>
    xEnv&
    tx(JsonValue&& jv, FN const&... fN)
    {
        env_(std::forward<JsonValue>(jv), fN...);
        return *this;
    }

    STAmount
    balance(jtx::Account const& account) const
    {
        return env_.balance(account);
    }
};

template <class T>
struct Balance
{
    jtx::Account const& account_;
    T& env_;
    STAmount startAmount;

    Balance(T& env, jtx::Account const& account) : account_(account), env_(env)
    {
        startAmount = env_.balance(account_).value();
    }

    STAmount
    diff() const
    {
        return env_.balance(account_).value() - startAmount;
    }
};

template <class T>
struct BalanceTransfer
{
    using balance = Balance<T>;

    balance from_;
    balance to_;
    balance payor_;                        // pays the rewards
    std::vector<balance> reward_accounts;  // receives the reward
    XRPAmount txFees_;

    BalanceTransfer(
        T& env,
        jtx::Account const& from_acct,
        jtx::Account const& to_acct,
        jtx::Account const& payor,
        std::vector<jtx::Account> const& payees,
        bool withClaim)
        : from_(env, from_acct)
        , to_(env, to_acct)
        , payor_(env, payor)
        , reward_accounts([&]() {
            std::vector<balance> r;
            r.reserve(payees.size());
            for (auto& ra : payees)
                r.emplace_back(env, ra);
            return r;
        }())
        , txFees_(withClaim ? env.env_.current()->fees().base : XRPAmount(0))
    {
    }

    bool
    check_most_balances(STAmount const& amt, STAmount const& reward)
    {
        return from_.diff() == -amt && to_.diff() == amt &&
            std::all_of(
                   reward_accounts.begin(),
                   reward_accounts.end(),
                   [&](const balance& b) { return b.diff() == reward; });
    }

    bool
    has_happened(
        STAmount const& amt,
        STAmount const& reward,
        bool check_payer = true)
    {
        auto reward_cost =
            multiply(reward, STAmount(reward_accounts.size()), reward.issue());
        return check_most_balances(amt, reward) &&
            (!check_payer || payor_.diff() == -(reward_cost + txFees_));
    }

    bool
    has_not_happened()
    {
        return check_most_balances(STAmount(0), STAmount(0)) &&
            payor_.diff() <= txFees_;  // could have paid fee for failed claim
    }
};

struct XChain_test : public beast::unit_test::suite,
                     public jtx::XChainBridgeObjects
{
    XRPAmount
    reserve(std::uint32_t count)
    {
        return xEnv(*this).env_.current()->fees().accountReserve(count);
    }

    XRPAmount
    txFee()
    {
        return xEnv(*this).env_.current()->fees().base;
    }

    void
    testBridgeCreate()
    {
        XRPAmount res1 = reserve(1);

        using namespace jtx;
        testcase("Bridge Create");

        // Bridge not owned by one of the door account.
        xEnv(*this).tx(create_bridge(mcBob), ter(temSIDECHAIN_NONDOOR_OWNER));

        // Create twice on the same account
        xEnv(*this)
            .tx(create_bridge(mcDoor))
            .close()
            .tx(create_bridge(mcDoor), ter(tecDUPLICATE));

        // Create USD bridge Alice -> Bob ... should succeed
        xEnv(*this).tx(
            create_bridge(
                mcAlice, bridge(mcAlice, mcAlice["USD"], mcBob, mcBob["USD"])),
            ter(tesSUCCESS));

        // Create where both door accounts are on the same chain. The second
        // bridge create should fail.
        xEnv(*this)
            .tx(create_bridge(
                mcAlice, bridge(mcAlice, mcAlice["USD"], mcBob, mcBob["USD"])))
            .close()
            .tx(create_bridge(
                    mcBob,
                    bridge(mcAlice, mcAlice["USD"], mcBob, mcBob["USD"])),
                ter(tecDUPLICATE));

        // Bridge where the two door accounts are equal.
        xEnv(*this).tx(
            create_bridge(
                mcBob, bridge(mcBob, mcBob["USD"], mcBob, mcBob["USD"])),
            ter(temEQUAL_DOOR_ACCOUNTS));

        // Create an bridge on an account with exactly enough balance to
        // meet the new reserve should succeed
        xEnv(*this)
            .fund(res1, mcuDoor)  // exact reserve for account + 1 object
            .close()
            .tx(create_bridge(mcuDoor, jvub), ter(tesSUCCESS));

        // Create an bridge on an account with no enough balance to meet the
        // new reserve
        xEnv(*this)
            .fund(res1 - 1, mcuDoor)  // just short of required reserve
            .close()
            .tx(create_bridge(mcuDoor, jvub), ter(tecINSUFFICIENT_RESERVE));

        // Reward amount is non-xrp
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, mcUSD(1)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Reward amount is XRP and negative
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(-1)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Reward amount is zero
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(0)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Reward amount is 1 xrp => should succeed
        xEnv(*this).tx(create_bridge(mcDoor, jvb, XRP(1)), ter(tesSUCCESS));

        // Min create amount is 1 xrp, mincreate is 1 xrp => should succeed
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(1), XRP(1)), ter(tesSUCCESS));

        // Min create amount is non-xrp
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(1), mcUSD(100)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));

        // Min create amount is zero (should fail, currently succeeds)
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(1), XRP(0)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));

        // Min create amount is negative
        xEnv(*this).tx(
            create_bridge(mcDoor, jvb, XRP(1), XRP(-1)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));
    }

    void
    testBridgeCreateMatrix()
    {
        using namespace jtx;
        testcase("Bridge Create Matrix");

        // Test all combinations of the following:`
        // --------------------------------------
        // - Locking chain is IOU with locking chain door account as issuer
        // - Locking chain is IOU with issuing chain door account that
        //   exists on the locking chain as issuer
        // - Locking chain is IOU with issuing chain door account that does
        //   not exists on the locking chain as issuer
        // - Locking chain is IOU with non-door account (that exists on the
        //   locking chain ledger) as issuer
        // - Locking chain is IOU with non-door account (that does not exist
        //   exists on the locking chain ledger) as issuer
        // - Locking chain is XRP
        // ---------------------------------------------------------------------
        // - Issuing chain is IOU with issuing chain door account as the
        //   issuer
        // - Issuing chain is IOU with locking chain door account (that
        //   exists on the issuing chain ledger) as the issuer
        // - Issuing chain is IOU with locking chain door account (that does
        //   not exist on the issuing chain ledger) as the issuer
        // - Issuing chain is IOU with non-door account (that exists on the
        //   issuing chain ledger) as the issuer
        // - Issuing chain is IOU with non-door account (that does not
        //   exists on the issuing chain ledger) as the issuer
        // - Issuing chain is XRP and issuing chain door account is not the
        //   root account
        // - Issuing chain is XRP and issuing chain door account is the root
        //   account
        // ---------------------------------------------------------------------
        // That's 42 combinations. The only combinations that should succeed
        // are:
        // - Locking chain is any IOU,
        // - Issuing chain is IOU with issuing chain door account as the issuer
        //   Locking chain is XRP,
        // - Issuing chain is XRP with issuing chain is the root account.
        // ---------------------------------------------------------------------
        Account a, b;
        Issue ia, ib;

        std::tuple lcs{
            std::make_pair(
                "Locking chain is IOU(locking chain door)",
                [&](auto& env, bool) {
                    a = mcDoor;
                    ia = mcDoor["USD"];
                }),
            std::make_pair(
                "Locking chain is IOU(issuing chain door funded on locking "
                "chain)",
                [&](auto& env, bool shouldFund) {
                    a = mcDoor;
                    ia = scDoor["USD"];
                    if (shouldFund)
                        env.fund(XRP(10000), scDoor);
                }),
            std::make_pair(
                "Locking chain is IOU(issuing chain door account unfunded "
                "on locking chain)",
                [&](auto& env, bool) {
                    a = mcDoor;
                    ia = scDoor["USD"];
                }),
            std::make_pair(
                "Locking chain is IOU(bob funded on locking chain)",
                [&](auto& env, bool) {
                    a = mcDoor;
                    ia = mcGw["USD"];
                }),
            std::make_pair(
                "Locking chain is IOU(bob unfunded on locking chain)",
                [&](auto& env, bool) {
                    a = mcDoor;
                    ia = mcuGw["USD"];
                }),
            std::make_pair("Locking chain is XRP", [&](auto& env, bool) {
                a = mcDoor;
                ia = xrpIssue();
            })};

        std::tuple ics{
            std::make_pair(
                "Issuing chain is IOU(issuing chain door account)",
                [&](auto& env, bool) {
                    b = scDoor;
                    ib = scDoor["USD"];
                }),
            std::make_pair(
                "Issuing chain is IOU(locking chain door funded on issuing "
                "chain)",
                [&](auto& env, bool shouldFund) {
                    b = scDoor;
                    ib = mcDoor["USD"];
                    if (shouldFund)
                        env.fund(XRP(10000), mcDoor);
                }),
            std::make_pair(
                "Issuing chain is IOU(locking chain door unfunded on "
                "issuing chain)",
                [&](auto& env, bool) {
                    b = scDoor;
                    ib = mcDoor["USD"];
                }),
            std::make_pair(
                "Issuing chain is IOU(bob funded on issuing chain)",
                [&](auto& env, bool) {
                    b = scDoor;
                    ib = mcGw["USD"];
                }),
            std::make_pair(
                "Issuing chain is IOU(bob unfunded on issuing chain)",
                [&](auto& env, bool) {
                    b = scDoor;
                    ib = mcuGw["USD"];
                }),
            std::make_pair(
                "Issuing chain is XRP and issuing chain door account is "
                "not the root account",
                [&](auto& env, bool) {
                    b = scDoor;
                    ib = xrpIssue();
                }),
            std::make_pair(
                "Issuing chain is XRP and issuing chain door account is "
                "the root account ",
                [&](auto& env, bool) {
                    b = Account::master;
                    ib = xrpIssue();
                })};

        std::vector<std::pair<int, int>> expected_result{
            {tesSUCCESS, tesSUCCESS},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {tesSUCCESS, tesSUCCESS},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {tecNO_ISSUER, tesSUCCESS},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {tesSUCCESS, tesSUCCESS},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {tecNO_ISSUER, tesSUCCESS},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {temSIDECHAIN_BAD_ISSUES, temSIDECHAIN_BAD_ISSUES},
            {tesSUCCESS, tesSUCCESS}};

        std::vector<std::tuple<TER, TER, bool>> test_result;

        auto testcase = [&](auto const& lc, auto const& ic) {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            lc.second(mcEnv, true);
            lc.second(scEnv, false);

            ic.second(mcEnv, false);
            ic.second(scEnv, true);

            auto const& expected = expected_result[test_result.size()];

            mcEnv.tx(
                create_bridge(a, bridge(a, ia, b, ib)),
                ter(TER::fromInt(expected.first)));
            TER mcTER = mcEnv.env_.ter();

            scEnv.tx(
                create_bridge(b, bridge(a, ia, b, ib)),
                ter(TER::fromInt(expected.second)));
            TER scTER = scEnv.env_.ter();

            bool pass = mcTER == tesSUCCESS && scTER == tesSUCCESS;

            test_result.emplace_back(mcTER, scTER, pass);
        };

        auto apply_ics = [&](auto const& lc, auto const& ics) {
            std::apply(
                [&](auto const&... ic) { (testcase(lc, ic), ...); }, ics);
        };

        std::apply([&](auto const&... lc) { (apply_ics(lc, ics), ...); }, lcs);

#if GENERATE_MTX_OUTPUT
        // optional output of matrix results in markdown format
        // ----------------------------------------------------
        std::string fname{std::tmpnam(nullptr)};
        fname += ".md";
        std::cout << "Markdown output for matrix test: " << fname << "\n";

        auto print_res = [](auto tup) -> std::string {
            std::string status = std::string(transToken(std::get<0>(tup))) +
                " / " + transToken(std::get<1>(tup));

            if (std::get<2>(tup))
                return status;
            else
            {
                // red
                return std::string("`") + status + "`";
            }
        };

        auto output_table = [&](auto print_res) {
            size_t test_idx = 0;
            std::string res;
            res.reserve(10000);  // should be enough :-)

            // first two header lines
            res += "|  `issuing ->` | ";
            std::apply(
                [&](auto const&... ic) {
                    ((res += ic.first, res += " | "), ...);
                },
                ics);
            res += "\n";

            res += "| :--- | ";
            std::apply(
                [&](auto const&... ic) {
                    (((void)ic.first, res += ":---: |  "), ...);
                },
                ics);
            res += "\n";

            auto output = [&](auto const& lc, auto const& ic) {
                res += print_res(test_result[test_idx]);
                res += " | ";
                ++test_idx;
            };

            auto output_ics = [&](auto const& lc, auto const& ics) {
                res += "| ";
                res += lc.first;
                res += " | ";
                std::apply(
                    [&](auto const&... ic) { (output(lc, ic), ...); }, ics);
                res += "\n";
            };

            std::apply(
                [&](auto const&... lc) { (output_ics(lc, ics), ...); }, lcs);

            return res;
        };

        std::ofstream(fname) << output_table(print_res);

        std::string ter_fname{std::tmpnam(nullptr)};
        std::cout << "ter output for matrix test: " << ter_fname << "\n";

        std::ofstream ofs(ter_fname);
        for (auto& t : test_result)
        {
            ofs << "{ " << std::string(transToken(std::get<0>(t))) << ", "
                << std::string(transToken(std::get<1>(t))) << "}\n,";
        }
#endif
    }

    void
    testBridgeModify()
    {
        using namespace jtx;
        testcase("Bridge Modify");

        // Changing a non-existent bridge should fail
        xEnv(*this).tx(
            bridge_modify(
                mcAlice,
                bridge(mcAlice, mcAlice["USD"], mcBob, mcBob["USD"]),
                XRP(2),
                XRP(10)),
            ter(tecNO_ENTRY));

        // must change something
        // xEnv(*this)
        //    .tx(create_bridge(mcDoor, jvb, XRP(1), XRP(1)))
        //    .tx(bridge_modify(mcDoor, jvb, XRP(1), XRP(1)),
        //    ter(temMALFORMED));

        // must change something
        xEnv(*this)
            .tx(create_bridge(mcDoor, jvb, XRP(1), XRP(1)))
            .close()
            .tx(bridge_modify(mcDoor, jvb, {}, {}), ter(temMALFORMED));

        // Reward amount is non-xrp
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, mcUSD(2), XRP(10)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Reward amount is XRP and negative
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, XRP(-2), XRP(10)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Reward amount is zero
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, XRP(0), XRP(10)),
            ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT));

        // Min create amount is non-xrp
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, XRP(2), mcUSD(10)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));

        // Min create amount is zero
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, XRP(2), XRP(0)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));

        // Min create amount is negative
        xEnv(*this).tx(
            bridge_modify(mcDoor, jvb, XRP(2), XRP(-10)),
            ter(temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT));

        // First check the regular claim process (without bridge_modify)
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            Json::Value batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
            }

            BEAST_EXPECT(transfer.has_happened(amt, split_reward));
        }

        // Check that the reward paid from a claim Id was the reward when
        // the claim id was created, not the reward since the bridge was
        // modified.
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            // Now modify the reward on the bridge
            mcEnv.tx(bridge_modify(mcDoor, jvb, XRP(2), XRP(10))).close();
            scEnv.tx(bridge_modify(Account::master, jvb, XRP(2), XRP(10)))
                .close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            Json::Value batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
            }

            // make sure the reward accounts indeed received the original
            // split reward (1 split 5 ways) instead of the updated 2 XRP.
            BEAST_EXPECT(transfer.has_happened(amt, split_reward));
        }

        // Check that the signatures used to verify attestations and decide
        // if there is a quorum are the current signer's list on the door
        // account, not the signer's list that was in effect when the claim
        // id was created.
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            // change signers - claim should not be processed is the batch is
            // signed by original signers
            scEnv
                .tx(jtx::signers(
                    Account::master, alt_signers.size(), alt_signers))
                .close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            // submit claim using outdated signers - should fail
            Json::Value batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv
                .tx(xchain_add_attestation_batch(scAttester, batch),
                    ter(tecXCHAIN_PROOF_UNKNOWN_KEY))
                .close();

            if (withClaim)
            {
                // need to submit a claim transactions
                scEnv
                    .tx(xchain_claim(scAlice, jvb, claimID, amt, scBob),
                        ter(tecXCHAIN_CLAIM_NO_QUORUM))
                    .close();
            }

            // make sure transfer has not happened as we sent attestations using
            // outdated signers
            BEAST_EXPECT(transfer.has_not_happened());

            // submit claim using current signers - should succeed
            batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, alt_signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
            }

            // make sure the transfer went through as we sent attestations using
            // new signers
            BEAST_EXPECT(transfer.has_happened(amt, split_reward, false));
        }
    }

    void
    testBridgeCreateClaimID()
    {
        using namespace jtx;
        XRPAmount res1 = reserve(1);

        testcase("Bridge Create ClaimID");

        // normal bridge create for sanity check with the exact necessary
        // account balance
        xEnv(*this, true)
            .tx(create_bridge(Account::master, jvb))
            .fund(res1, scuAlice)  // acct reserve + 1 object
            .close()
            .tx(xchain_create_claim_id(scuAlice, jvb, reward, mcAlice))
            .close();

        // Non-existent bridge
        xEnv(*this, true)
            .tx(xchain_create_claim_id(
                    scAlice,
                    bridge(mcAlice, mcAlice["USD"], scBob, scBob["USD"]),
                    reward,
                    mcAlice),
                ter(tecNO_ENTRY))
            .close();

        // Creating the new object would put the account below the reserve
        xEnv(*this, true)
            .tx(create_bridge(Account::master, jvb))
            .fund(res1 - xrp_dust, scuAlice)  // barely not enough
            .close()
            .tx(xchain_create_claim_id(scuAlice, jvb, reward, mcAlice),
                ter(tecINSUFFICIENT_RESERVE))
            .close();

        // The specified reward doesn't match the reward on the bridge (test by
        // giving the reward amount for the other side, as well as a completely
        // non-matching reward)
        xEnv(*this, true)
            .tx(create_bridge(Account::master, jvb))
            .close()
            .tx(xchain_create_claim_id(scAlice, jvb, split_reward, mcAlice),
                ter(tecXCHAIN_REWARD_MISMATCH))
            .close();

        // A reward amount that isn't XRP
        xEnv(*this, true)
            .tx(create_bridge(Account::master, jvb))
            .close()
            .tx(xchain_create_claim_id(scAlice, jvb, mcUSD(1), mcAlice),
                ter(temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT))
            .close();
    }

    void
    testBridgeCommit()
    {
        using namespace jtx;
        XRPAmount res0 = reserve(0);

        testcase("Bridge Commit");

        // Commit to a non-existent bridge
        xEnv(*this).tx(
            xchain_commit(mcAlice, jvb, 1, one_xrp, scBob), ter(tecNO_ENTRY));

        // Commit a negative amount
        xEnv(*this)
            .tx(create_bridge(mcDoor, jvb))
            .close()
            .tx(xchain_commit(mcAlice, jvb, 1, XRP(-1), scBob),
                ter(temBAD_AMOUNT));

        // Commit an amount whose issue that does not match the expected issue
        // on the bridge (either LockingChainIssue or IssuingChainIssue,
        // depending on the chain).
        xEnv(*this)
            .tx(create_bridge(mcDoor, jvb))
            .close()
            .tx(xchain_commit(mcAlice, jvb, 1, mcUSD(100), scBob),
                ter(tecBAD_XCHAIN_TRANSFER_ISSUE));

        // Commit an amount that would put the sender below the required reserve
        // (if XRP)
        xEnv(*this)
            .tx(create_bridge(mcDoor, jvb))
            .fund(res0 + one_xrp - xrp_dust, mcuAlice)  // barely not enough
            .close()
            .tx(xchain_commit(mcuAlice, jvb, 1, one_xrp, scBob),
                ter(tecINSUFFICIENT_FUNDS));

        xEnv(*this)
            .tx(create_bridge(mcDoor, jvb))
            .fund(
                res0 + one_xrp +
                    xrp_dust,  // todo: "+ xrp_dust" should not be needed
                mcuAlice)      // exactly enough => should succeed
            .close()
            .tx(xchain_commit(mcuAlice, jvb, 1, one_xrp, scBob));

        // Commit an amount above the account's balance (for both XRP and IOUs)
        xEnv(*this)
            .tx(create_bridge(mcDoor, jvb))
            .fund(res0, mcuAlice)  // barely not enough
            .close()
            .tx(xchain_commit(mcuAlice, jvb, 1, res0 + one_xrp, scBob),
                ter(tecINSUFFICIENT_FUNDS));

        auto jvb_USD = bridge(mcDoor, mcUSD, scGw, scUSD);

        // commit sent from iou issuer (mcGw) succeeds - should it?
        xEnv(*this)
            .tx(trust(mcDoor, mcUSD(10000)))  // door needs to have a trustline
            .tx(create_bridge(mcDoor, jvb_USD))
            .close()
            .tx(xchain_commit(mcGw, jvb_USD, 1, mcUSD(1), scBob));

        // commit to a door account from the door account. This should fail.
        xEnv(*this)
            .tx(trust(mcDoor, mcUSD(10000)))  // door needs to have a trustline
            .tx(create_bridge(mcDoor, jvb_USD))
            .close()
            .tx(xchain_commit(mcDoor, jvb_USD, 1, mcUSD(1), scBob),
                ter(tecXCHAIN_SELF_COMMIT));

        // commit sent from mcAlice which has no IOU balance => should fail
        xEnv(*this)
            .tx(trust(mcDoor, mcUSD(10000)))  // door needs to have a trustline
            .tx(create_bridge(mcDoor, jvb_USD))
            .close()
            .tx(xchain_commit(mcAlice, jvb_USD, 1, mcUSD(1), scBob),
                ter(terNO_LINE));

        // commit sent from mcAlice which has no IOU balance => should fail
        // just changed the destination to scGw (which is the door account and
        // may not make much sense)
        xEnv(*this)
            .tx(trust(mcDoor, mcUSD(10000)))  // door needs to have a trustline
            .tx(create_bridge(mcDoor, jvb_USD))
            .close()
            .tx(xchain_commit(mcAlice, jvb_USD, 1, mcUSD(1), scGw),
                ter(terNO_LINE));

        // commit sent from mcAlice which has a IOU balance => should succeed
        xEnv(*this)
            .tx(trust(mcDoor, mcUSD(10000)))
            .tx(trust(mcAlice, mcUSD(10000)))
            .close()
            .tx(pay(mcGw, mcAlice, mcUSD(10)))
            .tx(create_bridge(mcDoor, jvb_USD))
            .close()
            //.tx(pay(mcAlice, mcDoor, mcUSD(10)));
            .tx(xchain_commit(mcAlice, jvb_USD, 1, mcUSD(10), scAlice));
    }

    void
    testBridgeAddAttestation()
    {
        using namespace jtx;

        testcase("Bridge Add Attestation");

        // ++++++++++++todo

        // Add an attestation to a claim id that has already reached quorum.
        // This should succeed and share in the reward.
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
            }

            BEAST_EXPECT(transfer.has_happened(amt, split_reward));
        }

        // Add a batch of attestations where one has an invalid signature. The
        // entire transaction should fail.

        // Test combinations of the following when adding a batch of
        // attestations for different claim ids: All the claim id exist One
        // claim id exists and other has already been claimed None of the claim
        // ids exist When the claim ids exist, test for both reaching quorum,
        // going over quorum, and not reaching qurorum.

        // Add attestations where some of the attestations are inconsistent with
        // each other. The entire transaction should fail. Being inconsistent
        // means attesting to different values.

        // Test that signature weights are correctly handled. Assign signature
        // weights of 1,2,4,4 and a quorum of 7. Check that the 4,4 signatures
        // reach a quorum, the 1,2,4, reach a quorum, but the 4,2, 4,1 and 1,2
        // do not.

        // Add more than the maximum number of allowed attestations (8). This
        // should fail.

        // Add attestations for both account create and claims.

        // Confirm that account create transactions happen in the correct order.
        // If they reach quorum out of order they should not execute until they
        // reach quorum. Re-adding an attestation should move funds.

        // Check that creating an account with less the minimum reserve fails.

        // Check that sending funds with an account create txn to an existing
        // account works.

        // Check that sending funds to an existing account with deposit auth set
        // fails - for both claim and account create transactions.

        // If an account is unable to pay the reserver, check that it fails.

        // Create several account with a single batch attestation. This should
        // succeed.

        // If an attestation already exists for that server and claim id, the
        // new attestation should replace the old attestation.
    }

    void
    testBridgeClaim()
    {
        using namespace jtx;

        XRPAmount res0 = reserve(0);
        XRPAmount tx_fee = txFee();

        testcase("Bridge Claim");

        // Claim where the amount matches what is attested to, to an account
        // that exists, and there are enough attestations to reach a quorum
        // => should succeed
        // -----------------------------------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
            }

            BEAST_EXPECT(transfer.has_happened(amt, split_reward));
        }

        // Claim against non-existent bridge
        // ---------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            auto jvb_unknown =
                bridge(mcBob, xrpIssue(), Account::master, xrpIssue());

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(
                        scAlice, jvb_unknown, reward, mcAlice),
                    ter(tecNO_ENTRY))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv
                .tx(xchain_commit(mcAlice, jvb_unknown, claimID, amt, dst),
                    ter(tecNO_ENTRY))
                .close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb_unknown, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv
                .tx(xchain_add_attestation_batch(scAttester, batch),
                    ter(tecNO_ENTRY))
                .close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv
                    .tx(xchain_claim(scAlice, jvb_unknown, claimID, amt, scBob),
                        ter(tecNO_ENTRY))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }

        // Claim against non-existent claim id
        // -----------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            // attest using non-existent claim id
            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, 999, dst, signers);
            scEnv
                .tx(xchain_add_attestation_batch(scAttester, batch),
                    ter(tecXCHAIN_NO_CLAIM_ID))
                .close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // claim using non-existent claim id
                scEnv
                    .tx(xchain_claim(scAlice, jvb, 999, amt, scBob),
                        ter(tecXCHAIN_NO_CLAIM_ID))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }

        // Claim against a claim id owned by another account
        // -------------------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            // submit attestations to the wrong account (scGw instead of
            // scAlice)
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // submit a claim transaction with the wrong account (scGw
                // instead of scAlice)
                scEnv
                    .tx(xchain_claim(scGw, jvb, claimID, amt, scBob),
                        ter(tecXCHAIN_BAD_CLAIM_ID))
                    .close();
                BEAST_EXPECT(transfer.has_not_happened());
            }
            else
            {
                BEAST_EXPECT(transfer.has_happened(amt, split_reward));
            }
        }

        // Claim against a claim id with no attestations
        // ---------------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            // don't send any attestations

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv
                    .tx(xchain_claim(scAlice, jvb, claimID, amt, scBob),
                        ter(tecXCHAIN_CLAIM_NO_QUORUM))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }

        // Claim against a claim id with attestations, but not enough to make a
        // quorum
        // --------------------------------------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto tooFew = quorum - 1;
            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers, tooFew);

            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv
                    .tx(xchain_claim(scAlice, jvb, claimID, amt, scBob),
                        ter(tecXCHAIN_CLAIM_NO_QUORUM))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }

        // Claim id of zero
        // ----------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, 0, dst, signers);

            scEnv
                .tx(xchain_add_attestation_batch(scAttester, batch),
                    ter(tecXCHAIN_NO_CLAIM_ID))
                .close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv
                    .tx(xchain_claim(scAlice, jvb, 0, amt, scBob),
                        ter(tecXCHAIN_NO_CLAIM_ID))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }

        // Claim issue that does not match the expected issue on the bridge
        // (either LockingChainIssue or IssuingChainIssue, depending on the
        // chain). The claim id should already have enough attestations to reach
        // a quorum for this amount (for a different issuer).
        // ---------------------------------------------------------------------
        for (auto withClaim : {true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv
                    .tx(xchain_claim(scAlice, jvb, claimID, scUSD(1000), scBob),
                        ter(temBAD_AMOUNT))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }
        // Claim to a destination that does not already exist on the chain
        // -----------------------------------------------------------------
        for (auto withClaim : {true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv
                    .tx(xchain_claim(scAlice, jvb, claimID, amt, scuBob),
                        ter(tecNO_DST))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }

        // Claim where the claim id owner does not have enough XRP to pay the
        // reward
        // ------------------------------------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();
            STAmount huge_reward{XRP(20000)};
            BEAST_EXPECT(huge_reward > scEnv.balance(scAlice).value());

            scEnv.tx(create_bridge(Account::master, jvb, huge_reward))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, huge_reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            if (withClaim)
            {
                scEnv.tx(xchain_add_attestation_batch(scAttester, batch))
                    .close();

                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv
                    .tx(xchain_claim(scAlice, jvb, claimID, amt, scBob),
                        ter(tecINSUFFICIENT_FUNDS))
                    .close();
            }
            else
            {
                scEnv
                    .tx(xchain_add_attestation_batch(scAttester, batch),
                        ter(tecINSUFFICIENT_FUNDS))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }

        // Claim where the claim id owner has enough XRP to pay the reward, but
        // it would put his balance below the reserve
        // --------------------------------------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .fund(
                    res0 + reward, scuAlice)  // just not enough because of fees
                .close()
                .tx(xchain_create_claim_id(scuAlice, jvb, reward, mcAlice),
                    ter(tecINSUFFICIENT_RESERVE))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scuAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv
                .tx(xchain_add_attestation_batch(scAttester, batch),
                    ter(tecXCHAIN_NO_CLAIM_ID))
                .close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv
                    .tx(xchain_claim(scuAlice, jvb, claimID, amt, scBob),
                        ter(tecXCHAIN_NO_CLAIM_ID))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }

        // Pay to an account with deposit auth set
        // ---------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .tx(fset("scBob", asfDepositAuth))  // set deposit auth
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            if (withClaim)
            {
                scEnv.tx(xchain_add_attestation_batch(scAttester, batch))
                    .close();

                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv
                    .tx(xchain_claim(scAlice, jvb, claimID, amt, scBob),
                        ter(tecNO_PERMISSION))
                    .close();
            }
            else
            {
                scEnv
                    .tx(xchain_add_attestation_batch(scAttester, batch),
                        ter(tecNO_PERMISSION))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }

        // Claim where the amount different from what is attested to
        // ---------------------------------------------------------
        for (auto withClaim : {true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // claim wrong amount
                scEnv
                    .tx(xchain_claim(scAlice, jvb, claimID, one_xrp, scBob),
                        ter(tecXCHAIN_CLAIM_NO_QUORUM))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }

        // Verify that rewards are paid from the account that owns the claim id
        // --------------------------------------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);
            Balance scAlice_bal(scEnv, scAlice);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            STAmount claim_cost = reward;

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
                claim_cost += tx_fee;
            }

            BEAST_EXPECT(transfer.has_happened(amt, split_reward));
            BEAST_EXPECT(
                scAlice_bal.diff() == -claim_cost);  // because reward % 5 == 0
        }

        // Verify that if a reward is not evenly divisible amung the reward
        // accounts, the remaining amount goes to the claim id owner.
        // ----------------------------------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb, tiny_reward)).close();

            scEnv.tx(create_bridge(Account::master, jvb, tiny_reward))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, tiny_reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, payees, withClaim);
            Balance scAlice_bal(scEnv, scAlice);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();
            STAmount claim_cost = tiny_reward;

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
                claim_cost += tx_fee;
            }

            BEAST_EXPECT(transfer.has_happened(amt, tiny_reward_split));
            BEAST_EXPECT(
                scAlice_bal.diff() == -(claim_cost - tiny_reward_remainder));
        }

        // If a reward distribution fails for one of the reward accounts (the
        // reward account doesn't exist or has deposit auth set), then the txn
        // should still succeed, but that portion should go to the claim id
        // owner.
        // -------------------------------------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            std::vector<Account> alt_payees = payees;
            alt_payees.back() = Account("inexistent");

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, alt_payees, true, claimID, dst, signers);

            alt_payees.resize(alt_payees.size() - 1);  // last payee is bad acct
            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, alt_payees, withClaim);

            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
            }

            BEAST_EXPECT(transfer.has_happened(amt, split_reward));
        }

        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            std::vector<Account> alt_payees = payees;

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .tx(fset(alt_payees.back(), asfDepositAuth))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, alt_payees, true, claimID, dst, signers);

            alt_payees.resize(
                alt_payees.size() - 1);  // last payee has deposit auth
            BalanceTransfer transfer(
                scEnv, Account::master, scBob, scAlice, alt_payees, withClaim);

            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
            }

            BEAST_EXPECT(transfer.has_happened(amt, split_reward));
        }

#if 0
        // +++++++++++++++++++++++

        // Verify that if a batch of attestations brings the signatures over
        // quorum (say the quorum is 4 and there are 5 attestations) then the
        // reward should be split amung the five accounts.
        // ------------------------------------------------------------------
        for (auto withClaim : {false, true})
        {
            xEnv mcEnv(*this);
            xEnv scEnv(*this, true);

            mcEnv.tx(create_bridge(mcDoor, jvb)).close();

            scEnv.tx(create_bridge(Account::master, jvb))
                .tx(jtx::signers(Account::master, signers.size(), signers))
                .close()
                .tx(xchain_create_claim_id(scAlice, jvb, reward, mcAlice))
                .close();

            auto dst(withClaim ? std::nullopt : std::optional<Account>{scBob});
            auto const amt = XRP(1000);
            std::uint32_t const claimID = 1;
            mcEnv.tx(xchain_commit(mcAlice, jvb, claimID, amt, dst)).close();

            BalanceTransfer transfer(scEnv, Account::master, scBob, scAlice, payees, withClaim);

            auto batch = attestation_claim_batch(
                jvb, mcAlice, amt, payees, true, claimID, dst, signers);
            scEnv.tx(xchain_add_attestation_batch(scAttester, batch)).close();

            if (withClaim)
            {
                BEAST_EXPECT(transfer.has_not_happened());

                // need to submit a claim transactions
                scEnv.tx(xchain_claim(scAlice, jvb, claimID, amt, scBob))
                    .close();
            }

            BEAST_EXPECT(transfer.has_not_happened());
        }
#endif
    }

    void
    testBridgeCreateAccount()
    {
        using namespace jtx;

        testcase("Bridge Create Account");
    }

    void
    testBridgeDeleteDoor()
    {
        using namespace jtx;

        testcase("Bridge Delete Door Account");

        // Deleting a account that owns bridge should fail

        // Deleting an account that owns a claim id should fail
    }

    void
    run() override
    {
        testBridgeCreate();
        testBridgeCreateMatrix();
        testBridgeModify();
        testBridgeCreateClaimID();
        testBridgeCommit();
        testBridgeAddAttestation();
        testBridgeClaim();
    }
};

BEAST_DEFINE_TESTSUITE(XChain, app, ripple);

}  // namespace ripple::test
