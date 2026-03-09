#include <gtest/gtest.h>

#include <string>

#include "iremote_broker.h"
#include "iremote_stub.h"
#include "iservice_registry.h"
#include "isystem_ability_load_callback.h"
#include "system_ability.h"

namespace {

class IDemoLoadAbility : public OHOS::IRemoteBroker {
 public:
  ~IDemoLoadAbility() override = default;
  virtual std::string Ping() = 0;
};

class DemoLoadAbilityService : public OHOS::SystemAbility,
                               public OHOS::IRemoteStub<IDemoLoadAbility> {
 public:
  DemoLoadAbilityService() : OHOS::SystemAbility(40102, true) {}

  std::string Ping() override
  {
    return "load-pong";
  }
};

class TestLoadCallback : public OHOS::SystemAbilityLoadCallbackStub {
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

}  // namespace

TEST(OhosSaLoadCallbackTest, LoadSystemAbilitySucceedsForPublishedService) {
  auto service = std::make_shared<DemoLoadAbilityService>();
  ASSERT_TRUE(service->Publish(service.get()));

  auto registry = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
  auto callback = std::make_shared<TestLoadCallback>();

  ASSERT_EQ(registry->LoadSystemAbility(40102, callback), OHOS::ERR_OK);
  ASSERT_NE(callback->loaded_object, nullptr);
  EXPECT_EQ(callback->loaded_sid, 40102);

  auto proxy = OHOS::iface_cast<IDemoLoadAbility>(callback->loaded_object);
  ASSERT_NE(proxy, nullptr);
  EXPECT_EQ(proxy->Ping(), "load-pong");
}

TEST(OhosSaLoadCallbackTest, LoadSystemAbilityFailsForUnknownService) {
  auto registry = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
  auto callback = std::make_shared<TestLoadCallback>();

  EXPECT_EQ(registry->LoadSystemAbility(49999, callback), OHOS::ERR_NAME_NOT_FOUND);
  EXPECT_EQ(callback->failed_sid, 49999);
  EXPECT_EQ(callback->loaded_object, nullptr);
}
