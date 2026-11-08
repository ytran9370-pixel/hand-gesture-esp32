// gesture_client.hpp - high-level API to the ESP32-S3 (Facade over port, codec and parser).
//
//   auto port = hg::openSerialPort("/dev/ttyUSB0");
//   hg::GestureClient client(*port);
//   hg::PingInfo info = client.ping();
//
// Request/response rules follow HG-ICD-001 section 4 and FR-COM-06: each request gets a new
// sequence number, the client waits up to `timeoutMs` for the matching response and retries
// at most `retries` times on timeout, CRC or BUSY errors. Unsolicited events received at any
// time are forwarded to registered listeners (Observer pattern).
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#include "hg/frame_parser.hpp"
#include "hg/messages.hpp"
#include "hg/serial_port.hpp"

namespace hg {

// Observer interface for device events.
class IClientListener {
public:
    virtual ~IClientListener() = default;
    virtual void onStateChanged(const StateEvent& event) = 0;
};

struct ClientConfig {
    int timeoutMs = 1000;  // FR-COM-06
    int retries = 2;       // FR-COM-06
};

struct ClientStats {
    int requests = 0;
    int retries = 0;
    int timeouts = 0;
    int crcErrors = 0;
    int events = 0;
};

class GestureClient {
public:
    explicit GestureClient(ISerialPort& port, ClientConfig config = {});

    PingInfo ping();
    SysState getState();
    SysState setOutput(OutputTarget target, bool on);
    InferResult infer(const std::vector<std::uint8_t>& rgb64x64);

    // Process incoming bytes for up to timeoutMs without sending anything (to receive events).
    void poll(int timeoutMs);

    // Listeners are not owned; they must outlive the client or be removed first.
    void addListener(IClientListener* listener);
    void removeListener(IClientListener* listener);

    const ClientStats& stats() const { return stats_; }

private:
    Frame request(MsgType type, const std::vector<std::uint8_t>& payload);
    // Feed buffered/received bytes into the parser until it reports something or the deadline passes.
    // Bytes that arrive after a complete frame stay in rxBuf_ for the next call, so nothing is lost.
    FrameParser::Event nextParserEvent(std::chrono::steady_clock::time_point deadline);
    std::uint8_t nextSeq();
    void dispatchEvent(const Frame& frame);

    ISerialPort& port_;
    ClientConfig config_;
    FrameParser parser_;
    std::uint8_t seq_ = 0;
    std::vector<IClientListener*> listeners_;
    ClientStats stats_;
    std::array<std::uint8_t, 512> rxBuf_{};
    std::size_t rxPos_ = 0;
    std::size_t rxLen_ = 0;
};

}  // namespace hg
