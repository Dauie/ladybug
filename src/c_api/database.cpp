#include <atomic>

#include "c_api/helpers.h"
#include "c_api/lbug.h"
#include "common/exception/exception.h"
#include "common/file_system/file_system.h"
#include "main/lbug.h"
using namespace lbug::main;
using namespace lbug::common;

static lbug_state initDatabase(const char* database_path, lbug_system_config config,
    const lbug_file_system* file_system, lbug_database* out_database) {
    if (out_database == nullptr) {
        return LbugError;
    }
    // Every error path, including exceptions the guard below catches, leaves this NULL.
    out_database->_database = nullptr;
    if (database_path == nullptr) {
        return LbugError;
    }
    LBUG_C_API_GUARD_BEGIN
    try {
        clearLastCAPIErrorMessage();
        std::string database_path_str = database_path;
        auto systemConfig = SystemConfig(config.buffer_pool_size, config.max_num_threads,
            config.enable_compression, config.read_only, config.max_db_size, config.auto_checkpoint,
            config.checkpoint_threshold, true, config.throw_on_wal_replay_failure,
            config.enable_checksums, config.enable_multi_writes, config.enable_default_hash_index,
            config.physical_plan_pool_per_thread);

#if defined(__APPLE__)
        systemConfig.threadQos = config.thread_qos;
#endif
        out_database->_database =
            file_system != nullptr ?
                new Database(database_path_str, systemConfig,
                    createCallbackFileSystem(*file_system)) :
                new Database(database_path_str, systemConfig);
    } catch (Exception& e) {
        out_database->_database = nullptr;
        setLastCAPIErrorMessage(e.what());
        return LbugError;
    }
    return LbugSuccess;
    LBUG_C_API_GUARD_END(LbugError)
}

lbug_state lbug_database_init(const char* database_path, lbug_system_config config,
    lbug_database* out_database) {
    return initDatabase(database_path, config, nullptr, out_database);
}

lbug_state lbug_database_init_with_file_system(const char* database_path,
    lbug_system_config config, const lbug_file_system* file_system, lbug_database* out_database) {
    if (out_database == nullptr) {
        return LbugError;
    }
    out_database->_database = nullptr;
    if (file_system == nullptr || !isCompleteFileSystem(*file_system)) {
        setLastCAPIErrorMessage("lbug_file_system needs every callback.");
        return LbugError;
    }
    return initDatabase(database_path, config, file_system, out_database);
}

void lbug_database_destroy(lbug_database* database) {
    LBUG_C_API_GUARD_BEGIN
    if (database == nullptr) {
        return;
    }
    std::atomic_ref<void*> atomic_db(database->_database);
    void* ptr = atomic_db.exchange(nullptr);
    if (ptr != nullptr) {
        delete static_cast<Database*>(ptr);
    }
    LBUG_C_API_GUARD_END_VOID
}

lbug_system_config lbug_default_system_config() {
    LBUG_C_API_GUARD_BEGIN
    SystemConfig config = SystemConfig();
    auto cSystemConfig = lbug_system_config();
    cSystemConfig.buffer_pool_size = config.bufferPoolSize;
    cSystemConfig.max_num_threads = config.maxNumThreads;
    cSystemConfig.enable_compression = config.enableCompression;
    cSystemConfig.read_only = config.readOnly;
    cSystemConfig.max_db_size = config.maxDBSize;
    cSystemConfig.auto_checkpoint = config.autoCheckpoint;
    cSystemConfig.checkpoint_threshold = config.checkpointThreshold;
    cSystemConfig.throw_on_wal_replay_failure = config.throwOnWalReplayFailure;
    cSystemConfig.enable_checksums = config.enableChecksums;
    cSystemConfig.enable_multi_writes = config.enableMultiWrites;
    cSystemConfig.enable_default_hash_index = config.enableDefaultHashIndex;
    cSystemConfig.physical_plan_pool_per_thread = config.physicalPlanPoolPerThread;
#if defined(__APPLE__)
    cSystemConfig.thread_qos = config.threadQos;
#endif
    return cSystemConfig;
    LBUG_C_API_GUARD_END(lbug_system_config{})
}
