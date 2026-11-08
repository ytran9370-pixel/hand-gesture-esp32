// serial_port_factory.cpp - Factory: hides which ISerialPort implementation is created.
#include <memory>

#include "hg/errors.hpp"
#include "hg/mock_serial_port.hpp"
#include "hg/serial_port.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include "hg/posix_serial_port.hpp"
#endif

namespace hg {

std::unique_ptr<ISerialPort> openSerialPort(const std::string& name, int baud) {
    if (name == "mock") {
        return std::make_unique<MockSerialPort>();
    }
#if defined(__unix__) || defined(__APPLE__)
    return std::make_unique<PosixSerialPort>(name, baud);
#else
    (void)baud;
    throw SerialError("serial ports are not implemented on this OS yet (use \"mock\")");
#endif
}

}  // namespace hg
