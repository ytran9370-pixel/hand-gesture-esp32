# Hand Gesture ESP32

![CI](https://github.com/ytran9370-pixel/hand-gesture-esp32/actions/workflows/build.yml/badge.svg)

A gesture-controlled embedded system that runs a model convolutional neural network (CNN) INT8 on an ESP32-S3 microcontroller.
The system lets a user switch room lights and open or close a door with hand gestures, without touching any physical switch.

## How it works
A PC webcam captures the user's hand, and the cropped image is streamed over UART to an ESP32-S3.
On the microcontroller, a lightweight convolutional neural network (CNN) classifies the gesture, a control state machine decides which action to take. 
The firmware drives the outputs: three LEDs representing room lights, a servo representing a door, an I2C OLED status display and an SPI RFID reader that must unlock the system before any gesture is accepted.

![System architecture](docs/uml/architecture.svg)

1. The PC webcam captures a webcam frame, finds the hand and resizes it to 64×64 RGB.
2. The image is sent to the ESP32-S3 in a binary frame with a CRC-16 over UART at 921 600 baud 
3. On the microcontroller ESP32-S3, a lightweight convolutional neural network (CNN) classifies the gesture, a control state machine decides which action to take. 
4. The firmware drives the outputs: three LEDs representing room lights, a servo representing a door, an I2C OLED status display and an SPI RFID reader that must unlock the system before any gesture is accepted.

| Gesture | Action |
|---|---|
| palm | All lights on |
| fist | All lights off |
| one / rock / mute | Toggle light 1 / 2 / 3 |
| like | Open door |
| dislike | Close door |
| stop | Lock the system |

## Model

Input: 64 x 64 RGB hand image. Output: 18 gesture classes (HaGRID dataset).

| Block | Layers | Output size |
|---|---|---|
| Stem | Conv 3x3, stride 2, 16 filters | 32 x 32 x 16 |
| Stage 1 | Conv 3x3 (32), Conv 1x1 (32), MaxPool 2x2 | 16 x 16 x 32 |
| Stage 2 | Conv 3x3 (64), Conv 1x1 (64), MaxPool 2x2 | 8 x 8 x 64 |
| Stage 3 | Conv 3x3 (96), Conv 1x1 (96), MaxPool 2x2 | 4 x 4 x 96 |
| Head | Global average pooling, fully connected | 18 |

| | FP32 | INT8 (deployed) |
|---|---|---|
| Test accuracy (5,032 images, split by user) | 94.6% | 94.2% |
| Weight size | 370 KB | 92.6 KB |
| Peak activation memory | 256 KB | 64 KB |

## Hardware

| Part | Interface | ESP32-S3 pins |
|---|---|---|
| ESP32-S3-N16R8 dev board (TDMaker TDM2533) | — | — |
| PC link (on-board USB-UART bridge) | UART0, 921 600 baud | TX 43, RX 44 |
| SSD1306 0.96" OLED | I2C, 400 kHz | SDA 8, SCL 9 |
| RC522 RFID reader | SPI | SCK 12, MOSI 11, MISO 13, CS 10, RST 14 |
| 3 LEDs (220 Ω) | GPIO | 4, 5, 6 |
| SG90 servo | PWM 50 Hz (LEDC) | 7 |

## Repository layout

```
.github/workflows/  CI: host test + ESP-IDF firmware build on every push
docs/               requirements, UART protocol, UML diagrams (PlantUML sources + SVG)
firmware/           ESP-IDF project for the ESP32-S3 (C)
host/               Linux build of the inference engine + bit-exact test
model/              INT8 export script (PyTorch checkpoint -> C arrays)
pc_app/             C++ host application (Phase 3)
tests/              end-to-end tests (Phase 5)
```

## Documentation

- [Requirements specification](docs/requirements.md)
- [UART protocol specification](docs/protocol.md)
- [UML diagrams](docs/uml/)

## Author

Tran Yen Quynh — Electronics and Communications Engineering Technology, HCMUTE
