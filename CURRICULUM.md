# GIÁO TRÌNH ĐÀO TẠO THỰC CHIẾN: GATEWAY HEALTH MONITOR
### Làm chủ NCS (Zephyr RTOS) trên nRF52840 & ESP-IDF (FreeRTOS) trên ESP32-S3

---

## Giới thiệu & Định hướng phương pháp học
Khóa học này được thiết kế theo phương pháp **Project-Based Learning (Học qua dự án thực tế)**. Bạn không học lý thuyết suông mà học đến đâu, code chạy trên phần cứng thật đến đó:
- **Kit 1:** nRF52840 Pro Micro / SuperMini (target board Zephyr: **`nice_nano`**, nạp qua USB Type-C UF2 Bootloader) phụ trách BLE Multi-link 10 Node.
- **Kit 2:** ESP32-S3 DevKit (chạy ESP-IDF / FreeRTOS) phụ trách Wi-Fi và ThingsBoard Gateway MQTT Cloud.
- **Giao tiếp:** Cầu nối UART DMA an toàn $460800\text{ bps}$.

---

# GIAI ĐOẠN 1: NỀN TẢNG NRF CONNECT SDK (NCS) & ZEPHYR RTOS TRÊN nRF52840

## Bài 1: Thiết lập môi trường Toolchain NCS & Công cụ West
### 1. Mục tiêu bài học
- Cài đặt thành công nRF Connect SDK (phiên bản v2.6.x hoặc v2.7.x) và tiện ích mở rộng nRF Connect trên VS Code.
- Hiểu công cụ `west` là gì và cách nó quản lý mã nguồn Zephyr.
### 2. Bản chất kiến trúc
- **Zephyr không phải là một thư viện:** Zephyr là một hệ điều hành nhúng đa nền tảng.
- **`west` là gì?** `west` tương đương với `repo` của Android hoặc `git submodules`. Nó giúp kéo Zephyr kernel, các driver nrfx, và thư viện mbedTLS về máy tính một cách đồng bộ.
### 3. Thực hành
- Cài đặt nRF Command Line Tools và VS Code Extension "nRF Connect for VS Code".
- Kiểm tra cài đặt trong terminal:
  ```bash
  west --version
  ```
### 4. Tiêu chí hoàn thành
- [ ] Gõ `west --version` không báo lỗi. VS Code nhận diện được SDK NCS.

---

## Bài 2: Board Target `nice_nano`, DeviceTree & USB CDC ACM Console
### 1. Mục tiêu bài học
- Hiểu được tại sao kit nRF52840 Pro Micro dùng target board chuẩn là **`nice_nano`**.
- Kích hoạt cổng Serial Console ảo **USB CDC ACM** trên cổng Type-C của SoC nRF52840 để in log `printk`.
- Tạo ứng dụng "Blink LED & USB Serial Console" đầu tiên.
### 2. Bản chất kiến trúc
- **DeviceTree (`.dts`, `.overlay`):** Mô tả phần cứng. Board `nice_nano` đã định nghĩa sẵn chân LED (`led0`) và phân vùng Flash cho Adafruit UF2 Bootloader.
- **Bẫy USB Native:** nRF52840 Pro Micro nối thẳng cổng Type-C vào chân D+/D- của chip (không qua chip USB-to-UART rời). Vì vậy, bắt buộc phải kích hoạt USB Device Stack trong `prj.conf` để máy tính nhận cổng COM.
### 3. Cấu hình Kconfig trong `prj.conf`
```ini
CONFIG_GPIO=y
CONFIG_SERIAL=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_USB_DEVICE_STACK=y
CONFIG_UART_LINE_CTRL=y
```
### 4. Tiêu chí hoàn thành
- [ ] Code biên dịch thành công cho board `nice_nano`. LED nhấp nháy chu kỳ 1s và Serial Monitor trên máy tính in log `printk`.

---

## Bài 3: Bí quyết tạo file UF2 & Nạp trực tiếp qua cổng USB Type-C
### 1. Mục tiêu bài học
- Nạp code vào kit nRF52840 Pro Micro hoàn toàn qua cổng USB Type-C bằng bootloader UF2, không cần mua ST-Link hay J-Link.
### 2. Bản chất kiến trúc
- Mạch nRF52840 Pro Micro / Nice!Nano có sẵn **Adafruit UF2 Bootloader**.
- Khi nhấn đúp nút Reset, vi điều khiển nhảy vào vùng bootloader và biến thành một chiếc USB ảo (ổ đĩa Mass Storage).
### 3. Cấu hình Kconfig
Trong file `prj.conf`, thêm dòng sau:
```ini
CONFIG_BUILD_OUTPUT_UF2=y
```
Khi chạy `west build -b nice_nano`, hệ thống sẽ tự động tạo file `build/zephyr/zephyr.uf2` với offset flash hoàn toàn chính xác.
### 4. Thao tác nạp
1. Cắm cáp Type-C vào máy tính.
2. Nhấn đúp nút Reset trên mạch (trong vòng 0.5s) $\rightarrow$ Đèn LED trên mạch nhấp nháy chậm và máy tính hiện ổ đĩa (ví dụ `NICENANO`).
3. Kéo thả file `zephyr.uf2` vào ổ đĩa. Mạch tự khởi động lại và chạy firmware mới ngay lập tức!
### 5. Tiêu chí hoàn thành
- [ ] Tự nạp được file `zephyr.uf2` qua cáp USB và thấy đèn LED trên mạch chớp nháy.

