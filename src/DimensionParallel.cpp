#include "DimensionParallel.h"
#include <ll/api/io/Logger.h>
#include <ll/api/event/EventBus.h>
#include <ll/api/event/server/ServerStartedEvent.h>
#include <ll/api/event/server/ServerStoppingEvent.h>
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>      // 新增，用于 ll::service::getLevel()
#include <ll/api/mod/RegisterHelper.h>   // 新增，用于 LL_REGISTER_MOD
#include <mc/world/level/Level.h>
#include <mc/world/level/dimension/Dimension.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/actor/Actor.h>
#include <mc/legacy/ActorUniqueID.h>
#include <mc/deps/core/math/Vec3.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <gsl/gsl>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dimension_parallel {

// ==================== DimensionParallelMod ====================

DimensionParallelMod::DimensionParallelMod()
    : mSelf(*ll::mod::NativeMod::current()) {
    // 不再需要全局变量 gModInstance
}

DimensionParallelMod& DimensionParallelMod::getInstance() {
    static DimensionParallelMod instance;
    return instance;
}

bool DimensionParallelMod::load() {
    getLogger().info("Loading DimensionParallel mod...");

    try {
        mConfigManager = std::make_unique<ConfigManager>();
        if (!mConfigManager->loadConfig()) {
            getLogger().warn("Failed to load config, using defaults");
        }

        getLogger().info("DimensionParallel mod loaded.");
        return true;
    } catch (const std::exception& e) {
        getLogger().error("Failed to load mod: {}", e.what());
        return false;
    }
}

bool DimensionParallelMod::enable() {
    getLogger().info("Enabling DimensionParallel mod...");

    try {
        mThreadManager = std::make_unique<DimensionThreadManager>();
        mSyncManager = std::make_unique<CrossDimensionSync>();

        auto& eventBus = ll::event::EventBus::getInstance();
        
        eventBus.emplaceListener<ll::event::ServerStartedEvent>(
            [this](ll::event::ServerStartedEvent const&) {
                getLogger().info("Server started, initializing dimension threads...");
                mThreadManager->initialize();
                mEnabled = true;
            }
        );
        
        eventBus.emplaceListener<ll::event::ServerStoppingEvent>(
            [this](ll::event::ServerStoppingEvent const&) {
                getLogger().info("Server stopping, cleaning up...");
                mEnabled = false;
                if (mThreadManager) {
                    mThreadManager->shutdown();
                }
            }
        );

        installHooks();

        mEnabled = true;
        getLogger().info("DimensionParallel mod enabled.");
        return true;
    } catch (const std::exception& e) {
        getLogger().error("Failed to enable mod: {}", e.what());
        return false;
    }
}

void DimensionParallelMod::disable() {
    getLogger().info("Disabling DimensionParallel mod...");

    mEnabled = false;
    
    try {
        uninstallHooks();
        
        if (mThreadManager) {
            mThreadManager->shutdown();
            mThreadManager.reset();
        }
        
        mSyncManager.reset();
        
        getLogger().info("DimensionParallel mod disabled.");
    } catch (const std::exception& e) {
        getLogger().error("Error during disable: {}", e.what());
    }
}

// ==================== ConfigManager ====================

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::loadConfig() {
    std::ifstream file(mConfigPath);
    if (!file.is_open()) {
        DimensionParallelMod::getInstance().getLogger().warn("Config file not found, creating default config");
        return saveConfig();
    }

    try {
        nlohmann::json json;
        file >> json;

        mConfig.enableParallelTicking = json.value("enableParallelTicking", mConfig.enableParallelTicking);
        mConfig.globalThreadCount = json.value("globalThreadCount", mConfig.globalThreadCount);
        mConfig.tickTimeoutMs = json.value("tickTimeoutMs", mConfig.tickTimeoutMs);
        mConfig.enablePerformanceMonitoring = json.value("enablePerformanceMonitoring", mConfig.enablePerformanceMonitoring);
        mConfig.logLevel = json.value("logLevel", mConfig.logLevel);
        mConfig.logTickTimes = json.value("logTickTimes", mConfig.logTickTimes);

        DimensionParallelMod::getInstance().getLogger().info("Config loaded successfully");
        return true;
    } catch (const std::exception& e) {
        DimensionParallelMod::getInstance().getLogger().error("Failed to parse config: {}", e.what());
        return false;
    }
}

