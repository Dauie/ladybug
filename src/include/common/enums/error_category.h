#pragma once

#include <cstdint>
#include <exception>

#include "common/api.h"

namespace lbug::common {

// Kept in step with the additive C API enum; never classify by error-message text.
enum class ErrorCategory : uint8_t {
    NONE = 0,
    PARSER = 1,
    BINDER = 2,
    RUNTIME = 3,
    CONSTRAINT = 4,
    INTERRUPT = 5,
    IO = 6,
};

LBUG_API ErrorCategory classifyException(const std::exception& exception) noexcept;

} // namespace lbug::common
