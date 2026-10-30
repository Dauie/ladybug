#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "c_api/helpers.h"
#include "c_api/lbug.h"
#include "common/exception/io.h"
#include "common/file_system/file_info.h"
#include "common/file_system/file_system.h"

using namespace lbug::common;

namespace {

// Holds the embedder's handle; the embedder closes it when the engine drops the file.
struct CallbackFileInfo final : public FileInfo {
    CallbackFileInfo(std::string path, FileSystem* fileSystem, const lbug_file_system& callbacks,
        void* file)
        : FileInfo{std::move(path), fileSystem}, callbacks{callbacks}, file{file} {}
    ~CallbackFileInfo() override { callbacks.close_file(callbacks.user_data, file); }

    const lbug_file_system& callbacks;
    void* file;
    // Sequential reads through readFile and seek. Reads and writes at an offset don't use it.
    uint64_t position = 0;
};

// Adapts the C callback table to the engine's FileSystem. The callbacks report failure with
// return values, because they run in embedders without exceptions; this turns those into the
// exceptions the engine expects.
class CallbackFileSystem final : public FileSystem, public DirectorySyncFileSystem {
public:
    explicit CallbackFileSystem(const lbug_file_system& callbacks) : callbacks{callbacks} {}

    std::unique_ptr<FileInfo> openFile(const std::string& path, FileOpenFlags flags,
        lbug::main::ClientContext* /*context*/) override {
        const bool write = flags.flags & FileFlags::WRITE;
        const bool truncate = flags.flags & FileFlags::CREATE_AND_TRUNCATE_IF_EXISTS;
        const bool create = truncate || (flags.flags & FileFlags::CREATE_IF_NOT_EXISTS);
        const auto lock = static_cast<lbug_file_lock>(flags.lockType);
        void* file =
            callbacks.open_file(callbacks.user_data, path.c_str(), write, create, truncate, lock);
        if (file == nullptr) {
            throw IOException(std::format("Cannot open file {}.", path));
        }
        return std::make_unique<CallbackFileInfo>(path, this, callbacks, file);
    }

    // The engine globs only for the external file scans, which embedders don't get here.
    std::vector<std::string> glob(lbug::main::ClientContext* /*context*/,
        const std::string& /*path*/) const override {
        return {};
    }

    void overwriteFile(const std::string& from, const std::string& to) override {
        renameFile(from, to);
    }

    void renameFile(const std::string& from, const std::string& to) override {
        if (!callbacks.rename_file(callbacks.user_data, from.c_str(), to.c_str())) {
            throw IOException(std::format("Cannot rename file {} to {}.", from, to));
        }
    }

    void createDir(const std::string& dir) const override {
        if (!callbacks.create_directory(callbacks.user_data, dir.c_str())) {
            throw IOException(std::format("Cannot create directory {}.", dir));
        }
    }

    void removeFileIfExists(const std::string& path,
        const lbug::main::ClientContext* /*context*/) override {
        if (!callbacks.remove_file(callbacks.user_data, path.c_str())) {
            throw IOException(std::format("Cannot remove file {}.", path));
        }
    }

    bool fileOrPathExists(const std::string& path,
        lbug::main::ClientContext* /*context*/) override {
        return callbacks.path_exists(callbacks.user_data, path.c_str());
    }

    bool canHandleFile(const std::string_view /*path*/) const override { return true; }

    void syncFile(const FileInfo& fileInfo) const override {
        auto& info = fileInfo.constCast<CallbackFileInfo>();
        if (!callbacks.sync_file(callbacks.user_data, info.file)) {
            throw IOException(std::format("Cannot sync file {}.", fileInfo.path));
        }
    }

    void syncParentDirectory(const std::string& path) const override {
        if (!callbacks.sync_parent_directory(callbacks.user_data, path.c_str())) {
            throw IOException(std::format("Cannot sync the directory of {}.", path));
        }
    }

protected:
    void readFromFile(FileInfo& fileInfo, void* buffer, uint64_t numBytes,
        uint64_t position) const override {
        auto& info = fileInfo.cast<CallbackFileInfo>();
        auto* out = static_cast<uint8_t*>(buffer);
        uint64_t done = 0;
        while (done < numBytes) {
            const int64_t count = callbacks.read_file(callbacks.user_data, info.file, out + done,
                numBytes - done, position + done);
            if (count <= 0) {
                throw IOException(std::format("Cannot read {} bytes at offset {} from file {}.",
                    numBytes, position, fileInfo.path));
            }
            done += static_cast<uint64_t>(count);
        }
    }

    int64_t readFile(FileInfo& fileInfo, void* buf, size_t numBytes) const override {
        auto& info = fileInfo.cast<CallbackFileInfo>();
        const int64_t count = callbacks.read_file(callbacks.user_data, info.file, buf, numBytes,
            info.position);
        if (count < 0) {
            throw IOException(std::format("Cannot read from file {}.", fileInfo.path));
        }
        info.position += static_cast<uint64_t>(count);
        return count;
    }

    void writeFile(FileInfo& fileInfo, const uint8_t* buffer, uint64_t numBytes,
        uint64_t offset) const override {
        auto& info = fileInfo.cast<CallbackFileInfo>();
        if (!callbacks.write_file(callbacks.user_data, info.file, buffer, numBytes, offset)) {
            throw IOException(std::format("Cannot write {} bytes at offset {} to file {}.",
                numBytes, offset, fileInfo.path));
        }
    }

    int64_t seek(FileInfo& fileInfo, uint64_t offset, int whence) const override {
        auto& info = fileInfo.cast<CallbackFileInfo>();
        switch (whence) {
        case SEEK_SET:
            info.position = offset;
            break;
        case SEEK_CUR:
            info.position += offset;
            break;
        case SEEK_END:
            info.position = getFileSize(fileInfo) + offset;
            break;
        default:
            throw IOException(std::format("Invalid seek origin for file {}.", fileInfo.path));
        }
        return static_cast<int64_t>(info.position);
    }

    void reset(FileInfo& fileInfo) override { fileInfo.cast<CallbackFileInfo>().position = 0; }

    void truncate(FileInfo& fileInfo, uint64_t size) const override {
        auto& info = fileInfo.cast<CallbackFileInfo>();
        if (!callbacks.truncate_file(callbacks.user_data, info.file, size)) {
            throw IOException(std::format("Cannot truncate file {}.", fileInfo.path));
        }
    }

    uint64_t getFileSize(const FileInfo& fileInfo) const override {
        auto& info = fileInfo.constCast<CallbackFileInfo>();
        const int64_t size = callbacks.get_file_size(callbacks.user_data, info.file);
        if (size < 0) {
            throw IOException(std::format("Cannot read the size of file {}.", fileInfo.path));
        }
        return static_cast<uint64_t>(size);
    }

private:
    const lbug_file_system callbacks;
};

} // namespace

bool isCompleteFileSystem(const lbug_file_system& fs) {
    return fs.open_file && fs.close_file && fs.read_file && fs.write_file && fs.sync_file &&
           fs.truncate_file && fs.get_file_size && fs.path_exists && fs.remove_file &&
           fs.rename_file && fs.create_directory && fs.sync_parent_directory;
}

std::unique_ptr<FileSystem> createCallbackFileSystem(const lbug_file_system& fileSystem) {
    return std::make_unique<CallbackFileSystem>(fileSystem);
}
