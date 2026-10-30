#pragma once

#include <format>
#include <string_view>

#include "common/exception/binder.h"

namespace lbug {
namespace common {

// Embedders that run the engine in a sandbox configure CMake with ENABLE_EXTERNAL_ACCESS=OFF,
// which defines LBUG_NO_EXTERNAL_ACCESS. Such a build has no way to read or write files
// outside the database directory, reach the network, or load native code: the statements
// and functions that would do so are refused, and the extension installer (HTTP client and
// OpenSSL) is not built. Statically linked extensions still load.
inline constexpr bool EXTERNAL_ACCESS_ENABLED =
#if defined(LBUG_NO_EXTERNAL_ACCESS)
    false;
#else
    true;
#endif

inline void throwIfExternalAccessDisabled(std::string_view feature) {
    if constexpr (!EXTERNAL_ACCESS_ENABLED) {
        throw BinderException(std::format("{} is not available in this build: it reads or "
                                          "writes outside the database, uses the network or "
                                          "loads native code.",
            feature));
    }
}

} // namespace common
} // namespace lbug