bool ConfigManager::saveConfig() {
    nlohmann::json json;
    json["enableParallelTicking"] = mConfig.enableParallelTicking;
    json["globalThreadCount"] = mConfig.globalThreadCount;
    json["tickTimeoutMs"] = mConfig.tickTimeoutMs;
    json["enablePerformanceMonitoring"] = mConfig.enablePerformanceMonitoring;
    json["logLevel"] = mConfig.logLevel;
    json["logTickTimes"] = mConfig.logTickTimes;

    try {
        std::filesystem::create_directories(std::filesystem::path(mConfigPath).parent_path());
        std::ofstream file(mConfigPath);
        if (!file) return false;
        file << json.dump(4);
        return true;
    } catch (...) {
        return false;
    }
}

// ==================== DimensionStats ====================

void DimensionStats::recordTick(uint64_t microseconds, uint64_t threshold) {
    totalTicks.fetch_add(1, std::memory_order_relaxed);
    totalTickTime.fetch_add(microseconds, std::memory_order_relaxed);
    
    uint64_t currentMax = maxTickTime.load(std::memory_order_relaxed);
    while (microseconds > currentMax && 
           !maxTickTime.compare_exchange_weak(currentMax, microseconds, 
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {}
    
    if (microseconds > threshold) {
        slowTickCount.fetch_add(1, std::memory_order_relaxed);
    }
}

double DimensionStats::getAverageTickTime() const {
    uint64_t ticks = totalTicks.load(std::memory_order_relaxed);
    return ticks > 0 ? static_cast<double>(totalTickTime.load(std::memory_order_relaxed)) / ticks : 0.0;
}

void DimensionStats::reset() {
    totalTicks = 0;
    totalTickTime = 0;
    maxTickTime = 0;
    slowTickCount = 0;
}

// ==================== DimensionWorker ====================

DimensionWorker::DimensionWorker(int id) : mId(id) {}

DimensionWorker::~DimensionWorker() {
    stop();
    join();
}

void DimensionWorker::start() {
    if (mRunning.exchange(true)) return;
    
    mThread = std::thread(&DimensionWorker::workerThread, this);

#ifdef _WIN32
    try {
        std::string threadName = "DimWorker_" + std::to_string(mId);
        std::wstring wName(threadName.begin(), threadName.end());
        SetThreadDescription(mThread.native_handle(), wName.c_str());
    } catch (...) {}
#endif
}

void DimensionWorker::stop() {
    if (!mRunning.exchange(false)) return;
    mCv.notify_all();
    mCompletionCv.notify_all();
}

void DimensionWorker::join() {
    if (mThread.joinable()) {
        mThread.join();
    }
}

void DimensionWorker::submitTask(std::function<void()>&& task) {
    if (!mRunning.load(std::memory_order_relaxed)) {
        return;
    }
    mTaskQueue.enqueue(std::move(task));
    mHasTask.store(true, std::memory_order_release);
    mCv.notify_one();
}

bool DimensionWorker::waitForTickCompletion(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mCompletionMutex);
    return mCompletionCv.wait_for(lock, timeout, [this] { 
        return mPendingTasks.load(std::memory_order_acquire) == 0; 
    });
}

void DimensionWorker::workerThread() {
    auto& logger = DimensionParallelMod::getInstance().getLogger();
    logger.debug("DimensionWorker {} started", mId);

    while (mRunning.load(std::memory_order_acquire)) {
        processTasks();

        std::unique_lock lock(mCvMutex);
        mCv.wait_for(lock, std::chrono::milliseconds(50), [this] {
            return mHasTask.load(std::memory_order_acquire) || !mRunning.load(std::memory_order_acquire);
        });
        mHasTask.store(false, std::memory_order_release);
    }

    processTasks();
    
    logger.debug("DimensionWorker {} stopped", mId);
}

void DimensionWorker::processTasks() {
    auto& logger = DimensionParallelMod::getInstance().getLogger();
    auto& config = ConfigManager::getInstance().getConfig();
    
    std::function<void()> task;
    while (mTaskQueue.try_dequeue(task)) {
        mPendingTasks.fetch_add(1, std::memory_order_acq_rel);

        auto guard = gsl::finally([this] {
            if (mPendingTasks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                mCompletionCv.notify_all();
            }
        });
        
        try {
            auto start = std::chrono::high_resolution_clock::now();
            task();
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            
            mStats.recordTick(duration, config.tickTimeoutMs * 1000);
            
            if (config.logTickTimes && config.enablePerformanceMonitoring) {
                if (duration > config.tickTimeoutMs * 1000) {
                    logger.warn("Dimension {} tick took {}us (threshold: {}ms)", 
                                mId, duration, config.tickTimeoutMs);
                }
            }
        } catch (const std::exception& e) {
            logger.error("Task failed in dimension {}: {}", mId, e.what());
        } catch (...) {
            logger.error("Unknown error in dimension {} task", mId);
        }
    }
}

// ==================== DimensionThreadManager ====================

DimensionThreadManager& DimensionThreadManager::getInstance() {
    static DimensionThreadManager instance;
    return instance;
}

void DimensionThreadManager::initialize() {
    if (mInitialized.exchange(true)) return;

    auto& logger = DimensionParallelMod::getInstance().getLogger();

    for (int i = 0; i <= 2; ++i) {
        registerDimension(i);
    }

    logger.info("DimensionThreadManager initialized with {} dimensions", mWorkers.size());
}

void DimensionThreadManager::shutdown() {
    if (!mInitialized.exchange(false)) return;

    auto& logger = DimensionParallelMod::getInstance().getLogger();
    logger.info("Shutting down DimensionThreadManager...");

    {
        std::shared_lock lock(mWorkersMutex);
        for (auto& [id, worker] : mWorkers) {
            worker->stop();
        }
    }
    
    {
        std::shared_lock lock(mWorkersMutex);
        for (auto& [id, worker] : mWorkers) {
            worker->join();
        }
    }
    
    {
        std::unique_lock lock(mWorkersMutex);
        mWorkers.clear();
    }

    logger.info("DimensionThreadManager shutdown complete");
}

void DimensionThreadManager::registerDimension(int id) {
    std::unique_lock lock(mWorkersMutex);
    if (mWorkers.find(id) != mWorkers.end()) {
        return;
    }

    auto worker = std::make_unique<DimensionWorker>(id);
    if (mInitialized.load(std::memory_order_acquire)) {
        worker->start();
    }
    mWorkers[id] = std::move(worker);

    auto& logger = DimensionParallelMod::getInstance().getLogger();
    logger.debug("Registered dimension {}", id);
}

void DimensionThreadManager::unregisterDimension(int id) {
    std::unique_lock lock(mWorkersMutex);
    auto it = mWorkers.find(id);
    if (it != mWorkers.end()) {
        it->second->stop();
        it->second->join();
        mWorkers.erase(it);
    }
}

bool DimensionThreadManager::isDimensionRegistered(int id) const {
    std::shared_lock lock(mWorkersMutex);
    return mWorkers.find(id) != mWorkers.end();
}

void DimensionThreadManager::tickAllDimensions() {
    tickAllDimensionsWithTimeout(std::chrono::seconds(5));
}

bool DimensionThreadManager::tickAllDimensionsWithTimeout(std::chrono::milliseconds timeout) {
    if (!mInitialized.load(std::memory_order_acquire)) {
        return false;
    }

    auto& logger = DimensionParallelMod::getInstance().getLogger();
    auto startTime = std::chrono::high_resolution_clock::now();

    {
        std::shared_lock lock(mWorkersMutex);
        for (auto& [id, worker] : mWorkers) {
            worker->submitTask([id]() {
                // 修正：使用 ll::service::getLevel()
                auto* level = ll::service::getLevel();
                if (!level) return;
                auto* dim = level->getDimension(id);
                if (!dim) return;
                dim->tick();
            });
        }
    }

    bool allCompleted = true;
    {
        std::shared_lock lock(mWorkersMutex);
        for (auto& [id, worker] : mWorkers) {
            if (!worker->waitForTickCompletion(timeout)) {
                logger.warn("Dimension {} tick timeout after {}ms", id, timeout.count());
                allCompleted = false;
            }
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    
    if (totalDuration > 50) {
        logger.debug("All dimensions ticked in {}ms", totalDuration);
    }

    return allCompleted;
}

const DimensionStats& DimensionThreadManager::getDimensionStats(int id) const {
    std::shared_lock lock(mWorkersMutex);
    static DimensionStats empty;
    auto it = mWorkers.find(id);
    return it != mWorkers.end() ? it->second->getStats() : empty;
}

size_t DimensionThreadManager::getWorkerCount() const {
    std::shared_lock lock(mWorkersMutex);
    return mWorkers.size();
}

// ==================== CrossDimensionSync ====================

CrossDimensionSync& CrossDimensionSync::getInstance() {
    static CrossDimensionSync instance;
    return instance;
}

void CrossDimensionSync::registerEntity(Actor* entity, int dim) {
    if (!entity) return;
    std::unique_lock lock(mEntityMapMutex);
    mEntityDimensionMap[entity->getOrCreateUniqueID()] = dim;
}

void CrossDimensionSync::unregisterEntity(ActorUniqueID id) {
    std::unique_lock lock(mEntityMapMutex);
    mEntityDimensionMap.erase(id);
}

int CrossDimensionSync::getEntityDimension(ActorUniqueID id) const {
    std::shared_lock lock(mEntityMapMutex);
    auto it = mEntityDimensionMap.find(id);
    return it != mEntityDimensionMap.end() ? it->second : -1;
}

void CrossDimensionSync::queueTeleportRequest(Player* player, int from, int to, Vec3 const& targetPos) {
    if (!player) return;
    
    std::lock_guard lock(mTeleportMutex);
    
    if (mTeleportQueue.size() >= MAX_PENDING_TELEPORTS) {
        mTeleportQueue.pop();
    }
    
    TeleportRequest req{
        player->getOrCreateUniqueID(),
        from,
        to,
        targetPos,
        std::chrono::steady_clock::now(),
        nullptr
    };
    mTeleportQueue.push(std::move(req));
}

void CrossDimensionSync::processPendingTeleports() {
    std::queue<TeleportRequest> localQueue;
    {
        std::lock_guard lock(mTeleportMutex);
        std::swap(localQueue, mTeleportQueue);
    }

    // 修正：使用 ll::service::getLevel()
    auto* level = ll::service::getLevel();
    if (!level) return;

    while (!localQueue.empty()) {
        auto& req = localQueue.front();
        
        auto* player = level->getPlayer(req.playerId);
        if (player) {
            std::unique_lock lock(mEntityMapMutex);
            mEntityDimensionMap[req.playerId] = req.toDim;
            if (req.callback) req.callback();
        }
        
        localQueue.pop();
    }
}

void CrossDimensionSync::onPlayerTeleported(Player* player, int from, int to) {
    if (!player) return;
    std::unique_lock lock(mEntityMapMutex);
    mEntityDimensionMap[player->getOrCreateUniqueID()] = to;
}

void CrossDimensionSync::processPendingOperations() {
    processPendingTeleports();
}

size_t CrossDimensionSync::getPendingTeleportCount() const {
    std::lock_guard lock(mTeleportMutex);
    return mTeleportQueue.size();
}

// ==================== Hooks ====================
// 关键：使用 $ 前缀的 thunk 函数，参考 MobAIOptimizer 插件

LL_AUTO_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    auto& mod = DimensionParallelMod::getInstance();
    auto& config = mod.getConfigManager().getConfig();

    if (config.enableParallelTicking && mod.getThreadManager().getWorkerCount() > 0) {
        mod.getThreadManager().tickAllDimensions();
        mod.getSyncManager().processPendingOperations();
        return;
    }
    
    origin();
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    DimensionTickHook,
    ll::memory::HookPriority::High,
    Dimension,
    &Dimension::$tick,
    void
) {
    auto& mod = DimensionParallelMod::getInstance();
    auto& config = mod.getConfigManager().getConfig();
    
    if (config.enableParallelTicking && mod.getThreadManager().isDimensionRegistered(this->getDimensionId())) {
        return;
    }
    
    origin();
}

void installHooks() {
    DimensionParallelMod::getInstance().getLogger().info("Hooks installed successfully");
}

void uninstallHooks() {
    DimensionParallelMod::getInstance().getLogger().info("Hooks uninstalled");
}

} // namespace dimension_parallel

// 注册宏：传入单例的 getInstance 方法
LL_REGISTER_MOD(dimension_parallel::DimensionParallelMod, dimension_parallel::DimensionParallelMod::getInstance());
