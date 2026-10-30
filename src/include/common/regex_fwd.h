#pragma once

// Forward declaration of lbug::regex::RE2 for headers; see common/regex.h.
#if defined(LBUG_USE_EXTERNAL_RE2)
namespace re2 {
class RE2;
} // namespace re2
namespace lbug::regex {
using ::re2::RE2;
} // namespace lbug::regex
#else
namespace lbug::regex {
class RE2;
} // namespace lbug::regex
#endif
