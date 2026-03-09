/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 */
#include <iostream>
#include <unordered_map>
#include <string>

#include "virus_protection_service_log.h"
#include "feature_manager.h"
#include "feature_updater.h"
#include "security_policy_manager.h"
#include "event_bus.h"
#include "signal_guard.h"
#include "virus_information_helper.h"
#include "virus_engine_manager.h"

#include "debug_config.h"

namespace OHOS::Security::VirusProtectionService {
namespace {
std::unordered_map<VirusEngine, std::string> LIBRARY_PATHS = {
    {VirusEngine::CSPL_STATIC_ENGINE, "/system/lib64/libcspl_static_engine_utils.z.so"},
    {VirusEngine::CSPL_DYNAMIC_ENGINE, "/system/lib64/libcspl_dynamic_engine_utils.z.so"},
    {VirusEngine::TRUSTONE_STATIC_ENGINE, "/system/lib64/libtrustone_static_engine_utils.z.so"},
    {VirusEngine::QOWL_STATIC_ENGINE, "/system/lib64/libqowl_static_engine_utils.z.so"},
    {VirusEngine::ATCORE_STATIC_ENGINE, "/system/lib64/libatcore_static_engine_utils.z.so"},
};
std::unordered_map<std::string, VirusEngine> g_virusEngineNameMaps = {
    {CSPL_STATIC_ENGINE, VirusEngine::CSPL_STATIC_ENGINE},
    {CSPL_DYNAMIC_ENGINE, VirusEngine::CSPL_DYNAMIC_ENGINE},
    {TRUSTONE_STATIC_ENGINE, VirusEngine::TRUSTONE_STATIC_ENGINE},
    {QOWL_STATIC_ENGINE, VirusEngine::QOWL_STATIC_ENGINE},
    {ATCORE_STATIC_ENGINE, VirusEngine::ATCORE_STATIC_ENGINE},
};
std::unordered_map<VirusEngine, std::string> g_residentEngineNameMaps = {
    {VirusEngine::TRUSTONE_STATIC_ENGINE, TRUSTONE_STATIC_ENGINE},
    {VirusEngine::ATCORE_STATIC_ENGINE, ATCORE_STATIC_ENGINE},
};
constexpr int32_t DEFAULT_INIT_VAL = -1;
std::vector<int32_t> SIGNALS = {
    SIGILL,
    SIGTRAP,
    SIGABRT,
    SIGBUS,
    SIGFPE,
    SIGSEGV,
    SIGSTKFLT,
    SIGSYS,
};
const std::vector<VirusEngine> g_nonResidentVirusEngineList = {
    VirusEngine::CSPL_STATIC_ENGINE,
    VirusEngine::CSPL_DYNAMIC_ENGINE,
    VirusEngine::QOWL_STATIC_ENGINE
};
}  // namespace

static std::unordered_set<const ScanTask *> g_Tasks;
static std::mutex g_taskMutex;

VirusEngineManager::VirusEngineManager()
{
    workerThread_ = std::thread(&VirusEngineManager::WorkerThread, this);
}

VirusEngineManager::~VirusEngineManager()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        taskQueue_.push(TaskType::SHUTDOWN);
        isRunning_ = false;
    }
    taskCv_.notify_one();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void VirusEngineManager::WorkerThread()
{
    HILOGI("Virus engine manager worker thread started");
    while (isRunning_) {
        TaskType task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            taskCv_.wait(lock, [this] { return !taskQueue_.empty() || !isRunning_; });
            if (!isRunning_) {
                break;
            }
            if (taskQueue_.empty()) {
                continue;
            }
            task = taskQueue_.front();
            taskQueue_.pop();
        }

        switch (task) {
            case TaskType::INIT:
                initResult_ = PerformInit();
                break;
            case TaskType::DEINIT:
                PerformDeInit();
                break;
            case TaskType::SHUTDOWN:
                isRunning_ = false;
                break;
        }
    }
    HILOGI("Virus engine manager worker thread stopped");
}

VirusEngineManager &VirusEngineManager::GetInstance()
{
    static VirusEngineManager instance;
    return instance;
}

