#pragma once

#include <memory>
#include <string>

// Ladybug vendors mbedtls for SHA-256. An embedder that already ships BoringSSL or OpenSSL
// (for example Chromium) defines LBUG_USE_OPENSSL_SHA256 and provides <openssl/sha.h> to
// common/sha256.cpp. The header stays free of <openssl/sha.h>: its global SHA256() function
// would collide with this class wherever lbug::common is a using-directive.
#if !defined(LBUG_USE_OPENSSL_SHA256)
#include "mbedtls/sha256.h"
#endif

namespace lbug {
namespace common {

class SHA256 {
public:
    static constexpr size_t SHA256_HASH_LENGTH_BYTES = 32;
    static constexpr size_t SHA256_HASH_LENGTH_TEXT = 64;

public:
    SHA256();
    ~SHA256();
    void addString(const std::string& str);
    void finishSHA256(char* out);
    static void toBase16(const char* in, char* out, size_t len);

private:
#if defined(LBUG_USE_OPENSSL_SHA256)
    struct Context;
    std::unique_ptr<Context> shaContext;
#else
    typedef mbedtls_sha256_context SHA256Context;

    SHA256Context shaContext;
#endif
};

} // namespace common
} // namespace lbug
