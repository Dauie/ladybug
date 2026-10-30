#include "common/sha256.h"

#include "common/exception/runtime.h"

#if defined(LBUG_USE_OPENSSL_SHA256)
#include <openssl/sha.h>
#endif

namespace lbug {
namespace common {

#if defined(LBUG_USE_OPENSSL_SHA256)
struct SHA256::Context {
    SHA256_CTX ctx;
};
#endif

SHA256::SHA256() : shaContext{} {
#if defined(LBUG_USE_OPENSSL_SHA256)
    shaContext = std::make_unique<Context>();
    if (SHA256_Init(&shaContext->ctx) != 1) {
        throw RuntimeException{"SHA256 Error"};
    }
#else
    mbedtls_sha256_init(&shaContext);

    // These errors would only occur if there's an issue with shaContext which is wrapped inside
    // SHA256, or with the mbedtls library itself
    if (mbedtls_sha256_starts(&shaContext, false)) {
        throw RuntimeException{"SHA256 Error"};
    }
#endif
}

SHA256::~SHA256() {
#if !defined(LBUG_USE_OPENSSL_SHA256)
    mbedtls_sha256_free(&shaContext);
#endif
}

void SHA256::addString(const std::string& str) {
#if defined(LBUG_USE_OPENSSL_SHA256)
    if (SHA256_Update(&shaContext->ctx, str.data(), str.size()) != 1) {
        throw RuntimeException{"SHA256 Error"};
    }
#else
    if (mbedtls_sha256_update(&shaContext, reinterpret_cast<const unsigned char*>(str.data()),
            str.size())) {
        throw RuntimeException{"SHA256 Error"};
    }
#endif
}

void SHA256::finishSHA256(char* out) {
    std::string hash;
    hash.resize(SHA256_HASH_LENGTH_BYTES);

#if defined(LBUG_USE_OPENSSL_SHA256)
    if (SHA256_Final(reinterpret_cast<unsigned char*>(hash.data()), &shaContext->ctx) != 1) {
        throw RuntimeException{"SHA256 Error"};
    }
#else
    if (mbedtls_sha256_finish(&shaContext, reinterpret_cast<unsigned char*>(hash.data()))) {
        throw RuntimeException{"SHA256 Error"};
    }
#endif

    toBase16(hash.c_str(), out, SHA256_HASH_LENGTH_BYTES);
}

void SHA256::toBase16(const char* in, char* out, size_t len) {
    static char const HEX_CODES[] = "0123456789abcdef";
    size_t i = 0, j = 0;

    for (j = i = 0; i < len; i++) {
        int a = in[i];
        out[j++] = HEX_CODES[(a >> 4) & 0xf];
        out[j++] = HEX_CODES[a & 0xf];
    }
}

} // namespace common
} // namespace lbug
