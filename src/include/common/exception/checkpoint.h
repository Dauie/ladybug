#pragma once

#include <string>
#include <utility>

#include "common/api.h"
#include "exception.h"

namespace lbug {
namespace common {

// Keep the original category when adding checkpoint context to an active exception.
class LBUG_API CheckpointException : public Exception, public std::nested_exception {
public:
    explicit CheckpointException(const std::exception& e) : Exception(e.what()) {};
    explicit CheckpointException(std::string message) : Exception(std::move(message)) {};
};

} // namespace common
} // namespace lbug
