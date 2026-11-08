// test_frame_parser.cpp - receiver state machine (FR-COM-02, FR-COM-03).
#include <gtest/gtest.h>

#include <random>

#include "hg/frame_parser.hpp"

using Bytes = std::vector<std::uint8_t>;
using Ev = hg::FrameParser::Event;

namespace {
// Feed a byte stream and collect the events that are not None.
std::vector<Ev> feedAll(hg::FrameParser& p, const Bytes& stream) {
    std::vector<Ev> events;
    for (auto b : stream) {
        const Ev e = p.feed(b);
        if (e != Ev::None) {
            events.push_back(e);
        }
    }
    return events;
}
}  // namespace

TEST(FrameParser, ParsesEncodedFrame) {
    hg::FrameParser p;
    const Bytes payload{1, 2, 3, 0xA5, 0x5A};  // payload may contain the SOF pattern
    EXPECT_EQ(feedAll(p, hg::encodeFrame(0x82, 42, payload)), std::vector<Ev>{Ev::FrameReady});
    EXPECT_EQ(p.frame().type, 0x82);
    EXPECT_EQ(p.frame().seq, 42);
    EXPECT_EQ(p.frame().payload, payload);
}

TEST(FrameParser, ParsesFullSizeImageFrame) {
    hg::FrameParser p;
    Bytes img(hg::kImageBytes);
    for (std::size_t i = 0; i < img.size(); ++i) {
        img[i] = static_cast<std::uint8_t>(i * 31 + 7);
    }
    EXPECT_EQ(feedAll(p, hg::encodeFrame(0x02, 1, img)), std::vector<Ev>{Ev::FrameReady});
    EXPECT_EQ(p.frame().payload, img);
}

TEST(FrameParser, ResynchronisesAfterBootNoise) {
    hg::FrameParser p;
    Bytes stream{'E', 'S', 'P', '-', 'R', 'O', 'M', 0xA5, 0xA5};  // ends with A5 A5
    const Bytes frame = hg::encodeFrame(0x81, 1);
    stream.insert(stream.end(), frame.begin(), frame.end());
    EXPECT_EQ(feedAll(p, stream), std::vector<Ev>{Ev::FrameReady});
}

TEST(FrameParser, DetectsCorruptedByteThenRecovers) {
    hg::FrameParser p;
    Bytes bad = hg::encodeFrame(0x02, 5, Bytes(100, 7));
    bad[50] ^= 0x10;  // single bit flip
    const Bytes good = hg::encodeFrame(0x04, 6);
    bad.insert(bad.end(), good.begin(), good.end());
    EXPECT_EQ(feedAll(p, bad), (std::vector<Ev>{Ev::CrcError, Ev::FrameReady}));
    EXPECT_EQ(p.frame().seq, 6);
}

TEST(FrameParser, RejectsOversizedLength) {
    hg::FrameParser p;
    EXPECT_EQ(feedAll(p, Bytes{0xA5, 0x5A, 0x02, 9, 0xFF, 0xFF}), std::vector<Ev>{Ev::LenError});
    EXPECT_FALSE(p.busy());
}

TEST(FrameParser, ResetDropsPartialFrame) {
    hg::FrameParser p;
    const Bytes f = hg::encodeFrame(0x02, 1, Bytes(64, 1));
    feedAll(p, Bytes(f.begin(), f.begin() + 20));
    EXPECT_TRUE(p.busy());
    p.reset();  // what the firmware does after a 50 ms gap (FR-COM-04)
    EXPECT_FALSE(p.busy());
    EXPECT_EQ(feedAll(p, hg::encodeFrame(0x01, 2)), std::vector<Ev>{Ev::FrameReady});
}

// Property test: random frames, a quarter of them corrupted. A corrupted frame must never be accepted.
TEST(FrameParser, NeverAcceptsCorruptedFrames) {
    std::mt19937 rng(1234);
    for (int i = 0; i < 300; ++i) {
        Bytes payload(rng() % 300);
        for (auto& b : payload) {
            b = static_cast<std::uint8_t>(rng());
        }
        Bytes f = hg::encodeFrame(0x02, static_cast<std::uint8_t>(i), payload);
        const bool corrupt = (rng() % 4) == 0;
        if (corrupt) {
            f[2 + rng() % (f.size() - 2)] ^= static_cast<std::uint8_t>(1U << (rng() % 8));
        }
        hg::FrameParser p;
        const auto events = feedAll(p, f);
        const bool accepted = !events.empty() && events.back() == Ev::FrameReady;
        if (corrupt) {
            EXPECT_FALSE(accepted) << "corrupted frame " << i << " was accepted";
        } else {
            EXPECT_TRUE(accepted);
            EXPECT_EQ(p.frame().payload, payload);
        }
    }
}
