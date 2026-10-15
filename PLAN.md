# Implementation Plan: Dual-MCU Wearable Health Monitor Gateway
### nRF52840 (NCS / Zephyr, board: `nice_nano`) + ESP32-S3 (ESP-IDF / FreeRTOS)

## 1. Overview
Kế hoạch triển khai kỹ thuật và lộ trình giảng dạy từng bước cho hệ thống Gateway thu thập dữ liệu y tế từ 10 Node đeo tay. Hệ thống phân chia vai trò:
- **nRF52840 Pro Micro:** Quản lý kết nối BLE Multi-link tới 10 Node (board target `nice_nano`), chủ động gửi yêu cầu ATT MTU Exchange, nhận dữ liệu Notify (ECG 100B, Vitals 5B), chuyển tiếp qua UART DMA ở tốc độ an toàn $460800\text{ bps}$.
- **ESP32-S3 DevKit:** Nhận UART DMA, kiểm tra CRC-8, tổng hợp timestamp vi sai (+4ms/mẫu), quản lý Wi-Fi và đẩy dữ liệu lên ThingsBoard qua Gateway API (`v1/gateway/telemetry`) theo mẻ 1 giây để bảo vệ Rate Limit.

Toàn bộ các task được thiết kế theo kích thước **S và M (1 - 3 files)**, có tiêu chí nghiệm thu độc lập và lệnh kiểm thử rõ ràng, kết hợp giải thích bản chất kiến trúc để người học làm chủ hai nền tảng NCS và ESP-IDF.

---

## 2. Architecture Decisions (Đã cập nhật sau Phản biện Nghịch đảo)

1. **Phân lập hoàn toàn không gian làm việc trong `gateway/`:**
   - Mã nguồn nRF Central đặt tại `gateway/nrf_central/` (build độc lập bằng `west -b nice_nano`).
   - Mã nguồn ESP32 đặt tại `gateway/esp32_cloud/` (build độc lập bằng `idf.py`).
   - Toàn bộ thư mục gốc của Device Node giữ nguyên $100\%$ không chỉnh sửa.
2. **Target Board chính thức trong Zephyr: `nice_nano`:**
   - Kit nRF52840 Pro Micro / SuperMini sử dụng định nghĩa board chuẩn upstream Zephyr là `nice_nano`. Board này đã cấu hình sẵn phân vùng Flash khớp với Adafruit UF2 Bootloader.
3. **Kích hoạt USB CDC ACM Console cho `printk`:**
   - Vì Pro Micro dùng cổng USB Type-C nối trực tiếp vào chân D+/D- của SoC nRF52840, hệ thống bật `CONFIG_USB_DEVICE_STACK=y` để tạo cổng COM ảo (CDC ACM), tách biệt hoàn toàn với UART phần cứng nối sang ESP32-S3.
4. **Bắt buộc Chủ động ATT MTU Exchange:**
   - Central nRF52840 phải gọi `bt_gatt_exchange_mtu()` ngay khi kết nối. Nếu thiếu bước này, Node sẽ chặn toàn bộ mảng ECG 100 bytes với lỗi `NRF_ERROR_DATA_SIZE`.
5. **Cấu hình Active Scan để nhận diện Service UUID:**
   - Service UUID `0x1400` của Node nằm trong gói **Scan Response** (`srdata`), do đó Central phải bật `BT_LE_SCAN_ACTIVE` để kích hoạt gửi yêu cầu quét và so khớp.
6. **Hạ tốc độ UART xuống $460800\text{ bps}$ để đảm bảo an toàn 3 dây:**
   - Loại bỏ nguy cơ tràn FIFO 128 bytes của ESP32-S3 khi có ngắt Wi-Fi. Băng thông $46\text{ kB/s}$ vẫn gấp 10 lần lưu lượng thực tế ($5\text{ kB/s}$ cho 10 node).
7. **Bảo vệ ThingsBoard Rate Limit bằng Batching & Gateway API:**
   - ESP32-S3 gom dữ liệu theo chu kỳ $1\text{ s}$ đẩy 1 lần lên topic `v1/gateway/telemetry`.
   - Sử dụng bộ đệm tĩnh và hàm `snprintf()` (Zero dynamic allocation) để định dạng JSON, không dùng `cJSON` ở tần số cao tránh phân mảnh heap RAM.
