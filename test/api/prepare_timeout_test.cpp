#include <chrono>
#include <thread>

#include "graph_test/private_graph_test.h"

using namespace lbug::testing;

namespace {

class PrepareTimeoutTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
    }
};

} // namespace

// Binding can build lists, which check the timeout and interruption, but a
// statement prepared on its own isn't timed, and an earlier statement's state
// mustn't stop it: the failure would stay with the statement.
TEST_F(PrepareTimeoutTest, PrepareIsNotStoppedByAnEarlierStatement) {
    conn->setQueryTimeOut(200 /* timeoutInMS */);
    auto timedOut = conn->query(
        "UNWIND RANGE(1,100000) AS x UNWIND RANGE(1, 100000) AS y RETURN COUNT(x + y);");
    ASSERT_EQ(timedOut->getErrorMessage(), "Interrupted.");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto prepared = conn->prepare("RETURN size(range(1, 1000000));");
    ASSERT_TRUE(prepared->isSuccess()) << prepared->getErrorMessage();
    auto result = conn->execute(prepared.get());
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1000000);
}
