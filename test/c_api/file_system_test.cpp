#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "c_api/lbug.h"
#include "graph_test/base_graph_test.h"
#include "gtest/gtest.h"

using namespace lbug::testing;

namespace {

// Keeps every file in memory, so anything in the engine that bypasses the callbacks either loses
// its data across a reopen or shows up on disk, where the tests look for it.
class MemoryFileSystem {
public:
    struct File {
        std::shared_ptr<std::vector<uint8_t>> bytes;
    };

    lbug_file_system callbacks() {
        lbug_file_system fs{};
        fs.user_data = this;
        fs.open_file = open;
        fs.close_file = close;
        fs.read_file = read;
        fs.write_file = write;
        fs.sync_file = sync;
        fs.truncate_file = truncate;
        fs.get_file_size = size;
        fs.path_exists = exists;
        fs.remove_file = remove;
        fs.rename_file = rename;
        fs.create_directory = createDirectory;
        fs.sync_parent_directory = syncParent;
        return fs;
    }

    bool hasFile(const std::string& path) {
        std::lock_guard lock{mutex};
        return files.contains(path);
    }

    bool failOpens = false;
    bool failWrites = false;
    uint64_t syncs = 0;

private:
    static MemoryFileSystem* self(void* userData) {
        return static_cast<MemoryFileSystem*>(userData);
    }

    static void* open(void* userData, const char* path, bool /*write*/, bool create, bool truncate,
        lbug_file_lock /*lock*/) {
        auto* fs = self(userData);
        std::lock_guard lock{fs->mutex};
        if (fs->failOpens) {
            return nullptr;
        }
        auto it = fs->files.find(path);
        if (it == fs->files.end()) {
            if (!create) {
                return nullptr;
            }
            it = fs->files.emplace(path, std::make_shared<std::vector<uint8_t>>()).first;
        } else if (truncate) {
            it->second->clear();
        }
        return new File{it->second};
    }

    static void close(void* /*userData*/, void* file) { delete static_cast<File*>(file); }

    static int64_t read(void* userData, void* file, void* buffer, uint64_t size, uint64_t offset) {
        auto* fs = self(userData);
        std::lock_guard lock{fs->mutex};
        auto& bytes = *static_cast<File*>(file)->bytes;
        if (offset >= bytes.size()) {
            return 0;
        }
        const auto count = std::min<uint64_t>(size, bytes.size() - offset);
        std::memcpy(buffer, bytes.data() + offset, count);
        return static_cast<int64_t>(count);
    }

    static bool write(void* userData, void* file, const void* buffer, uint64_t size,
        uint64_t offset) {
        auto* fs = self(userData);
        std::lock_guard lock{fs->mutex};
        if (fs->failWrites) {
            return false;
        }
        auto& bytes = *static_cast<File*>(file)->bytes;
        if (bytes.size() < offset + size) {
            bytes.resize(offset + size);
        }
        std::memcpy(bytes.data() + offset, buffer, size);
        return true;
    }

    static bool sync(void* userData, void* /*file*/) {
        auto* fs = self(userData);
        std::lock_guard lock{fs->mutex};
        fs->syncs++;
        return true;
    }

    static bool truncate(void* userData, void* file, uint64_t size) {
        auto* fs = self(userData);
        std::lock_guard lock{fs->mutex};
        static_cast<File*>(file)->bytes->resize(size);
        return true;
    }

    static int64_t size(void* userData, void* file) {
        auto* fs = self(userData);
        std::lock_guard lock{fs->mutex};
        return static_cast<int64_t>(static_cast<File*>(file)->bytes->size());
    }

    static bool exists(void* userData, const char* path) {
        auto* fs = self(userData);
        std::lock_guard lock{fs->mutex};
        return fs->files.contains(path) || fs->directories.contains(path);
    }

    static bool remove(void* userData, const char* path) {
        auto* fs = self(userData);
        std::lock_guard lock{fs->mutex};
        fs->files.erase(path);
        return true;
    }

    static bool rename(void* userData, const char* from, const char* to) {
        auto* fs = self(userData);
        std::lock_guard lock{fs->mutex};
        auto it = fs->files.find(from);
        if (it == fs->files.end()) {
            return false;
        }
        fs->files[to] = it->second;
        fs->files.erase(it);
        return true;
    }

    static bool createDirectory(void* userData, const char* path) {
        auto* fs = self(userData);
        std::lock_guard lock{fs->mutex};
        fs->directories.insert(path);
        return true;
    }

    static bool syncParent(void* /*userData*/, const char* /*path*/) { return true; }

    std::mutex mutex;
    std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files;
    std::set<std::string> directories;
};

class CApiFileSystemTest : public BaseGraphTest {
protected:
    std::string getInputDir() override { UNREACHABLE_CODE; }