---

## Bài 4: Đa luồng trong Zephyr RTOS (Threads, Mutex, Message Queue)
### 1. Mục tiêu bài học
- Hiểu cách tạo và điều phối đa luồng (Multi-threading) trong Zephyr.
- Sử dụng Hàng đợi tin nhắn (`k_msgq`) để truyền dữ liệu an toàn giữa các luồng.
### 2. Bản chất kiến trúc
- Zephyr sử dụng bộ điều phối ưu tiên (Preemptive Scheduler).
- `k_msgq` là hàng đợi vòng tròn (Ring buffer) có sẵn cơ chế đồng bộ thread-safe, cực kỳ lý tưởng để nhận dữ liệu từ ngắt BLE và chuyển cho luồng UART.
### 3. Thực hành
- Khởi tạo 2 threads: `Thread_Producer` (giả lập nhận mẫu ECG) và `Thread_Consumer` (in dữ liệu ra Serial USB) giao tiếp qua `k_msgq`.
### 4. Tiêu chí hoàn thành
- [ ] 2 luồng chạy nhịp nhàng, dữ liệu truyền qua `k_msgq` không bị rớt mẫu.

---

# GIAI ĐOẠN 2: BLE MULTI-LINK CENTRAL 10 NODE TRÊN nRF52840

## Bài 5: Kiến trúc BLE Central trong Zephyr (Active Scan & Lọc Scan Response)
### 1. Mục tiêu bài học
- Nắm vững chu trình làm việc của BLE Central: `bt_enable()` $\rightarrow$ `bt_scan_start()` $\rightarrow$ Bắt gói tin quảng bá $\rightarrow$ Nhận diện UUID `0x1400`.
### 2. Bản chất kiến trúc
- **Bẫy Scan Response:** Service UUID 128-bit của Node nằm trong gói **Scan Response** (`srdata`). Do đó, Central bắt buộc phải dùng **Active Scan** (`BT_LE_SCAN_ACTIVE`) để gửi SCAN_REQ thì Node mới trả lời.
- Lọc thiết bị dựa trên tên `BT_DATA_NAME_COMPLETE = "ECG_dev"` kết hợp 128-bit Base UUID.
### 3. Tiêu chí hoàn thành
- [ ] Terminal in ra địa chỉ MAC và RSSI của Node đeo tay khi nó bật quảng bá.

---

## Bài 6: Bắt tay ATT MTU Exchange & Bật GATT Notification (CCCD)
### 1. Mục tiêu bài học
- Thực hiện kết nối thành công tới 1 Node đeo tay.
- **Bắt buộc chủ động gửi yêu cầu ATT MTU Exchange (`bt_gatt_exchange_mtu()`)** để Node mở cờ `m_mtu_negotiated`.
- Khám phá GATT để lấy Handle của TX Characteristic (`0x1401`) và ghi CCCD để nhận dữ liệu.
### 2. Bản chất kiến trúc
- Nếu thiếu MTU Exchange, Node sẽ chặn toàn bộ mảng ECG 100 bytes với lỗi `NRF_ERROR_DATA_SIZE`.
- Hàm `notify_cb()` nhận trực tiếp mảng byte từ nRF Node.
### 3. Thực hành
- Nhận gói tin trong `notify_cb()`:
  - Nếu `len == 5`: In thông số Vitals (Nhịp tim, SpO2, Nhiệt độ).
  - Nếu `len == 100`: In 50 mẫu ECG đầu tiên ra console.
### 4. Tiêu chí hoàn thành
- [ ] MTU đàm phán thành công 247 bytes. In được sóng ECG và chỉ số sinh tồn của 1 Node ra Serial console.

---

## Bài 7: Máy trạng thái Kết nối tuần tự (Mở rộng lên 10 Node đồng thời)
### 1. Mục tiêu bài học
- Mở rộng từ 1 kết nối lên tối đa 10 kết nối đồng thời mà không bị crash Radio.
### 2. Bản chất kiến trúc
- **Quy tắc vàng:** Không bao giờ kết nối song song!
- Triển khai máy trạng thái tuần tự:
  ```
  [SCANNING] ──(Thấy Node i)──► [DỪNG SCAN] ──► [KẾT NỐI] ──► [MTU EXCHANGE] ──► [BẬT CCCD] ──► [LƯU conn[i]] ──► [TIẾP TỤC SCAN]
  ```
