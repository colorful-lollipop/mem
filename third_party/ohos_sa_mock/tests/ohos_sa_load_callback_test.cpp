#include <gtest/gtest.h>

#include <string>

#include "iremote_broker.h"
#include "iremote_stub.h"
#include "iservice_registry.h"
#include "system_ability.h"

namespace {

class IDemoLoadAbility : public OHOS::IRemoteBroker {
public:
    ~IDemoLoadAbility() override = default;
    virtual std::string Ping() = 0;
};

class DemoLoadAbilityService : public OHOS::SystemAbility, public OHOS::IRemoteStub<IDemoLoadAbility> {
public:
    DemoLoadAbilityService()
        : OHOS::SystemAbility(40102, true)
    {
    }

    std::string Ping() override
    {
        return "load-pong";
    }
};

}  // namespace

TEST(OhosSaLoadCallbackTest, LoadSystemAbilitySucceedsForPublishedService)
{
    auto service = std::make_shared<DemoLoadAbilityService>();
    ASSERT_TRUE(service->Publish(service.get()));

    auto registry = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();

    auto object = registry->LoadSystemAbility(40102, 5000);
    ASSERT_NE(object, nullptr);

    auto proxy = OHOS::iface_cast<IDemoLoadAbility>(object);
    ASSERT_NE(proxy, nullptr);
    EXPECT_EQ(proxy->Ping(), "load-pong");
}

TEST(OhosSaLoadCallbackTest, LoadSystemAbilityFailsForUnknownService)
{
    auto registry = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();

    auto object = registry->LoadSystemAbility(49999, 5000);
    EXPECT_EQ(object, nullptr);
}
