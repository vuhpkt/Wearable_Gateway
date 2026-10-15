# SPEC: Wearable Health Monitor Gateway Subsystem

## 1. Assumptions & Constraints
1. **Zero-Impact on Device Node:** Tuyệt đối không thay đổi, di chuyển hoặc can thiệp vào bất kỳ file mã nguồn nào của Device Node ở thư mục gốc. Node vẫn chạy nRF5 SDK v17.1.0 với các đặc tả BLE GATT hiện có (`0x1400`, `0x1401`, `0x1402`).
2. **Dual-MCU Hardware Trio Architecture:**
   - **BLE Central Co-Processor:** nRF52840 Pro Micro (Form-factor Pro Micro / Nice!Nano) chạy **nRF Connect SDK (NCS) / Zephyr RTOS**, target board là **`nice_nano`**.
   - **Cloud/Wi-Fi Processor:** ESP32-S3 chạy **ESP-IDF v5.x / FreeRTOS**.
   - **Inter-Chip Interconnect:** UART phần cứng 3 dây (TX, RX, GND chung), Baudrate an toàn **$460800\text{ bps}$** (8-N-1, tránh tràn FIFO 128B của ESP32-S3 khi có Wi-Fi).
3. **Flashing & Development Constraints:**
   - nRF52840 Pro Micro: Nạp firmware qua cổng USB Type-C bằng **UF2 Bootloader** (`CONFIG_BUILD_OUTPUT_UF2=y`), kéo-thả file `.uf2` vào ổ đĩa ảo (ví dụ `NICENANO`).
   - Debug Console trên nRF52840: Sử dụng **USB CDC ACM** ảo (`CONFIG_USB_DEVICE_STACK=y`) để in log `printk` ra cổng Type-C, tách biệt hoàn toàn với chân UART nối sang ESP32-S3.
   - ESP32-S3: Nạp và debug trực tiếp qua cổng USB bằng `idf.py flash monitor`.
4. **Target Multi-Link Capacity:** Hỗ trợ tối đa **10 Node đồng thời** bằng máy trạng thái kết nối tuần tự (Sequential State Machine).
5. **BLE GATT Exchange Contract:**
   - Central (nRF52840) **bắt buộc phải chủ động gửi yêu cầu ATT MTU Exchange** (`bt_gatt_exchange_mtu()`) ngay khi kết nối để Node kích hoạt cờ `m_mtu_negotiated = true`. Nếu thiếu bước này, Node sẽ chặn toàn bộ gói ECG 100 bytes với lỗi `NRF_ERROR_DATA_SIZE`.
   - Central phải dùng **Active Scan** (`BT_LE_SCAN_ACTIVE`) vì Service UUID 128-bit của Node nằm trong gói **Scan Response** (`srdata`), đồng thời lọc theo tên thiết bị `BT_DATA_NAME_COMPLETE = "ECG_dev"`.
6. **Data Integrity & Timing:** ESP32-S3 chịu trách nhiệm gán mốc thời gian vi sai đều đặn ($+4\text{ ms}$ cho mỗi mẫu ECG trong mảng $50\text{ mẫu}$) trước khi đóng gói gửi MQTT lên ThingsBoard.
7. **ThingsBoard Rate-Limiting & Gateway API:**
   - ESP32-S3 gom dữ liệu theo chu kỳ $1\text{ s}$ đẩy 1 lần (Batching).
   - Sử dụng **ThingsBoard Gateway API** (`v1/gateway/telemetry`) phân chia theo tên thiết bị (`Node_0` .. `Node_9`) để tránh bị Rate Limiter của ThingsBoard đóng kết nối TCP.

---

## 2. Capability Map

| Module ID | Trách nhiệm chính | Phụ thuộc | Target MCU / Toolchain |
|---|---|---|---|
| `common-proto` | Định nghĩa wire format, BLE UUIDs, Opcode lệnh, và khung truyền UART chung | — | C Header (`gateway/common/`) |
| `nrf-central` | Active Scan, quản lý 10 kết nối BLE, MTU Exchange, nhận Notify 0x1401, chuyển tiếp UART DMA | `common-proto` | nRF52840 (NCS / Zephyr, board `nice_nano`) |
| `esp32-cloud` | Nhận UART DMA, phân giải timestamp vi sai, kết nối Wi-Fi, đẩy ThingsBoard Gateway MQTT | `common-proto` | ESP32-S3 (ESP-IDF) |
| `curriculum` | Giáo trình đào tạo thực chiến (NCS Zephyr $\rightarrow$ ESP-IDF $\rightarrow$ Ghép nối) | Tất cả các module | Markdown Docs |

**Thứ tự triển khai & học tập:** `common-proto` $\longrightarrow$ `nrf-central` $\longrightarrow$ `esp32-cloud` $\longrightarrow$ Tích hợp toàn hệ thống.

---

## 3. Objective & Success Criteria

