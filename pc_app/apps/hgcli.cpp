// hgcli.cpp - command-line tool to talk to the ESP32-S3 (or to the mock device).
//
//   hgcli mock ping
//   hgcli /dev/ttyUSB0 state
//   hgcli /dev/ttyUSB0 set led2 on
//   hgcli /dev/ttyUSB0 infer image.rgb      (raw 64x64x3 RGB file, 12288 bytes)
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "hg/errors.hpp"
#include "hg/gesture_client.hpp"
#include "hg/mock_serial_port.hpp"

namespace {

void printState(const hg::SysState& s) {
    std::cout << "lock=" << hg::toString(s.lock) << "  LED1=" << s.led(1) << " LED2=" << s.led(2)
              << " LED3=" << s.led(3) << "  door=" << (s.doorOpen() ? "open" : "closed")
              << "  auto-lock in " << s.unlockRemainingS << " s\n";
}

// Observer: prints every event pushed by the device.
class PrintListener : public hg::IClientListener {
public:
    void onStateChanged(const hg::StateEvent& e) override {
        std::cout << "[event reason=" << static_cast<int>(e.reason) << "] ";
        printState(e.state);
    }
};

int usage() {
    std::cerr << "usage: hgcli <port|mock> ping | state | set <led1|led2|led3|door> <on|off> | infer <file.rgb>\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    const std::string portName = argv[1];
    const std::string cmd = argv[2];

    try {
        auto port = hg::openSerialPort(portName);
        hg::GestureClient client(*port);
        PrintListener listener;
        client.addListener(&listener);

        // With the mock, simulate a registered card first so SET_OUTPUT is allowed.
        if (auto* mock = dynamic_cast<hg::MockSerialPort*>(port.get()); mock != nullptr && cmd == "set") {
            mock->tapCard(true);
        }

        if (cmd == "ping") {
            const hg::PingInfo p = client.ping();
            std::cout << "protocol v" << int(p.protoVersion) << ", model " << p.modelName << ", " << int(p.nClasses)
                      << " classes, input " << p.inW << "x" << p.inH << "x" << int(p.inC) << ", backend "
                      << p.backend << ", firmware " << p.fwVersion << "\n";
        } else if (cmd == "state") {
            printState(client.getState());
        } else if (cmd == "set" && argc == 5) {
            const std::string t = argv[3];
            const hg::OutputTarget target = t == "led1"   ? hg::OutputTarget::Led1
                                            : t == "led2" ? hg::OutputTarget::Led2
                                            : t == "led3" ? hg::OutputTarget::Led3
                                                          : hg::OutputTarget::Door;
            printState(client.setOutput(target, std::string(argv[4]) == "on"));
        } else if (cmd == "infer" && argc == 4) {
            std::ifstream f(argv[3], std::ios::binary);
            std::vector<std::uint8_t> rgb((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            const hg::InferResult r = client.infer(rgb);
            std::cout << "class " << int(r.classId) << ", confidence " << r.confPermille / 10.0 << " %, "
                      << r.inferUs / 1000.0 << " ms on device\n";
        } else {
            return usage();
        }
        client.poll(50);  // print events that arrived after the response
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
