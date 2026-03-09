#include <iostream>
#include <string>

#include "iremote_stub.h"
#include "iservice_registry.h"
#include "isystem_ability_load_callback.h"
#include "system_ability.h"

namespace {

constexpr int32_t kDemoSaId = 40110;

class IDemoPing : public OHOS::IRemoteBroker {
 public:
  ~IDemoPing() override = default;
  virtual std::string Ping() = 0;
};

class DemoPingService : public OHOS::SystemAbility,
                        public OHOS::IRemoteStub<IDemoPing> {
 public:
  DemoPingService() : OHOS::SystemAbility(kDemoSaId, true) {}

  std::string Ping() override
  {
    return "pong-from-sa";
  }
};

class DemoLoadCallback : public OHOS::SystemAbilityLoadCallbackStub {
 public:
  void OnLoadSystemAbilitySuccess(
      int32_t systemAbilityId,
      const OHOS::sptr<OHOS::IRemoteObject>& object) override
  {
    loaded_sid = systemAbilityId;
    loaded_object = object;
  }

  void OnLoadSystemAbilityFail(int32_t systemAbilityId) override
  {
    failed_sid = systemAbilityId;
  }

  int32_t loaded_sid = -1;
  int32_t failed_sid = -1;
  OHOS::sptr<OHOS::IRemoteObject> loaded_object;
};

class DemoDeathRecipient : public OHOS::IRemoteObject::DeathRecipient {
 public:
  void OnRemoteDied(const OHOS::wptr<OHOS::IRemoteObject>& remote) override
  {
    called = !remote.expired();
  }

  bool called = false;
};

}  // namespace

int main() {
  auto registry = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
  auto service = std::make_shared<DemoPingService>();

  service->OnStart();
  if (!service->Publish(service.get())) {
    std::cerr << "publish failed" << std::endl;
    return 1;
  }

  auto object = registry->GetSystemAbility(kDemoSaId);
  auto ping = OHOS::iface_cast<IDemoPing>(object);
  if (ping == nullptr) {
    std::cerr << "iface_cast failed" << std::endl;
    return 1;
  }
  std::cout << "get: " << ping->Ping() << std::endl;

  auto callback = std::make_shared<DemoLoadCallback>();
  if (registry->LoadSystemAbility(kDemoSaId, callback) != OHOS::ERR_OK ||
      callback->loaded_object == nullptr) {
    std::cerr << "load failed" << std::endl;
    return 1;
  }
  auto loaded_ping = OHOS::iface_cast<IDemoPing>(callback->loaded_object);
  std::cout << "load: " << loaded_ping->Ping() << std::endl;

  auto recipient = std::make_shared<DemoDeathRecipient>();
  if (!object->AddDeathRecipient(recipient)) {
    std::cerr << "add death recipient failed" << std::endl;
    return 1;
  }

  if (registry->UnloadSystemAbility(kDemoSaId) != OHOS::ERR_OK) {
    std::cerr << "unload failed" << std::endl;
    return 1;
  }
  std::cout << "death_recipient_called: " << (recipient->called ? "true" : "false")
            << std::endl;
  return recipient->called ? 0 : 1;
}
