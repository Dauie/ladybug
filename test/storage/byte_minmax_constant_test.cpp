#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "common/types/types.h"
#include "gtest/gtest.h"
#include "storage/compression/compression.h"
#include "storage/table/column_chunk_metadata.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace testing {

// Regression tests for gh-1447. MSVC's vectorized std::minmax_element can return
// wrong elements for some 1-byte arrays (for example 8192 copies of "ab" followed
// by "second" reports max == 'a'). The flush-time min/max fold used it, so a
// non-constant UINT8/INT8 chunk (including the byte data of every STRING column)
// could get bounds that miss its values: with min == max it was stored as
// CONSTANT, and a too-narrow range truncated values in the bit-packing frame.

namespace {

std::vector<uint8_t> repeatedThenSuffix(std::string_view unit, size_t repeats,
    std::string_view suffix) {
    std::vector<uint8_t> bytes;
    bytes.reserve(unit.size() * repeats + suffix.size());
    for (size_t i = 0; i < repeats; ++i) {
        bytes.insert(bytes.end(), unit.begin(), unit.end());
    }
    bytes.insert(bytes.end(), suffix.begin(), suffix.end());
    return bytes;
}

template<typename T>
std::vector<T> asType(const std::vector<uint8_t>& bytes) {
    std::vector<T> values(bytes.size());
    std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}

template<typename T>
std::pair<T, T> scalarMinMax(const std::vector<T>& values) {
    T min = values[0];
    T max = values[0];
    for (const auto value : values) {
        min = value < min ? value : min;
        max = value > max ? value : max;
    }
    return {min, max};
}

template<typename T>
std::pair<T, T> storageMinMax(const std::vector<T>& values, PhysicalTypeID physicalType) {
    auto [min, max] = getMinMaxStorageValue(reinterpret_cast<const uint8_t*>(values.data()), 0,
        values.size(), physicalType, nullptr, true);
    EXPECT_TRUE(min.has_value());
    EXPECT_TRUE(max.has_value());
    return {min->get<T>(), max->get<T>()};
}

template<typename T>
std::span<const uint8_t> toBytes(const std::vector<T>& values) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(values.data()),
        values.size() * sizeof(T));
}

// The production flush decision: getMinMaxStorageValue followed by the top-level
// GetCompressionMetadata dispatcher, as in ColumnChunkData::getMetadataToFlush.
template<typename T>
ColumnChunkMetadata decideMetadata(const std::vector<T>& values, const LogicalType& dataType,
    std::shared_ptr<CompressionAlg> alg) {
    auto [min, max] = getMinMaxStorageValue(reinterpret_cast<const uint8_t*>(values.data()), 0,
        values.size(), dataType.getPhysicalType(), nullptr, true);
    return GetCompressionMetadata(std::move(alg), dataType)(toBytes(values), values.size(),
        min.value_or(StorageValue{}), max.value_or(StorageValue{}));
}

// Named inputs whose wrong results were observed with MSVC 19.44 (VS 2022 17.14).
std::vector<std::vector<uint8_t>> knownByteInputs() {
    return {
        repeatedThenSuffix("ab", 8192, "second"),
        repeatedThenSuffix("ab", 8192, "0123456789"),
        repeatedThenSuffix("\xc3\xa9", 8192, "second"),
        repeatedThenSuffix("\xc3\xa9", 4096, "second"),
    };
}

// Seeded inputs: a long two-value alternating body (the shape that exposed the bug)
// followed by a short random tail, plus fully random arrays.
std::vector<std::vector<uint8_t>> seededByteInputs() {
    std::mt19937_64 rng(1447);
    std::vector<std::vector<uint8_t>> inputs;
    for (int i = 0; i < 400; ++i) {
        const size_t body = 1 + rng() % 20000;
        const size_t tail = rng() % 40;
        std::vector<uint8_t> bytes(body + tail);
        const auto a = static_cast<uint8_t>(rng());
        const auto b = static_cast<uint8_t>(rng());
        const bool random = i % 4 == 3;
        for (size_t j = 0; j < body; ++j) {
            bytes[j] = random ? static_cast<uint8_t>(rng()) : ((j & 1) ? b : a);
        }
        for (size_t j = body; j < bytes.size(); ++j) {
            bytes[j] = static_cast<uint8_t>(rng());
        }
        inputs.push_back(std::move(bytes));
    }
    return inputs;
}

} // namespace

TEST(ByteMinMaxConstantTests, UInt8MinMaxMatchesScalarFold) {
    for (const auto& bytes : knownByteInputs()) {
        EXPECT_EQ(storageMinMax(bytes, PhysicalTypeID::UINT8), scalarMinMax(bytes))
            << "length " << bytes.size();
    }
    for (const auto& bytes : seededByteInputs()) {
        EXPECT_EQ(storageMinMax(bytes, PhysicalTypeID::UINT8), scalarMinMax(bytes))
            << "length " << bytes.size();
    }
}

TEST(ByteMinMaxConstantTests, Int8MinMaxMatchesScalarFold) {
    for (const auto& bytes : knownByteInputs()) {
        const auto values = asType<int8_t>(bytes);
        EXPECT_EQ(storageMinMax(values, PhysicalTypeID::INT8), scalarMinMax(values))
            << "length " << values.size();
    }
    for (const auto& bytes : seededByteInputs()) {
        const auto values = asType<int8_t>(bytes);
        EXPECT_EQ(storageMinMax(values, PhysicalTypeID::INT8), scalarMinMax(values))
            << "length " << values.size();
    }
}

TEST(ByteMinMaxConstantTests, StringDataChunkIsNotConstant) {
    // A STRING column's dictionary bytes live in an uncompressed UINT8 chunk; this is
    // the exact buffer of "ab" * 8192 followed by a second row holding "second".
    const auto bytes = repeatedThenSuffix("ab", 8192, "second");
    const auto metadata = decideMetadata(bytes, LogicalType::UINT8(),
        std::make_shared<Uncompressed>(LogicalType::UINT8()));
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(ByteMinMaxConstantTests, UInt8ColumnChunkIsNotConstant) {
    const auto bytes = repeatedThenSuffix("ab", 8192, "second");
    const auto metadata = decideMetadata(bytes, LogicalType::UINT8(),
        std::make_shared<IntegerBitpacking<uint8_t>>());
    EXPECT_NE(metadata.compMeta.compression, CompressionType::CONSTANT);
}

TEST(ByteMinMaxConstantTests, TrueConstantChunksStillUseConstant) {
    // Guard against overcorrection: uniform chunks must still use CONSTANT.
    const std::vector<uint8_t> bytes(16390, 'a');
    const auto byteMetadata = decideMetadata(bytes, LogicalType::UINT8(),
        std::make_shared<Uncompressed>(LogicalType::UINT8()));
    EXPECT_EQ(byteMetadata.compMeta.compression, CompressionType::CONSTANT);
    EXPECT_EQ(byteMetadata.compMeta.min.get<uint8_t>(), uint8_t{'a'});
    const std::vector<int64_t> wide(1024, 7);
    const auto wideMetadata =
        decideMetadata(wide, LogicalType::INT64(), std::make_shared<IntegerBitpacking<int64_t>>());
    EXPECT_EQ(wideMetadata.compMeta.compression, CompressionType::CONSTANT);
    EXPECT_EQ(wideMetadata.compMeta.min.get<int64_t>(), 7);
}

} // namespace testing
} // namespace lbug
