# Đưa model Hand Gesture INT8 (lượng tử đối xứng) lên ESP32-S3: hướng dẫn chi tiết

Mục tiêu: lấy model W8A8 đã lượng tử trong notebook `nckh-conv3x3`, chuyển thành code C, nạp lên
ESP32-S3-N16R8 và chứng minh chip chạy **đúng y hệt** model trên máy tính.

Kết quả bạn sẽ thấy trên Serial Monitor:

```
===== Hand gesture INT8 tren ESP32-S3 =====
#    nhan that        du doan            conf       ms logits==golden?
0    palm             palm              97.3%    xx.xx OK
1    like             like              88.1%    xx.xx OK
...
=> Trung tung bit voi golden model: 10/10   |  Dung nhan: 10/10
Toc do trung binh (50 lan): xx.xx ms/anh
   stem / stage1_conv / ... (thoi gian tung lop)
```

---

## 1. Hiểu nguyên lý trước (5 phút, rất hay bị hỏi khi phỏng vấn)

### 1.1 Lượng tử đối xứng là gì

Mỗi tensor số thực được biểu diễn bằng số nguyên 8 bit và một hệ số `scale`:

```
giá_trị_thực = scale × q        q ∈ [-128, 127],  zero_point = 0 (vì "đối xứng")
```

* Trọng số: mỗi kênh ra có một `scale` riêng (per-channel): `s_w = max|w| / 127`.
* Activation: mỗi tensor có một `scale`: `s = running_max / 127`. Giá trị này lấy từ bước calibration trong notebook.
* BatchNorm đã được gộp vào conv (`fold_bn`), nên mỗi lớp chỉ còn lại conv + bias + ReLU.

### 1.2 Một lớp conv chạy trên chip như thế nào

```
acc  = bias_q + Σ x_q · w_q                        (cộng dồn int32, không có số thực)
y_q  = clamp( round(acc × M), 0, 127 )             (ReLU = kẹp ở 0)
M    = s_in · s_w / s_out
```

`M` là số thực nhỏ (khoảng 0.001–0.1). Để không cần FPU, ta viết `M = M0 × 2^(shift−31)`, trong đó
`M0` là số nguyên 31 bit. Phép nhân với `M` khi đó chỉ còn là một phép nhân 64 bit, làm tròn rồi dịch bit
(hàm `requant()` trong `model.c`). Đây đúng là cách TFLite Micro và ESP-NN làm.

`bias_q = round(b / (s_in · s_w))` cũng được tính sẵn khi export.

### 1.3 Đường đi của dữ liệu

```
ảnh RGB 64×64 (uint8)
   │  bảng tra 256 phần tử:  q = round(((p/255 − 0.5)/0.5) / s_in)
   ▼
int8 64×64×3 ─ stem 3×3/s2 ─ 32×32×16
           ─ stage1 3×3 ─ 1×1 ─ maxpool ─ 16×16×32
           ─ stage2 3×3 ─ 1×1 ─ maxpool ─  8×8×64
           ─ stage3 3×3 ─ 1×1 ─ maxpool ─  4×4×96
           ─ GAP ─ 96 ─ FC ─ 18 logits int32 ─ argmax = cử chỉ
```

Bộ nhớ dùng: 2 buffer 32 KB dùng luân phiên (tensor lớn nhất là 32×32×32 = 32 KB). Trọng số 92 KB nằm luôn
trong flash (`const`), nên không tốn RAM.

### 1.4 "Golden model" là gì và để làm gì

Script export chạy lại toàn bộ phép tính số nguyên ở trên bằng NumPy (hàm `run_int`) rồi ghi logits ra file.
Firmware chạy cùng phép tính bằng C. Nếu hai bên **trùng từng bit** thì code C chắc chắn đúng.
Đây chính là cách kiểm thử firmware AI trong công nghiệp.

---

## 2. Chuẩn bị

| Thứ cần có | Ghi chú |
|---|---|
| Board ESP32-S3-N16R8 + cáp USB-C **có dây data** | Cáp chỉ để sạc sẽ không nhận cổng |
| Máy tính Windows / Linux / macOS | |
| Notebook Kaggle đã chạy xong | đã có `float_model`, `make_qnet`, `TEST_X`, `TEST_Y` trong bộ nhớ |

Cấu trúc project:

```
esp32-int8/
├── tools/export_int8.py        ← dán vào Kaggle
├── firmware/                   ← project ESP-IDF
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults      (240 MHz, flash 16 MB, -O2)
│   └── main/
│       ├── main.c              kiểm tra + benchmark
│       ├── model.h / model.c   engine int8 (~150 dòng C)
│       ├── model_data.c        ← SINH TỪ KAGGLE (trọng số)
│       └── test_images.c       ← SINH TỪ KAGGLE (10 ảnh test + logits mong đợi)
└── host/                       chạy thử engine trên PC (tuỳ chọn)
```

