#pragma once
#include <ll/api/mod/NativeMod.h>
#include <ll/api/mod/Manifest.h>
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
#include <mc/math/Vec3.h>
#include <gsl/gsl>

#include <moodycamel/concurrentqueue.h>

namespace dimension_parallel {

// ==================== 前向声明 ====================
class DimensionWorker;
class DimensionThreadManager;
class CrossDimensionSync;
class ConfigManager;

// ==================== 主模组类 ====================
class DimensionParallelMod : public ll::mod::NativeMod {
public:
    explicit DimensionParallelMod(ll::mod::Manifest const& manifest);

    static DimensionParallelMod& getInstance();

    bool load() override;
    bool enable() override;
    void disable() override;

    DimensionThreadManager& getThreadManager() { return *mThreadManager; }
    CrossDimensionSync& getSyncManager() { return *mSyncManager; }
    ConfigManager& getConfigManager() { return *mConfigManager; }

    ~DimensionParallelMod() override = default;

private:
    std::unique_ptr<DimensionThreadManager> mThreadManager;
    std::unique_ptr<CrossDimensionSync> mSyncManager;
    std::unique_ptr<ConfigManager> mConfigManager;
    std::atomic<bool> mEnabled{false};
    
    ll::event::ListenerId mServerStartedListener;
    ll::event::ListenerId mServerStoppingListener;
};

// ==================== 配置管理器 ====================
class ConfigManager {
public:
    static ConfigManager& getInstance();

    bool loadConfig();
    bool saveConfig();

    struct ModConfig {
        bool enableParallelTicking = true;
        int globalThreadCount = 0;
        int tickTimeoutMs = 100;
        bool enablePerformanceMonitoring = true;
        int logLevel = 2;
        bool logTickTimes = true;
    };

    const ModConfig& getConfig() const { return mConfig; }
    ModConfig& getConfig() { return mConfig; }

private:
    ConfigManager() = default;
    ModConfig mConfig;
    std::string mConfigPath = "plugins/DimensionParallel/config.json";
};

// ==================== 维度统计 ====================
struct DimensionStats {
    std::atomic<uint64_t> totalTicks{0};
    std::atomic<uint64_t> totalTickTime{0};
    std::atomic<uint64_t> maxTickTime{0};
    std::atomic<uint64_t> slowTickCount{0};

    void recordTick(uint64_t microseconds, uint64_t threshold = 50000);
    double getAverageTickTime() const;
    uint64_t getMaxTickTime() const { return maxTickTime.load(); }
    uint64_t getSlowTickCount() const { return slowTickCount.load(); }
    void reset();
};

// ==================== 维度工作线程 ====================
class DimensionWorker {
public:
    explicit DimensionWorker(int id);
    ~DimensionWorker();

    void start();
    void stop();
    void join();

    void submitTask(std::function<void()>&& task);
    bool waitForTickCompletion(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    int getId() const { return mId; }
    const DimensionStats& getStats() const { return mStats; }
    bool isRunning() const { return mRunning.load(); }

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

// ==================== 维度线程管理器 ====================
class DimensionThreadManager {
public:
    static DimensionThreadManager& getInstance();

    void initialize();
    void shutdown();

    void registerDimension(int id);
    void unregisterDimension(int id);
    bool isDimensionRegistered(int id) const;

    void tickAllDimensions();
    bool tickAllDimensionsWithTimeout(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    const DimensionStats& getDimensionStats(int id) const;
    size_t getWorkerCount() const;

private:
    DimensionThreadManager() = default;
    ~DimensionThreadManager() = default;

    std::unordered_map<int, std::unique_ptr<DimensionWorker>> mWorkers;
    mutable std::shared_mutex mWorkersMutex;
    std::atomic<bool> mInitialized{false};
};

// ==================== 跨维度同步 ====================
class CrossDimensionSync {
public:
    static CrossDimensionSync& getInstance();

    void registerEntity(class Actor* entity, int dim);
    void unregisterEntity(class ActorUniqueID id);
    int getEntityDimension(class ActorUniqueID id) const;

    void queueTeleportRequest(class Player* player, int from, int to, class Vec3 const& targetPos);
    void processPendingTeleports();
    void onPlayerTeleported(class Player* player, int from, int to);

    void processPendingOperations();
    size_t getPendingTeleportCount() const;

private:
    CrossDimensionSync() = default;

    std::unordered_map<ActorUniqueID, int> mEntityDimensionMap;
    mutable std::shared_mutex mEntityMapMutex;

    struct TeleportRequest {
        ActorUniqueID playerId;
        int fromDim;
        int toDim;
        Vec3 targetPos;
        std::chrono::steady_clock::time_point createTime;
        std::function<void()> callback;
    };
    
    std::queue<TeleportRequest> mTeleportQueue;
    std::mutex mTeleportMutex;
    
    static constexpr size_t MAX_PENDING_TELEPORTS = 1000;
};

// ==================== 钩子安装 ====================
void installHooks();
void uninstallHooks();

} // namespace dimension_parallel
