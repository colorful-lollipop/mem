#ifndef OHOS_SA_MOCK_SYSTEM_ABILITY_H
#define OHOS_SA_MOCK_SYSTEM_ABILITY_H

#include <string>

#include "iservice_registry.h"
#include "mock_service_socket.h"

namespace OHOS {

class SystemAbility : public virtual RefBase {
 public:
  SystemAbility(int32_t systemAbilityId, bool runOnCreate);
  ~SystemAbility() override;

  virtual void OnStart();
  virtual void OnStop();
  virtual void OnAddSystemAbility(int32_t systemAbilityId, const std::string& deviceId);
  virtual void OnRemoveSystemAbility(int32_t systemAbilityId, const std::string& deviceId);

  int32_t GetSystemAbilityId() const;
  bool IsRunOnCreate() const;

  template <typename T>
  bool Publish(T* service)
  {
    if (service == nullptr) {
      return false;
    }
    return Publish(service->AsObject());
  }

  bool Publish(const sptr<IRemoteObject>& object);

 private:
  int32_t system_ability_id_;
  bool run_on_create_;
  MockServiceSocket transport_;
};

#define DECLARE_SYSTEM_ABILITY(class_name)
#define REGISTER_SYSTEM_ABILITY_BY_ID(class_name, sa_id, run_on_create)

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_SYSTEM_ABILITY_H