8. **Đánh đổi về Downlink:**
   - Bản V1 tập trung hoàn thiện $100\%$ luồng thu thập dữ liệu (Uplink). Tính năng gửi lệnh Downlink (CMD_*) được quy hoạch thành bài tập nâng cao ở cuối Giai đoạn 4.

---

## 3. Dependency Graph

```text
[gateway/common/ Headers] (Hoàn thành)
       │
       ├──────────────────────────────────────────┐
       ▼                                          ▼
[Giai đoạn 1: nRF52840 NCS Cơ bản]       [Giai đoạn 3: ESP32-S3 ESP-IDF Cơ bản]
  - Bài 1: Setup West/NCS                  - Bài 9: Setup ESP-IDF & CMake
  - Bài 2: Board nice_nano & CDC ACM       - Bài 10: FreeRTOS Tasks & Queues
  - Bài 3: Nạp USB UF2                     - Bài 11: Wi-Fi STA Auto-Reconnect
  - Bài 4: Zephyr Threads & k_msgq         - Bài 12: ThingsBoard Gateway MQTT
       │                                          │
       ▼                                          │
[Giai đoạn 2: nRF52840 BLE Multi-Link]            │
  - Bài 5: Active Scan & 0x1400 Filter            │
  - Bài 6: Connect, MTU Exch & CCCD               │
  - Bài 7: Multi-Link 10 Nodes Matrix             │
  - Bài 8: Auto-Reconnect Handler                 │
       │                                          │
       └───────────────────┬──────────────────────┘
                           ▼
[Giai đoạn 4: Ghép nối UART & Tích hợp Toàn hệ thống]
  - Bài 13: UART DMA 460800 baud nRF52840 ◄──► ESP32-S3
  - Bài 14: UART Frame Encoder/Decoder & CRC-8
  - Bài 15: Timestamp Synthesizer (+4ms) & Rate Limiting
  - Bài 16: End-to-End System Verification (10 Nodes ──► ThingsBoard)
```

---

## 4. Risks & Mitigations (Sau thẩm định)

| Rủi ro kỹ thuật | Mức độ | Biện pháp giảm thiểu |
|---|---|---|
| **Lỗi board name khi build Zephyr** | Đã khắc phục | Sử dụng board chuẩn upstream `nice_nano`, tự động tích hợp flash partition cho UF2 bootloader. |
| **Node từ chối gửi gói ECG 100B (DATA_SIZE)** | Đã khắc phục | Central nRF52840 chủ động thực hiện `bt_gatt_exchange_mtu()` ngay trong callback kết nối. |
| **Không tìm thấy Node khi quét BLE** | Đã khắc phục | Sử dụng `BT_LE_SCAN_ACTIVE` để lấy Scan Response chứa Service UUID và lọc theo tên `ECG_dev`. |
| **Tràn UART FIFO của ESP32-S3 khi có Wi-Fi** | Đã khắc phục | Hạ baudrate từ 921600 xuống $460800\text{ bps}$ an toàn; cấu hình RingBuffer DMA sâu 1024 bytes. |
| **Bị ThingsBoard đóng kết nối do gửi 60 msg/s** | Đã khắc phục | Gom dữ liệu 1s/lần và chuyển sang topic ThingsBoard Gateway API (`v1/gateway/telemetry`). |
| **Phân mảnh RAM heap trên ESP32-S3** | Đã khắc phục | Định dạng JSON bằng mảng tĩnh và `snprintf()`, cấm cấp phát động `cJSON` liên tục trong vòng lặp. |
| **Trôi mốc thời gian khi mất gói BLE** | Trung bình | ESP32-S3 duy trì nhịp thời gian dựa trên `esp_timer_get_time()`, tự bù mốc thời gian nếu gói tin bị trễ. |

---

## 5. Phương thức Học tập & Tương tác

Quy trình **3 bước cho mỗi bài học**:
1. **Giải thích bản chất (Concepts & Why):** Tại sao cần MTU Exchange? Tại sao DeviceTree lại cấu hình chân như vậy?
2. **Cung cấp code mẫu & Hướng dẫn thao tác:** Người học nạp vào kit thật và quan sát kết quả.
3. **Kiểm tra & Nghiệm thu (Check of Done):** Người học xác nhận kết quả chạy trên phần cứng trước khi chuyển sang bài tiếp theo.
