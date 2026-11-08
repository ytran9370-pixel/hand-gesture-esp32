// serial_port.hpp - abstraction of a byte-oriented serial link (requirement FR-PC-04).
//
// GestureClient only talks to this interface. The concrete implementation is chosen at run
// time: PosixSerialPort for a real board on Linux, MockSerialPort for tests without hardware,
// and a Windows implementation later. This is the Strategy pattern combined with dependency
// injection: the client receives the port from outside instead of creating it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace hg {

class ISerialPort {
public:
    virtual ~ISerialPort() = default;

    // Write all bytes. Throws SerialError on failure.
    virtual void write(const std::uint8_t* data, std::size_t len) = 0;

    // Read up to maxLen bytes, waiting at most timeoutMs. Returns the number of bytes read
    // (0 on timeout). Throws SerialError on failure.
    virtual std::size_t read(std::uint8_t* buffer, std::size_t maxLen, int timeoutMs) = 0;

    // Human-readable description, e.g. "/dev/ttyUSB0 @ 921600" or "mock".
    virtual std::string description() const = 0;
};

// Factory: "mock" -> MockSerialPort, anything else -> the OS serial port with that name.
std::unique_ptr<ISerialPort> openSerialPort(const std::string& name, int baud = 921600);

}  // namespace hg
