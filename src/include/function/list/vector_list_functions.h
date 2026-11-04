#pragma once

#include "common/vector/value_vector.h"
#include "function/function.h"

namespace lbug {
namespace function {

// The most list elements an expression that builds lists (RANGE, a list lambda,
// list concatenation) may hold in one evaluation, which covers at most a chunk
// of rows: 512 MiB of INT64 values, twice that of the 128-bit types, and briefly
// more while the buffer grows. Such an expression runs inside one operator
// call, so this bounds its memory where no timeout is set; the statement's
// timeout and interruption are checked as it builds. A plan that splits rows
// into smaller chunks can stay under it where a full chunk wouldn't.
constexpr uint64_t MAX_EVALUATION_LIST_ELEMENTS = 1ull << 26;

// The elements in a list vector's data, counting the lists nested in it.
uint64_t countListElements(const common::ValueVector& vector);

// Throws if an expression's lists hold more than MAX_EVALUATION_LIST_ELEMENTS.
void checkEvaluationListElements(uint64_t elements, const char* expression);

struct ListCreationFunction {
    static constexpr const char* name = "LIST_CREATION";

    static function_set getFunctionSet();
    static void execFunc(const std::vector<std::shared_ptr<common::ValueVector>>& parameters,
        const std::vector<common::SelectionVector*>& parameterSelVectors,
        common::ValueVector& result, common::SelectionVector* resultSelVector,
        void* /*dataPtr*/ = nullptr);
};

struct ListRangeFunction {
    static constexpr const char* name = "RANGE";
    // The longest list RANGE builds: 128 MiB of INT64 values, twice that of the
    // 128-bit types.
    static constexpr uint64_t MAX_LIST_ELEMENTS = 1ull << 24;

    static function_set getFunctionSet();
};

struct SizeFunction {
    static constexpr const char* name = "SIZE";

    static function_set getFunctionSet();
};

struct CardinalityFunction {
    using alias = SizeFunction;

    static constexpr const char* name = "CARDINALITY";
};

struct ListExtractFunction {
    static constexpr const char* name = "LIST_EXTRACT";

    static function_set getFunctionSet();
};

struct ListElementFunction {
    using alias = ListExtractFunction;

    static constexpr const char* name = "LIST_ELEMENT";
};

struct ListConcatFunction {
    static constexpr const char* name = "LIST_CONCAT";

    static function_set getFunctionSet();
    static std::unique_ptr<FunctionBindData> bindFunc(const ScalarBindFuncInput& input);
};

struct ListCatFunction {
    using alias = ListConcatFunction;

    static constexpr const char* name = "LIST_CAT";
};

struct ListAppendFunction {
    static constexpr const char* name = "LIST_APPEND";

    static function_set getFunctionSet();
};

struct ListPrependFunction {
    static constexpr const char* name = "LIST_PREPEND";

    static function_set getFunctionSet();
};

struct ListPositionFunction {
    static constexpr const char* name = "LIST_POSITION";

    static function_set getFunctionSet();
};

struct ListIndexOfFunction {
    using alias = ListPositionFunction;

    static constexpr const char* name = "LIST_INDEXOF";
};

struct ListContainsFunction {
    static constexpr const char* name = "LIST_CONTAINS";

    static function_set getFunctionSet();
};

struct ListHasFunction {
    using alias = ListContainsFunction;

    static constexpr const char* name = "LIST_HAS";
};

struct ListSliceFunction {
    static constexpr const char* name = "LIST_SLICE";

    static function_set getFunctionSet();
};

struct ListSortFunction {
    static constexpr const char* name = "LIST_SORT";

    static function_set getFunctionSet();
};

struct ListReverseSortFunction {
    static constexpr const char* name = "LIST_REVERSE_SORT";

    static function_set getFunctionSet();
};

struct ListSumFunction {
    static constexpr const char* name = "LIST_SUM";

    static function_set getFunctionSet();
};

struct ListProductFunction {
    static constexpr const char* name = "LIST_PRODUCT";

    static function_set getFunctionSet();
};

struct ListMinFunction {
    static constexpr const char* name = "LIST_MIN";

    static function_set getFunctionSet();
};

struct ListMaxFunction {
    static constexpr const char* name = "LIST_MAX";

    static function_set getFunctionSet();
};

struct ListDistinctFunction {
    static constexpr const char* name = "LIST_DISTINCT";

    static function_set getFunctionSet();
};

struct ListUniqueFunction {
    static constexpr const char* name = "LIST_UNIQUE";

    static function_set getFunctionSet();
};

struct ListAnyValueFunction {
    static constexpr const char* name = "LIST_ANY_VALUE";

    static function_set getFunctionSet();
};

struct ListReverseFunction {
    static constexpr const char* name = "LIST_REVERSE";

    static function_set getFunctionSet();
};

struct ListToStringFunction {
    static constexpr const char* name = "LIST_TO_STRING";

    static function_set getFunctionSet();
};

struct ListTransformFunction {
    static constexpr const char* name = "LIST_TRANSFORM";

    static function_set getFunctionSet();
};

struct ListFilterFunction {
    static constexpr const char* name = "LIST_FILTER";

    static function_set getFunctionSet();
};

struct ListReduceFunction {
    static constexpr const char* name = "LIST_REDUCE";

    static function_set getFunctionSet();
};

using quantifier_handler = std::function<bool(uint64_t numSelectedValues, uint64_t originalSize)>;

void execQuantifierFunc(quantifier_handler handler,
    const std::vector<std::shared_ptr<common::ValueVector>>& input,
    const std::vector<common::SelectionVector*>& inputSelVectors, common::ValueVector& result,
    common::SelectionVector* resultSelVector, void* bindData);

std::unique_ptr<FunctionBindData> bindQuantifierFunc(const ScalarBindFuncInput& input);

struct ListAnyFunction {
    static constexpr const char* name = "ANY";

    static function_set getFunctionSet();
};

struct ListAllFunction {
    static constexpr const char* name = "ALL";

    static function_set getFunctionSet();
};

struct ListNoneFunction {
    static constexpr const char* name = "None";

    static function_set getFunctionSet();
};

struct ListSingleFunction {
    static constexpr const char* name = "Single";

    static function_set getFunctionSet();
};

struct ListHasAllFunction {
    static constexpr const char* name = "LIST_HAS_ALL";

    static function_set getFunctionSet();
};

struct ListHasAnyFunction {
    static constexpr const char* name = "LIST_HAS_ANY";

    static function_set getFunctionSet();
};

} // namespace function
} // namespace lbug
