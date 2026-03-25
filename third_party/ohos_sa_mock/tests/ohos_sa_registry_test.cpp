#include <gtest/gtest.h>

#include <string>

#include "iremote_stub.h"
#include "iservice_registry.h"
#include "system_ability.h"

namespace {

class IDemoRegistryAbility : public OHOS::IRemoteBroker {
public:
    ~IDemoRegistryAbility() override = default;
    virtual std::string Ping() = 0;
};

class DemoRegistryAbilityService : public OHOS::SystemAbility, public OHOS::IRemoteStub<IDemoRegistryAbility> {
public:
    DemoRegistryAbilityService()
        : OHOS::SystemAbility(40101, true)
    {
    }

    std::string Ping() override
    {
        return "pong";
    }
};

}  // namespace

TEST(OhosSaRegistryTest, PublishMakesServiceDiscoverable)
{
    auto service = std::make_shared<DemoRegistryAbilityService>();

    auto registry = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    ASSERT_NE(registry, nullptr);
    EXPECT_EQ(registry->CheckSystemAbility(40101), nullptr);

    ASSERT_TRUE(service->Publish(service.get()));

    auto object = registry->GetSystemAbility(40101);
    EXPECT_NE(object, nullptr);
    EXPECT_NE(registry->CheckSystemAbility(40101), nullptr);
}