### 3.1. Mục tiêu (Objective)
Xây dựng một hệ thống Gateway 2 tầng (nRF52840 + ESP32-S3) chuẩn công nghiệp, có khả năng:
- Tự động quét và duy trì kết nối ổn định với tối đa 10 Node đeo tay.
- Thực hiện đầy đủ bắt tay ATT MTU để thông luồng ECG $250\text{ Hz}$ và các chỉ số sinh tồn (HR, SpO2, Temp).
- Đẩy dữ liệu thời gian thực lên ThingsBoard qua Gateway API với đồ thị sóng ECG mượt mà, không dồn cục thời gian.
- Đóng vai trò làm giáo trình thực hành giúp người học làm chủ Zephyr RTOS trên NCS và FreeRTOS trên ESP-IDF.

### 3.2. Tiêu chí nghiệm thu cụ thể (Concrete Success Criteria)
1. **Kết nối đa điểm ổn định:** nRF52840 kết nối thành công từ 1 đến 10 Node tuần tự, duy trì liên tục trong tối thiểu 1 giờ không xảy ra kernel panic, assert hoặc rớt kết nối đột ngột.
2. **Khôi phục kết nối (Auto-Reconnect):** Khi 1 Node tắt nguồn hoặc đi ra xa, nRF52840 tự động giải phóng slot và quét bắt lại ngay khi Node đó bật lại mà không làm nghẽn sóng radio của $9\text{ node}$ còn lại.
3. **Hiệu năng truyền thông UART:** Khung truyền UART giữa nRF52840 và ESP32-S3 đạt tốc độ $460800\text{ bps}$, tỷ lệ lỗi checksum CRC8 $< 0.01\%$, zero UART FIFO overflow.
4. **Đồ thị ECG chính xác:** Dữ liệu ECG hiển thị trên ThingsBoard giãn cách đều đặn đúng $4\text{ ms}$ cho từng điểm đo, hiển thị rõ nét dạng sóng P-QRS-T ở tần số tim chuẩn.
5. **Khả năng nạp UF2:** nRF52840 biên dịch với target `nice_nano` sinh ra file `zephyr.uf2` hoàn chỉnh, nạp được trực tiếp bằng thao tác kéo-thả vào ổ đĩa ảo qua cổng USB Type-C.

---

## 4. Tech Stack & Commands

### 4.1. Bộ công cụ & Phiên bản
- **nRF Central:** nRF Connect SDK (NCS) v2.6.x / v2.7.x (dựa trên Zephyr RTOS v3.x), board target: **`nice_nano`**.
- **ESP32 Gateway:** ESP-IDF v5.1.x / v5.2.x, công cụ quản trị `idf.py`.
- **Compiler:** ARM GNU Toolchain (`arm-none-eabi-gcc` trong toolchain NCS) & Xtensa/RISC-V Toolchain (trong ESP-IDF).

### 4.2. Câu lệnh thực thi (Commands)

#### nRF52840 Central (NCS / Zephyr):
- **Build (tạo file UF2 cho kit Pro Micro / Nice!Nano):**
  ```bash
  west build -b nice_nano gateway/nrf_central -d gateway/nrf_central/build
  ```
- **Clean build:**
  ```bash
  west build -t pristine -d gateway/nrf_central/build
  ```
- **Nạp code (Kéo thả USB):**
  Nhấn đúp nút Reset trên kit $\rightarrow$ Máy tính nhận ổ đĩa USB (ví dụ `NICENANO`) $\rightarrow$ Kéo file `build/zephyr/zephyr.uf2` thả vào ổ đĩa.

#### ESP32-S3 Cloud (ESP-IDF):
- **Cấu hình menuconfig:**
  ```bash
  idf.py -C gateway/esp32_cloud menuconfig
  ```
- **Build dự án:**
  ```bash
  idf.py -C gateway/esp32_cloud build
  ```
- **Flash & Mở Terminal Monitor:**
  ```bash
  idf.py -C gateway/esp32_cloud -p COMx flash monitor
  ```

---

## 5. Cấu trúc thư mục & Thiết kế module

