#include <gtest/gtest.h>

#include "vpsdemo/testkit/testkit_service.h"

namespace vpsdemo::testkit {

TEST(TestkitServiceTest, EchoAndAddWork) {
    TestkitService service;

    EchoRequest echoRequest;
    echoRequest.text = "ping";
    const EchoReply echoReply = service.Echo(echoRequest);
    EXPECT_EQ(echoReply.text, "ping");

    AddRequest addRequest;
    addRequest.lhs = 9;
    addRequest.rhs = 4;
    const AddReply addReply = service.Add(addRequest);
    EXPECT_EQ(addReply.sum, 13);
}

TEST(TestkitServiceTest, SleepWorks) {
    TestkitService service;

    SleepRequest sleepRequest;
    sleepRequest.delayMs = 1;
    const SleepReply sleepReply = service.Sleep(sleepRequest);
    EXPECT_EQ(sleepReply.status, 0);
}

}  // namespace vpsdemo::testkit