int32_t VirusEngineManager::Init()
{
    HILOGI("Virus engine manager init start");
    {
        std::shared_lock<std::shared_mutex> lock(engineMutex_);
        if (isInitialized_) {
            return SUCCESS;
        }
    }
    auto updateInfo = FeatureUpdater::GetInstance().GetUpdateInfo();
    if (updateInfo.localVersion.empty() || updateInfo.localVersion == "0.0.0.0") {
        HILOGI("Local feature version is null or 0.0.0.0, default scan ability turn on.");
        return SUCCESS;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        taskQueue_.push(TaskType::INIT);
    }
    HILOGI("taskQueue_.push(TaskType::INIT) end");
    initResult_.store(DEFAULT_INIT_VAL, std::memory_order_relaxed);
    taskCv_.notify_one();
    HILOGI("taskCv_.notify_one() end");
    while (initResult_.load(std::memory_order_acquire) == DEFAULT_INIT_VAL) {
        HILOGI("wait for init complete");
        ffrt::this_task::sleep_for(std::chrono::milliseconds(1000));
        HILOGI("sleep for 1000ms");
    }
    HILOGI("Virus engine manager init end");
    return initResult_;
}

void VirusEngineManager::DeInit()
{
    {
        std::shared_lock<std::shared_mutex> lock(engineMutex_);
        if (!isInitialized_.load(std::memory_order_acquire)) {
            return;
        }
    }
    HILOGI("Virus engine manager deinit start");
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        taskQueue_.push(TaskType::DEINIT);
    }
    HILOGI("taskQueue_.push(TaskType::DEINIT) end");
    taskCv_.notify_one();
    HILOGI("Virus engine manager deinit end");
}

int32_t VirusEngineManager::ScanBehavior(uint32_t accessToken, const std::string &event, const std::string &bundleName)
{
    std::shared_lock<std::shared_mutex> readLock(engineMutex_);
    if (engineLoaders_.find(VirusEngine::CSPL_DYNAMIC_ENGINE) == engineLoaders_.end()) {
        HILOGE("not load virusEngine");
        return FAILED;
    }
    if (engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE].get() == nullptr) {
        HILOGE("dynamic loader is null");
        return FAILED;
    }
    IVirusEngine *virusEngine = engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE]->GetVirusEngine();
    if (virusEngine == nullptr) {
        HILOGE("Get dynamicEngine error");
        return FAILED;
    }
    int32_t ret = virusEngine->ScanBehavior(accessToken, event, bundleName);
    if (ret != SUCCESS) {
        HILOGE("ScanBehavior error, ret=%{public}d", ret);
        return ret;
    }
    return SUCCESS;
}

int32_t VirusEngineManager::IsExistAnalysisEngine(uint32_t accessToken)
{
    std::shared_lock<std::shared_mutex> readLock(engineMutex_);
    if (engineLoaders_.find(VirusEngine::CSPL_DYNAMIC_ENGINE) == engineLoaders_.end()) {
        HILOGE("not load virusEngine");
        return FAILED;
    }
    if (engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE].get() == nullptr) {
        HILOGE("dynamic loader is null");
        return FAILED;
    }
    IVirusEngine *virusEngine = engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE]->GetVirusEngine();
    if (virusEngine == nullptr) {
        HILOGE("Get dynamicEngine error");
        return FAILED;
    }
    int32_t ret = virusEngine->IsExistAnalysisEngine(accessToken);
    if (ret != SUCCESS) {
        HILOGW("current app is not exist AnalysisEngine, ret=%{public}d", ret);
        return FAILED;
    }
    return SUCCESS;
}

int32_t VirusEngineManager::CreateAnalysisEngine(uint32_t accessToken)
{
    std::shared_lock<std::shared_mutex> readLock(engineMutex_);
    if (engineLoaders_.find(VirusEngine::CSPL_DYNAMIC_ENGINE) == engineLoaders_.end()) {
        HILOGE("not load virusEngine");
        return FAILED;
    }
    if (engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE].get() == nullptr) {
        HILOGE("dynamic loader is null");
        return FAILED;
    }
    IVirusEngine *virusEngine = engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE]->GetVirusEngine();
    if (virusEngine == nullptr) {
        HILOGE("Get dynamicEngine error");
        return FAILED;
    }
    int32_t ret = virusEngine->CreateAnalysisEngine(accessToken);
    if (ret != SUCCESS) {
        HILOGE("dynamicVirusEngine CreateAnalysisEngine error, ret=%{public}d", ret);
        return FAILED;
    }
    return SUCCESS;
}

int32_t VirusEngineManager::DestroyAnalysisEngine(uint32_t accessToken)
{
    std::shared_lock<std::shared_mutex> readLock(engineMutex_);
    if (engineLoaders_.find(VirusEngine::CSPL_DYNAMIC_ENGINE) == engineLoaders_.end()) {
        HILOGE("not load virusEngine");
        return FAILED;
    }
    if (engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE].get() == nullptr) {
        HILOGE("dynamic loader is null");
        return FAILED;
    }
    IVirusEngine *virusEngine = engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE]->GetVirusEngine();
    if (virusEngine == nullptr) {
        HILOGE("Get dynamicEngine error");
        return FAILED;
    }
    int32_t ret = virusEngine->DestroyAnalysisEngine(accessToken);
    if (ret != SUCCESS) {
        HILOGE("dynamicVirusEngine DestroyAnalysisEngine error, ret=%{public}d", ret);
        return FAILED;
    }
    return SUCCESS;
}

