"""Unit tests for hg_link.py (no hardware needed):  python -m unittest discover tools"""
import struct
import unittest

from hg_link import FrameParser, crc16, encode_frame


class ProtocolTest(unittest.TestCase):
    def test_crc_check_value(self):
        self.assertEqual(crc16(b"123456789"), 0x29B1)

    def test_example_frames(self):
        # HG-ICD-001 section 6 - the same bytes are checked in host/test_logic.c (firmware side)
        self.assertEqual(encode_frame(0x01, 1).hex(" "), "a5 5a 01 01 00 00 44 c5")
        self.assertEqual(encode_frame(0x06, 2).hex(" "), "a5 5a 06 02 00 00 39 cd")
        self.assertEqual(encode_frame(0x07, 3, bytes([2, 1])).hex(" "), "a5 5a 07 03 02 00 02 01 a8 b3")
        self.assertEqual(encode_frame(0x7F, 7, bytes([1, 1])).hex(" "), "a5 5a 7f 07 02 00 01 01 23 27")

    def test_parser_noise_split_and_bad_crc(self):
        good = encode_frame(0x86, 5, struct.pack("<BBBBH", 1, 0x0B, 4, 0, 27))
        bad = bytearray(encode_frame(0x81, 6))
        bad[3] ^= 1
        stream = b"\x00\xA5\x13" + bytes(bad) + good
        p = FrameParser()
        frames = []
        for i in range(0, len(stream), 3):          # arrives in small pieces, like a real UART
            frames += p.feed(stream[i:i + 3])
        self.assertEqual(len(frames), 1)
        self.assertEqual((frames[0].type, frames[0].seq), (0x86, 5))
        self.assertEqual(p.crc_errors, 1)


if __name__ == "__main__":
    unittest.main()
