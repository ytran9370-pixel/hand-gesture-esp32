// messages.cpp - little-endian (de)serialisation of the payloads in HG-ICD-001 section 5.1.
#include "hg/messages.hpp"

#include "hg/errors.hpp"

namespace hg {

namespace {

// Reads fields sequentially from a payload and checks every access against its length.
class ByteReader {
public:
    explicit ByteReader(const std::vector<std::uint8_t>& data) : data_(data) {}

    std::uint8_t u8() {
        need(1);
        return data_[pos_++];
    }
    std::uint16_t u16() {
        need(2);
        const auto v = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }
    std::uint32_t u32() {
        need(4);
        std::uint32_t v = 0;
        for (int i = 3; i >= 0; --i) {
            v = (v << 8) | data_[pos_ + static_cast<std::size_t>(i)];
        }
        pos_ += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    // Fixed-size, zero-terminated text field.
    std::string text(std::size_t size) {
        need(size);
        std::string s;
        for (std::size_t i = 0; i < size && data_[pos_ + i] != 0; ++i) {
            s.push_back(static_cast<char>(data_[pos_ + i]));
        }
        pos_ += size;
        return s;
    }

private:
    void need(std::size_t n) const {
        if (pos_ + n > data_.size()) {
            throw ProtocolError("payload too short: need " + std::to_string(pos_ + n) + " bytes, got " +
                                std::to_string(data_.size()));
        }
    }

    const std::vector<std::uint8_t>& data_;
    std::size_t pos_ = 0;
};

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

SysState readSysState(ByteReader& r) {
    SysState s;
    s.lock = static_cast<LockState>(r.u8());
    s.outputs = r.u8();
    s.lastGesture = r.u8();
    s.deniedCount = r.u8();
    s.unlockRemainingS = r.u16();
    return s;
}

}  // namespace

PingInfo decodePing(const std::vector<std::uint8_t>& payload) {
    ByteReader r(payload);
    PingInfo p;
    p.protoVersion = r.u8();
    p.nClasses = r.u8();
    p.inW = r.u16();
    p.inH = r.u16();
    p.inC = r.u8();
    p.nLayers = r.u8();
    p.modelHash = r.u32();
    p.macs = r.u32();
    p.cpuMhz = r.u32();
    p.backend = r.text(8);
    p.modelName = r.text(32);
    p.fwVersion = r.text(16);
    return p;
}

SysState decodeSysState(const std::vector<std::uint8_t>& payload) {
    ByteReader r(payload);
    return readSysState(r);
}

InferResult decodeInfer(const std::vector<std::uint8_t>& payload) {
    ByteReader r(payload);
    InferResult res;
    res.classId = r.u8();
    const std::uint8_t n = r.u8();
    res.confPermille = r.u16();
    res.inferUs = r.u32();
    res.deviceUs = r.u32();
    res.action = r.u8();
    res.filterCount = r.u8();
    res.lock = static_cast<LockState>(r.u8());
    res.outputs = r.u8();
    res.logits.reserve(n);
    for (int i = 0; i < n; ++i) {
        res.logits.push_back(r.i32());
    }
    return res;
}

StateEvent decodeStateEvent(const std::vector<std::uint8_t>& payload) {
    ByteReader r(payload);
    StateEvent e;
    e.state = readSysState(r);
    e.reason = static_cast<Reason>(r.u8());
    return e;
}

DeviceErrorInfo decodeError(const std::vector<std::uint8_t>& payload) {
    ByteReader r(payload);
    DeviceErrorInfo e;
    e.code = static_cast<ErrorCode>(r.u8());
    e.requestType = r.u8();
    return e;
}

std::vector<std::uint8_t> encodeSysState(const SysState& s) {
    std::vector<std::uint8_t> out{static_cast<std::uint8_t>(s.lock), s.outputs, s.lastGesture, s.deniedCount};
    putU16(out, s.unlockRemainingS);
    return out;
}

std::vector<std::uint8_t> encodeSetOutput(OutputTarget target, bool on) {
    return {static_cast<std::uint8_t>(target), static_cast<std::uint8_t>(on ? 1 : 0)};
}

const char* toString(ErrorCode code) {
    switch (code) {
    case ErrorCode::Crc: return "CRC";
    case ErrorCode::Len: return "LEN";
    case ErrorCode::UnknownCmd: return "UNKNOWN_CMD";
    case ErrorCode::Busy: return "BUSY";
    case ErrorCode::Timeout: return "TIMEOUT";
    case ErrorCode::Internal: return "INTERNAL";
    case ErrorCode::Locked: return "LOCKED";
    }
    return "UNKNOWN";
}

const char* toString(LockState state) {
    switch (state) {
    case LockState::Locked: return "LOCKED";
    case LockState::Unlocked: return "UNLOCKED";
    case LockState::Lockout: return "LOCKOUT";
    }
    return "UNKNOWN";
}

}  // namespace hg