int32_t VirusEngineManager::RegisterScanResultListener(std::shared_ptr<ScanResultListener> &listener)
{
    std::shared_lock<std::shared_mutex> readLock(engineMutex_);
    if (engineLoaders_.find(VirusEngine::CSPL_DYNAMIC_ENGINE) == engineLoaders_.end()) {
        HILOGE("not load virusEngine");
        return FAILED;
    }
    if (engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE].get() == nullptr) {
        HILOGE("dynamic loader is null");
        return FAILED;
    }
    IVirusEngine *virusEngine = engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE]->GetVirusEngine();
    if (virusEngine == nullptr) {
        HILOGE("Get dynamicEngine error");
        return FAILED;
    }
    int32_t ret = virusEngine->RegisterScanResultListener(listener);
    if (ret != SUCCESS) {
        HILOGE("dynamicVirusEngine RegisterScanResultListener error, ret=%{public}d", ret);
        return FAILED;
    }
    return SUCCESS;
}

int32_t VirusEngineManager::UnRegisterScanResultListener(std::shared_ptr<ScanResultListener> &listener)
{
    std::shared_lock<std::shared_mutex> readLock(engineMutex_);
    if (engineLoaders_.find(VirusEngine::CSPL_DYNAMIC_ENGINE) == engineLoaders_.end()) {
        HILOGE("not load virusEngine");
        return FAILED;
    }
    if (engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE].get() == nullptr) {
        HILOGE("dynamic loader is null");
        return FAILED;
    }
    IVirusEngine *virusEngine = engineLoaders_[VirusEngine::CSPL_DYNAMIC_ENGINE]->GetVirusEngine();
    if (virusEngine == nullptr) {
        HILOGE("Get dynamicEngine error");
        return FAILED;
    }
    int32_t ret = virusEngine->UnRegisterScanResultListener(listener);
    if (ret != SUCCESS) {
        HILOGE("dynamicVirusEngine UnRegisterScanResultListener error, ret=%{public}d", ret);
        return FAILED;
    }
    return SUCCESS;
}

int32_t VirusEngineManager::PerformInit()
{
    HILOGI("Virus engine manager begin init");
    std::unique_lock<std::shared_mutex> lock(engineMutex_);
    if (isInitialized_) {
        return SUCCESS;
    }
    VirusEngineConfig config{};
#ifdef VPS_DEBUG_BUILD
    if (DebugConfig::DisableVirusClear()) {
        config.allowVirusClear = false;
    }
#endif
    for (const auto &engineName : g_virusEngineNameMaps) {
        if (engineLoaders_.find(engineName.second) == engineLoaders_.end() &&
            SecurityPolicyManager::GetInstance().IsEngineEnabled(engineName.first)) {
            HILOGI("Loading engine: %{public}s", engineName.first.c_str());
            std::unique_ptr<LibLoader> engineLoader = std::make_unique<LibLoader>(LIBRARY_PATHS[engineName.second]);
            int32_t ret = engineLoader->CreateVirusEngine(&config);
            if (ret != SUCCESS) {
                HILOGE(
                    "LoadLib error, ret=%{public}d, path : %{private}s", ret, LIBRARY_PATHS[engineName.second].c_str());
                continue;
            }
            engineLoaders_.emplace(engineName.second, std::move(engineLoader));
        }
    }

    auto onEvent = [this](const EventType type) {
        if (type == EventType::FEATURE_UPDATED) {
            this->UpdateFeatureLib();
        }
    };
    EventBus::GetInstance().Subscribe(EventType::FEATURE_UPDATED, onEvent);
    EventBus::GetInstance().PublishAsyn(EventType::DYNAMIC_FEATURE_UPDATE);
    EventBus::GetInstance().PublishAsyn(EventType::DYNAMIC_FEATURE_APP_INSPECT);
    isInitialized_ = true;
    HILOGI("Virus engine manager init success");
    return engineLoaders_.empty() ? FAILED : SUCCESS;
}

