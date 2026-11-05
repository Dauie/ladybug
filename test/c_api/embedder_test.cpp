#include <cstring>
#include <fstream>
#include <memory>
#include <thread>

#include "c_api/lbug.h"
#include "catalog/catalog.h"
#include "common/exception/buffer_manager.h"
#include "common/exception/constraint.h"
#include "common/exception/interrupt.h"
#include "common/exception/io.h"
#include "common/external_access.h"
#include "function/scalar_function.h"
#include "function/table/bind_data.h"
#include "function/table/table_function.h"
#include "graph_test/private_graph_test.h"
#include "main/client_context.h"

using namespace lbug::common;
using namespace lbug::testing;

namespace {

struct Result {
    lbug_query_result value{};
    ~Result() { lbug_query_result_destroy(&value); }
};

struct Statement {
    lbug_prepared_statement value{};
    ~Statement() { lbug_prepared_statement_destroy(&value); }
};

using OwnedValue = std::unique_ptr<lbug_value, decltype(&lbug_value_destroy)>;

std::string takeError() {
    auto* error = lbug_get_last_error();
    std::string result = error ? error : "";
    lbug_destroy_string(error);
    return result;
}

class CApiEmbedderTest : public EmptyDBTest {
protected:
    void SetUp() override {
        EmptyDBTest::SetUp();
        createDBAndConn();
        connection._connection = conn.get();
    }

    void succeeds(const char* query) {
        Result result;
        ASSERT_EQ(lbug_connection_query(&connection, query, &result.value), LbugSuccess)
            << takeError();
        EXPECT_EQ(lbug_query_result_get_error_category(&result.value), LBUG_ERROR_NONE);
    }

    void fails(const char* query, lbug_error_category category) {
        Result result;
        ASSERT_EQ(lbug_connection_query(&connection, query, &result.value), LbugError) << query;
        EXPECT_EQ(lbug_query_result_get_error_category(&result.value), category) << takeError();
        EXPECT_EQ(lbug_get_last_error_category(), category);
    }

    void expectBytes(lbug_value* value, const std::string& bytes) {
        char* output = nullptr;
        uint64_t length = UINT64_MAX;
        ASSERT_EQ(lbug_value_get_string_with_length(value, &output, &length), LbugSuccess)
            << takeError();
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(length, bytes.size());
        EXPECT_EQ(std::string(output, length), bytes);
        EXPECT_EQ(output[length], '\0');
        lbug_destroy_string(output);
    }

    void addTableProbe(const std::string& name, lbug::catalog::CatalogEntryType type,
        int& bindCalls) {
        using namespace lbug;
        auto function =
            std::make_unique<function::TableFunction>(name, std::vector<LogicalTypeID>{});
        function->bindFunc = [&bindCalls](main::ClientContext*, const function::TableFuncBindInput*)
            -> std::unique_ptr<function::TableFuncBindData> {
            ++bindCalls;
            throw IOException("probe bind callback");
        };
        function::function_set functions;
        functions.push_back(std::move(function));
        catalog::Catalog::Get(*conn->getClientContext())
            ->addFunction(&transaction::DUMMY_CHECKPOINT_TRANSACTION, type, name,
                std::move(functions), false);
    }