> Trong zip đang có sẵn `model_data.c` / `test_images.c` của một **model giả** (nhãn `c0…c17`, trọng số ngẫu nhiên)
> chỉ để bạn build thử được ngay. Bạn phải thay bằng file thật ở bước 3.

---

## 3. Bước 1: Export model trên Kaggle

1. Mở notebook, bấm **Run All** (hoặc chạy tới hết CELL 8 là đủ cho PTQ, tới CELL 12 nếu muốn dùng QAT).
   Checkpoint đã lưu sẽ được nạp lại nên không phải train lại.
2. Tạo cell mới ở cuối, dán **toàn bộ nội dung** `tools/export_int8.py`.
3. Tạo thêm một cell nữa và chạy:

   ```python
   net = make_qnet(uniform_bits(8), "pct")       # PTQ W8A8, test acc 94.61% (khuyên dùng)
   # net = QAT_NETS["W8A8"]                      # hoặc QAT W8A8, 94.24%
   export_int8(net, TEST_X, TEST_Y, HAGRID_CLASSES, out_dir="/kaggle/working/esp32_int8")
   ```

   Nên dùng PTQ: trong notebook, PTQ W8A8 (94.61%) còn cao hơn QAT W8A8 (94.24%) và không phải train thêm.

4. Kiểm tra output:

   ```
   PyTorch fake-quant acc : 0.946x
   Integer (C/NumPy) acc  : 0.94xx      ← phải gần dòng trên (chênh < 0.5%)
   Trung du doan          : 0.99xx      ← phải > 0.99
   ```

   Hai con số khớp nhau nghĩa là model số nguyên mà chip sẽ chạy cho kết quả như PyTorch.
   Lệch nhỏ là do làm tròn bias và GAP (tối đa 1 LSB).

