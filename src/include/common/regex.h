#pragma once

// Selects the regular-expression library behind lbug::regex. Ladybug vendors an older re2
// renamed to lbug::regex. An embedder that already ships re2 (for example Chromium, which
// requires dependencies it already has to be reused) defines LBUG_USE_EXTERNAL_RE2 and
// provides <re2/re2.h>; lbug::regex then names that copy.
#if defined(LBUG_USE_EXTERNAL_RE2)
#include "re2/re2.h"
#include "re2/stringpiece.h"

namespace lbug::regex {
using ::re2::RE2;
using ::re2::StringPiece;
} // namespace lbug::regex
#else
#include "re2.h"
#endif
