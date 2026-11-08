// posix_serial_port.hpp - ISerialPort for Linux / macOS (termios).
#pragma once

#include <string>

#include "hg/serial_port.hpp"

namespace hg {

// RAII: the port is opened in the constructor and closed in the destructor, so it can never
// leak, even when an exception is thrown. Copying is forbidden (two objects must not close
// the same file descriptor); moving is allowed.
class PosixSerialPort final : public ISerialPort {
public:
    PosixSerialPort(const std::string& path, int baud);
    ~PosixSerialPort() override;

    PosixSerialPort(const PosixSerialPort&) = delete;
    PosixSerialPort& operator=(const PosixSerialPort&) = delete;
    PosixSerialPort(PosixSerialPort&& other) noexcept;
    PosixSerialPort& operator=(PosixSerialPort&& other) noexcept;

    void write(const std::uint8_t* data, std::size_t len) override;
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen, int timeoutMs) override;
    std::string description() const override;

private:
    int fd_ = -1;
    std::string path_;
    int baud_ = 0;
};

}  // namespace hg