### 3. Cấu hình Kconfig cho 10 kết nối
```ini
CONFIG_BT_MAX_CONN=10
CONFIG_BT_CONN_TX_MAX=16
CONFIG_BT_BUF_ACL_RX_SIZE=251
CONFIG_BT_CTLR_DATA_LENGTH_MAX=251
```
### 4. Tiêu chí hoàn thành
- [ ] nRF52840 kết nối ổn định với từ 2 đến nhiều Node cùng lúc, nhận dữ liệu song song.

---

## Bài 8: Xử lý Ngắt kết nối & Tự động kết nối lại an toàn
### 1. Mục tiêu bài học
- Khi 1 bệnh nhân tắt nguồn Node hoặc đi xa làm rớt kết nối, nRF52840 tự động giải phóng bộ nhớ và quét bắt lại an toàn mà không làm rớt 9 Node còn lại.
### 2. Bản chất kiến trúc
- Callback `disconnected()` trong Zephyr: Lấy ra `conn_index` bị ngắt, xóa handle trong bảng quản lý, và khởi động lại bộ quét với chu kỳ ngắt quãng an toàn (Duty cycle thấp).
### 3. Tiêu chí hoàn thành
- [ ] Tắt nguồn Node $\rightarrow$ Terminal báo "Node disconnected". Bật nguồn lại $\rightarrow$ Gateway tự động kết nối lại trong vòng 5 giây.

---

# GIAI ĐOẠN 3: LÀM CHỦ ESP-IDF & FREERTOS TRÊN ESP32-S3

## Bài 9: Môi trường ESP-IDF, CMake & Cấu trúc Dự án ESP32-S3
### 1. Mục tiêu bài học
- Cài đặt ESP-IDF Extension trên VS Code, hiểu lệnh `idf.py build` và `idf.py -p COMx flash monitor`.
- Phân tích cấu trúc thư mục chuẩn của một dự án ESP-IDF.
### 2. Bản chất kiến trúc
- ESP-IDF dùng hệ thống build dựa trên **CMake**. Mỗi thư mục con là một "Component".
- Tập tin `sdkconfig` lưu trữ cấu hình sinh ra từ `menuconfig`.
### 3. Tiêu chí hoàn thành
- [ ] Biên dịch và nạp thành công chương trình cơ bản lên kit ESP32-S3.

---

## Bài 10: FreeRTOS trên ESP32-S3 (Tasks, Queues, Memory Architecture)
### 1. Mục tiêu bài học
- Hiểu mô hình 2 lõi (Dual-core) của ESP32-S3.
- Ghim task vào Core cụ thể (`xTaskCreatePinnedToCore`) và dùng `QueueHandle_t` truyền dữ liệu.
### 2. Bản chất kiến trúc
- Phân bổ 2 lõi:
  - **Core 0:** Dành cho Wi-Fi Stack, TCP/IP, MQTT.
  - **Core 1:** Dành cho UART DMA Driver và xử lý dữ liệu cảm biến.
### 3. Tiêu chí hoàn thành
- [ ] 2 Task chạy trên 2 Core khác nhau, trao đổi dữ liệu qua FreeRTOS Queue mượt mà.

---

## Bài 11: Wi-Fi Station Manager & Cơ chế Auto-Reconnect
### 1. Mục tiêu bài học
- Cấu hình ESP32-S3 kết nối vào Router Wi-Fi với cơ chế tự phục hồi kết nối chuẩn công nghiệp.
### 2. Bản chất kiến trúc
- Cơ chế Event Loop của ESP-IDF (`esp_event_loop_create_default`).
- Bắt sự kiện `WIFI_EVENT_STA_DISCONNECTED` để kích hoạt `esp_wifi_connect()` tự động thử lại theo hàm mũ.
### 3. Tiêu chí hoàn thành
- [ ] ESP32-S3 tự lấy IP từ Router, rút nguồn Router cắm lại $\rightarrow$ ESP32 tự động kết nối lại.

---

## Bài 12: ThingsBoard Gateway MQTT Client & Batching 1s (Chống Rate Limit)
### 1. Mục tiêu bài học
- Kết nối tới ThingsBoard Cloud qua giao thức MQTT sử dụng **ThingsBoard Gateway API** (`v1/gateway/telemetry`).
- Định dạng JSON bằng buffer tĩnh (`snprintf()`), gom mẻ 1 giây/lần để chống sập kết nối do Rate Limit.
### 2. Bản chất kiến trúc
- Topic `v1/devices/me/telemetry` chỉ dùng cho 1 device. Gateway quản lý 10 node phải dùng topic Gateway API.
- Cấm dùng `cJSON` cấp phát động liên tục 50Hz để tránh phân mảnh heap RAM.
### 3. Tiêu chí hoàn thành
- [ ] Trên ThingsBoard Dashboard hiển thị được thông số của các Node con (`Node_0`, `Node_1`...) độc lập.