```text
gateway/
├── SPEC_GATEWAY.md                         <-- Tài liệu đặc tả này
├── PLAN.md                                 <-- Kế hoạch triển khai & Quản lý rủi ro
├── TODO.md                                 <-- Danh sách 16 task chi tiết
├── CURRICULUM.md                           <-- Giáo trình & Lộ trình học tập chi tiết
│
├── common/                                 <-- Giao thức dùng chung
│   ├── ble_protocol_def.h                  <-- UUID, Opcode lệnh, cấu trúc Vitals/ECG
│   └── uart_frame_def.h                    <-- Khung truyền UART 460800 baud có CRC-8
│
├── nrf_central/                            <-- Dự án NCS Zephyr cho nRF52840 (Target: nice_nano)
│   ├── CMakeLists.txt                      <-- Cấu hình CMake Zephyr
│   ├── prj.conf                            <-- Kconfig: BLE Central, MTU Exchange, USB CDC ACM, UART Async
│   └── src/
│       ├── main.c                          <-- Khởi tạo Zephyr threads & USB CDC ACM
│       ├── ble_central_mgr.c               <-- Quản lý Active Scan, MTU Exchange & 10 kết nối BLE
│       ├── ble_central_mgr.h
│       ├── uart_tx_bridge.c                <-- Đóng khung và gửi UART DMA 460800 baud
│       └── uart_tx_bridge.h
│
└── esp32_cloud/                            <-- Dự án ESP-IDF cho ESP32-S3
    ├── CMakeLists.txt                      <-- Cấu hình CMake ESP-IDF
    ├── sdkconfig                           <-- Cấu hình FreeRTOS, Wi-Fi, ThingsBoard MQTT
    └── main/
        ├── CMakeLists.txt
        ├── main.c                          <-- Khởi tạo FreeRTOS tasks
        ├── uart_rx_parser.c                <-- Nhận UART DMA, kiểm tra CRC8
        ├── uart_rx_parser.h
        ├── data_synthesizer.c              <-- Phân giải timestamp vi sai (+4ms)
        ├── data_synthesizer.h
        ├── wifi_sta_mgr.c                  <-- Kết nối Wi-Fi & auto-reconnect
        ├── wifi_sta_mgr.h
        ├── thingsboard_mqtt.c              <-- Đẩy telemetry qua ThingsBoard Gateway API
        └── thingsboard_mqtt.h
```

---

## 6. Đặc tả Giao thức Cầu nối UART (Inter-Chip Wire Contract)

### 6.1. Cấu trúc Khung truyền (Frame Format)
```text
┌───────┬────────────┬───────────┬──────────────┬────────────────────────┬────────┐
│ SOF   │ NODE_ID    │ PKT_TYPE  │ LENGTH (L)   │ PAYLOAD (L bytes)      │ CRC-8  │
│ (1 B) │ (1 B: 0-9) │ (1 B)     │ (1 B: 1-128) │ ECG (100B) / Vit (5B)  │ (1 B)  │
└───────┴────────────┴───────────┴──────────────┴────────────────────────┴────────┘
```
- **Baudrate:** **$460800\text{ bps}$** (8-N-1).
- **SOF:** Cố định `0xAA`.
- **NODE_ID:** Chỉ số định danh cố định ánh xạ từ địa chỉ MAC của Node ($0 \le \text{ID} \le 9$).
- **PKT_TYPE:**
  - `0x01`: Gói ECG Batch (Mặc định 100 bytes = 50 mẫu `int16_t`).
  - `0x02`: Gói Vitals (5 bytes: `[hrEcg][hrPpg][spo2][temp_x10 LE]`).
  - `0x03`: Gói Báo cáo trạng thái kết nối (`0x00`: Ngắt kết nối, `0x01`: Đã kết nối).
  - `0x80`: Lệnh Downlink gửi ngược về Node (CMD_ECG_CFG, CMD_THR...).
- **CRC-8:** Đa thức $X^8 + X^2 + X + 1$ (`0x07`), tính trên toàn bộ byte từ `NODE_ID` đến hết `PAYLOAD`. Khai báo rõ ràng thành trường `uint8_t crc8` trong `uart_frame_t`.

---

## 7. Quy tắc lập trình & Giới hạn an toàn (Boundaries)

### Always (Bắt buộc tuân thủ):
- **Chủ động ATT MTU Exchange:** Central nRF52840 phải gọi `bt_gatt_exchange_mtu()` ngay sau khi kết nối BLE.
- **Active Scan:** Central phải bật `BT_LE_SCAN_ACTIVE` để lấy Scan Response chứa Service UUID.
- **Zero Dynamic Allocation trong Hot-Loops:** Trên ESP32-S3, định dạng chuỗi JSON bằng buffer tĩnh (`char scratchpad[512]`) và `snprintf()`, không dùng `cJSON_Create...` liên tục 50Hz để tránh phân mảnh heap RAM.
- **ThingsBoard Gateway API:** Sử dụng topic `v1/gateway/telemetry` và gom mẻ 1 giây/lần để bảo vệ kết nối khỏi bộ chặn Rate Limit của broker.

### Ask First (Phải hỏi ý kiến trước khi làm):
- Thay đổi cấu hình chân phần cứng (Pinout) kết nối UART giữa 2 kit.
- Thay đổi định dạng dữ liệu hoặc cấu trúc JSON gửi lên ThingsBoard.
- Mở rộng thêm tính năng Downlink sang thiết bị.

### Never (Tuyệt đối không làm):
- Không chỉnh sửa, di chuyển hoặc ghi đè bất kỳ file nào thuộc thư mục Device Node ở gốc.
- Không dùng hàm delay blocking (như `k_sleep()` hoặc `vTaskDelay()` quá $10\text{ ms}$) bên trong các hàm Callback ngắt hoặc UART ISR.
- Không cố nạp file `.hex` thô vào ổ đĩa UF2 mà phải dùng file `.uf2` có cấu trúc phân vùng hợp lệ.
