// protocol.hpp - constants, enums and frame encoding of the UART protocol.
// Specification: docs/protocol.md (HG-ICD-001, protocol version 2).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hg {

constexpr std::uint8_t kSof0 = 0xA5;
constexpr std::uint8_t kSof1 = 0x5A;
constexpr std::size_t kHeaderSize = 6;            // SOF0 SOF1 TYPE SEQ LEN_LO LEN_HI
constexpr std::size_t kCrcSize = 2;
constexpr std::size_t kMaxPayload = 12352;        // one 64x64x3 image + 64 bytes margin
constexpr std::size_t kImageBytes = 64 * 64 * 3;  // INFER payload
constexpr std::uint8_t kProtocolVersion = 2;
constexpr std::uint8_t kResponseFlag = 0x80;

// TYPE byte (HG-ICD-001 section 5)
enum class MsgType : std::uint8_t {
    Ping = 0x01,
    Infer = 0x02,
    SelfTest = 0x03,
    Stats = 0x04,
    Profile = 0x05,
    GetState = 0x06,
    SetOutput = 0x07,
    EvtStateChanged = 0x41,
    Error = 0x7F,
};

enum class ErrorCode : std::uint8_t {
    Crc = 1,
    Len = 2,
    UnknownCmd = 3,
    Busy = 4,
    Timeout = 5,
    Internal = 6,
    Locked = 7,
};

enum class LockState : std::uint8_t { Locked = 0, Unlocked = 1, Lockout = 2 };

enum class Reason : std::uint8_t {
    RfidOk = 1,
    RfidDenied = 2,
    Gesture = 3,
    AutoLockTimeout = 4,
    HostCommand = 5,
    LockoutEnd = 6,
};

enum class OutputTarget : std::uint8_t { Led1 = 1, Led2 = 2, Led3 = 3, Door = 4 };

constexpr std::uint8_t responseType(MsgType request) {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(request) | kResponseFlag);
}
constexpr bool isEventType(std::uint8_t type) { return type >= 0x40 && type <= 0x4F; }

// One decoded protocol frame (without SOF and CRC).
struct Frame {
    std::uint8_t type = 0;
    std::uint8_t seq = 0;
    std::vector<std::uint8_t> payload;
};

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR. Check value 0x29B1.
std::uint16_t crc16(const std::uint8_t* data, std::size_t len, std::uint16_t crc = 0xFFFF);

// Build the complete byte sequence of a frame: SOF, header, payload, CRC (little-endian).
std::vector<std::uint8_t> encodeFrame(std::uint8_t type, std::uint8_t seq,
                                      const std::vector<std::uint8_t>& payload = {});

}  // namespace hg