---

# GIAI ĐOẠN 4: GHÉP NỐI TOÀN HỆ THỐNG & KIỂM THỬ ĐẦU - CUỐI

## Bài 13: Cấu hình UART DMA Async Tốc độ an toàn 460800 bps
### 1. Mục tiêu bài học
- Đấu nối 3 dây giữa nRF52840 Pro Micro và ESP32-S3: **TX $\rightarrow$ RX**, **RX $\rightarrow$ TX**, **GND $\rightarrow$ GND**.
- Cấu hình UART trên nRF52840 dùng Zephyr Async API (EasyDMA).
- Cấu hình UART trên ESP32-S3 dùng Ring Buffer DMA sâu 1024 bytes ở tốc độ **$460800\text{ bps}$**.
### 2. Bản chất kiến trúc
- Ở $460800\text{ bps}$, băng thông đạt $46\text{ kB/s}$ (gấp 10 lần nhu cầu $5\text{ kB/s}$), hoàn toàn triệt tiêu nguy cơ tràn FIFO 128 bytes của ESP32-S3 khi có ngắt Wi-Fi mà không cần đến chân RTS/CTS.
### 3. Tiêu chí hoàn thành
- [ ] nRF52840 bắn 10,000 byte sang ESP32-S3, ESP32-S3 nhận đủ $100\%$, không mất 1 byte nào.

---

## Bài 14: Hiện thực Khung truyền UART & Checksum CRC-8
### 1. Mục tiêu bài học
- Lập trình đóng gói khung truyền trên nRF52840:
  `[0xAA (SOF)][Node_ID][Pkt_Type][Len][Payload][CRC8]`
- Lập trình máy trạng thái giải mã (State Machine Parser) trên ESP32-S3.
### 2. Bản chất kiến trúc
- Thuật toán tìm kiếm byte bắt đầu `0xAA` để tự đồng bộ lại luồng dữ liệu nếu xảy ra nhiễu đường dây.
- Kiểm tra toàn vẹn bằng bảng tra CRC-8 (LUT CRC-8) chạy trong vài chu kỳ lệnh.
### 3. Tiêu chí hoàn thành
- [ ] ESP32-S3 phân loại chính xác: "Gói Vitals của Node 2", "Gói ECG của Node 0". Loại bỏ toàn bộ các gói bị gài lỗi bit.

---

## Bài 15: Thuật toán Phân giải Timestamp Vi sai (+4 ms) cho ECG
### 1. Mục tiêu bài học
- Giải quyết triệt để vấn đề: "Làm sao để Server không coi 50 mẫu ECG là đo cùng 1 thời điểm?".
### 2. Bản chất kiến trúc
- Khi ESP32-S3 bóc 50 mẫu `int16_t` từ khung UART:
- Tự động cộng dồn mốc thời gian ảo: $t_i = t_{\text{prev}} + 4\text{ ms}$.
- Đóng gói thành mảng JSON ThingsBoard Gateway API có gắn nhãn thời gian riêng biệt cho từng điểm.
### 3. Tiêu chí hoàn thành
- [ ] Dữ liệu gửi lên ThingsBoard có trường `"ts"` tăng đều $4\text{ ms}$ cho từng điểm đo.

---

## Bài 16: Tích hợp Toàn diện & Nghiệm thu Hệ thống
### 1. Mục tiêu bài học
- Nối mạng toàn bộ hệ thống thực tế: Node đeo tay (chạy nRF5 SDK) $\rightarrow$ nRF52840 Central (chạy NCS) $\rightarrow$ ESP32-S3 (chạy ESP-IDF) $\rightarrow$ ThingsBoard Dashboard.
### 2. Thao tác nghiệm thu
- Mở Web Dashboard trên máy tính/điện thoại.
- Bật Node đeo tay lên.
- Quan sát:
  1. Đèn trên nRF52840 báo kết nối.
  2. Đèn trên ESP32-S3 báo nhận dữ liệu.
  3. Đồ thị sóng ECG trên Web bắt đầu quét liên tục, mượt mà ở tần số $250\text{ Hz}$.
  4. Các thông số HR, SpO2, Nhiệt độ hiển thị đúng với cảm biến.
### 3. Tiêu chí hoàn thành tối thượng
- [ ] Toàn bộ hệ thống chạy liên tục không lỗi trong 2 giờ. Bạn hoàn toàn làm chủ mã nguồn của cả hai nền tảng NCS và ESP-IDF!
