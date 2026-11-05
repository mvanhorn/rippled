#include <test/jtx/CaptureLogs.h>
#include <test/jtx/Env.h>
#include <test/jtx/envconfig.h>
#include <test/jtx/fee.h>
#include <test/jtx/jtx_json.h>
#include <test/jtx/seq.h>
#include <test/jtx/ter.h>
#include <test/jtx/ticket.h>

#include <xrpl/basics/strHex.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/core/HashRouter.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/jss.h>

#include <memory>

namespace xrpl::test {

class NetworkOPs_test : public beast::unit_test::Suite
{
public:
    void
    run() override
    {
        testBadTransaction();
        testAllBadHeldTransactions();
    }

    void
    testBadTransaction()
    {
        testcase("Reject cached bad transaction");

        using namespace jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();

        auto const sequence = env.seq(alice);
        auto const balance = env.balance(alice);
        auto const openTxCount = env.current()->txCount();
        auto const jt = env.jt(ticket::create(alice, 1));
        env.app().getHashRouter().setFlags(jt.stx->getTransactionID(), HashRouterFlags::BAD);

        Serializer s;
        jt.stx->add(s);
        json::Value args{json::ValueType::Object};
        args[jss::tx_blob] = strHex(s.slice());
        auto const jr = env.rpc("json", "submit", args.toStyledString());
        BEAST_EXPECT(jr[jss::result][jss::engine_result] == "temBAD_SIGNATURE");
        BEAST_EXPECT(env.seq(alice) == sequence);
        BEAST_EXPECT(env.balance(alice) == balance);
        BEAST_EXPECT(env.current()->txCount() == openTxCount);
    }

    void
    testAllBadHeldTransactions()
    {
        // All transactions are already marked as SF_BAD, and we should be able
        // to handle the case properly without an assertion failure
        testcase("No valid transactions in batch");

        std::string logs;

        {
            using namespace jtx;
            auto const alice = Account{"alice"};
            Env env{*this, envconfig(), std::make_unique<CaptureLogs>(&logs), beast::Severity::All};
            env.memoize(env.master);
            env.memoize(alice);

            auto const jtx = env.jt(ticket::create(alice, 1), Seq(1), Fee(10));

            auto const transactionId = jtx.stx->getTransactionID();
            env.app().getHashRouter().setFlags(transactionId, HashRouterFlags::HELD);

            env(jtx, Json(jss::Sequence, 1), Ter(terNO_ACCOUNT));

            env.app().getHashRouter().setFlags(transactionId, HashRouterFlags::BAD);

            env.close();
        }

        BEAST_EXPECT(logs.contains("No transaction to process!"));
    }
};

BEAST_DEFINE_TESTSUITE(NetworkOPs, app, xrpl);

}  // namespace xrpl::test
