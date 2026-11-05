#include <cstdlib>
#include <set>
#include <stdexcept>

#include "c_api/helpers.h"
#include "c_api/lbug.h"
#include "common/exception/parser.h"
#include "main/client_context.h"
#include "main/connection.h"
#include "parser/parsed_statement_visitor.h"
#include "parser/parser.h"

using namespace lbug::common;
using namespace lbug::main;

static_assert(static_cast<int>(ErrorCategory::NONE) == LBUG_ERROR_NONE);
static_assert(static_cast<int>(ErrorCategory::PARSER) == LBUG_ERROR_PARSER);
static_assert(static_cast<int>(ErrorCategory::BINDER) == LBUG_ERROR_BINDER);
static_assert(static_cast<int>(ErrorCategory::RUNTIME) == LBUG_ERROR_RUNTIME);
static_assert(static_cast<int>(ErrorCategory::CONSTRAINT) == LBUG_ERROR_CONSTRAINT);
static_assert(static_cast<int>(ErrorCategory::INTERRUPT) == LBUG_ERROR_INTERRUPT);
static_assert(static_cast<int>(ErrorCategory::IO) == LBUG_ERROR_IO);

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

PreparedStatement& getStatement(lbug_prepared_statement* statement) {
    require(statement && statement->_prepared_statement, "Invalid prepared statement.");
    return *static_cast<PreparedStatement*>(statement->_prepared_statement);
}

ClientContext& getContext(lbug_connection* connection) {
    require(connection && connection->_connection, "Invalid connection.");
    return *static_cast<Connection*>(connection->_connection)->getClientContext();
}

std::string copyBytes(const char* value, uint64_t length) {
    require(value || length == 0, "A null string pointer requires zero length.");
    require(length <= std::string{}.max_size(), "String length is too large.");
    return std::string(value ? value : "", static_cast<size_t>(length));
}

std::set<std::string> getParameterNames(lbug_prepared_statement* statement) {
    auto& prepared = getStatement(statement);
    require(prepared.isSuccess(), "Cannot enumerate parameters of a failed statement.");
    auto known = prepared.getKnownParameters();
    std::set<std::string> names(known.begin(), known.end());
    // C API prepare has no parameter values; unresolved names are not in parameterMap yet.
    const auto& unknown = prepared.getUnknownParameters();
    names.insert(unknown.begin(), unknown.end());
    return names;
}

class StatementInspector final : public lbug::parser::StatementVisitor {
public:
    bool hasLoadFrom = false;

private:
    void visitLoadFrom(const lbug::parser::ReadingClause*) override { hasLoadFrom = true; }
};

} // namespace

