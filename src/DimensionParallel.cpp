#include "DimensionParallel.h"
#include <ll/api/io/Logger.h>
#include <ll/api/event/EventBus.h>
#include <ll/api/event/server/ServerStartedEvent.h>
#include <ll/api/memory/Hook.h>
#include <ll/api/service/ServerInfo.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/dimension/Dimension.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/actor/Actor.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dimension_parallel {

// ==================== DimensionParallelMod ====================

DimensionParallelMod& DimensionParallelMod::getInstance() {
    static DimensionParallelMod instance;
    return instance;
}

DimensionParallelMod::DimensionParallelMod() : ll::mod::NativeMod("DimensionParallel") {}

bool DimensionParallelMod::load() {
    getLogger().info("Loading DimensionParallel mod...");

    mConfigManager = std::make_unique<ConfigManager>();
    if (!mConfigManager->loadConfig()) {
        getLogger().warn("Failed to load config, using defaults");
    }

    getLogger().info("DimensionParallel mod loaded.");
    return true;
}

bool DimensionParallelMod::enable() {
    getLogger().info("Enabling DimensionParallel mod...");

    mThreadManager = std::make_unique<DimensionThreadManager>();
    mSyncManager = std::make_unique<CrossDimensionSync>();

    auto& eventBus = ll::event::EventBus::getInstance();
    eventBus.emplaceListener<ll::event::ServerStartedEvent>([this](ll::event::ServerStartedEvent const&) {
        getLogger().info("Server started, initializing dimension threads...");
        mThreadManager->initialize();
    });

    installHooks();

    getLogger().info("DimensionParallel mod enabled.");
    return true;
}

bool DimensionParallelMod::disable() {
    getLogger().info("Disabling DimensionParallel mod...");

    if (mThreadManager) {
        mThreadManager->shutdown();
    }
    mThreadManager.reset();
    mSyncManager.reset();

    getLogger().info("DimensionParallel mod disabled.");
    return true;
}

// ==================== ConfigManager ====================

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::loadConfig() {
    auto& logger = DimensionParallelMod::getInstance().getLogger();

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

        logger.info("Config loaded");
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

    std::filesystem::create_directories(std::filesystem::path(mConfigPath).parent_path());
    std::ofstream file(mConfigPath);
    if (!file) return false;
    file << json.dump(4);
    return true;
}

// ==================== DimensionStats ====================

void DimensionStats::recordTick(uint64_t microseconds) {
    totalTicks++;
    totalTickTime += microseconds;
    uint64_t currentMax = maxTickTime.load();
    while (microseconds > currentMax && !maxTickTime.compare_exchange_weak(currentMax, microseconds)) {}
}

double DimensionStats::getAverageTickTime() const {
    uint64_t ticks = totalTicks.load();
    return ticks > 0 ? static_cast<double>(totalTickTime.load()) / ticks : 0.0;
}

// ==================== DimensionWorker ====================

DimensionWorker::DimensionWorker(int id) : mId(id) {}

DimensionWorker::~DimensionWorker() {
    stop();
    join();
}

void DimensionWorker::start() {
    if (mRunning) return;
    mRunning = true;
    mThread = std::thread(&DimensionWorker::workerThread, this);

#ifdef _WIN32
    std::string threadName = "DimWorker_" + std::to_string(mId);
    SetThreadDescription(mThread.native_handle(), std::wstring(threadName.begin(), threadName.end()).c_str());
#endif
}

void DimensionWorker::stop() {
    mRunning = false;
    mCv.notify_all();
}

void DimensionWorker::join() {
    if (mThread.joinable()) mThread.join();
}

void DimensionWorker::submitTask(std::function<void()>&& task) {
    mTaskQueue.enqueue(std::move(task));
    mHasTask = true;
    mCv.notify_one();
}

void DimensionWorker::waitForTickCompletion() {
    std::unique_lock lock(mCompletionMutex);
    mCompletionCv.wait(lock, [this] { return mPendingTasks == 0; });
}

void DimensionWorker::workerThread() {
    auto& logger = DimensionParallelMod::getInstance().getLogger();
    logger.debug("DimensionWorker {} started", mId);

    while (mRunning) {
        processTasks();

        std::unique_lock lock(mCvMutex);
        mCv.wait_for(lock, std::chrono::milliseconds(50), [this] {
            return mHasTask.load() || !mRunning;
        });
        mHasTask = false;
    }

    logger.debug("DimensionWorker {} stopped", mId);
}

void DimensionWorker::processTasks() {
    std::function<void()> task;
    while (mTaskQueue.try_dequeue(task)) {
        mPendingTasks++;

        auto start = std::chrono::high_resolution_clock::now();
        task();
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        mStats.recordTick(duration);

        mPendingTasks--;
        if (mPendingTasks == 0) mCompletionCv.notify_all();
    }
}

// ==================== DimensionThreadManager ====================

DimensionThreadManager& DimensionThreadManager::getInstance() {
    static DimensionThreadManager instance;
    return instance;
}

void DimensionThreadManager::initialize() {
    if (mInitialized) return;

    auto& logger = DimensionParallelMod::getInstance().getLogger();

    // 注册主世界、下界、末地
    for (int i = 0; i <= 2; ++i) {
        registerDimension(i);
    }

    mInitialized = true;
    logger.info("DimensionThreadManager initialized with {} dimensions", mWorkers.size());
}

