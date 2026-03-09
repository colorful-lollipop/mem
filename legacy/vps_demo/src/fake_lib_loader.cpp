#include "vps_demo/lib_loader.h"

#include <memory>
#include <utility>

namespace OHOS::Security::VirusProtectionService {

std::unique_ptr<IVirusEngine> CreateFakeVirusEngine(const std::string& libPath);

LibLoader::LibLoader(std::string soPath) : libPath_(std::move(soPath)) {}

LibLoader::~LibLoader() {
  DestroyVirusEngine();
}

int32_t LibLoader::CreateVirusEngine(const VirusEngineConfig* config) {
  if (virusEngine_ != nullptr) {
    return SUCCESS;
  }
  virusEngine_ = CreateFakeVirusEngine(libPath_);
  if (virusEngine_ == nullptr) {
    return FAILED;
  }
  return virusEngine_->Init(config);
}

int32_t LibLoader::DestroyVirusEngine() {
  if (virusEngine_ == nullptr) {
    return SUCCESS;
  }
  const int32_t result = virusEngine_->DeInit();
  virusEngine_.reset();
  return result;
}

IVirusEngine* LibLoader::GetVirusEngine() const {
  return virusEngine_.get();
}

}  // namespace OHOS::Security::VirusProtectionService
