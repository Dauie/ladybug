#include <string>
#include <type_traits>

#include "common/exception/runtime.h"
#include "common/type_utils.h"
#include "function/list/vector_list_functions.h"
#include "function/scalar_function.h"
#include "main/client_context.h"

using namespace lbug::common;

namespace lbug {
namespace function {

struct Range {
    // range function:
    // - include end
    // - when start = end: there is only one element in result list
    // - when end - start are of opposite sign of step, the result will be empty
    // - default step = 1
    template<typename T>
    static void operation(T& end, list_entry_t& result, ValueVector& endVector,
        ValueVector& resultVector, main::ClientContext* context) {
        T step = 1;
        T start = 0;
        operation(start, end, step, result, endVector, resultVector, context);
    }

    template<typename T>
    static void operation(T& start, T& end, list_entry_t& result, ValueVector& leftVector,
        ValueVector& /*rightVector*/, ValueVector& resultVector, main::ClientContext* context) {
        T step = 1;
        operation(start, end, step, result, leftVector, resultVector, context);
    }

    // Building a list runs inside one expression, where the engine's checks
    // between chunks of rows can't stop it. So each list, and all the lists one
    // evaluation adds, are capped, and building checks the statement's timeout
    // and interruption as it goes.
    template<typename T>
    static void operation(T& start, T& end, T& step, list_entry_t& result,
        ValueVector& /*inputVector*/, ValueVector& resultVector, main::ClientContext* context) {
        if (step == 0) {
            throw RuntimeException("Step of range cannot be 0.");
        }
        const auto size = countElements(start, end, step);
        if (size > ListRangeFunction::MAX_EVALUATION_ELEMENTS -
                       ListVector::getDataVectorSize(&resultVector)) {
            throw RuntimeException("RANGE would build more than " +
                                   std::to_string(ListRangeFunction::MAX_EVALUATION_ELEMENTS) +
                                   " list elements in one evaluation.");
        }

        // start, start + step, start + 2step, ..., end
        result = ListVector::addList(&resultVector, size);
        auto resultDataVector = ListVector::getDataVector(&resultVector);
        T number = start;
        for (uint64_t i = 0; i < size; i++) {
            resultDataVector->setValue(result.offset + i, number);
            // Not past the last element, which could overflow T.
            if (i + 1 < size) {
                number += step;
            }
            if (context != nullptr && (i & (CHECK_INTERVAL - 1)) == CHECK_INTERVAL - 1) {
                context->checkInterrupted();
            }
        }
    }

private:
    static constexpr uint64_t CHECK_INTERVAL = 1 << 16;

    // The number of elements from start to end, exactly. It is 0 when end lies on
    // the other side of start from where step goes.
    template<typename T>
    static uint64_t countElements(const T& start, const T& end, const T& step) {
        const T zero = 0;
        const bool up = step > zero;
        if (up ? end < start : start < end) {
            return 0;
        }
        bool tooMany = false;
        uint64_t steps = 0;
        if constexpr (std::is_integral_v<T>) {
            // The span always fits in 64 unsigned bits, and conversion to an
            // unsigned type is modular, so this is exact even where end - start
            // would overflow T.
            const auto span = up ? static_cast<uint64_t>(end) - static_cast<uint64_t>(start) :
                                   static_cast<uint64_t>(start) - static_cast<uint64_t>(end);
            const auto stride =
                up ? static_cast<uint64_t>(step) : uint64_t{0} - static_cast<uint64_t>(step);
            steps = span / stride;
            tooMany = steps >= ListRangeFunction::MAX_LIST_ELEMENTS;
        } else {
            // INT128 and UINT128 divide exactly, and throw on overflow.
            const T span = up ? end - start : start - end;
            const T stride = up ? step : zero - step;
            const T exact = span / stride;
            tooMany = exact >= T(ListRangeFunction::MAX_LIST_ELEMENTS);
            if (!tooMany) {
                steps = static_cast<uint64_t>(exact);
            }
        }
        if (tooMany) {
            throw RuntimeException("RANGE would build a list of more than " +
                                   std::to_string(ListRangeFunction::MAX_LIST_ELEMENTS) +
                                   " elements.");
        }
        return steps + 1;
    }
};

// The list wrappers drop the bind data, which carries the client context.
struct BinaryRangeWrapper {
    template<typename LEFT_TYPE, typename RIGHT_TYPE, typename RESULT_TYPE, typename OP>
    static void operation(LEFT_TYPE& left, RIGHT_TYPE& right, RESULT_TYPE& result,
        ValueVector* leftValueVector, ValueVector* rightValueVector, ValueVector* resultValueVector,
        uint64_t /*resultPos*/, void* dataPtr) {
        OP::operation(left, right, result, *leftValueVector, *rightValueVector, *resultValueVector,
            contextOf(dataPtr));
    }

