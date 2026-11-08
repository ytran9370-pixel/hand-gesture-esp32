// frame_parser.cpp - one switch case per state of docs/uml/state_frame_parser.puml.
#include "hg/frame_parser.hpp"

namespace hg {

namespace {
std::uint16_t crcByte(std::uint16_t crc, std::uint8_t b) { return crc16(&b, 1, crc); }
}  // namespace

void FrameParser::reset() {
    state_ = State::WaitSof0;
    len_ = 0;
    crcCalc_ = 0xFFFF;
}

FrameParser::Event FrameParser::feed(std::uint8_t b) {
    switch (state_) {
    case State::WaitSof0:
        if (b == kSof0) {
            state_ = State::WaitSof1;
        }
        break;

    case State::WaitSof1:
        // "A5 A5 5A" must still synchronise: a repeated 0xA5 keeps us in WaitSof1.
        if (b == kSof1) {
            state_ = State::Type;
            crcCalc_ = 0xFFFF;
        } else if (b != kSof0) {
            state_ = State::WaitSof0;
        }
        break;

    case State::Type:
        frame_.type = b;
        crcCalc_ = crcByte(crcCalc_, b);
        state_ = State::Seq;
        break;

    case State::Seq:
        frame_.seq = b;
        crcCalc_ = crcByte(crcCalc_, b);
        state_ = State::LenLo;
        break;

    case State::LenLo:
        len_ = b;
        crcCalc_ = crcByte(crcCalc_, b);
        state_ = State::LenHi;
        break;

    case State::LenHi:
        len_ |= static_cast<std::size_t>(b) << 8;
        crcCalc_ = crcByte(crcCalc_, b);
        if (len_ > kMaxPayload) {
            reset();
            return Event::LenError;
        }
        frame_.payload.clear();
        frame_.payload.reserve(len_);
        state_ = (len_ > 0) ? State::Payload : State::CrcLo;
        break;

    case State::Payload:
        frame_.payload.push_back(b);
        crcCalc_ = crcByte(crcCalc_, b);
        if (frame_.payload.size() == len_) {
            state_ = State::CrcLo;
        }
        break;

    case State::CrcLo:
        crcRx_ = b;
        state_ = State::CrcHi;
        break;

    case State::CrcHi: {
        crcRx_ = static_cast<std::uint16_t>(crcRx_ | (b << 8));
        const bool ok = (crcRx_ == crcCalc_);
        reset();
        return ok ? Event::FrameReady : Event::CrcError;
    }
    }
    return Event::None;
}

}  // namespace hg