lbug_state lbug_parse_statement(const char* query, lbug_statement_info* out_info) {
    if (out_info) {
        *out_info = {LBUG_STATEMENT_UNKNOWN, false};
    }
    LBUG_C_API_GUARD_BEGIN
    clearLastCAPIErrorMessage();
    require(query && out_info, "Query and statement-info output are required.");
    // ClientContext::parseQuery is NOT parse-only: its standalone-call rewriter binds functions.
    // Use the syntax parser without extension callbacks or a ClientContext.
    auto statements = lbug::parser::Parser::parseQuery(query);
    if (statements.size() != 1) {
        throw ParserException("Expected exactly one statement.");
    }
    StatementInspector inspector;
    inspector.visit(*statements.front());
    out_info->type = static_cast<lbug_statement_type>(statements.front()->getStatementType());
    out_info->has_load_from = inspector.hasLoadFrom;
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_state lbug_connection_set_function_allowlist(lbug_connection* connection,
    const char* const* names, uint64_t num_names) {
    LBUG_C_API_GUARD_BEGIN
    clearLastCAPIErrorMessage();
    require(names || num_names == 0, "Function names are required for a non-empty allowlist.");
    std::unordered_set<std::string> copied;
    for (uint64_t i = 0; i < num_names; ++i) {
        require(names[i] && names[i][0], "Function names must be non-empty.");
        copied.emplace(names[i]);
    }
    getContext(connection).setFunctionAllowlist(std::move(copied));
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_state lbug_connection_clear_function_allowlist(lbug_connection* connection) {
    LBUG_C_API_GUARD_BEGIN
    clearLastCAPIErrorMessage();
    getContext(connection).setFunctionAllowlist(std::nullopt);
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_value* lbug_value_create_string_with_length(const char* value, uint64_t length) {
    LBUG_C_API_GUARD_BEGIN
    clearLastCAPIErrorMessage();
    auto cppValue = std::make_unique<Value>(copyBytes(value, length));
    auto* result = static_cast<lbug_value*>(std::calloc(1, sizeof(lbug_value)));
    if (!result) {
        throw std::bad_alloc();
    }
    result->_value = cppValue.release();
    return result;
    LBUG_C_API_GUARD_END(nullptr)
}

lbug_state lbug_prepared_statement_bind_string_with_length(
    lbug_prepared_statement* prepared_statement, const char* param_name, const char* value,
    uint64_t length) {
    LBUG_C_API_GUARD_BEGIN
    clearLastCAPIErrorMessage();
    require(getStatement(prepared_statement).isSuccess(), "Cannot bind a failed statement.");
    require(param_name && prepared_statement->_bound_values,
        "Parameter name and bindings are required.");
    auto cppValue = std::make_unique<Value>(copyBytes(value, length));
    auto& values = *static_cast<std::unordered_map<std::string, std::unique_ptr<Value>>*>(
        prepared_statement->_bound_values);
    // Do not use the legacy void helper: it catches allocation failures and cannot report them.
    values.insert_or_assign(param_name, std::move(cppValue));
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_state lbug_value_get_string_with_length(lbug_value* value, char** out_result,
    uint64_t* out_length) {
    if (out_result) {
        *out_result = nullptr;
    }
    if (out_length) {
        *out_length = 0;
    }
    LBUG_C_API_GUARD_BEGIN
    clearLastCAPIErrorMessage();
    require(value && value->_value && out_result && out_length,
        "Value and string outputs are required.");
    const auto& cppValue = *static_cast<Value*>(value->_value);
    require(!cppValue.isNull() &&
                cppValue.getDataType().getLogicalTypeID() == LogicalTypeID::STRING,
        "Value must be a non-null STRING.");
    const auto& bytes = cppValue.getValue<std::string>();
    auto* copy = convertToOwnedCString(bytes);
    if (!copy) {
        throw std::bad_alloc();
    }
    *out_result = copy;
    *out_length = bytes.size();
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_state lbug_prepared_statement_get_parameter_count(lbug_prepared_statement* prepared_statement,
    uint64_t* out_count) {
    if (out_count) {
        *out_count = 0;
    }
    LBUG_C_API_GUARD_BEGIN
    clearLastCAPIErrorMessage();
    require(out_count, "Parameter count output is required.");
    *out_count = getParameterNames(prepared_statement).size();
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_state lbug_prepared_statement_get_parameter_name(lbug_prepared_statement* prepared_statement,
    uint64_t index, char** out_name) {
    if (out_name) {
        *out_name = nullptr;
    }
    LBUG_C_API_GUARD_BEGIN
    clearLastCAPIErrorMessage();
    require(out_name, "Parameter name output is required.");
    const auto names = getParameterNames(prepared_statement);
    require(index < names.size(), "Parameter index is out of range.");
    auto it = names.begin();
    std::advance(it, index);
    auto* name = convertToOwnedCString(*it);
    if (!name) {
        throw std::bad_alloc();
    }
    *out_name = name;
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_error_category lbug_prepared_statement_get_error_category(
    lbug_prepared_statement* prepared_statement) {
    LBUG_C_API_GUARD_BEGIN
    return static_cast<lbug_error_category>(getStatement(prepared_statement).getErrorCategory());
    LBUG_C_API_GUARD_END(LBUG_ERROR_RUNTIME)
}

lbug_error_category lbug_query_result_get_error_category(lbug_query_result* query_result) {
    LBUG_C_API_GUARD_BEGIN
    require(query_result && query_result->_query_result, "Invalid query result.");
    return static_cast<lbug_error_category>(
        static_cast<QueryResult*>(query_result->_query_result)->getErrorCategory());
    LBUG_C_API_GUARD_END(LBUG_ERROR_RUNTIME)
}
