// errors.hpp - exception types of the host library.
#pragma once

#include <stdexcept>
#include <string>

#include "hg/protocol.hpp"

namespace hg {

// Malformed data (wrong payload length, unexpected response type...).
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// No valid response after all retries.
class TimeoutError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The device answered with an ERROR frame that cannot be fixed by retrying.
class DeviceError : public std::runtime_error {
public:
    DeviceError(ErrorCode code, const std::string& what) : std::runtime_error(what), code_(code) {}
    ErrorCode code() const { return code_; }

private:
    ErrorCode code_;
};

// Opening or using the serial port failed.
class SerialError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace hg
