#include "storage/wal/checksum_reader.h"

#include <cstring>

#include "common/checksum.h"
#include "common/exception/storage.h"
#include "common/file_system/file_info.h"
#include "common/serializer/buffered_file.h"
#include "common/serializer/deserializer.h"
#include <bit>

namespace lbug::storage {
static constexpr uint64_t INITIAL_BUFFER_SIZE = common::LBUG_PAGE_SIZE;

ChecksumReader::ChecksumReader(common::FileInfo& fileInfo, MemoryManager& memoryManager,
    std::string_view checksumMismatchMessage)
    : deserializer(std::make_unique<common::BufferedFileReader>(fileInfo)),
      fileSize(fileInfo.getFileSize()),
      entryBuffer(memoryManager.allocateBuffer(false, INITIAL_BUFFER_SIZE)),
      checksumMismatchMessage(checksumMismatchMessage) {}

static void resizeBufferIfNeeded(std::unique_ptr<MemoryBuffer>& entryBuffer, uint64_t currentSize,
    uint64_t requestedSize) {
    const auto currentBufferSize = entryBuffer->getBuffer().size_bytes();
    if (requestedSize > currentBufferSize) {
        auto* memoryManager = entryBuffer->getMemoryManager();
        auto newBuffer = memoryManager->allocateBuffer(false, std::bit_ceil(requestedSize));
        std::memcpy(newBuffer->getData(), entryBuffer->getData(), currentSize);
        entryBuffer = std::move(newBuffer);
    }
}

void ChecksumReader::read(uint8_t* data, uint64_t size) {
    if (verifiedOffset.has_value()) {
        if (size > *currentEntrySize - *verifiedOffset) {
            throw common::StorageException(std::string{checksumMismatchMessage});
        }
        std::memcpy(data, entryBuffer->getData() + *verifiedOffset, size);
        *verifiedOffset += size;
        return;
    }
    try {
        deserializer.read(data, size);
    } catch (const std::exception&) {
        if (currentEntrySize.has_value()) {
            throw common::StorageException(std::string{checksumMismatchMessage});
        }
        throw;
    }
    if (currentEntrySize.has_value()) {
        resizeBufferIfNeeded(entryBuffer, *currentEntrySize, *currentEntrySize + size);
        std::memcpy(entryBuffer->getData() + *currentEntrySize, data, size);
        *currentEntrySize += size;
    }
}

bool ChecksumReader::finished() {
    return deserializer.finished();
}

void ChecksumReader::onObjectBegin() {
    currentEntrySize.emplace(0);
}

void ChecksumReader::onObjectSize(uint64_t size) {
    DASSERT(currentEntrySize.has_value());
    // A corrupted size must not reach the allocator, or a read past the end of the file.
    const auto available = fileSize - getReadOffset();
    if (available < sizeof(uint64_t) || size > available - sizeof(uint64_t)) {
        throw common::StorageException(std::string{checksumMismatchMessage});
    }
    resizeBufferIfNeeded(entryBuffer, *currentEntrySize, *currentEntrySize + size);
    const auto entryStart = *currentEntrySize;
    deserializer.read(entryBuffer->getData() + entryStart, size);
    *currentEntrySize += size;
    verifyEntry();
    verifiedOffset.emplace(entryStart);
}

void ChecksumReader::onObjectEnd() {
    DASSERT(currentEntrySize.has_value());
    if (!verifiedOffset.has_value()) {
        verifyEntry();
    }
    verifiedOffset.reset();
    currentEntrySize.reset();
}

void ChecksumReader::verifyEntry() {
    const uint64_t computedChecksum = common::checksum(entryBuffer->getData(), *currentEntrySize);
    uint64_t storedChecksum{};
    deserializer.deserializeValue(storedChecksum);
    if (storedChecksum != computedChecksum) {
        throw common::StorageException(std::string{checksumMismatchMessage});
    }
}

uint64_t ChecksumReader::getReadOffset() const {
    const auto fileOffset =
        deserializer.getReader()->cast<common::BufferedFileReader>()->getReadOffset();
    if (!verifiedOffset.has_value()) {
        return fileOffset;
    }
    // The file reader is already past the entry and its checksum.
    return fileOffset - sizeof(uint64_t) - (*currentEntrySize - *verifiedOffset);
}

} // namespace lbug::storage