void DimensionThreadManager::shutdown() {
    if (!mInitialized) return;

    std::unique_lock lock(mWorkersMutex);
    for (auto& [id, worker] : mWorkers) {
        worker->stop();
    }
    for (auto& [id, worker] : mWorkers) {
        worker->join();
    }
    mWorkers.clear();
    mInitialized = false;

    auto& logger = DimensionParallelMod::getInstance().getLogger();
    logger.info("DimensionThreadManager shutdown complete");
}

void DimensionThreadManager::registerDimension(int id) {
    std::unique_lock lock(mWorkersMutex);
    if (mWorkers.find(id) != mWorkers.end()) return;

    auto worker = std::make_unique<DimensionWorker>(id);
    if (mInitialized) worker->start();
    mWorkers[id] = std::move(worker);

    auto& logger = DimensionParallelMod::getInstance().getLogger();
    logger.info("Registered dimension {}", id);
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
    if (!mInitialized) return;

    std::shared_lock lock(mWorkersMutex);
    for (auto& [id, worker] : mWorkers) {
        worker->submitTask([id]() {
            auto* level = ll::service::getLevel();
            if (!level) return;
            auto* dim = level->getDimension(id);
            if (!dim) return;
            dim->tick();
        });
    }

    for (auto& [id, worker] : mWorkers) {
        worker->waitForTickCompletion();
    }
}

const DimensionStats& DimensionThreadManager::getDimensionStats(int id) const {
    std::shared_lock lock(mWorkersMutex);
    static DimensionStats empty;
    auto it = mWorkers.find(id);
    return it != mWorkers.end() ? it->second->getStats() : empty;
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

int CrossDimensionSync::getEntityDimension(ActorUniqueID id) {
    std::shared_lock lock(mEntityMapMutex);
    auto it = mEntityDimensionMap.find(id);
    return it != mEntityDimensionMap.end() ? it->second : -1;
}

void CrossDimensionSync::queueTeleportRequest(Player* player, int from, int to) {
    TeleportRequest req{player, from, to, player->getPosition(), nullptr};
    std::lock_guard lock(mTeleportMutex);
    mTeleportQueue.push(std::move(req));
}

void CrossDimensionSync::processPendingTeleports() {
    std::queue<TeleportRequest> localQueue;
    {
        std::lock_guard lock(mTeleportMutex);
        std::swap(localQueue, mTeleportQueue);
    }

    while (!localQueue.empty()) {
        auto& req = localQueue.front();
        {
            std::unique_lock lock(mEntityMapMutex);
            mEntityDimensionMap[req.player->getOrCreateUniqueID()] = req.toDim;
        }
        if (req.callback) req.callback();
        localQueue.pop();
    }
}

void CrossDimensionSync::onPlayerTeleported(Player* player, int from, int to) {
    std::unique_lock lock(mEntityMapMutex);
    mEntityDimensionMap[player->getOrCreateUniqueID()] = to;
}

void CrossDimensionSync::processPendingOperations() {
    processPendingTeleports();
}

// ==================== Hooks ====================

// Level::tick Hook
LL_AUTO_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    auto& mod = DimensionParallelMod::getInstance();
    auto& config = mod.getConfigManager().getConfig();

    if (config.enableParallelTicking) {
        mod.getThreadManager().tickAllDimensions();
        mod.getSyncManager().processPendingOperations();
        // 如果原版 tick 中还有其他必要逻辑，可在此调用 origin()
        // 但为了性能，我们跳过 origin()，因为维度 tick 已执行。
        return;
    }
    origin();
}

// Dimension::tick Hook (拦截，避免重复)
LL_AUTO_TYPE_INSTANCE_HOOK(
    DimensionTickHook,
    ll::memory::HookPriority::High,
    Dimension,
    &Dimension::$tick,
    void
) {
    auto& mod = DimensionParallelMod::getInstance();
    auto& config = mod.getConfigManager().getConfig();
    auto dimId = this->getDimensionId();

    if (config.enableParallelTicking && mod.getThreadManager().isDimensionRegistered(dimId)) {
        // 由线程管理器调度，此处跳过
        return;
    }
    origin();
}

// Player::changeDimension Hook
LL_AUTO_TYPE_INSTANCE_HOOK(
    PlayerChangeDimensionHook,
    ll::memory::HookPriority::Normal,
    Player,
    &Player::$changeDimension,
    void,
    int targetDim
) {
    auto& mod = DimensionParallelMod::getInstance();
    auto& sync = mod.getSyncManager();
    auto currentDim = this->getDimensionId();

    sync.queueTeleportRequest(this, currentDim, targetDim);
    origin(targetDim);
    sync.onPlayerTeleported(this, currentDim, targetDim);
}

// Actor 生成/移除 Hook
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorAddHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$addEntity,
    bool,
    Actor* actor
) {
    bool result = origin(actor);
    if (result && actor) {
        auto& sync = DimensionParallelMod::getInstance().getSyncManager();
        sync.registerEntity(actor, actor->getDimensionId());
    }
    return result;
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorRemoveHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$removeEntity,
    void,
    Actor& actor
) {
    auto& sync = DimensionParallelMod::getInstance().getSyncManager();
    sync.unregisterEntity(actor.getOrCreateUniqueID());
    origin(actor);
}

void installHooks() {
    // 钩子自动注册
}

} // namespace dimension_parallel

LL_REGISTER_MOD(dimension_parallel::DimensionParallelMod, dimension_parallel::DimensionParallelMod::getInstance());
