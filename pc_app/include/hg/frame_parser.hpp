// frame_parser.hpp - byte-by-byte state machine that extracts frames from a UART byte stream.
// State diagram: docs/uml/state_frame_parser.svg
#pragma once

#include <cstddef>
#include <cstdint>

#include "hg/protocol.hpp"

namespace hg {

class FrameParser {
public:
    enum class Event { None, FrameReady, CrcError, LenError };

    // Feed one received byte. When FrameReady is returned, the frame is available via frame().
    Event feed(std::uint8_t byte);

    // Last complete, CRC-valid frame.
    const Frame& frame() const { return frame_; }

    // True while a frame is partially received (used for the inter-byte timeout).
    bool busy() const { return state_ != State::WaitSof0; }

    // Drop a partially received frame and wait for the next start-of-frame.
    void reset();

private:
    enum class State { WaitSof0, WaitSof1, Type, Seq, LenLo, LenHi, Payload, CrcLo, CrcHi };

    State state_ = State::WaitSof0;
    Frame frame_;
    std::size_t len_ = 0;
    std::uint16_t crcCalc_ = 0xFFFF;
    std::uint16_t crcRx_ = 0;
};

}  // namespace hg