int32_t VirusEngineManager::UpdateFeatureLib()
{
    HILOGI("UpdateFeatureLib start");
    if (!isInitialized_) {
        HILOGE("VirusEngineManager has not been initialized");
        return FAILED;
    }
    HILOGI("UpdateFeatureLib deinit starts");
    DeInit();
    HILOGI("UpdateFeatureLib deinit ends");
    return Init();
}

void VirusEngineManager::SignalHandler(int32_t signal)
{
    int32_t errCode = 0;
    int64_t realTimes = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::lock_guard<std::mutex> lock(g_taskMutex);
    for (auto task : g_Tasks) {
        auto fileInfo = task->fileInfos.front();
        std::vector<std::any> virusInfoRecords;
        int32_t queryRet =
            VirusInformationHelper::GetInstance()->QueryByIntColumn("VIRUS_ID", fileInfo->inode, virusInfoRecords);
        if (queryRet != SUCCESS) {
            return;
        }
        if (virusInfoRecords.empty()) {
            DBBeanVirusInformation virusDBInfo = DBBeanVirusInformation::Builder().SetVirusId(fileInfo->inode).Build();
            virusDBInfo.SetScanTime(realTimes);
            virusDBInfo.SetFilePath(fileInfo->filePath);
            virusDBInfo.SetLastModifyTime(fileInfo->mtime);
            virusDBInfo.SetUserID(task->accountId);
            virusDBInfo.SetBundleName(task->bundleName);
            virusDBInfo.SetLevel(static_cast<int32_t>(ThreatLevel::CRASH_RISK));
            virusDBInfo.SetHash(fileInfo->fileHash);
            virusDBInfo.SetFileSize(fileInfo->fileSize);
            virusDBInfo.SetEngineInfo("");
            virusDBInfo.SetVirusTypes(static_cast<int32_t>(VirusType::VIRUS_TYPE_UNKNOWN));
            virusDBInfo.SetVirusStatus(static_cast<int32_t>(VirusStatus::VIRUS_STATUS_IGNORED));
            virusDBInfo.SetIsPrivacy(true);
            VirusInformationHelper::GetInstance()->InsertDBBean(virusDBInfo, errCode);
        } else {
            DBBeanVirusInformation virusDBInfo = std::any_cast<DBBeanVirusInformation>(virusInfoRecords.front());
            virusDBInfo.SetScanTime(realTimes);
            virusDBInfo.SetVirusStatus(static_cast<int32_t>(VirusStatus::VIRUS_STATUS_IGNORED));
            VirusInformationHelper::GetInstance()->UpdateDBBeanById(virusDBInfo, errCode);
        }
    }
}

void VirusEngineManager::ScanFile(const ScanTask *scanTask, ScanResult *scanResult)
{
    if (!isInitialized_) {
        int32_t initRet = PerformInit();
        if (initRet != SUCCESS) {
            HILOGE("Failed to init before ScanFile, ret = %{public}d, engineLoaders.size = %{public}zu",
                initRet,
                engineLoaders_.size());
            return;
        }
    }
    {
        std::lock_guard<std::mutex> taskLock(g_taskMutex);
        SignalGuard guard;
        for (auto sig : SIGNALS) {
            guard.Register(sig, SignalHandler);
        }
        g_Tasks.insert(scanTask);
    }
    {
        std::shared_lock<std::shared_mutex> lock(engineMutex_);
        for (auto &[engine, loader] : engineLoaders_) {
            if (g_residentEngineNameMaps.find(engine) != g_residentEngineNameMaps.end() &&
                !SecurityPolicyManager::GetInstance().IsEngineEnabled(g_residentEngineNameMaps[engine])) {
                continue;
            }
            if (IVirusEngine *virusEngine = loader->GetVirusEngine(); virusEngine) {
                int32_t ret = virusEngine->ScanFile(scanTask, scanResult);
                if (ret != SUCCESS) {
                    HILOGW("Scan failed for engine %{public}d: error %{public}d", static_cast<int32_t>(engine), ret);
                }
            }
        }
    }
    std::lock_guard<std::mutex> taskLock(g_taskMutex);
    g_Tasks.erase(scanTask);
}

void VirusEngineManager::PerformDeInit()
{
    HILOGI("Virus engine manager perform deInit");
    std::unique_lock<std::shared_mutex> lock(engineMutex_);
    if (!isInitialized_) {
        return;
    }
    EventBus::GetInstance().UnSubscribe(EventType::FEATURE_UPDATED);
    for (const auto virusEngine : g_nonResidentVirusEngineList) {
        engineLoaders_.erase(virusEngine);
    }
    isInitialized_ = false;
    HILOGI("Virus engine manager end to perform deinit");
}
}  // namespace OHOS::Security::VirusProtectionService
