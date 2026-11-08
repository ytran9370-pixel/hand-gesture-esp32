// gesture_client.cpp - request/response handling with sequence numbers, timeout and retries.
// Sequence diagram: docs/uml/sequence_client_request.svg
#include "hg/gesture_client.hpp"

#include <algorithm>

#include "hg/errors.hpp"

namespace hg {

namespace {
using Clock = std::chrono::steady_clock;

int msLeft(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}
}  // namespace

GestureClient::GestureClient(ISerialPort& port, ClientConfig config) : port_(port), config_(config) {}

std::uint8_t GestureClient::nextSeq() {
    seq_ = static_cast<std::uint8_t>(seq_ == 255 ? 1 : seq_ + 1);  // 1..255, 0 is never used
    return seq_;
}

void GestureClient::addListener(IClientListener* listener) {
    if (listener != nullptr && std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end()) {
        listeners_.push_back(listener);
    }
}

void GestureClient::removeListener(IClientListener* listener) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

void GestureClient::dispatchEvent(const Frame& frame) {
    if (frame.type != static_cast<std::uint8_t>(MsgType::EvtStateChanged)) {
        return;  // unknown event type: ignore (forward compatibility, HG-ICD-001 section 8)
    }
    ++stats_.events;
    const StateEvent event = decodeStateEvent(frame.payload);
    for (IClientListener* l : listeners_) {
        l->onStateChanged(event);
    }
}

FrameParser::Event GestureClient::nextParserEvent(Clock::time_point deadline) {
    for (;;) {
        while (rxPos_ < rxLen_) {
            const FrameParser::Event ev = parser_.feed(rxBuf_[rxPos_++]);
            if (ev != FrameParser::Event::None) {
                return ev;
            }
        }
        const int left = msLeft(deadline);
        rxPos_ = 0;
        rxLen_ = port_.read(rxBuf_.data(), rxBuf_.size(), left);
        if (rxLen_ == 0 && left == 0) {
            return FrameParser::Event::None;  // deadline reached
        }
    }
}

Frame GestureClient::request(MsgType type, const std::vector<std::uint8_t>& payload) {
    ++stats_.requests;
    const auto rawType = static_cast<std::uint8_t>(type);

    for (int attempt = 0; attempt <= config_.retries; ++attempt) {
        if (attempt > 0) {
            ++stats_.retries;
        }
        const std::uint8_t seq = nextSeq();
        const std::vector<std::uint8_t> bytes = encodeFrame(rawType, seq, payload);
        parser_.reset();
        port_.write(bytes.data(), bytes.size());

        const auto deadline = Clock::now() + std::chrono::milliseconds(config_.timeoutMs);
        for (bool retry = false; !retry;) {
            const FrameParser::Event ev = nextParserEvent(deadline);
            if (ev == FrameParser::Event::None) {
                ++stats_.timeouts;
                break;  // timeout -> next attempt
            }
            if (ev == FrameParser::Event::CrcError) {
                ++stats_.crcErrors;  // corrupted response: send the request again
                retry = true;
                continue;
            }
            if (ev != FrameParser::Event::FrameReady) {
                continue;
            }
            const Frame& f = parser_.frame();
            if (isEventType(f.type)) {
                dispatchEvent(f);
                continue;
            }
            if (f.seq != seq) {
                continue;  // stale answer to an earlier attempt
            }
            if (f.type == static_cast<std::uint8_t>(MsgType::Error)) {
                const DeviceErrorInfo err = decodeError(f.payload);
                if (err.code == ErrorCode::Crc || err.code == ErrorCode::Busy) {
                    retry = true;  // transient: the request got damaged or the device was busy
                    continue;
                }
                throw DeviceError(err.code, std::string("device error ") + toString(err.code));
            }
            if (f.type != responseType(type)) {
                throw ProtocolError("unexpected response type " + std::to_string(f.type));
            }
            return f;
        }
    }
    throw TimeoutError("no valid response after " + std::to_string(config_.retries + 1) + " attempts");
}

void GestureClient::poll(int timeoutMs) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const FrameParser::Event ev = nextParserEvent(deadline);
        if (ev == FrameParser::Event::None) {
            return;
        }
        if (ev == FrameParser::Event::FrameReady && isEventType(parser_.frame().type)) {
            dispatchEvent(parser_.frame());
        }
    }
}

PingInfo GestureClient::ping() {
    PingInfo info = decodePing(request(MsgType::Ping, {}).payload);
    if (info.protoVersion != kProtocolVersion) {
        throw ProtocolError("device speaks protocol v" + std::to_string(info.protoVersion) + ", expected v" +
                            std::to_string(kProtocolVersion));
    }
    return info;
}

SysState GestureClient::getState() { return decodeSysState(request(MsgType::GetState, {}).payload); }

SysState GestureClient::setOutput(OutputTarget target, bool on) {
    return decodeSysState(request(MsgType::SetOutput, encodeSetOutput(target, on)).payload);
}

InferResult GestureClient::infer(const std::vector<std::uint8_t>& rgb64x64) {
    if (rgb64x64.size() != kImageBytes) {
        throw ProtocolError("INFER needs exactly " + std::to_string(kImageBytes) + " bytes");
    }
    return decodeInfer(request(MsgType::Infer, rgb64x64).payload);
}

}  // namespace hg
