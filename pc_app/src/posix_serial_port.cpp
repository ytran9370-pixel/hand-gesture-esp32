// posix_serial_port.cpp - raw 8N1 serial port on Linux / macOS using termios.
#include "hg/posix_serial_port.hpp"

#include <cerrno>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "hg/errors.hpp"

namespace hg {

namespace {

speed_t toSpeed(int baud) {
    switch (baud) {
    case 9600: return B9600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: throw SerialError("unsupported baud rate: " + std::to_string(baud));
    }
}

[[noreturn]] void fail(const std::string& what) {
    throw SerialError(what + ": " + std::strerror(errno));
}

}  // namespace

PosixSerialPort::PosixSerialPort(const std::string& path, int baud) : path_(path), baud_(baud) {
    fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd_ < 0) {
        fail("cannot open " + path);
    }

    termios tio{};
    if (::tcgetattr(fd_, &tio) != 0) {
        ::close(fd_);
        fail("tcgetattr " + path);
    }
    ::cfmakeraw(&tio);                  // binary data: no echo, no line editing, no CR/LF changes
    tio.c_cflag |= (CLOCAL | CREAD);    // ignore modem lines, enable receiver
    tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | PARENB | CRTSCTS);  // 1 stop bit, no parity, no flow ctrl
    tio.c_cc[VMIN] = 0;                 // read() returns whatever is available (we wait with poll())
    tio.c_cc[VTIME] = 0;
    const speed_t speed = toSpeed(baud);
    ::cfsetispeed(&tio, speed);
    ::cfsetospeed(&tio, speed);
    if (::tcsetattr(fd_, TCSANOW, &tio) != 0) {
        ::close(fd_);
        fail("tcsetattr " + path);
    }
    ::tcflush(fd_, TCIOFLUSH);          // drop bytes received before we opened (boot ROM text)
}

PosixSerialPort::~PosixSerialPort() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

PosixSerialPort::PosixSerialPort(PosixSerialPort&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), path_(std::move(other.path_)), baud_(other.baud_) {}

PosixSerialPort& PosixSerialPort::operator=(PosixSerialPort&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
        path_ = std::move(other.path_);
        baud_ = other.baud_;
    }
    return *this;
}

void PosixSerialPort::write(const std::uint8_t* data, std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = ::write(fd_, data + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("write " + path_);
        }
        done += static_cast<std::size_t>(n);
    }
    ::tcdrain(fd_);  // wait until the bytes have left the UART
}

std::size_t PosixSerialPort::read(std::uint8_t* buffer, std::size_t maxLen, int timeoutMs) {
    pollfd pfd{fd_, POLLIN, 0};
    const int ready = ::poll(&pfd, 1, timeoutMs);
    if (ready < 0) {
        if (errno == EINTR) {
            return 0;
        }
        fail("poll " + path_);
    }
    if (ready == 0) {
        return 0;  // timeout
    }
    const ssize_t n = ::read(fd_, buffer, maxLen);
    if (n < 0) {
        fail("read " + path_);
    }
    return static_cast<std::size_t>(n);
}

std::string PosixSerialPort::description() const {
    return path_ + " @ " + std::to_string(baud_);
}

}  // namespace hg