    void SetUp() override {
        BaseGraphTest::SetUp();
        // The engine normalizes the path before handing it to the callbacks.
        path = (std::filesystem::path(databasePath) / "graph.lbdb").lexically_normal().string();
        config = lbug_default_system_config();
        config.buffer_pool_size = 64ull * 1024 * 1024;
        config.max_num_threads = 2;
        config.auto_checkpoint = false;
    }

    void openStore(MemoryFileSystem& fs) {
        auto callbacks = fs.callbacks();
        ASSERT_EQ(lbug_database_init_with_file_system(path.c_str(), config, &callbacks, &database),
            LbugSuccess);
        ASSERT_EQ(lbug_connection_init(&database, &connection), LbugSuccess);
    }

    void closeStore() {
        lbug_connection_destroy(&connection);
        lbug_database_destroy(&database);
    }

    void run(const std::string& statement) {
        lbug_query_result result;
        EXPECT_EQ(lbug_connection_query(&connection, statement.c_str(), &result), LbugSuccess)
            << statement;
        lbug_query_result_destroy(&result);
    }

    int64_t count() {
        lbug_query_result result;
        EXPECT_EQ(lbug_connection_query(&connection, "MATCH (i:Item) RETURN count(*);", &result),
            LbugSuccess);
        lbug_flat_tuple tuple;
        EXPECT_EQ(lbug_query_result_get_next(&result, &tuple), LbugSuccess);
        lbug_value value;
        EXPECT_EQ(lbug_flat_tuple_get_value(&tuple, 0, &value), LbugSuccess);
        int64_t rows = -1;
        EXPECT_EQ(lbug_value_get_int64(&value, &rows), LbugSuccess);
        lbug_value_destroy(&value);
        lbug_flat_tuple_destroy(&tuple);
        lbug_query_result_destroy(&result);
        return rows;
    }

    std::string path;
    lbug_system_config config{};
    lbug_database database{};
    lbug_connection connection{};
};

TEST_F(CApiFileSystemTest, TheEngineRunsEntirelyThroughTheCallbacks) {
    MemoryFileSystem fs;
    openStore(fs);
    run("CREATE NODE TABLE Item(id INT64, name STRING, PRIMARY KEY(id));");
    run("UNWIND range(1, 5000) AS i CREATE (:Item {id: i, name: 'item' + string(i)});");
    run("CHECKPOINT;");
    run("CREATE (:Item {id: 5001, name: 'after the checkpoint'});");
    EXPECT_EQ(count(), 5001);
    closeStore();

    EXPECT_TRUE(fs.hasFile(path));
    EXPECT_GT(fs.syncs, 0u);
    EXPECT_FALSE(std::filesystem::exists(path));

    openStore(fs);
    EXPECT_EQ(count(), 5001);
    closeStore();
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(CApiFileSystemTest, LogReplaysThroughTheCallbacks) {
    MemoryFileSystem fs;
    openStore(fs);
    run("CALL force_checkpoint_on_close=false;");
    run("CREATE NODE TABLE Item(id INT64, PRIMARY KEY(id));");
    run("CREATE (:Item {id: 1});");
    closeStore();
    EXPECT_TRUE(fs.hasFile(path + ".wal"));

    openStore(fs);
    EXPECT_EQ(count(), 1);
    closeStore();
}

TEST_F(CApiFileSystemTest, FailedCallbacksBecomeErrors) {
    MemoryFileSystem fs;
    fs.failOpens = true;
    auto callbacks = fs.callbacks();
    ASSERT_EQ(lbug_database_init_with_file_system(path.c_str(), config, &callbacks, &database),
        LbugError);
    EXPECT_EQ(database._database, nullptr);
    char* error = lbug_get_last_error();
    ASSERT_NE(error, nullptr);
    EXPECT_NE(std::string(error).find("Cannot open file"), std::string::npos) << error;
    lbug_destroy_string(error);

    fs.failOpens = false;
    openStore(fs);
    run("CREATE NODE TABLE Item(id INT64, PRIMARY KEY(id));");
    fs.failWrites = true;
    lbug_query_result result;
    EXPECT_EQ(lbug_connection_query(&connection, "CREATE (:Item {id: 1});", &result), LbugError);
    lbug_query_result_destroy(&result);
    fs.failWrites = false;
    closeStore();
}

TEST_F(CApiFileSystemTest, IncompleteCallbacksAreRefused) {
    MemoryFileSystem fs;
    auto callbacks = fs.callbacks();
    callbacks.sync_file = nullptr;
    EXPECT_EQ(lbug_database_init_with_file_system(path.c_str(), config, &callbacks, &database),
        LbugError);
    EXPECT_EQ(database._database, nullptr);
    EXPECT_EQ(lbug_database_init_with_file_system(path.c_str(), config, nullptr, &database),
        LbugError);
}

} // namespace
