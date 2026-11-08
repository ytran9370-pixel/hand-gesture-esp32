// protocol.cpp - CRC-16 and frame encoding (HG-ICD-001 section 3).
#include "hg/protocol.hpp"

#include "hg/errors.hpp"

namespace hg {

std::uint16_t crc16(const std::uint8_t* data, std::size_t len, std::uint16_t crc) {
    for (std::size_t i = 0; i < len; ++i) {
        unsigned c = crc ^ (static_cast<unsigned>(data[i]) << 8U);
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 0x8000U) ? ((c << 1U) ^ 0x1021U) : (c << 1U);
        }
        crc = static_cast<std::uint16_t>(c & 0xFFFFU);
    }
    return crc;
}

std::vector<std::uint8_t> encodeFrame(std::uint8_t type, std::uint8_t seq,
                                      const std::vector<std::uint8_t>& payload) {
    if (payload.size() > kMaxPayload) {
        throw ProtocolError("payload too large: " + std::to_string(payload.size()) + " bytes");
    }
    const auto len = static_cast<std::uint16_t>(payload.size());

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + payload.size() + kCrcSize);
    out.push_back(kSof0);
    out.push_back(kSof1);
    out.push_back(type);
    out.push_back(seq);
    out.push_back(static_cast<std::uint8_t>(len & 0xFF));  // little-endian
    out.push_back(static_cast<std::uint8_t>(len >> 8));
    out.insert(out.end(), payload.begin(), payload.end());

    // CRC covers TYPE, SEQ, LEN and PAYLOAD, i.e. everything after the two SOF bytes.
    const std::uint16_t crc = crc16(out.data() + 2, out.size() - 2);
    out.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    out.push_back(static_cast<std::uint8_t>(crc >> 8));
    return out;
}

}  // namespace hg
