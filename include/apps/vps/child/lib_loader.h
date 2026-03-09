#ifndef APPS_VPS_CHILD_LIB_LOADER_H_
#define APPS_VPS_CHILD_LIB_LOADER_H_

#include <memory>
#include <string>

#include "apps/vps/child/i_virus_engine.h"

namespace OHOS::Security::VirusProtectionService {

class LibLoader {
 public:
  explicit LibLoader(std::string soPath);
  ~LibLoader();

  int32_t CreateVirusEngine(const VirusEngineConfig* config);
  int32_t DestroyVirusEngine();
  IVirusEngine* GetVirusEngine() const;

 private:
  std::string libPath_;
  std::unique_ptr<IVirusEngine> virusEngine_;
};

}  // namespace OHOS::Security::VirusProtectionService

#endif  // APPS_VPS_CHILD_LIB_LOADER_H_
