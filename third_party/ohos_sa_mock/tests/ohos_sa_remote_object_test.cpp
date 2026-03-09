#include <gtest/gtest.h>

#include <atomic>

#include "iremote_object.h"

namespace {

class TestDeathRecipient : public OHOS::IRemoteObject::DeathRecipient {
 public:
  void OnRemoteDied(const OHOS::wptr<OHOS::IRemoteObject>& remote) override
  {
    called.store(true);
    last_remote = remote;
  }

  std::atomic<bool> called{false};
  OHOS::wptr<OHOS::IRemoteObject> last_remote;
};

}  // namespace

TEST(OhosSaRemoteObjectTest, NotifiesDeathRecipients) {
  auto object = std::make_shared<OHOS::IRemoteObject>();
  auto recipient = std::make_shared<TestDeathRecipient>();

  ASSERT_TRUE(object->AddDeathRecipient(recipient));
  object->NotifyRemoteDiedForTest();

  EXPECT_TRUE(recipient->called.load());
  EXPECT_FALSE(recipient->last_remote.expired());
}
