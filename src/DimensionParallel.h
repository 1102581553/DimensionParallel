#pragma once
#include <ll/api/mod/NativeMod.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <queue>
#include <string>
#include <mc/legacy/ActorUniqueID.h>
#include <mc/world/level/dimension/Dimension.h>
#include <mc/world/actor/player/Player.h>

// 需要包含 concurrentqueue 头文件，可从 https://github.com/cameron314/concurrentqueue 获取
#include "concurrentqueue.h"

namespace dimension_parallel {

// 前向声明
class DimensionWorker;
class DimensionThreadManager;
class CrossDimensionSync;
class ConfigManager;

// 主模组类
class DimensionParallelMod : public ll::mod::NativeMod {
public:
    static DimensionParallelMod& getInstance();

    bool load() override;
    bool enable() override;
    bool disable() override;

    DimensionThreadManager& getThreadManager() { return *mThreadManager; }
    CrossDimensionSync& getSyncManager() { return *mSyncManager; }
    ConfigManager& getConfigManager() { return *mConfigManager; }

private:
    DimensionParallelMod();

    std::unique_ptr<DimensionThreadManager> mThreadManager;
    std::unique_ptr<CrossDimensionSync> mSyncManager;
    std::unique_ptr<ConfigManager> mConfigManager;
};

// 配置管理器（简化）
class ConfigManager {
public:
    static ConfigManager& getInstance();

    bool loadConfig();
    bool saveConfig();

    struct ModConfig {
        bool enableParallelTicking = true;
        int globalThreadCount = 0;          // 0 = auto
        int tickTimeoutMs = 100;
        bool enablePerformanceMonitoring = true;
        int logLevel = 2;                   // 0=Trace,1=Debug,2=Info,3=Warn,4=Error
        bool logTickTimes = true;
    };

    const ModConfig& getConfig() const { return mConfig; }
    ModConfig& getConfig() { return mConfig; }

private:
    ConfigManager() = default;
    ModConfig mConfig;
    std::string mConfigPath = "plugins/DimensionParallel/config.json";
};

// 维度统计
struct DimensionStats {
    std::atomic<uint64_t> totalTicks{0};
    std::atomic<uint64_t> totalTickTime{0};
    std::atomic<uint64_t> maxTickTime{0};

    void recordTick(uint64_t microseconds);
    double getAverageTickTime() const;
};

// 维度工作线程
class DimensionWorker {
public:
    DimensionWorker(int id);
    ~DimensionWorker();

    void start();
    void stop();
    void join();

    void submitTask(std::function<void()>&& task);
    void waitForTickCompletion();

    int getId() const { return mId; }
    const DimensionStats& getStats() const { return mStats; }

private:
    void workerThread();
    void processTasks();

    int mId;
    std::thread mThread;
    std::atomic<bool> mRunning{false};

    moodycamel::ConcurrentQueue<std::function<void()>> mTaskQueue;
    std::mutex mCvMutex;
    std::condition_variable mCv;
    std::atomic<bool> mHasTask{false};

    std::atomic<size_t> mPendingTasks{0};
    std::condition_variable mCompletionCv;
    std::mutex mCompletionMutex;

    DimensionStats mStats;
};

// 维度线程管理器
class DimensionThreadManager {
public:
    static DimensionThreadManager& getInstance();

    void initialize();
    void shutdown();

    void registerDimension(int id);
    void unregisterDimension(int id);
    bool isDimensionRegistered(int id) const;

    void tickAllDimensions();

    const DimensionStats& getDimensionStats(int id) const;

private:
    DimensionThreadManager() = default;
    ~DimensionThreadManager() = default;

    std::unordered_map<int, std::unique_ptr<DimensionWorker>> mWorkers;
    mutable std::shared_mutex mWorkersMutex;
    std::atomic<bool> mInitialized{false};
};

// 跨维度同步
class CrossDimensionSync {
public:
    static CrossDimensionSync& getInstance();

    void registerEntity(class Actor* entity, int dim);
    void unregisterEntity(ActorUniqueID id);
    int getEntityDimension(ActorUniqueID id);

    void queueTeleportRequest(class Player* player, int from, int to);
    void processPendingTeleports();
    void onPlayerTeleported(class Player* player, int from, int to);

    void processPendingOperations();

private:
    CrossDimensionSync() = default;

    std::unordered_map<ActorUniqueID, int> mEntityDimensionMap;
    std::shared_mutex mEntityMapMutex;

    struct TeleportRequest {
        Player* player;
        int fromDim;
        int toDim;
        Vec3 targetPos;
        std::function<void()> callback;
    };
    std::queue<TeleportRequest> mTeleportQueue;
    std::mutex mTeleportMutex;
};

// 钩子安装
void installHooks();

} // namespace dimension_parallel
