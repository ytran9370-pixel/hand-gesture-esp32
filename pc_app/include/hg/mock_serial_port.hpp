// mock_serial_port.hpp - a simulated ESP32-S3 that speaks HG-ICD-001.
//
// Lets the whole host side be developed and tested without hardware. Bytes written by the
// host are parsed with the same FrameParser; the mock answers like the firmware would and
// can inject faults (corrupted CRC, lost response, line noise) to test error handling.
#pragma once

#include <cstdint>
#include <deque>
#include <string>

#include "hg/frame_parser.hpp"
#include "hg/messages.hpp"
#include "hg/serial_port.hpp"

namespace hg {

class MockSerialPort final : public ISerialPort {
public:
    enum class Fault { None, CorruptResponseCrc, DropResponse, NoiseBeforeResponse };

    MockSerialPort();

    void write(const std::uint8_t* data, std::size_t len) override;
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen, int timeoutMs) override;
    std::string description() const override { return "mock ESP32-S3"; }

    // --- test controls -------------------------------------------------------------------
    // Apply a fault to the next `count` responses.
    void injectFault(Fault fault, int count = 1);
    // Simulate an RFID card (FR-SEC-03 / FR-SEC-04); sends EVT_STATE_CHANGED.
    void tapCard(bool registered);
    const SysState& state() const { return state_; }
    int framesReceived() const { return framesReceived_; }
    int crcErrorsReceived() const { return crcErrors_; }

private:
    void handle(const Frame& request);
    void send(std::uint8_t type, std::uint8_t seq, const std::vector<std::uint8_t>& payload);
    void sendError(ErrorCode code, std::uint8_t reqType, std::uint8_t seq);
    void sendEvent(Reason reason);

    FrameParser parser_;
    std::deque<std::uint8_t> toHost_;
    SysState state_;
    Fault fault_ = Fault::None;
    int faultCount_ = 0;
    std::uint8_t eventSeq_ = 0;
    int framesReceived_ = 0;
    int crcErrors_ = 0;
};

}  // namespace hg
