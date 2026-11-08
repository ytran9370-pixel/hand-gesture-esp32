// messages.hpp - payload structures of HG-ICD-001 and their (de)serialisation.
//
// Payloads are decoded field by field from little-endian bytes instead of casting the buffer
// to a packed struct. This is portable (no alignment or endianness assumptions) and every
// length is checked, so a short or corrupted payload raises ProtocolError instead of reading
// past the end of the buffer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hg/protocol.hpp"

namespace hg {

struct PingInfo {                // rsp_ping_t, 76 bytes
    std::uint8_t protoVersion = 0;
    std::uint8_t nClasses = 0;
    std::uint16_t inW = 0, inH = 0;
    std::uint8_t inC = 0;
    std::uint8_t nLayers = 0;
    std::uint32_t modelHash = 0;
    std::uint32_t macs = 0;
    std::uint32_t cpuMhz = 0;
    std::string backend;
    std::string modelName;
    std::string fwVersion;
};

struct SysState {                // sys_state_t, 6 bytes
    LockState lock = LockState::Locked;
    std::uint8_t outputs = 0;    // bit 0..2 = LED1..LED3, bit 3 = door open
    std::uint8_t lastGesture = 0xFF;
    std::uint8_t deniedCount = 0;
    std::uint16_t unlockRemainingS = 0;

    bool led(int index) const { return (outputs >> (index - 1)) & 1U; }  // index 1..3
    bool doorOpen() const { return (outputs >> 3) & 1U; }
};

struct InferResult {             // rsp_infer_t, 16 + 4 * n bytes
    std::uint8_t classId = 0;
    std::uint16_t confPermille = 0;
    std::uint32_t inferUs = 0;
    std::uint32_t deviceUs = 0;
    std::uint8_t action = 0;
    std::uint8_t filterCount = 0;
    LockState lock = LockState::Locked;
    std::uint8_t outputs = 0;
    std::vector<std::int32_t> logits;
};

struct StateEvent {              // evt_state_changed_t, 7 bytes
    SysState state;
    Reason reason = Reason::HostCommand;
};

struct DeviceErrorInfo {         // rsp_error_t, 2 bytes
    ErrorCode code = ErrorCode::Internal;
    std::uint8_t requestType = 0;
};

PingInfo decodePing(const std::vector<std::uint8_t>& payload);
SysState decodeSysState(const std::vector<std::uint8_t>& payload);
InferResult decodeInfer(const std::vector<std::uint8_t>& payload);
StateEvent decodeStateEvent(const std::vector<std::uint8_t>& payload);
DeviceErrorInfo decodeError(const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encodeSysState(const SysState& s);
std::vector<std::uint8_t> encodeSetOutput(OutputTarget target, bool on);

const char* toString(ErrorCode code);
const char* toString(LockState state);

}  // namespace hg
