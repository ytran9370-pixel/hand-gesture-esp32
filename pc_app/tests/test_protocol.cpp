// test_protocol.cpp - CRC, frame encoding and payload decoding (FR-COM-01, FR-COM-02).
#include <gtest/gtest.h>

#include "hg/errors.hpp"
#include "hg/messages.hpp"
#include "hg/protocol.hpp"

using Bytes = std::vector<std::uint8_t>;

TEST(Crc16, MatchesStandardCheckValue) {
    const char* text = "123456789";
    EXPECT_EQ(hg::crc16(reinterpret_cast<const std::uint8_t*>(text), 9), 0x29B1);
}

// The example frames of docs/protocol.md section 6 must be reproduced byte for byte.
TEST(EncodeFrame, MatchesSpecExamples) {
    EXPECT_EQ(hg::encodeFrame(0x01, 1), (Bytes{0xA5, 0x5A, 0x01, 0x01, 0x00, 0x00, 0x44, 0xC5}));
    EXPECT_EQ(hg::encodeFrame(0x06, 2), (Bytes{0xA5, 0x5A, 0x06, 0x02, 0x00, 0x00, 0x39, 0xCD}));
    EXPECT_EQ(hg::encodeFrame(0x07, 3, {0x02, 0x01}),
              (Bytes{0xA5, 0x5A, 0x07, 0x03, 0x02, 0x00, 0x02, 0x01, 0xA8, 0xB3}));
    EXPECT_EQ(hg::encodeFrame(0x7F, 7, {0x01, 0x01}),
              (Bytes{0xA5, 0x5A, 0x7F, 0x07, 0x02, 0x00, 0x01, 0x01, 0x23, 0x27}));
}

TEST(EncodeFrame, LengthIsLittleEndian) {
    const Bytes f = hg::encodeFrame(0x02, 9, Bytes(hg::kImageBytes, 0));
    EXPECT_EQ(f[4], 0x00);  // 12288 = 0x3000
    EXPECT_EQ(f[5], 0x30);
    EXPECT_EQ(f.size(), hg::kImageBytes + 8);
}

TEST(EncodeFrame, RejectsOversizedPayload) {
    EXPECT_THROW(hg::encodeFrame(0x02, 1, Bytes(hg::kMaxPayload + 1, 0)), hg::ProtocolError);
}

TEST(Messages, SysStateRoundTrip) {
    hg::SysState s;
    s.lock = hg::LockState::Unlocked;
    s.outputs = 0b1010;  // LED2 on, door open
    s.lastGesture = 4;
    s.unlockRemainingS = 300;
    const hg::SysState d = hg::decodeSysState(hg::encodeSysState(s));
    EXPECT_EQ(d.lock, hg::LockState::Unlocked);
    EXPECT_FALSE(d.led(1));
    EXPECT_TRUE(d.led(2));
    EXPECT_TRUE(d.doorOpen());
    EXPECT_EQ(d.unlockRemainingS, 300);
}

TEST(Messages, ShortPayloadThrowsInsteadOfReadingPastTheEnd) {
    EXPECT_THROW(hg::decodeSysState(Bytes{0x01, 0x00}), hg::ProtocolError);
    EXPECT_THROW(hg::decodePing(Bytes(10, 0)), hg::ProtocolError);
}

TEST(Messages, DecodeInferReadsSignedLogits) {
    Bytes p{3, 2, 0x84, 0x03, 1, 0, 0, 0, 2, 0, 0, 0, 0, 1, 1, 0};
    p.insert(p.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x00, 0x00, 0x00});  // -1, 16
    const hg::InferResult r = hg::decodeInfer(p);
    EXPECT_EQ(r.classId, 3);
    EXPECT_EQ(r.confPermille, 900);
    ASSERT_EQ(r.logits.size(), 2U);
    EXPECT_EQ(r.logits[0], -1);
    EXPECT_EQ(r.logits[1], 16);
}
