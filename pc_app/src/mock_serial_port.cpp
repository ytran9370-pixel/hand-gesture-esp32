// mock_serial_port.cpp - behaves like the ESP32-S3 firmware described in docs/requirements.md.
#include "hg/mock_serial_port.hpp"

#include <algorithm>

namespace hg {

MockSerialPort::MockSerialPort() {
    state_.lock = LockState::Locked;  // FR-SEC-01: start LOCKED, outputs off
}

void MockSerialPort::write(const std::uint8_t* data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        switch (parser_.feed(data[i])) {
        case FrameParser::Event::FrameReady:
            ++framesReceived_;
            handle(parser_.frame());
            break;
        case FrameParser::Event::CrcError:
            ++crcErrors_;
            sendError(ErrorCode::Crc, parser_.frame().type, parser_.frame().seq);
            break;
        case FrameParser::Event::LenError:
            sendError(ErrorCode::Len, 0, 0);
            break;
        case FrameParser::Event::None:
            break;
        }
    }
}

std::size_t MockSerialPort::read(std::uint8_t* buffer, std::size_t maxLen, int /*timeoutMs*/) {
    // A real port would wait up to timeoutMs; the mock returns at once so tests stay fast.
    const std::size_t n = std::min(maxLen, toHost_.size());
    for (std::size_t i = 0; i < n; ++i) {
        buffer[i] = toHost_.front();
        toHost_.pop_front();
    }
    return n;
}

void MockSerialPort::injectFault(Fault fault, int count) {
    fault_ = fault;
    faultCount_ = count;
}

void MockSerialPort::tapCard(bool registered) {
    if (registered) {
        state_.lock = (state_.lock == LockState::Unlocked) ? LockState::Locked : LockState::Unlocked;
        state_.deniedCount = 0;
        state_.unlockRemainingS = (state_.lock == LockState::Unlocked) ? 30 : 0;
        sendEvent(Reason::RfidOk);
    } else {
        ++state_.deniedCount;
        sendEvent(Reason::RfidDenied);
    }
}

void MockSerialPort::handle(const Frame& req) {
    const auto type = static_cast<MsgType>(req.type);
    switch (type) {
    case MsgType::Ping: {
        std::vector<std::uint8_t> p(76, 0);
        p[0] = kProtocolVersion;
        p[1] = 18;                 // classes
        p[2] = 64;                 // in_w (u16 LE)
        p[4] = 64;                 // in_h
        p[6] = 3;                  // in_c
        p[7] = 9;                  // layers
        const char backend[] = "mock";
        const char model[] = "hagrid18_mock";
        const char fw[] = "0.0.0-mock";
        std::copy(backend, backend + sizeof(backend) - 1, p.begin() + 20);
        std::copy(model, model + sizeof(model) - 1, p.begin() + 28);
        std::copy(fw, fw + sizeof(fw) - 1, p.begin() + 60);
        send(responseType(type), req.seq, p);
        break;
    }
    case MsgType::GetState:
        send(responseType(type), req.seq, encodeSysState(state_));
        break;

    case MsgType::SetOutput: {
        if (req.payload.size() != 2 || req.payload[0] < 1 || req.payload[0] > 4) {
            sendError(ErrorCode::Len, req.type, req.seq);
            break;
        }
        if (state_.lock != LockState::Unlocked) {
            sendError(ErrorCode::Locked, req.type, req.seq);  // not allowed while LOCKED
            break;
        }
        const auto bit = static_cast<std::uint8_t>(1U << (req.payload[0] - 1));
        state_.outputs = req.payload[1] ? static_cast<std::uint8_t>(state_.outputs | bit)
                                        : static_cast<std::uint8_t>(state_.outputs & ~bit);
        send(responseType(type), req.seq, encodeSysState(state_));
        sendEvent(Reason::HostCommand);
        break;
    }
    case MsgType::Infer: {
        if (req.payload.size() != kImageBytes) {
            sendError(ErrorCode::Len, req.type, req.seq);
            break;
        }
        // Deterministic fake classifier: class = (sum of all pixels) mod 18.
        std::uint32_t sum = 0;
        for (auto b : req.payload) {
            sum += b;
        }
        const auto cls = static_cast<std::uint8_t>(sum % 18);
        std::vector<std::uint8_t> p{cls, 18, 0x84, 0x03};  // class, n, conf = 900 permille
        for (std::uint32_t v : {42000U, 140000U}) {         // infer_us, device_us
            for (int i = 0; i < 4; ++i) {
                p.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
            }
        }
        p.insert(p.end(), {0, 1, static_cast<std::uint8_t>(state_.lock), state_.outputs});
        for (int c = 0; c < 18; ++c) {
            const std::int32_t logit = (c == cls) ? 5000 : -1000 - c;
            for (int i = 0; i < 4; ++i) {
                p.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(logit) >> (8 * i)));
            }
        }
        send(responseType(type), req.seq, p);
        break;
    }
    default:
        sendError(ErrorCode::UnknownCmd, req.type, req.seq);
        break;
    }
}

void MockSerialPort::send(std::uint8_t type, std::uint8_t seq, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> bytes = encodeFrame(type, seq, payload);
    const bool isResponse = !isEventType(type);
    if (isResponse && faultCount_ > 0) {
        --faultCount_;
        switch (fault_) {
        case Fault::DropResponse:
            return;
        case Fault::CorruptResponseCrc:
            bytes.back() ^= 0xFF;
            break;
        case Fault::NoiseBeforeResponse:
            toHost_.insert(toHost_.end(), {'E', 'S', 'P', '-', 'R', 'O', 'M', kSof0, 0x00, kSof0});
            break;
        case Fault::None:
            break;
        }
    }
    toHost_.insert(toHost_.end(), bytes.begin(), bytes.end());
}

void MockSerialPort::sendError(ErrorCode code, std::uint8_t reqType, std::uint8_t seq) {
    send(static_cast<std::uint8_t>(MsgType::Error), seq, {static_cast<std::uint8_t>(code), reqType});
}

void MockSerialPort::sendEvent(Reason reason) {
    std::vector<std::uint8_t> p = encodeSysState(state_);
    p.push_back(static_cast<std::uint8_t>(reason));
    send(static_cast<std::uint8_t>(MsgType::EvtStateChanged), ++eventSeq_, p);
}

}  // namespace hg