5. Bên phải màn hình, mở **Output → /kaggle/working/** và tải `esp32_int8.zip` về.

---

## 4. Bước 2: Chép file vào firmware

Giải nén `esp32_int8.zip` rồi chép **ghi đè** 2 file vào `firmware/main/`:

```
model_data.c   → firmware/main/model_data.c
test_images.c  → firmware/main/test_images.c
```

Mở thử `model_data.c`: bạn sẽ thấy các mảng `stem_w`, `stem_b`, `stem_mult`, `stem_shift`... và dòng
`g_labels` có tên thật (`"call", "dislike", ...`).

---

## 5. Bước 3 (tuỳ chọn nhưng nên làm): Chạy thử trên PC

Bước này kiểm tra code C ngay trên máy tính, trước khi có chip. Cần `gcc`
(Linux/macOS có sẵn; Windows dùng WSL hoặc MSYS2):

```bash
make -C host
```

Kết quả mong đợi: `10/10 anh trung tung bit voi golden model -> PASS`.
Bản build này còn bật AddressSanitizer, nên lỗi tràn mảng hay đọc sai bộ nhớ sẽ bị báo ngay.

---

## 6. Bước 4: Cài ESP-IDF (một lần duy nhất)

Nên dùng **ESP-IDF v5.3** trở lên.

**Windows (dễ nhất):**
1. Tải *ESP-IDF Tools Installer* tại https://dl.espressif.com/dl/esp-idf/ và chọn bản v5.3.x.
2. Cài xong sẽ có biểu tượng **"ESP-IDF 5.3 CMD"** (hoặc PowerShell). Mọi lệnh `idf.py` bên dưới gõ trong cửa sổ đó.

**Linux / macOS:**
```bash
sudo apt install git wget flex bison gperf python3 python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
git clone -b v5.3.1 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf
~/esp-idf/install.sh esp32s3
. ~/esp-idf/export.sh          # chạy lại lệnh này mỗi khi mở terminal mới
```

**VS Code:** cài extension "ESP-IDF" của Espressif và làm theo trình hướng dẫn (chọn v5.3).

Kiểm tra: `idf.py --version` in ra `ESP-IDF v5.3.x`.

---

## 7. Bước 5: Build và nạp

### 7.1 Tìm cổng của board

Board N16R8 thường có 2 cổng USB-C: **COM** (qua chip CH343) và **USB** (USB native của ESP32-S3).
Cắm cáp vào cổng **COM**.

* Windows: mở Device Manager → Ports, sẽ thấy `USB-SERIAL CH343 (COM5)`. Không thấy thì cài driver CH343 của WCH.
* Linux: `ls /dev/ttyUSB* /dev/ttyACM*` → thường là `/dev/ttyUSB0` (cổng USB native là `/dev/ttyACM0`).
  Nếu báo `Permission denied`: `sudo usermod -aG dialout $USER` rồi đăng xuất và đăng nhập lại.
* macOS: `/dev/cu.wchusbserial...`

### 7.2 Build, nạp, xem kết quả

```bash
cd firmware
idf.py set-target esp32s3        # chỉ cần chạy lần đầu
idf.py build
idf.py -p COM5 flash monitor     # Linux: -p /dev/ttyUSB0
```

* `build` lần đầu mất 1–3 phút.
* `flash` ghi bootloader, bảng phân vùng và firmware vào chip.
* `monitor` mở Serial Monitor. Thoát bằng `Ctrl + ]`.

Nếu nạp báo `Failed to connect to ESP32-S3`: giữ nút **BOOT**, bấm rồi nhả **RST**, nhả **BOOT**, sau đó chạy lại lệnh flash.

---

## 8. Bước 6: Đọc kết quả

```
=> Trung tung bit voi golden model: 10/10   |  Dung nhan: 9/10
```

* **Trùng từng bit 10/10**: phần quan trọng nhất. Chip tính ra đúng từng con số như NumPy trên Kaggle.
* **Đúng nhãn**: model có độ chính xác ~94.6%, nên với 10 ảnh thì đôi khi sai 1 ảnh là bình thường.
* **ms/ảnh**: tốc độ thật trên chip ở 240 MHz. Bảng thời gian từng lớp cho thấy `stage1_conv` và
  `stage2_conv` tốn nhiều nhất, vì 2 lớp này chiếm ~60% số phép nhân-cộng (MAC).

Chụp màn hình phần này để đưa vào README hoặc CV.

---

## 9. Giải thích code (để tự tin khi bị hỏi)

| File / hàm | Làm gì |
|---|---|
| `export_int8.py: build_params` | gộp BN, tính `scale`, lượng tử trọng số, tính `bias_q`, `M0/shift`, tạo bảng tra đầu vào |
| `export_int8.py: run_int` | golden model: chạy mạng chỉ bằng số nguyên trong NumPy |
| `model.c: requant` | nhân với `M` bằng số nguyên: `high_mul` (nhân 64 bit, làm tròn, lấy 32 bit cao) rồi `round_shift_right` |
| `model.c: conv2d` | vòng lặp conv NHWC. Ở viền ảnh thì cắt bớt cửa sổ kernel (padding = 0 vì zero_point = 0). Mỗi hàng kernel là một tích vô hướng liên tục |
| `model.c: maxpool2x2` | lấy max 4 ô. Với int8 thì max cho kết quả y như số thực, không cần requant |
| `model.c: model_run` | bảng tra đầu vào → 7 conv (2 buffer luân phiên) → GAP → FC |
| `main.c` | so `memcmp` logits với golden, in bảng, benchmark 50 lần |

Vì sao trọng số xếp theo OHWI và activation theo NHWC? Vì khi đó 3 giá trị `kx` liên tiếp nhân với tất cả kênh
`c` nằm liền nhau trong bộ nhớ. Tích vô hướng đọc tuần tự rất nhanh, và đây cũng là layout mà ESP-NN/TFLite dùng.

---

## 10. Lỗi thường gặp

| Hiện tượng | Nguyên nhân / cách sửa |
|---|---|
| `Trung tung bit` < 10/10 | `model_data.c` và `test_images.c` không cùng một lần export → chép lại cả 2 file |
| Build lỗi `undefined reference to g_conv_layers` | thiếu `model_data.c` trong `firmware/main/` |
| `Integer acc` thấp hơn PyTorch nhiều (> 1%) | chọn nhầm `net`: phải là QNet (`make_qnet(...)` hoặc `QAT_NETS[...]`), không phải `float_model` |
| Không thấy cổng COM | cáp chỉ sạc, hoặc thiếu driver CH343 |
| Monitor in ký tự rác | đúng cổng nhưng sai baud: `idf.py monitor` tự dùng 115200, đừng mở bằng tool khác ở baud khác |
| Board liên tục reset, log có `PSRAM` | không xảy ra với cấu hình này vì PSRAM không bật; nếu bạn tự bật thì N16R8 phải chọn *Octal* |

---

## 11. Làm tiếp khi đã chạy được

1. **Nhận ảnh thật qua UART**: PC crop bàn tay rồi gửi 12 288 byte RGB xuống, chip trả về nhãn.
   Bản project đầy đủ ở lần trước có sẵn phần này: giao thức có CRC, chạy trên FreeRTOS.
2. **Tăng tốc bằng ESP-NN**: thay `conv2d` bằng `esp_nn_conv_s8` (lệnh SIMD của ESP32-S3), thường nhanh hơn
   nhiều lần. Kết quả vẫn trùng từng bit vì ESP-NN dùng đúng công thức requant này.
3. **Hiển thị lên OLED I2C** hoặc màn hình SPI.
