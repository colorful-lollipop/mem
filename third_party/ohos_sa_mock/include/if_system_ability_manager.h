#ifndef OHOS_SA_MOCK_IF_SYSTEM_ABILITY_MANAGER_H
#define OHOS_SA_MOCK_IF_SYSTEM_ABILITY_MANAGER_H

#include "errors.h"
#include "iremote_broker.h"

namespace OHOS {

class ISystemAbilityManager : public IRemoteBroker {
 public:
  ~ISystemAbilityManager() override = default;

  virtual ErrCode AddSystemAbility(int32_t systemAbilityId,
                                   const sptr<IRemoteObject>& object) = 0;
  virtual sptr<IRemoteObject> GetSystemAbility(int32_t systemAbilityId) = 0;
  virtual sptr<IRemoteObject> CheckSystemAbility(int32_t systemAbilityId) = 0;
  virtual sptr<IRemoteObject> LoadSystemAbility(int32_t systemAbilityId,
                                                int32_t timeout) = 0;
  virtual ErrCode UnloadSystemAbility(int32_t systemAbilityId) = 0;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_IF_SYSTEM_ABILITY_MANAGER_H
