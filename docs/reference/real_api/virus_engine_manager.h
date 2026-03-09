#ifndef VIRUS_PROTECTION_SERVICE_VIRUS_ENGINE_MANAGER_H
#define VIRUS_PROTECTION_SERVICE_VIRUS_ENGINE_MANAGER_H

#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include "virus_protection_service_define.h"
#include "lib_loader.h"
#include "i_virus_engine.h"

namespace OHOS::Security::VirusProtectionService {

class VirusEngineManager {
public:
    static VirusEngineManager &GetInstance();
    int32_t Init();
    void DeInit();
    void ScanFile(const ScanTask *scanTask, ScanResult *scanResult);
    ~VirusEngineManager();

    int32_t ScanBehavior(uint32_t accessToken, const std::string &event, const std::string &bundleName);
    int32_t IsExistAnalysisEngine(uint32_t accessToken);
    int32_t CreateAnalysisEngine(uint32_t accessToken);
    int32_t DestroyAnalysisEngine(uint32_t accessToken);
    int32_t RegisterScanResultListener(std::shared_ptr<ScanResultListener> &listener);
    int32_t UnRegisterScanResultListener(std::shared_ptr<ScanResultListener> &listener);
    int32_t UpdateFeatureLib();

private:
    VirusEngineManager(const VirusEngineManager &) = delete;
    VirusEngineManager &operator=(const VirusEngineManager &) = delete;
    VirusEngineManager();

    static void SignalHandler(int32_t signal);
    void WorkerThread();
    int32_t PerformInit();
    void PerformDeInit();

    enum class TaskType { INIT, DEINIT, SHUTDOWN };

    std::thread workerThread_;
    std::mutex queueMutex_;
    std::condition_variable taskCv_;
    std::queue<TaskType> taskQueue_;
    std::atomic<bool> isRunning_{true};
    std::atomic<int32_t> initResult_;
    std::atomic<bool> isInitialized_{false};
    std::shared_mutex engineMutex_;
    std::unordered_map<VirusEngine, std::unique_ptr<LibLoader>> engineLoaders_;
};
}  // namespace OHOS::Security::VirusProtectionService

#endif  // VIRUS_PROTECTION_SERVICE_VIRUS_ENGINE_MANAGER_H
