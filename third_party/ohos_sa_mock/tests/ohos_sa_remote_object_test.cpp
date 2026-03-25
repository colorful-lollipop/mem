#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

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

TEST(OhosSaRemoteObjectTest, NotifiesDeathRecipients)
{
    auto object = std::make_shared<OHOS::IRemoteObject>();
    auto recipient = std::make_shared<TestDeathRecipient>();

    EXPECT_FALSE(object->IsObjectDead());
    ASSERT_TRUE(object->AddDeathRecipient(recipient));
    object->NotifyRemoteDiedForTest();

    EXPECT_TRUE(object->IsObjectDead());
    EXPECT_TRUE(recipient->called.load());
    EXPECT_FALSE(recipient->last_remote.expired());
}

TEST(OhosSaRemoteObjectTest, ConcurrentAddDeathRecipientNotifiesAllRecipients)
{
    constexpr int kIterations = 32;
    constexpr int kThreads = 16;

    for (int iteration = 0; iteration < kIterations; ++iteration) {
        auto object = std::make_shared<OHOS::IRemoteObject>();
        std::atomic<bool> start{false};
        std::atomic<int> addFailures{0};
        std::vector<std::shared_ptr<TestDeathRecipient>> recipients;
        recipients.reserve(kThreads);
        std::vector<std::thread> threads;
        threads.reserve(kThreads);

        for (int index = 0; index < kThreads; ++index) {
            auto recipient = std::make_shared<TestDeathRecipient>();
            recipients.push_back(recipient);
            threads.emplace_back([&object, recipient, &start, &addFailures]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                if (!object->AddDeathRecipient(recipient)) {
                    addFailures.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& thread : threads) {
            thread.join();
        }

        EXPECT_EQ(addFailures.load(), 0);

        object->NotifyRemoteDiedForTest();
        for (const auto& recipient : recipients) {
            ASSERT_NE(recipient, nullptr);
            EXPECT_TRUE(recipient->called.load());
            EXPECT_FALSE(recipient->last_remote.expired());
        }
    }
}