    static main::ClientContext* contextOf(void* dataPtr) {
        return dataPtr == nullptr ? nullptr :
                                    static_cast<FunctionBindData*>(dataPtr)->clientContext;
    }
};

struct TernaryRangeWrapper {
    template<typename A_TYPE, typename B_TYPE, typename C_TYPE, typename RESULT_TYPE, typename OP>
    static void operation(A_TYPE& a, B_TYPE& b, C_TYPE& c, RESULT_TYPE& result, void* aValueVector,
        void* resultValueVector, void* dataPtr) {
        OP::operation(a, b, c, result, *(ValueVector*)aValueVector, *(ValueVector*)resultValueVector,
            BinaryRangeWrapper::contextOf(dataPtr));
    }
};

template<typename T>
static void binaryRange(const std::vector<std::shared_ptr<ValueVector>>& params,
    const std::vector<SelectionVector*>& paramSelVectors, ValueVector& result,
    SelectionVector* resultSelVector, void* dataPtr) {
    DASSERT(params.size() == 2);
    BinaryFunctionExecutor::executeSwitch<T, T, list_entry_t, Range, BinaryRangeWrapper>(
        *params[0], paramSelVectors[0], *params[1], paramSelVectors[1], result, resultSelVector,
        dataPtr);
}

template<typename T>
static void ternaryRange(const std::vector<std::shared_ptr<ValueVector>>& params,
    const std::vector<SelectionVector*>& paramSelVectors, ValueVector& result,
    SelectionVector* resultSelVector, void* dataPtr) {
    DASSERT(params.size() == 3);
    TernaryFunctionExecutor::executeSwitch<T, T, T, list_entry_t, Range, TernaryRangeWrapper>(
        *params[0], paramSelVectors[0], *params[1], paramSelVectors[1], *params[2],
        paramSelVectors[2], result, resultSelVector, dataPtr);
}

static scalar_func_exec_t getBinaryExecFunc(const LogicalType& type) {
    scalar_func_exec_t execFunc;
    TypeUtils::visit(
        type, [&execFunc]<IntegerTypes T>(T) { execFunc = binaryRange<T>; },
        [](auto) { UNREACHABLE_CODE; });
    return execFunc;
}

static scalar_func_exec_t getTernaryExecFunc(const LogicalType& type) {
    scalar_func_exec_t execFunc;
    TypeUtils::visit(
        type, [&execFunc]<IntegerTypes T>(T) { execFunc = ternaryRange<T>; },
        [](auto) { UNREACHABLE_CODE; });
    return execFunc;
}

static std::unique_ptr<FunctionBindData> bindFunc(const ScalarBindFuncInput& input) {
    auto type = LogicalType(input.definition->parameterTypeIDs[0]);
    auto resultType = LogicalType::LIST(type.copy());
    auto bindData = std::make_unique<FunctionBindData>(std::move(resultType));
    for (auto& _ : input.arguments) {
        (void)_;
        bindData->paramTypes.push_back(type.copy());
    }
    return bindData;
}

function_set ListRangeFunction::getFunctionSet() {
    function_set result;
    std::unique_ptr<ScalarFunction> func;
    for (auto typeID : LogicalTypeUtils::getIntegerTypeIDs()) {
        // start, end
        func = std::make_unique<ScalarFunction>(name, std::vector<LogicalTypeID>{typeID, typeID},
            LogicalTypeID::LIST, getBinaryExecFunc(LogicalType{typeID}));
        func->bindFunc = bindFunc;
        result.push_back(std::move(func));
        // start, end, step
        func = std::make_unique<ScalarFunction>(name,
            std::vector<LogicalTypeID>{typeID, typeID, typeID}, LogicalTypeID::LIST,
            getTernaryExecFunc(LogicalType{typeID}));
        func->bindFunc = bindFunc;
        result.push_back(std::move(func));
    }
    return result;
}

} // namespace function
} // namespace lbug
