#include "common/exception/exception.h"

#include "common/enums/error_category.h"
#include "common/exception/binder.h"
#include "common/exception/catalog.h"
#include "common/exception/constraint.h"
#include "common/exception/interrupt.h"
#include "common/exception/io.h"
#include "common/exception/parser.h"

namespace lbug {
namespace common {

Exception::Exception(std::string msg) : exception(), exception_message_(std::move(msg)) {}

ErrorCategory classifyException(const std::exception& exception) noexcept {
    if (const auto* wrapper = dynamic_cast<const std::nested_exception*>(&exception);
        wrapper && wrapper->nested_ptr()) {
        try {
            wrapper->rethrow_nested();
        } catch (const std::exception& cause) {
            return classifyException(cause);
        } catch (...) {
            return ErrorCategory::RUNTIME;
        }
    }
    if (dynamic_cast<const ParserException*>(&exception)) {
        return ErrorCategory::PARSER;
    }
    if (dynamic_cast<const BinderException*>(&exception) ||
        dynamic_cast<const CatalogException*>(&exception)) {
        return ErrorCategory::BINDER;
    }
    if (dynamic_cast<const ConstraintException*>(&exception) ||
        dynamic_cast<const CopyConstraintException*>(&exception)) {
        return ErrorCategory::CONSTRAINT;
    }
    if (dynamic_cast<const InterruptException*>(&exception)) {
        return ErrorCategory::INTERRUPT;
    }
    if (dynamic_cast<const IOException*>(&exception)) {
        return ErrorCategory::IO;
    }
    // Includes buffer-pool-full refusals and standard-library failures.
    return ErrorCategory::RUNTIME;
}

} // namespace common
} // namespace lbug
