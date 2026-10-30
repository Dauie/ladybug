#pragma once

// Selects the floating-point parser behind lbug_fast_float::from_chars. Ladybug vendors
// DuckDB's fork of fast_float. An embedder that already ships upstream fast_float defines
// LBUG_USE_EXTERNAL_FAST_FLOAT and provides <fast_float/fast_float.h>; Ladybug only calls the
// three-argument from_chars, which both provide.
#if defined(LBUG_USE_EXTERNAL_FAST_FLOAT)
#include "fast_float/fast_float.h"

namespace lbug_fast_float = ::fast_float;
#else
#include "fast_float.h"
#endif
