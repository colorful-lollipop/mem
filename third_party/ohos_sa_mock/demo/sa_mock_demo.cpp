#include <iostream>
#include <string>

#include "iremote_stub.h"
#include "iservice_registry.h"
#include "system_ability.h"

namespace {

constexpr int32_t DEMO_SA_ID = 40110;

class IDemoPing : public OHOS::IRemoteBroker {
 public:
  ~IDemoPing() override = default;
  virtual std::string Ping() = 0;
};

class DemoPingService : public OHOS::SystemAbility,
                        public OHOS::IRemoteStub<IDemoPing> {
 public:
  DemoPingService() : OHOS::SystemAbility(DEMO_SA_ID, true) {}

  std::string Ping() override
  {
    return "pong-from-sa";
  }
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

  auto object = registry->GetSystemAbility(DEMO_SA_ID);
  auto ping = OHOS::iface_cast<IDemoPing>(object);
  if (ping == nullptr) {
    std::cerr << "iface_cast failed" << std::endl;
    return 1;
  }
  std::cout << "get: " << ping->Ping() << std::endl;

  auto loaded_object = registry->LoadSystemAbility(DEMO_SA_ID, 5000);
  if (loaded_object == nullptr) {
    std::cerr << "load failed" << std::endl;
    return 1;
  }
  auto loaded_ping = OHOS::iface_cast<IDemoPing>(loaded_object);
  std::cout << "load: " << loaded_ping->Ping() << std::endl;

  auto recipient = std::make_shared<DemoDeathRecipient>();
  if (!object->AddDeathRecipient(recipient)) {
    std::cerr << "add death recipient failed" << std::endl;
    return 1;
  }

  if (registry->UnloadSystemAbility(DEMO_SA_ID) != OHOS::ERR_OK) {
    std::cerr << "unload failed" << std::endl;
    return 1;
  }
  std::cout << "death_recipient_called: " << (recipient->called ? "true" : "false")
            << std::endl;
  return recipient->called ? 0 : 1;
}
