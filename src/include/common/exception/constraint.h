#pragma once

#include "copy.h"
#include "runtime.h"

namespace lbug::common {

// Preserve the existing messages and base-class catches while making constraint
// failures distinguishable from unrelated runtime and COPY failures.
class LBUG_API ConstraintException : public RuntimeException {
public:
    explicit ConstraintException(const std::string& message) : RuntimeException(message) {}
};

class LBUG_API CopyConstraintException : public CopyException {
public:
    explicit CopyConstraintException(const std::string& message) : CopyException(message) {}
};

} // namespace lbug::common
