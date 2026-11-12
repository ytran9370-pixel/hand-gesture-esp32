# HAND GESTURE

A gesture-controlled embedded system that runs an INT8 convolutional neural network (CNN) on an ESP32-S3 microcontroller. The system lets a user switch room lights and open or close a door with hand gestures, without touching any physical switch.

## How it works

1. The PC application captures a webcam frame, finds the hand and resizes it to 64×64 RGB.
2. The image is sent to the ESP32-S3 in a binary frame protected by CRC-16 over UART at 921,600 baud.
3. On the ESP32-S3, the CNN classifies the gesture and a control state machine decides which action to take.
4. The firmware drives the outputs: three LEDs representing room lights, a servo representing a door and an I2C OLED status display.
5. An SPI RFID reader acts as access control: the system must be unlocked with a registered card before any gesture is accepted.

| Gesture | Action |
| --- | --- |
| palm | All lights on |
| fist | All lights off |
| one / rock / mute | Toggle light 1 / 2 / 3 |
| like | Open door |
| dislike | Close door |
| stop | Lock the system |

## Model

Input: 64×64 RGB hand image. 
Output: 18 gesture classes (HaGRID dataset).

| Block | Layers | Output size |
| --- | --- | --- |
| Stem | Conv 3x3, stride 2, 16 filters | 32 x 32 x 16 |
| Stage 1 | Conv 3x3 (32), Conv 1x1 (32), MaxPool 2x2 | 16 x 16 x 32 |
| Stage 2 | Conv 3x3 (64), Conv 1x1 (64), MaxPool 2x2 | 8 x 8 x 64 |
| Stage 3 | Conv 3x3 (96), Conv 1x1 (96), MaxPool 2x2 | 4 x 4 x 96 |
| Head | Global average pooling, fully connected | 18 |

| | FP32 | INT8 (deployed) |
| --- | --- | --- |
| Test accuracy | 94.6% | 94.2% |
| Weight size | 370 KB | 92.6 KB |

## Hardware

| Part | Interface | ESP32-S3 pins |
| --- | --- | --- |
| ESP32-S3-N16R8 dev board (TDM2533) | | |
| PC link (on-board USB-UART) | UART0, 921,600 baud | TX 43, RX 44 |
| SSD1306 0.96" OLED | I2C, 400 kHz | SDA 8, SCL 9 |
| RC522 RFID reader | SPI | SCK 12, MOSI 11, MISO 13, CS 10, RST 14 |
| 3 LEDs (220 ohm) | GPIO | 4, 5, 6 |
| SG90 servo | PWM 50 Hz (LEDC) | 7 |

## Results

Measured on the ESP32-S3 board (240 MHz, plain C engine).

| Metric | Result |
| --- | --- |
| Inference time | 580 ms per image |
| Match with the Python reference model | 10/10 test images bit-exact |
| Accuracy on real hand photos | 26/30 (86.7%) |
| RAM used by the firmware | about 49% |
| Unit tests | 25 GoogleTest (C++) + 85 checks (firmware logic in C) |

The accuracy on real photos is lower than on the test set because colour, lighting, background.

## Firmware architecture

The firmware runs on FreeRTOS.

| Task | Core | Priority | Job |
| --- | --- | --- | --- |
| link_rx | 0 | 12 | Reads UART bytes and rebuilds frames |
| infer | 1 | 10 | Runs the CNN, filters predictions, answers the PC |
| control | 0 | 6 | Polls the RFID reader and the auto-lock timer every 100 ms |
| display | 0 | 2 | Redraws the OLED every 200 ms |

- Two static 12 KB buffers let the next image arrive while the current one is processed.
- A gesture is accepted only after 3 matching predictions with at least 80% confidence.

## Demo

[Demo video](link)

## Author

Tran Yen Quynh, Electronics and Communications Engineering Technology, HCMUTE
