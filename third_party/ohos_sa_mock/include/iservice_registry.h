#ifndef OHOS_SA_MOCK_ISERVICE_REGISTRY_H
#define OHOS_SA_MOCK_ISERVICE_REGISTRY_H

#include "if_system_ability_manager.h"

namespace OHOS {

class SystemAbilityManagerClient {
 public:
  static SystemAbilityManagerClient& GetInstance();

  sptr<ISystemAbilityManager> GetSystemAbilityManager();

 private:
  SystemAbilityManagerClient() = default;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_ISERVICE_REGISTRY_H
