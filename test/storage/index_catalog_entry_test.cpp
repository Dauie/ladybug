#include <cstring>
#include <memory>
#include <string>

#include "catalog/catalog_entry/index_catalog_entry.h"
#include "common/serializer/buffer_reader.h"
#include "common/serializer/buffer_writer.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "gtest/gtest.h"

using namespace lbug::catalog;
using namespace lbug::common;

namespace {

constexpr char kOptions[] = "an extension's index options";

// Stands in for an extension's index, whose options the engine stores as bytes until the
// extension loads.
struct ExtensionAuxInfo final : IndexAuxInfo {
    std::shared_ptr<BufferWriter> serialize() const override {
        auto writer = std::make_shared<BufferWriter>();
        writer->write(reinterpret_cast<const uint8_t*>(kOptions), sizeof(kOptions));
        return writer;
    }
    std::unique_ptr<IndexAuxInfo> copy() override { return std::make_unique<ExtensionAuxInfo>(); }
    std::string toCypher(const IndexCatalogEntry&, const ToCypherInfo&) const override {
        return "";
    }
};

std::unique_ptr<IndexCatalogEntry> roundTrip(const IndexCatalogEntry& entry) {
    auto writer = std::make_shared<BufferWriter>();
    Serializer serializer(writer);
    entry.serialize(serializer);
    Deserializer deserializer(
        std::make_unique<BufferReader>(writer->getBlobData(), writer->getSize()));
    auto read = CatalogEntry::deserialize(deserializer);
    return std::unique_ptr<IndexCatalogEntry>(read.release()->ptrCast<IndexCatalogEntry>());
}

std::string options(const IndexCatalogEntry& entry) {
    auto reader = entry.getAuxBufferReader();
    std::string bytes(sizeof(kOptions), '\0');
    reader->read(reinterpret_cast<uint8_t*>(bytes.data()), bytes.size());
    return bytes;
}

} // namespace

// WAL replay copies every index entry it replays. An entry whose extension hasn't loaded it yet
// has no deserialized options, and copying it used to dereference them.
TEST(IndexCatalogEntryTest, CopiesAnEntryWhoseExtensionHasNotLoadedIt) {
    IndexCatalogEntry created{"EXTENSION_TYPE", 3 /* tableID */, "idx", {1} /* propertyIDs */,
        std::make_unique<ExtensionAuxInfo>()};
    auto unloaded = roundTrip(created);
    ASSERT_FALSE(unloaded->isLoaded());

    auto copied = unloaded->copy();

    ASSERT_FALSE(copied->isLoaded());
    EXPECT_EQ(copied->getIndexType(), "EXTENSION_TYPE");
    EXPECT_EQ(copied->getTableID(), 3u);
    EXPECT_EQ(copied->getIndexName(), "idx");
    EXPECT_EQ(options(*copied), std::string(kOptions, sizeof(kOptions)));
    // The copy stays readable by the extension once it loads.
    EXPECT_EQ(options(*roundTrip(*copied)), std::string(kOptions, sizeof(kOptions)));
}

TEST(IndexCatalogEntryTest, CopiesALoadedEntry) {
    IndexCatalogEntry created{"EXTENSION_TYPE", 3 /* tableID */, "idx", {1} /* propertyIDs */,
        std::make_unique<ExtensionAuxInfo>()};

    auto copied = created.copy();

    ASSERT_TRUE(copied->isLoaded());
    EXPECT_NE(dynamic_cast<const ExtensionAuxInfo*>(&copied->getAuxInfo()), nullptr);
}
