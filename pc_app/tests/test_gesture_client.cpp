// test_gesture_client.cpp - client behaviour against the mock device, including fault injection
// (FR-COM-02, FR-COM-03, FR-COM-05, FR-COM-06, FR-SEC-01, FR-SEC-02).
#include <gtest/gtest.h>

#include "hg/errors.hpp"
#include "hg/gesture_client.hpp"
#include "hg/mock_serial_port.hpp"

using Fault = hg::MockSerialPort::Fault;

namespace {

// Test fixture: a fresh mock device and client for every test. Short timeout keeps tests fast.
class GestureClientTest : public ::testing::Test {
protected:
    hg::MockSerialPort device;
    hg::GestureClient client{device, hg::ClientConfig{20, 2}};
};

// Observer used to check that events reach the application.
class RecordingListener : public hg::IClientListener {
public:
    void onStateChanged(const hg::StateEvent& e) override { events.push_back(e); }
    std::vector<hg::StateEvent> events;
};

}  // namespace

TEST_F(GestureClientTest, PingReturnsDeviceInfo) {
    const hg::PingInfo info = client.ping();
    EXPECT_EQ(info.protoVersion, hg::kProtocolVersion);
    EXPECT_EQ(info.nClasses, 18);
    EXPECT_EQ(info.inW, 64);
    EXPECT_EQ(info.backend, "mock");
}

TEST_F(GestureClientTest, StartsLocked) {  // FR-SEC-01
    const hg::SysState s = client.getState();
    EXPECT_EQ(s.lock, hg::LockState::Locked);
    EXPECT_EQ(s.outputs, 0);
}

TEST_F(GestureClientTest, SetOutputRejectedWhileLocked) {  // FR-SEC-02, ERROR(LOCKED)
    try {
        client.setOutput(hg::OutputTarget::Led1, true);
        FAIL() << "expected DeviceError";
    } catch (const hg::DeviceError& e) {
        EXPECT_EQ(e.code(), hg::ErrorCode::Locked);
    }
}

TEST_F(GestureClientTest, SetOutputAfterCardUnlockAndEventIsDelivered) {  // FR-SEC-03, FR-COM-05
    RecordingListener listener;
    client.addListener(&listener);

    device.tapCard(true);
    client.poll(10);
    ASSERT_EQ(listener.events.size(), 1U);
    EXPECT_EQ(listener.events[0].reason, hg::Reason::RfidOk);
    EXPECT_EQ(listener.events[0].state.lock, hg::LockState::Unlocked);

    const hg::SysState s = client.setOutput(hg::OutputTarget::Led2, true);
    EXPECT_TRUE(s.led(2));
    client.poll(10);
    ASSERT_EQ(listener.events.size(), 2U);
    EXPECT_EQ(listener.events[1].reason, hg::Reason::HostCommand);
}

TEST_F(GestureClientTest, InferReturnsClassAndLogits) {
    std::vector<std::uint8_t> img(hg::kImageBytes, 1);  // sum = 12288 -> class 12288 % 18 = 12
    const hg::InferResult r = client.infer(img);
    EXPECT_EQ(r.classId, 12);
    EXPECT_EQ(r.logits.size(), 18U);
    EXPECT_EQ(r.confPermille, 900);
}

TEST_F(GestureClientTest, InferRejectsWrongImageSize) {
    EXPECT_THROW(client.infer(std::vector<std::uint8_t>(100)), hg::ProtocolError);
}

TEST_F(GestureClientTest, RetriesAfterCorruptedResponse) {  // FR-COM-02 + FR-COM-06
    device.injectFault(Fault::CorruptResponseCrc, 1);
    EXPECT_NO_THROW(client.ping());
    EXPECT_EQ(client.stats().crcErrors, 1);
    EXPECT_EQ(client.stats().retries, 1);
}

TEST_F(GestureClientTest, RetriesAfterLostResponse) {  // FR-COM-06
    device.injectFault(Fault::DropResponse, 2);
    EXPECT_NO_THROW(client.getState());
    EXPECT_EQ(client.stats().timeouts, 2);
}

TEST_F(GestureClientTest, GivesUpAfterThreeLostResponses) {  // 1 attempt + 2 retries
    device.injectFault(Fault::DropResponse, 3);
    EXPECT_THROW(client.getState(), hg::TimeoutError);
}

TEST_F(GestureClientTest, IgnoresNoiseBeforeResponse) {  // FR-COM-03
    device.injectFault(Fault::NoiseBeforeResponse, 1);
    EXPECT_NO_THROW(client.ping());
    EXPECT_EQ(client.stats().retries, 0);
}

TEST_F(GestureClientTest, SequenceNumberWrapsAndSkipsZero) {
    for (int i = 0; i < 300; ++i) {  // more than 255 requests
        ASSERT_NO_THROW(client.getState());
    }
    EXPECT_EQ(device.framesReceived(), 300);
}
