#pragma once

#include <memory>
#include <optional>

#include "common/serializer/deserializer.h"
#include "common/serializer/reader.h"
#include "storage/buffer_manager/memory_manager.h"

namespace lbug {
namespace storage {
class ChecksumReader : public common::Reader {
public:
    explicit ChecksumReader(common::FileInfo& fileInfo, MemoryManager& memoryManager,
        std::string_view checksumMismatchMessage);

    void read(uint8_t* data, uint64_t size) override;
    bool finished() override;

    void onObjectBegin() override;
    // Reads the rest of the entry and verifies it, then serves it from memory. Nothing should
    // parse an entry's bytes before this: a corrupted entry can claim any element count, and
    // the deserializer zero-fills reads past a record's end, so a loop over that count would
    // never reach the end of the file.
    void onObjectSize(uint64_t size) override;
    // Verifies the entry that has just been read against the stored checksum, unless
    // onObjectSize already has.
    void onObjectEnd() override;

    uint64_t getReadOffset() const override;

private:
    void verifyEntry();

    common::Deserializer deserializer;
    uint64_t fileSize;

    std::optional<uint64_t> currentEntrySize;
    // The next byte to serve from entryBuffer, once an entry has been verified up front.
    std::optional<uint64_t> verifiedOffset;
    std::unique_ptr<MemoryBuffer> entryBuffer;

    std::string_view checksumMismatchMessage;
};
} // namespace storage
} // namespace lbug
