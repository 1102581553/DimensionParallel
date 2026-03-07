#include "DimensionParallel.h"
#include <ll/api/io/Logger.h>
#include <ll/api/event/EventBus.h>
#include <ll/api/event/server/ServerStartedEvent.h>
#include <ll/api/event/server/ServerStoppingEvent.h>
#include <ll/api/memory/Hook.h>
#include <ll/api/service/ServerInfo.h>
#include <ll/api/mod/ModManager.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/dimension/Dimension.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/actor/ActorUniqueID.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <gsl/gsl>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dimension_parallel {

// ==================== DimensionParallelMod ====================

DimensionParallelMod& DimensionParallelMod::getInstance() {
    static DimensionParallelMod* instance = nullptr;
    if (!instance) {
        throw std::runtime_error("DimensionParallelMod not initialized");
    }
    return *instance;
}

DimensionParallelMod::DimensionParallelMod(ll::mod::Manifest const& manifest) 
    : ll::mod::NativeMod(manifest) {
    // 设置全局实例指针
    // 注意：需要在 enable() 中设置，因为此时对象可能还未完全构造
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

        // 设置全局实例（在 enable 中设置，确保对象已完全构造）
        // 使用静态指针存储
        static DimensionParallelMod* modPtr = this;
        
        auto& eventBus = ll::event::EventBus::getInstance();
        
        // 注册服务器启动事件
        mServerStartedListener = eventBus.emplaceListener<ll::event::ServerStartedEvent>(
            [this](ll::event::ServerStartedEvent const&) {
                getLogger().info("Server started, initializing dimension threads...");
                mThreadManager->initialize();
                mEnabled = true;
            }
        );
        
        // 注册服务器停止事件
        mServerStoppingListener = eventBus.emplaceListener<ll::event::ServerStoppingEvent>(
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
        // 卸载钩子
        uninstallHooks();
        
        // 移除事件监听
        auto& eventBus = ll::event::EventBus::getInstance();
        eventBus.clear();
        
        // 关闭线程管理器
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

// 存储全局实例指针的静态变量
static DimensionParallelMod* gModInstance = nullptr;

// ==================== ConfigManager ====================

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::loadConfig() {
    auto& logger = ll::mod::ModManager::getInstance().getMod("DimensionParallel")->getLogger();

    std::ifstream file(mConfigPath);
    if (!file.is_open()) {
        logger.warn("Config file not found, creating default config");
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

        logger.info("Config loaded successfully");
        return true;
    } catch (const std::exception& e) {
        logger.error("Failed to parse config: {}", e.what());
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
    
    // 更新最大值（使用 CAS 循环）
    uint64_t currentMax = maxTickTime.load(std::memory_order_relaxed);
    while (microseconds > currentMax && 
           !maxTickTime.compare_exchange_weak(currentMax, microseconds, 
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {}
    
    // 记录慢 tick
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
    if (mRunning.exchange(true)) return;  // 已经运行
    
    mThread = std::thread(&DimensionWorker::workerThread, this);

#ifdef _WIN32
    try {
        std::string threadName = "DimWorker_" + std::to_string(mId);
        std::wstring wName(threadName.begin(), threadName.end());
        SetThreadDescription(mThread.native_handle(), wName.c_str());
    } catch (...) {
        // 线程命名失败不影响功能
    }
#endif
}

void DimensionWorker::stop() {
    if (!mRunning.exchange(false)) return;  // 已经停止
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
        return;  // 工作线程已停止，丢弃任务
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
    auto& logger = ll::mod::ModManager::getInstance().getMod("DimensionParallel")->getLogger();
    logger.debug("DimensionWorker {} started", mId);

    while (mRunning.load(std::memory_order_acquire)) {
        processTasks();

        std::unique_lock lock(mCvMutex);
        mCv.wait_for(lock, std::chrono::milliseconds(50), [this] {
            return mHasTask.load(std::memory_order_acquire) || !mRunning.load(std::memory_order_acquire);
        });
        mHasTask.store(false, std::memory_order_release);
    }

    // 处理剩余任务
    processTasks();
    
    logger.debug("DimensionWorker {} stopped", mId);
}

void DimensionWorker::processTasks() {
    auto& logger = ll::mod::ModManager::getInstance().getMod("DimensionParallel")->getLogger();
    auto& config = ConfigManager::getInstance().getConfig();
    
    std::function<void()> task;
    while (mTaskQueue.try_dequeue(task)) {
        mPendingTasks.fetch_add(1, std::memory_order_acq_rel);

        // RAII 守卫确保 pendingTasks 一定递减
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
                    logger.warn("Dimension {} tick took {}μs (threshold: {}ms)", 
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

    auto& logger = ll::mod::ModManager::getInstance().getMod("DimensionParallel")->getLogger();

    // 注册主世界 (0)、下界 (1)、末地 (2)
    for (int i = 0; i <= 2; ++i) {
        registerDimension(i);
    }

    logger.info("DimensionThreadManager initialized with {} dimensions", mWorkers.size());
}

void DimensionThreadManager::shutdown() {
    if (!mInitialized.exchange(false)) return;

    auto& logger = ll::mod::ModManager::getInstance().getMod("DimensionParallel")->getLogger();
    logger.info("Shutting down DimensionThreadManager...");

    // 先停止所有工作线程
    {
        std::shared_lock lock(mWorkersMutex);
        for (auto& [id, worker] : mWorkers) {
            worker->stop();
        }
    }
    
    // 等待所有线程结束
    {
        std::shared_lock lock(mWorkersMutex);
        for (auto& [id, worker] : mWorkers) {
            worker->join();
        }
    }
    
    // 清理
    {
        std::unique_lock lock(mWorkersMutex);
        mWorkers.clear();
    }

    logger.info("DimensionThreadManager shutdown complete");
}

void DimensionThreadManager::registerDimension(int id) {
    std::unique_lock lock(mWorkersMutex);
    if (mWorkers.find(id) != mWorkers.end()) {
        return;  // 已注册
    }

    auto worker = std::make_unique<DimensionWorker>(id);
    if (mInitialized.load(std::memory_order_acquire)) {
        worker->start();
    }
    mWorkers[id] = std::move(worker);

    auto& logger = ll::mod::ModManager::getInstance().getMod("DimensionParallel")->getLogger();
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

    auto& logger = ll::mod::ModManager::getInstance().getMod("DimensionParallel")->getLogger();
    auto startTime = std::chrono::high_resolution_clock::now();

    // 提交所有维度的 tick 任务
    {
        std::shared_lock lock(mWorkersMutex);
        for (auto& [id, worker] : mWorkers) {
            worker->submitTask([id]() {
                auto* level = ll::service::ServerInfo::getInstance().getLevel();
                if (!level) return;
                auto* dim = level->getDimension(id);
                if (!dim) return;
                dim->tick();
            });
        }
    }

    // 等待所有维度完成（带超时）
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
    
    if (totalDuration > 50) {  // 超过 50ms 记录日志
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
    
    // 限制队列大小
    if (mTeleportQueue.size() >= MAX_PENDING_TELEPORTS) {
        mTeleportQueue.pop();  // 移除最旧的请求
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

    auto* level = ll::service::ServerInfo::getInstance().getLevel();
    if (!level) return;

    while (!localQueue.empty()) {
        auto& req = localQueue.front();
        
        // 通过 ID 查找玩家（避免裸指针问题）
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

// 存储钩子实例以便卸载
static std::vector<std::unique_ptr<ll::memory::Hook>> gHooks;

// Level::tick Hook
class LevelTickHook {
public:
    static void install() {
        auto& mod = DimensionParallelMod::getInstance();
        
        ll::memory::Hook::create(
            "LevelTickHook",
            ll::memory::resolveSymbol("?tick@Level@@UEAAXXZ"),
            reinterpret_cast<void*>(&hookedLevelTick)
        )->enable();
    }
    
    static void uninstall() {
        // Hook 会自动清理
    }

private:
    static void hookedLevelTick(Level* self) {
        auto& mod = DimensionParallelMod::getInstance();
        auto& config = mod.getConfigManager().getConfig();

        if (config.enableParallelTicking && mod.getThreadManager().getWorkerCount() > 0) {
            mod.getThreadManager().tickAllDimensions();
            mod.getSyncManager().processPendingOperations();
            return;  // 跳过原版 tick（维度 tick 已并行执行）
        }
        
        // 调用原版
        self->Level::tick();
    }
};

// Player::changeDimension Hook
class PlayerChangeDimensionHook {
public:
    static void install() {
        ll::memory::Hook::create(
            "PlayerChangeDimensionHook",
            ll::memory::resolveSymbol("?changeDimension@Player@@UEAAXH_N@Z"),
            reinterpret_cast<void*>(&hookedChangeDimension)
        )->enable();
    }
    
private:
    static void hookedChangeDimension(Player* self, int targetDim, bool respawn) {
        auto& mod = DimensionParallelMod::getInstance();
        auto& sync = mod.getSyncManager();
        auto currentDim = self->getDimensionId();

        sync.queueTeleportRequest(self, currentDim, targetDim, self->getPosition());
        self->Player::changeDimension(targetDim, respawn);
        sync.onPlayerTeleported(self, currentDim, targetDim);
    }
};

// Actor 生成 Hook
class ActorAddHook {
public:
    static void install() {
        ll::memory::Hook::create(
            "ActorAddHook",
            ll::memory::resolveSymbol("?addEntity@Level@@QEAA_NAEAVActor@@_N@Z"),
            reinterpret_cast<void*>(&hookedAddEntity)
        )->enable();
    }
    
private:
    static bool hookedAddEntity(Level* self, Actor& actor, bool force) {
        bool result = self->Level::addEntity(actor, force);
        if (result) {
            auto& sync = DimensionParallelMod::getInstance().getSyncManager();
            sync.registerEntity(&actor, actor.getDimensionId());
        }
        return result;
    }
};

// Actor 移除 Hook
class ActorRemoveHook {
public:
    static void install() {
        ll::memory::Hook::create(
            "ActorRemoveHook",
            ll::memory::resolveSymbol("?removeEntity@Level@@QEAAXAEAVActor@@_N@Z"),
            reinterpret_cast<void*>(&hookedRemoveEntity)
        )->enable();
    }
    
private:
    static void hookedRemoveEntity(Level* self, Actor& actor, bool force) {
        auto& sync = DimensionParallelMod::getInstance().getSyncManager();
        sync.unregisterEntity(actor.getOrCreateUniqueID());
        self->Level::removeEntity(actor, force);
    }
};

void installHooks() {
    try {
        LevelTickHook::install();
        PlayerChangeDimensionHook::install();
        ActorAddHook::install();
        ActorRemoveHook::install();
    } catch (const std::exception& e) {
        ll::mod::ModManager::getInstance().getMod("DimensionParallel")->getLogger()
            .error("Failed to install hooks: {}", e.what());
    }
}

void uninstallHooks() {
    LevelTickHook::uninstall();
    // 其他钩子由 Hook 系统自动清理
}

} // namespace dimension_parallel

// 模组注册宏
LL_REGISTER_MOD(dimension_parallel::DimensionParallelMod, dimension_parallel::gModInstance);