    lbug_connection connection{};
};

TEST_F(CApiEmbedderTest, LengthAwareStringsRoundTrip) {
    for (const auto& bytes : std::vector<std::string>{"", std::string("\0", 1),
             std::string("\0a\0b\0", 5), "caf\xC3\xA9"}) {
        OwnedValue value(lbug_value_create_string_with_length(bytes.data(), bytes.size()),
            lbug_value_destroy);
        ASSERT_NE(value, nullptr);
        expectBytes(value.get(), bytes);

        Statement statement;
        ASSERT_EQ(lbug_connection_prepare(&connection, "RETURN $text", &statement.value),
            LbugSuccess);
        ASSERT_TRUE(lbug_prepared_statement_is_success(&statement.value));
        ASSERT_EQ(lbug_prepared_statement_bind_string_with_length(&statement.value, "text",
                      bytes.data(), bytes.size()),
            LbugSuccess);
        Result result;
        ASSERT_EQ(lbug_connection_execute(&connection, &statement.value, &result.value),
            LbugSuccess)
            << takeError();
        lbug_flat_tuple tuple{};
        ASSERT_EQ(lbug_query_result_get_next(&result.value, &tuple), LbugSuccess);
        lbug_value cell{};
        ASSERT_EQ(lbug_flat_tuple_get_value(&tuple, 0, &cell), LbugSuccess);
        expectBytes(&cell, bytes);
        lbug_value_destroy(&cell);
        lbug_flat_tuple_destroy(&tuple);
    }

    const char unterminated[] = {'a', 'b', 'c'};
    OwnedValue value(lbug_value_create_string_with_length(unterminated, sizeof(unterminated)),
        lbug_value_destroy);
    ASSERT_NE(value, nullptr);
    expectBytes(value.get(), "abc");
    OwnedValue empty(lbug_value_create_string_with_length(nullptr, 0), lbug_value_destroy);
    ASSERT_NE(empty, nullptr);
    expectBytes(empty.get(), "");
    OwnedValue legacy(lbug_value_create_string("a\0b"), lbug_value_destroy);
    expectBytes(legacy.get(), "a");
}

TEST_F(CApiEmbedderTest, LengthAwareStringsRejectInvalidInputs) {
    EXPECT_EQ(lbug_value_create_string_with_length(nullptr, 1), nullptr);
    EXPECT_EQ(lbug_get_last_error_category(), LBUG_ERROR_RUNTIME);
    EXPECT_EQ(lbug_value_create_string_with_length("a", UINT64_MAX), nullptr);
    char* output = reinterpret_cast<char*>(1);
    uint64_t length = 42;
    EXPECT_EQ(lbug_value_get_string_with_length(nullptr, &output, &length), LbugError);
    EXPECT_EQ(output, nullptr);
    EXPECT_EQ(length, 0);
    OwnedValue number(lbug_value_create_int64(7), lbug_value_destroy);
    EXPECT_EQ(lbug_value_get_string_with_length(number.get(), &output, &length), LbugError);
    OwnedValue null(lbug_value_create_null(), lbug_value_destroy);
    EXPECT_EQ(lbug_value_get_string_with_length(null.get(), &output, &length), LbugError);
    EXPECT_EQ(lbug_value_get_string_with_length(number.get(), nullptr, &length), LbugError);

    Statement statement;
    ASSERT_EQ(lbug_connection_prepare(&connection, "RETURN $text", &statement.value), LbugSuccess);
    EXPECT_EQ(lbug_prepared_statement_bind_string_with_length(&statement.value, nullptr, "", 0),
        LbugError);
    EXPECT_EQ(lbug_prepared_statement_bind_string_with_length(&statement.value, "text", nullptr, 1),
        LbugError);
    EXPECT_EQ(lbug_prepared_statement_bind_string_with_length(nullptr, "text", "", 0), LbugError);
    EXPECT_EQ(lbug_prepared_statement_bind_string_with_length(&statement.value, "text", nullptr, 0),
        LbugSuccess);
}

TEST_F(CApiEmbedderTest, ParseOnlyClassifiesWithoutCatalogOrBinding) {
    struct Case {
        const char* query;
        lbug_statement_type type;
        bool load;
    };
    for (const auto& test : std::vector<Case>{
             {"/* prefix */ MATCH (n:missing) RETURN missing_function(n)", LBUG_STATEMENT_QUERY,
                 false},
             {"CREATE NODE TABLE N(id INT64, PRIMARY KEY(id))", LBUG_STATEMENT_CREATE_TABLE, false},
             {"// prefix\nDROP TABLE missing", LBUG_STATEMENT_DROP, false},
             {"ALTER TABLE missing ADD title STRING", LBUG_STATEMENT_ALTER, false},
             {"CALL timeout=0", LBUG_STATEMENT_STANDALONE_CALL, false},
             {"CHECKPOINT", LBUG_STATEMENT_TRANSACTION, false},
             {"RETURN 'LOAD FROM does not make a clause'", LBUG_STATEMENT_QUERY, false},
             {"LOAD FROM 'no-such-directory/*.csv' RETURN *", LBUG_STATEMENT_QUERY, true},
             {"WITH 1 AS x LOAD FROM 'no-such-directory/*.csv' RETURN x", LBUG_STATEMENT_QUERY,
                 true},
             {"RETURN 1 AS x UNION LOAD FROM 'no-such-directory/*.csv' RETURN 1 AS x",
                 LBUG_STATEMENT_QUERY, true},
             {"EXPLAIN LOAD FROM 'no-such-directory/*.csv' RETURN *", LBUG_STATEMENT_EXPLAIN, true},
         }) {
        lbug_statement_info info{};
        ASSERT_EQ(lbug_parse_statement(test.query, &info), LbugSuccess)
            << test.query << takeError();
        EXPECT_EQ(info.type, test.type);
        EXPECT_EQ(info.has_load_from, test.load);
        EXPECT_FALSE(hasActiveTransaction(*conn));
    }

    int binds = 0;
    addTableProbe("PARSE_PROBE", lbug::catalog::CatalogEntryType::STANDALONE_TABLE_FUNCTION_ENTRY,
        binds);
    lbug_statement_info info{};
    ASSERT_EQ(lbug_parse_statement("CALL PARSE_PROBE()", &info), LbugSuccess);
    EXPECT_EQ(info.type, LBUG_STATEMENT_STANDALONE_CALL_FUNCTION);
    EXPECT_EQ(binds, 0);
    fails("CALL PARSE_PROBE()", LBUG_ERROR_IO);
    EXPECT_EQ(binds, 1);
}

TEST_F(CApiEmbedderTest, ParseOnlyRejectsInvalidOrMultipleStatements) {
    for (const char* query : {"", " ", "RETURN (", "RETURN 1; RETURN 2"}) {
        lbug_statement_info info{LBUG_STATEMENT_QUERY, true};
        EXPECT_EQ(lbug_parse_statement(query, &info), LbugError) << query;
        EXPECT_EQ(info.type, LBUG_STATEMENT_UNKNOWN);
        EXPECT_FALSE(info.has_load_from);
        EXPECT_EQ(lbug_get_last_error_category(), LBUG_ERROR_PARSER);
    }
    lbug_statement_info info{};
    EXPECT_EQ(lbug_parse_statement(nullptr, &info), LbugError);
    EXPECT_EQ(lbug_parse_statement("RETURN 1", nullptr), LbugError);
    EXPECT_EQ(lbug_parse_statement("RETURN 1;", &info), LbugSuccess);
    EXPECT_EQ(lbug_get_last_error_category(), LBUG_ERROR_NONE);
}

TEST_F(CApiEmbedderTest, FunctionAllowlistCoversNestedCallsMacrosAndTables) {
    succeeds("CREATE MACRO hidden_call(x) AS abs(x)");
    const char* names[] = {"LoWeR", "hidden_call", "count", "count_star"};
    ASSERT_EQ(lbug_connection_set_function_allowlist(&connection, names, 4), LbugSuccess);
    succeeds("RETURN LOWER('A')");
    succeeds("RETURN count(1), count(*)");
    fails("RETURN LCASE('A')", LBUG_ERROR_BINDER); // Aliases are separate entries.
    for (const char* query : {"RETURN upper('a')", "RETURN lower(upper('a'))",
             "RETURN hidden_call(1)", "RETURN unknown_function()", "CALL SHOW_TABLES() RETURN *",
             "CALL TABLE_INFO('missing') RETURN *"}) {
        fails(query, LBUG_ERROR_BINDER);
    }
    ASSERT_EQ(lbug_connection_clear_function_allowlist(&connection), LbugSuccess);
    succeeds("RETURN hidden_call(-1)");
}

TEST_F(CApiEmbedderTest, FunctionPolicyStopsCallbacksAndIsConnectionLocal) {
    int binds = 0;
    addTableProbe("TABLE_PROBE", lbug::catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY, binds);
    addTableProbe("STANDALONE_PROBE",
        lbug::catalog::CatalogEntryType::STANDALONE_TABLE_FUNCTION_ENTRY, binds);
    ASSERT_EQ(lbug_connection_set_function_allowlist(&connection, nullptr, 0), LbugSuccess);
    succeeds("RETURN 1");
    fails("RETURN 1+2", LBUG_ERROR_BINDER); // Parser-lowered calls also need an entry.
    fails("CALL TABLE_PROBE() RETURN *", LBUG_ERROR_BINDER);
    fails("CALL STANDALONE_PROBE()", LBUG_ERROR_BINDER);
    EXPECT_EQ(binds, 0);

    lbug::main::Connection other(database.get());
    lbug_connection otherHandle{&other};
    Result result;
    ASSERT_EQ(lbug_connection_query(&otherHandle, "RETURN abs(-1)", &result.value), LbugSuccess);
    const char* allowed[] = {"table_probe", "standalone_probe"};
    ASSERT_EQ(lbug_connection_set_function_allowlist(&connection, allowed, 2), LbugSuccess);
    fails("CALL TABLE_PROBE() RETURN *", LBUG_ERROR_IO);
    fails("CALL STANDALONE_PROBE()", LBUG_ERROR_IO);
    EXPECT_EQ(binds, 2);
}

TEST_F(CApiEmbedderTest, DeniedScalarAndOuterCallsCannotInvokeBindCallbacks) {
    using namespace lbug::function;
    int binds = 0;
    auto probe = std::make_unique<ScalarFunction>("SCALAR_PROBE", std::vector<LogicalTypeID>{},
        LogicalTypeID::INT64);
    probe->bindFunc = [&binds](const ScalarBindFuncInput&) -> std::unique_ptr<FunctionBindData> {
        ++binds;
        throw IOException("probe scalar bind callback");
    };
    function_set functions;
    functions.push_back(std::move(probe));
    conn->getClientContext()->addScalarFunction("SCALAR_PROBE", std::move(functions));
    const char* allowed[] = {"abs"};
    ASSERT_EQ(lbug_connection_set_function_allowlist(&connection, allowed, 1), LbugSuccess);
    fails("RETURN abs(SCALAR_PROBE())", LBUG_ERROR_BINDER);
    const char* onlyProbe[] = {"scalar_probe"};
    ASSERT_EQ(lbug_connection_set_function_allowlist(&connection, onlyProbe, 1), LbugSuccess);
    fails("RETURN abs(SCALAR_PROBE())", LBUG_ERROR_BINDER);
    EXPECT_EQ(binds, 0);
    fails("RETURN SCALAR_PROBE()", LBUG_ERROR_IO);
    EXPECT_EQ(binds, 1);
}

TEST_F(CApiEmbedderTest, PreparedHandlesCannotCrossConnections) {
    Statement statement;
    ASSERT_EQ(lbug_connection_prepare(&connection, "RETURN 1", &statement.value), LbugSuccess);
    lbug::main::Connection other(database.get());
    lbug_connection otherHandle{&other};
    Result result;
    EXPECT_EQ(lbug_connection_execute(&otherHandle, &statement.value, &result.value), LbugError);
    EXPECT_EQ(lbug_query_result_get_error_category(&result.value), LBUG_ERROR_BINDER);
}

TEST_F(CApiEmbedderTest, PolicyChangeInvalidatesPreparedPlans) {
    Statement old;
    ASSERT_EQ(lbug_connection_prepare(&connection, "RETURN abs($x)", &old.value), LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_int64(&old.value, "x", -1), LbugSuccess);
    for (int i = 0; i < 2; ++i) {
        Result result;
        ASSERT_EQ(lbug_connection_execute(&connection, &old.value, &result.value), LbugSuccess);
    }
    ASSERT_EQ(lbug_connection_set_function_allowlist(&connection, nullptr, 0), LbugSuccess);
    Result refused;
    EXPECT_EQ(lbug_connection_execute(&connection, &old.value, &refused.value), LbugError);
    EXPECT_EQ(lbug_query_result_get_error_category(&refused.value), LBUG_ERROR_BINDER);
    ASSERT_EQ(lbug_connection_clear_function_allowlist(&connection), LbugSuccess);
    Result stillOld;
    EXPECT_EQ(lbug_connection_execute(&connection, &old.value, &stillOld.value), LbugError);
    succeeds("RETURN abs(-1)");
}

TEST_F(CApiEmbedderTest, InvalidPolicyDoesNotReplaceExistingPolicy) {
    char name[] = "abs";
    const char* names[] = {name};
    ASSERT_EQ(lbug_connection_set_function_allowlist(&connection, names, 1), LbugSuccess);
    name[0] = 'x'; // The caller need not retain its input.
    succeeds("RETURN abs(1)");
    EXPECT_EQ(lbug_connection_set_function_allowlist(&connection, nullptr, 1), LbugError);
    const char* invalid[] = {nullptr};
    EXPECT_EQ(lbug_connection_set_function_allowlist(&connection, invalid, 1), LbugError);
    succeeds("RETURN abs(1)");
    fails("RETURN upper('a')", LBUG_ERROR_BINDER);
}

TEST_F(CApiEmbedderTest, ErrorCategoriesSurvivePrepareAndExecution) {
    fails("RETURN (", LBUG_ERROR_PARSER);
    fails("MATCH (n:missing) RETURN n", LBUG_ERROR_BINDER);
    fails("RETURN CAST('not-an-integer' AS INT64)", LBUG_ERROR_RUNTIME);
    succeeds("CREATE NODE TABLE N(id INT64, PRIMARY KEY(id))");
    succeeds("CREATE (:N {id: 1}), (:N {id: 2})");
    fails("CREATE (:N {id: 1})", LBUG_ERROR_CONSTRAINT);
    fails("CREATE (:N)", LBUG_ERROR_BINDER);
    fails("CREATE (:N {id: NULL})", LBUG_ERROR_CONSTRAINT);
    fails("MATCH (n:N {id: 2}) SET n.id=1", LBUG_ERROR_BINDER);
    fails("MATCH (n:N {id: 2}) SET n.id=NULL", LBUG_ERROR_BINDER);
    succeeds("CREATE REL TABLE R(FROM N TO N, MANY_ONE)");
    succeeds("MATCH (a:N {id: 1}), (b:N {id: 2}) CREATE (a)-[:R]->(b)");
    fails("MATCH (a:N {id: 1}), (b:N {id: 2}) CREATE (a)-[:R]->(b)", LBUG_ERROR_CONSTRAINT);
    fails("MATCH (n:N {id: 1}) DELETE n", LBUG_ERROR_CONSTRAINT);

    Statement failed;
    ASSERT_EQ(lbug_connection_prepare(&connection, "RETURN (", &failed.value), LbugSuccess);
    EXPECT_FALSE(lbug_prepared_statement_is_success(&failed.value));
    EXPECT_EQ(lbug_prepared_statement_get_error_category(&failed.value), LBUG_ERROR_PARSER);
    Result result;
    EXPECT_EQ(lbug_connection_execute(&connection, &failed.value, &result.value), LbugError);
    EXPECT_EQ(lbug_query_result_get_error_category(&result.value), LBUG_ERROR_PARSER);
    succeeds("RETURN 1");
    EXPECT_EQ(lbug_prepared_statement_get_error_category(&failed.value), LBUG_ERROR_PARSER);
    EXPECT_EQ(lbug_query_result_get_error_category(&result.value), LBUG_ERROR_PARSER);
    EXPECT_EQ(lbug_prepared_statement_get_error_category(nullptr), LBUG_ERROR_RUNTIME);
    EXPECT_EQ(lbug_query_result_get_error_category(nullptr), LBUG_ERROR_RUNTIME);
}

TEST_F(CApiEmbedderTest, CopyEndpointConstraintCategory) {
    if (!EXTERNAL_ACCESS_ENABLED || inMemMode) {
        GTEST_SKIP() << "COPY endpoint validation requires external access and an on-disk fixture.";
    }
    succeeds("CREATE NODE TABLE N(id INT64, PRIMARY KEY(id))");
    succeeds("CREATE REL TABLE R(FROM N TO N)");
    succeeds("CREATE (:N {id: 1})");
    const auto path = std::filesystem::path(databasePath).parent_path() / "missing-endpoint.csv";
    std::ofstream(path) << "1,999\n";
    const auto query = "COPY R FROM '" + path.generic_string() + "'";
    fails(query.c_str(), LBUG_ERROR_CONSTRAINT);
    std::filesystem::remove(path);
}

static int64_t bufferFailure() {
    throw BufferManagerException("Buffer pool is full");
}

static int64_t ioFailure() {
    throw IOException("opaque test failure");
}

static int64_t wrappedIoFailure() {
    try {
        try {
            throw IOException("opaque test failure");
        } catch (...) {
            std::throw_with_nested(RuntimeException("first wrapper"));
        }
    } catch (...) {
        std::throw_with_nested(RuntimeException("second wrapper"));
    }
}

static int64_t copyConstraintFailure() {
    throw CopyConstraintException("opaque test failure");
}

TEST_F(CApiEmbedderTest, IoAndInterruptCategoriesAndThreadLocalErrors) {
    conn->createScalarFunction("io_failure", &ioFailure);
    conn->createScalarFunction("buffer_failure", &bufferFailure);
    conn->createScalarFunction("wrapped_io_failure", &wrappedIoFailure);
    fails("RETURN buffer_failure()", LBUG_ERROR_RUNTIME);
    fails("RETURN wrapped_io_failure()", LBUG_ERROR_IO);
    conn->createScalarFunction("copy_constraint_failure", &copyConstraintFailure);
    fails("RETURN io_failure()", LBUG_ERROR_IO);
    fails("RETURN copy_constraint_failure()", LBUG_ERROR_CONSTRAINT);
    ASSERT_EQ(lbug_connection_set_query_timeout(&connection, 1), LbugSuccess);
    fails("UNWIND range(1, 100000) AS a UNWIND range(1, 100000) AS b RETURN count(*)",
        LBUG_ERROR_INTERRUPT);
    ASSERT_EQ(lbug_connection_set_query_timeout(&connection, 0), LbugSuccess);
    fails("RETURN io_failure()", LBUG_ERROR_IO);
    EXPECT_FALSE(takeError().empty());
    EXPECT_EQ(lbug_get_last_error_category(), LBUG_ERROR_IO);
    std::thread independent([] {
        EXPECT_EQ(lbug_get_last_error_category(), LBUG_ERROR_NONE);
        lbug_statement_info info{};
        EXPECT_EQ(lbug_parse_statement("RETURN (", &info), LbugError);
        EXPECT_EQ(lbug_get_last_error_category(), LBUG_ERROR_PARSER);
    });
    independent.join();
    EXPECT_EQ(lbug_get_last_error_category(), LBUG_ERROR_IO);
    succeeds("RETURN 1");
    EXPECT_EQ(lbug_get_last_error_category(), LBUG_ERROR_NONE);
}

TEST_F(CApiEmbedderTest, ConversionErrorsReplaceEarlierCategories) {
    lbug_statement_info info{};
    EXPECT_EQ(lbug_parse_statement("RETURN (", &info), LbugError);
    lbug_int128_t integer{};
    EXPECT_EQ(lbug_int128_t_from_string("xyz", &integer), LbugError);
    EXPECT_EQ(lbug_get_last_error_category(), LBUG_ERROR_RUNTIME);
    EXPECT_EQ(lbug_parse_statement("RETURN (", &info), LbugError);
    lbug_date_t date{};
    EXPECT_EQ(lbug_date_from_string("xyz", &date), LbugError);
    EXPECT_EQ(lbug_get_last_error_category(), LBUG_ERROR_RUNTIME);
}

TEST_F(CApiEmbedderTest, ParameterNamesIncludeUnresolvedAndExcludeUnusedBindings) {
    Statement statement;
    ASSERT_EQ(lbug_connection_prepare(&connection, "RETURN $z AS z, $a AS a, $z AS again",
                  &statement.value),
        LbugSuccess);
    ASSERT_TRUE(lbug_prepared_statement_is_success(&statement.value));
    auto checkNames = [&] {
        uint64_t count = 99;
        ASSERT_EQ(lbug_prepared_statement_get_parameter_count(&statement.value, &count),
            LbugSuccess);
        ASSERT_EQ(count, 2);
        for (uint64_t i = 0; i < count; ++i) {
            char* name = nullptr;
            ASSERT_EQ(lbug_prepared_statement_get_parameter_name(&statement.value, i, &name),
                LbugSuccess);
            EXPECT_STREQ(name, i == 0 ? "a" : "z");
            lbug_destroy_string(name);
        }
        char* name = reinterpret_cast<char*>(1);
        EXPECT_EQ(lbug_prepared_statement_get_parameter_name(&statement.value, 2, &name),
            LbugError);
        EXPECT_EQ(name, nullptr);
    };
    checkNames();
    ASSERT_EQ(lbug_prepared_statement_bind_int64(&statement.value, "z", 7), LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_int64(&statement.value, "a", 8), LbugSuccess);
    ASSERT_EQ(lbug_prepared_statement_bind_int64(&statement.value, "unused", 9), LbugSuccess);
    checkNames();
    Result result;
    ASSERT_EQ(lbug_connection_execute(&connection, &statement.value, &result.value), LbugSuccess);
    checkNames();

    Statement typed;
    ASSERT_EQ(lbug_connection_prepare(&connection, "RETURN CAST($typed AS INT64)", &typed.value),
        LbugSuccess);
    char* typedName = nullptr;
    ASSERT_EQ(lbug_prepared_statement_get_parameter_name(&typed.value, 0, &typedName), LbugSuccess);
    EXPECT_STREQ(typedName, "typed");
    lbug_destroy_string(typedName);
    EXPECT_EQ(lbug_prepared_statement_get_error_category(&typed.value), LBUG_ERROR_NONE);

    Statement none;
    ASSERT_EQ(lbug_connection_prepare(&connection, "RETURN 1", &none.value), LbugSuccess);
    uint64_t count = 99;
    EXPECT_EQ(lbug_prepared_statement_get_parameter_count(&none.value, &count), LbugSuccess);
    EXPECT_EQ(count, 0);
    EXPECT_EQ(lbug_prepared_statement_get_parameter_count(nullptr, &count), LbugError);
    EXPECT_EQ(count, 0);
    EXPECT_EQ(lbug_prepared_statement_get_parameter_count(&none.value, nullptr), LbugError);
    Statement failed;
    ASSERT_EQ(lbug_connection_prepare(&connection, "RETURN (", &failed.value), LbugSuccess);
    EXPECT_EQ(lbug_prepared_statement_get_parameter_count(&failed.value, &count), LbugError);
}

} // namespace
