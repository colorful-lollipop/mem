#include "vps_demo/i_virus_engine.h"

#include <memory>
#include <string>

namespace OHOS::Security::VirusProtectionService {

namespace {

VirusEngine DetectEngine(const std::string& libPath) {
  if (libPath.find("dynamic") != std::string::npos) {
    return VirusEngine::CSPL_DYNAMIC_ENGINE;
  }
  if (libPath.find("trustone") != std::string::npos) {
    return VirusEngine::TRUSTONE_STATIC_ENGINE;
  }
  if (libPath.find("qowl") != std::string::npos) {
    return VirusEngine::QOWL_STATIC_ENGINE;
  }
  if (libPath.find("atcore") != std::string::npos) {
    return VirusEngine::ATCORE_STATIC_ENGINE;
  }
  return VirusEngine::CSPL_STATIC_ENGINE;
}

class FakeVirusEngine final : public IVirusEngine {
 public:
  explicit FakeVirusEngine(std::string libPath)
      : lib_path_(std::move(libPath)), engine_(DetectEngine(lib_path_)) {}

  int32_t Init(const VirusEngineConfig* config) override {
    allow_virus_clear_ = config == nullptr ? true : config->allowVirusClear;
    initialized_ = true;
    return SUCCESS;
  }

  int32_t DeInit() override {
    initialized_ = false;
    return SUCCESS;
  }

  int32_t ScanFile(const ScanTask* scanTask, ScanResult* scanResult) override {
    if (!initialized_ || scanTask == nullptr || scanResult == nullptr) {
      return FAILED;
    }
    const size_t index = static_cast<size_t>(engine_);
    if (index >= scanResult->engineResults.size()) {
      return FAILED;
    }

    bool infected = false;
    for (const auto& file : scanTask->fileInfos) {
      if (file != nullptr && file->filePath.find("virus") != std::string::npos) {
        infected = true;
        break;
      }
    }

    auto& engine_result = scanResult->engineResults[index];
    if (infected) {
      engine_result.virusName = "Fake.Virus";
      engine_result.virusType = "test";
      engine_result.errorMsg = "detected";
      engine_result.errorCode = SUCCESS;
      engine_result.level = ThreatLevel::HIGH_RISK;
    } else {
      engine_result.virusName.clear();
      engine_result.virusType = "clean";
      engine_result.errorMsg = allow_virus_clear_ ? "clean" : "clean-no-clear";
      engine_result.errorCode = SUCCESS;
      engine_result.level = ThreatLevel::NO_RISK;
    }
    return SUCCESS;
  }

  int32_t ScanBehavior(uint32_t accessToken,
                       const std::string& event,
                       const std::string& bundleName) override {
    if (!initialized_ || accessToken == 0u || event.empty() || bundleName.empty()) {
      return FAILED;
    }
    return engine_ == VirusEngine::CSPL_DYNAMIC_ENGINE ? SUCCESS : FAILED;
  }

 private:
  std::string lib_path_;
  VirusEngine engine_;
  bool initialized_ = false;
  bool allow_virus_clear_ = true;
};

}  // namespace

std::unique_ptr<IVirusEngine> CreateFakeVirusEngine(const std::string& libPath) {
  return std::make_unique<FakeVirusEngine>(libPath);
}

}  // namespace OHOS::Security::VirusProtectionService
