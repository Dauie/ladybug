#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "common/external_access.h"
#include "graph_test/private_graph_test.h"
#include "main/connection.h"
#include "storage/storage_utils.h"
#include "storage/wal/local_wal.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace testing {

// Builds configured with ENABLE_EXTERNAL_ACCESS=OFF must refuse everything that reads or writes
// outside the database, uses the network or loads native code, while ordinary queries work.
class ExternalAccessTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        if constexpr (EXTERNAL_ACCESS_ENABLED) {
            GTEST_SKIP() << "Only meaningful with ENABLE_EXTERNAL_ACCESS=OFF.";
        }
        if (inMemMode) {
            GTEST_SKIP();
        }
    }

    static std::vector<char> readFile(const std::string& path) {
        std::ifstream stream{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }
};

TEST_F(ExternalAccessTest, RefusesStatementsThatReachOutside) {
    createDBAndConn();
    ASSERT_TRUE(conn->query("CREATE NODE TABLE Item(id INT64 PRIMARY KEY);")->isSuccess());
    // Forward slashes: a backslash inside a Cypher string literal is an escape.
    const auto outside =
        (std::filesystem::path(databasePath).parent_path() / "outside").generic_string();
    const std::vector<std::string> refused = {
        std::format("COPY Item FROM '{}.csv';", outside),
        std::format("LOAD FROM '{}.csv' RETURN *;", outside),
        std::format("COPY (MATCH (i:Item) RETURN i.id) TO '{}.csv';", outside),
        std::format("EXPORT DATABASE '{}';", outside),
        std::format("IMPORT DATABASE '{}';", outside),
        std::format("ATTACH '{}.lbdb' AS other (dbtype lbug);", outside),
        "INSTALL json;",
        std::format("LOAD EXTENSION '{}.dll';", outside),
        "UNINSTALL json;",
        "CREATE NODE TABLE External(id INT64 PRIMARY KEY) WITH (format = 'icebug-disk');",
    };
    for (const auto& statement : refused) {
        auto result = conn->query(statement);
        ASSERT_FALSE(result->isSuccess()) << statement;
        EXPECT_NE(result->getErrorMessage().find("is not available in this build"),
            std::string::npos)
            << statement << ": " << result->getErrorMessage();
    }
    // The file scan functions are not registered at all.
    EXPECT_FALSE(
        conn->query(std::format("CALL READ_CSV('{}.csv') RETURN *;", outside))->isSuccess());
    EXPECT_FALSE(std::filesystem::exists(outside + ".csv"));
    EXPECT_FALSE(std::filesystem::exists(outside));

    ASSERT_TRUE(conn->query("CREATE (:Item {id: 1});")->isSuccess());
    auto result = conn->query("MATCH (i:Item) RETURN COUNT(i);");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 1);
}

TEST_F(ExternalAccessTest, WALRecordThatLoadsAnExtensionFailsOpenAndKeepsData) {
    systemConfig->autoCheckpoint = false;
    systemConfig->forceCheckpointOnClose = false;
    systemConfig->throwOnWalReplayFailure = false;
    createDBAndConn();
    ASSERT_TRUE(conn->query("CREATE NODE TABLE Item(id INT64 PRIMARY KEY);")->isSuccess());
    // A log written by a build that could load extensions. This build cannot run LOAD
    // EXTENSION, so append the record directly.
    ASSERT_TRUE(conn->query("BEGIN TRANSACTION;")->isSuccess());
    ASSERT_TRUE(conn->query("CREATE (:Item {id: 1});")->isSuccess());
    transaction::Transaction::Get(*conn->getClientContext())
        ->getLocalWAL()
        .logLoadExtension("" /* main catalog */, "extension_from_another_build");
    ASSERT_TRUE(conn->query("COMMIT;")->isSuccess());
    conn.reset();
    database.reset();

    const auto walPath = StorageUtils::getWALFilePath(databasePath);
    ASSERT_TRUE(std::filesystem::exists(walPath));
    const auto before = readFile(walPath);
    try {
        createDBAndConn();
        FAIL() << "Opening a WAL that loads an extension must fail in this build.";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("Loading a native extension is not available"),
            std::string::npos)
            << e.what();
    }
    EXPECT_EQ(readFile(walPath), before);
    EXPECT_TRUE(std::filesystem::exists(databasePath));
}

} // namespace testing
} // namespace lbug
