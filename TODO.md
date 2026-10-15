# Task List: Gateway Implementation & Training Track

---

## Giai đoạn 1: Nền tảng nRF Connect SDK (NCS) & Zephyr RTOS

### Task 1: Thiết lập môi trường Toolchain NCS & Kiểm tra West
- **Description:** Cài đặt công cụ `west`, nRF Connect Toolchain v2.6.x/v2.7.x và nRF Connect Extension trên VS Code.
- **Acceptance criteria:**
  - [ ] Lệnh `west --version` chạy thành công trong terminal.
  - [ ] VS Code nhận diện được SDK và Toolchain của Nordic.
- **Verification:**
  - `west --version` trả về phiên bản (ví dụ `west v1.2.0`).
- **Dependencies:** None
- **Files likely touched:** None (Cấu hình môi trường)
- **Estimated scope:** XS

### Task 2: Tạo dự án nRF đầu tiên — Board `promicro_nrf52840/nrf52840/uf2` & USB CDC ACM Console
- **Description:** Tạo cấu trúc dự án chuẩn NCS (`CMakeLists.txt`, `prj.conf`, `src/main.c`) với target board **`promicro_nrf52840/nrf52840/uf2`**. Cấu hình cổng Serial Console ảo USB CDC ACM trên cổng Type-C và viết chương trình nhấp nháy đèn LED kèm in log `printk`.
- **Acceptance criteria:**
  - [ ] Người dùng tự tạo file và code thành công trong `gateway/nrf_central/`.
  - [ ] Biên dịch thành công cho board `promicro_nrf52840/nrf52840/uf2`.
  - [ ] LED trên kit nRF52840 Pro Micro nhấp nháy chu kỳ 1s và in log qua USB CDC ACM.
- **Verification:**
  - `west build -b promicro_nrf52840/nrf52840/uf2 W:/gateway/nrf_central` sinh ra `zephyr.uf2` thành công (97.2 KB, start address `0x26000`).
- **Dependencies:** Task 1
- **Files likely touched:**
  - `gateway/nrf_central/CMakeLists.txt`
  - `gateway/nrf_central/prj.conf`
  - `gateway/nrf_central/src/main.c`
- **Estimated scope:** S (3 files)

### Task 3: Kích hoạt tạo file UF2 & Nạp trực tiếp qua cổng Type-C
- **Description:** Cấu hình Kconfig để Zephyr tự động sinh file `zephyr.uf2` cho phân vùng bootloader của `promicro_nrf52840`. Thực hành nhấn đúp nút Reset trên kit và kéo thả file UF2 vào ổ đĩa ảo để nạp code.
- **Acceptance criteria:**
  - [x] Sau khi build, file `gateway/nrf_central/build/nrf_central/zephyr/zephyr.uf2` được tạo tự động với offset flash chính xác (`0x26000`).
  - [x] Nạp thành công vào kit qua cổng USB Type-C bằng thao tác kéo thả/copy file UF2.
  - [x] Đã thiết lập script Python persistent monitor tự động theo dõi cổng Serial COM11.
- **Verification:**
  - Kéo file `.uf2` vào ổ đĩa ảo `NICENANO` $\rightarrow$ Mạch tự khởi động lại, LED đỏ P0.15 chớp nháy và in log ra cổng COM11.
- **Dependencies:** Task 2
- **Files likely touched:**
  - `gateway/nrf_central/prj.conf`
  - `gateway/read_serial.py`
- **Estimated scope:** XS (1 file)

### Task 4: Làm chủ Đa luồng Zephyr (Threads & Message Queue k_msgq)
- **Description:** Tạo 2 thread độc lập trong Zephyr: Thread sinh dữ liệu và Thread in dữ liệu ra cổng Serial USB, truyền dữ liệu an toàn qua `k_msgq`.
- **Acceptance criteria:**
  - [x] 2 thread chạy đa nhiệm định kỳ không làm treo CPU.
  - [x] Dữ liệu truyền qua `k_msgq` được in ra Serial Monitor USB đầy đủ, không mất mẫu.
  - [x] Main thread nhấp nháy LED độc lập với Producer & Consumer.
- **Verification:**
  - Mở Serial Monitor (Baudrate 115200) quan sát log in ra liên tục mỗi 200ms.
- **Dependencies:** Task 3
- **Files likely touched:**
  - `gateway/nrf_central/prj.conf`
  - `gateway/nrf_central/src/main.c`
- **Estimated scope:** S (2 files)

---

## Checkpoint 1: Sau Giai đoạn 1 (Nền tảng nRF52840)
- [ ] Môi trường NCS hoạt động ổn định trên máy tính của bạn.
- [ ] Tự biên dịch và nạp được file UF2 vào nRF52840 qua cáp Type-C với board `nice_nano`.
- [ ] USB CDC ACM console in log `printk` bình thường qua cổng Type-C.

---

## Giai đoạn 2: BLE Multi-Link Central (10 Nodes) trên nRF52840

### Task 5: Lập trình BLE Active Scan & Lọc thiết bị Node
- **Description:** Kích hoạt ngăn xếp Bluetooth Central trong Zephyr. Cấu hình **Active Scan** (`BT_LE_SCAN_ACTIVE`) để yêu cầu gói Scan Response chứa Service UUID 128-bit, đồng thời lọc chính xác tên thiết bị `BT_DATA_NAME_COMPLETE = "ECG_dev"` và UUID `0x1400`.
- **Acceptance criteria:**
  - [x] Bật được Radio BLE ở chế độ Central Active Scan (`bt_le_scan_start`).
  - [x] Phân tích AD fields qua `bt_data_parse()`, lọc chính xác theo Prefix tên `ECG_dev` và Service UUID `0x1400`.
  - [x] In địa chỉ MAC, RSSI, tên thiết bị ra USB Serial console qua `LOG_INF`.
- **Verification:**
  - Bật Node đeo tay hoặc phát BLE `ECG_dev` $\rightarrow$ Terminal in: `[BLE Scan] Target Node Detected! MAC: ...`.
- **Dependencies:** Task 4
- **Files likely touched:**
  - `gateway/nrf_central/prj.conf`
  - `gateway/nrf_central/src/main.c`
- **Estimated scope:** M (2 files)

### Task 6: Kết nối 1 Node, Bắt tay ATT MTU Exchange & Nhận dữ liệu (CCCD)
- **Description:** Bắt tay kết nối với Node. **Bắt buộc chủ động gửi yêu cầu ATT MTU Exchange (`bt_gatt_exchange_mtu()`)** để Node mở cờ `m_mtu_negotiated`. Khám phá GATT tìm Characteristic TX `0x1401` và ghi CCCD để nhận Notify.
- **Acceptance criteria:**
  - [x] MTU được đàm phán thành công lên 247 bytes (Node không bị lỗi DATA_SIZE).
  - [x] Hàm notify_cb() nhận được dữ liệu: bóc tách được gói 5B Vitals và gói 100B ECG.
- **Verification:**
  - Serial Console in ra: `MTU exchange OK`, `HR: xx bpm, SpO2: xx%, Temp: xx C` và 50 mẫu ECG đầu tiên.
- **Dependencies:** Task 5
- **Files likely touched:**
  - `gateway/nrf_central/src/ble_central_mgr.c`
  - `gateway/nrf_central/src/main.c`
- **Estimated scope:** S (2 files)

### Task 7: Máy trạng thái Kết nối tuần tự — Mở rộng lên 10 Node đồng thời
- **Description:** Xây dựng mảng quản lý 10 kết nối `struct bt_conn *conns[10]`. Áp dụng máy trạng thái kết nối tuần tự: Quét $\rightarrow$ Kết nối Node $i$ $\rightarrow$ MTU Exchange $\rightarrow$ Bật CCCD $\rightarrow$ Quét tiếp Node $i+1$. Ánh xạ MAC sang `node_id` cố định.
- **Acceptance criteria:**
  - [x] nRF52840 duy trì đồng thời từ 2 đến 10 kết nối mà không bị lỗi EBUSY hay tràn RAM.
  - [x] Mỗi gói tin nhận được đều gắn đúng nhãn node_index (0 .. 9).
- **Verification:**
  - Bật lần lượt các thiết bị $\rightarrow$ nRF52840 lần lượt kết nối đủ và nhận dữ liệu song song.
- **Dependencies:** Task 6
- **Files likely touched:**
  - `gateway/nrf_central/prj.conf`
  - `gateway/nrf_central/src/ble_central_mgr.c`
  - `gateway/nrf_central/src/ble_central_mgr.h`
- **Estimated scope:** M (3 files)

### Task 8: Xử lý Ngắt kết nối (Disconnection) & Tự động kết nối lại an toàn
- **Description:** Bắt sự kiện ngắt kết nối trong Zephyr. Giải phóng slot trong mảng, dọn dẹp biến trạng thái và kích hoạt quét lại với chu kỳ ngắt quãng (Duty cycle an toàn) để không làm nghẽn sóng radio của các node đang kết nối.
- **Acceptance criteria:**
  - [x] Khi 1 Node tắt nguồn: Slot được giải phóng an toàn, các Node còn lại vẫn truyền bình thường, không bị Supervision Timeout.
  - [x] Khi Node bật lại: Tự động kết nối lại trong vòng < 5s (nhờ quét nền Duty Cycle 20%).
- **Verification:**
  - Tắt nguồn Node -> nRF báo Disconnected. Bật nguồn lại -> nRF tự kết nối lại và tiếp tục stream.
- **Dependencies:** Task 7
- **Files likely touched:**
  - `gateway/nrf_central/src/ble_central_mgr.c`
- **Estimated scope:** S (1-2 files)

---

## Checkpoint 2: Sau Giai đoạn 2 (Hoàn thiện nRF52840 BLE Central)
- [x] nRF52840 tự động quét, MTU Exchange và nhận dữ liệu ổn định từ nhiều Node.
- [x] Cơ chế Auto-Reconnect hoạt động an toàn, không gây đứt kết nối dây chuyền.

---

## Giai đoạn 3: Làm chủ ESP-IDF & FreeRTOS trên ESP32-S3

### Task 9: Thiết lập môi trường ESP-IDF & Cấu trúc Dự án
- **Description:** Cài đặt ESP-IDF, phân tích file `CMakeLists.txt`, cấu hình `menuconfig` và tạo dự án cơ bản trên ESP32-S3.
- **Acceptance criteria:**
  - [x] Biên dịch dự án bằng `idf.py build` thành công.
  - [x] Nạp vào kit ESP32-S3 và in log ra qua lệnh `idf.py monitor`.
- **Verification:**
  - Terminal in ra thông tin chip ESP32-S3 và thông báo "ESP32-S3 Gateway Init".
- **Dependencies:** None
- **Files likely touched:**
  - `gateway/esp32_cloud/CMakeLists.txt`
  - `gateway/esp32_cloud/main/CMakeLists.txt`
  - `gateway/esp32_cloud/main/main.c`
- **Estimated scope:** S (3 files)

### Task 10: Đa luồng FreeRTOS trên ESP32-S3 (Dual-Core Tasks & Queues)
- **Description:** Tạo 2 FreeRTOS Task ghim vào 2 core khác nhau (`Core 0` cho Wi-Fi/Cloud, `Core 1` cho UART), truyền dữ liệu an toàn qua `QueueHandle_t`.
- **Acceptance criteria:**
  - [x] Task_Core0 và Task_Core1 chạy đồng thời nhịp nhàng.
  - [x] Dữ liệu đẩy qua Queue không bị rò rỉ bộ nhớ (Memory Leak).
- **Verification:**
  - Monitor in log xác nhận Task chạy đúng trên Core 0 và Core 1.
- **Dependencies:** Task 9
- **Files likely touched:**
  - `gateway/esp32_cloud/main/main.c`
- **Estimated scope:** S (1 file)

### Task 11: Quản lý Wi-Fi Station & Cơ chế Tự kết nối lại (Auto-Reconnect)
- **Description:** Lập trình module Wi-Fi Station trên ESP-IDF sử dụng Event Loop. Xử lý tự động kết nối lại khi mất sóng Wi-Fi hoặc router khởi động lại.
- **Acceptance criteria:**
  - [x] ESP32-S3 kết nối thành công vào Wi-Fi và nhận IP từ DHCP.
  - [x] Khi ngắt Wi-Fi và bật lại, ESP32 tự động kết nối lại thành công.
- **Verification:**
  - Terminal báo `GOT_IP: 192.168.x.x` và ping thành công ra internet.
- **Dependencies:** Task 10
- **Files likely touched:**
  - `gateway/esp32_cloud/main/wifi_sta_mgr.c`
  - `gateway/esp32_cloud/main/wifi_sta_mgr.h`
  - `gateway/esp32_cloud/main/main.c`
- **Estimated scope:** M (3 files)

### Task 12: ThingsBoard Gateway MQTT Client & Batching 1s (Rate Limit Protection)
- **Description:** Kết nối tới ThingsBoard Cloud qua giao thức MQTT. Sử dụng ThingsBoard Gateway API (`v1/gateway/telemetry`) phân biệt theo thiết bị con (`Node_0` .. `Node_9`). Sử dụng buffer tĩnh và `snprintf()` (Zero dynamic allocation), gom bản tin chu kỳ 1s đẩy 1 lần.
- **Acceptance criteria:**
  - [x] ESP32-S3 kết nối thành công tới ThingsBoard MQTT Broker với Access Token.
  - [x] Telemetry của các Node hiển thị độc lập, không bị ghi đè.
  - [x] Broker không bị quá tải rate limit (kết nối MQTT duy trì liên tục).
- **Verification:**
  - Mở web ThingsBoard quan sát dữ liệu các Node nhảy số đều đặn mỗi 1s.
- **Dependencies:** Task 11
- **Files likely touched:**
  - `gateway/esp32_cloud/main/thingsboard_mqtt.c`
  - `gateway/esp32_cloud/main/thingsboard_mqtt.h`
  - `gateway/esp32_cloud/main/main.c`
- **Estimated scope:** M (3 files)

---

## Checkpoint 3: Sau Giai đoạn 3 (Hoàn thiện ESP32-S3 Cloud)
- [x] ESP32-S3 kết nối Wi-Fi ổn định và tự động kết nối lại khi mất mạng.
- [x] Kết nối MQTT tới ThingsBoard qua Gateway API hoạt động tin cậy, không bị ngắt kết nối do rate limit.

---

## Giai đoạn 4: Ghép nối UART & Tích hợp Toàn hệ thống

### Task 13: Cấu hình UART DMA Async 460800 bps an toàn giữa 2 Vi điều khiển
- **Description:** Đấu nối phần cứng TX/RX/GND giữa nRF52840 Pro Micro và ESP32-S3. Cấu hình Zephyr UART Async API trên nRF52840 và FreeRTOS UART RingBuffer DMA sâu 1024 bytes trên ESP32-S3 ở tốc độ an toàn **$460800\text{ bps}$**.
- **Acceptance criteria:**
  - [x] Truyền nhận dữ liệu liên tục ở tốc độ $460800\text{ bps}$ không bị lỗi tràn buffer FIFO khi Wi-Fi đang hoạt động.
- **Verification:**
  - [x] nRF52840 EasyDMA bắn 10,000 bytes sang ESP32-S3, ESP32-S3 nhận đủ 10,000 / 10,000 bytes, 0 byte errors (Data Integrity: 100.00%).
- **Dependencies:** Task 8, Task 12
- **Files likely touched:**
  - `gateway/nrf_central/src/uart_tx_bridge.c`
  - `gateway/esp32_cloud/main/uart_rx_parser.c`
- **Estimated scope:** M (3-4 files)

### Task 14: Hiện thực Khung truyền UART có đồng bộ SOF & Checksum CRC-8
- **Description:** Tích hợp file [`gateway/common/uart_frame_def.h`](file:///d:/Workspace/Projects/Wearable%20Device/gateway/common/uart_frame_def.h) (đã có trường `crc8` chuẩn). nRF52840 đóng gói khung truyền `[0xAA][Node_ID][Pkt_Type][Len][Data][CRC8]`. ESP32-S3 viết máy trạng thái giải mã và kiểm tra CRC-8.
- **Acceptance criteria:**
  - [x] ESP32-S3 phân loại chính xác từng gói dữ liệu tương ứng với từng Node ID (0-9).
  - [x] Bất kỳ gói tin nào bị lỗi bit đường truyền đều bị phát hiện bởi CRC-8 và loại bỏ an toàn.
- **Verification:**
  - [x] TDD Unit Test pass 5/5 test suites; nRF52840 đóng gói chuẩn; ESP32-S3 FSM giải mã và kiểm tra CRC-8 thành công.
- **Dependencies:** Task 13
- **Files likely touched:**
  - `gateway/nrf_central/src/uart_tx_bridge.c`
  - `gateway/esp32_cloud/main/uart_rx_parser.c`
  - `gateway/esp32_cloud/main/uart_rx_parser.h`
- **Estimated scope:** M (3 files)

### Task 15: Thuật toán Phân giải Timestamp Vi sai (+4 ms) cho Sóng ECG
- **Description:** Lập trình module `data_synthesizer.c` trên ESP32-S3. Khi nhận mảng 50 mẫu ECG từ UART, tự động gán nhãn thời gian ảo tăng dần $4\text{ ms}$ cho từng điểm đo trước khi đóng gói JSON đẩy lên ThingsBoard Gateway API.
- **Acceptance criteria:**
  - [x] Mảng 50 mẫu ECG được gắn mốc thời gian cách đều đúng $4\text{ ms}$ trên ThingsBoard.
  - [x] Đồ thị trên Web ThingsBoard hiển thị sóng ECG trải dài liên tục, không bị dồn cục tại một điểm.
- **Verification:**
  - [x] Module `data_synthesizer` tinh giản (65 dòng) đóng gói `ecg_batch` và `vitals` khớp 100% với giải mã vi sai `useTbWebSocket.js` của Wearable_Server.
- **Dependencies:** Task 14
- **Files likely touched:**
  - `gateway/esp32_cloud/main/data_synthesizer.c`
  - `gateway/esp32_cloud/main/data_synthesizer.h`
  - `gateway/esp32_cloud/main/thingsboard_mqtt.c`
- **Estimated scope:** S (2 files)

### Task 16: Tích hợp Đầu-Cuối & Kiểm thử Tải Hệ thống Hoàn chỉnh
- **Description:** Ghép nối hoàn chỉnh: Node đeo tay $\rightarrow$ nRF52840 Central $\rightarrow$ ESP32-S3 Gateway $\rightarrow$ ThingsBoard Dashboard. Kiểm thử độ bền và độ ổn định liên tục trong 1 giờ.
- **Acceptance criteria:**
  - [ ] Toàn bộ luồng dữ liệu ECG và Vitals từ Node truyền về hiển thị mượt mà trên ThingsBoard.
  - [ ] Hệ thống chạy liên tục 1 giờ không xảy ra crash, assert hay mất kết nối.
  - [ ] Bạn làm chủ hoàn toàn mã nguồn của cả hai nền tảng NCS và ESP-IDF!
- **Verification:**
  - Quan sát Dashboard hiển thị trực tiếp dữ liệu đo từ cảm biến thật trên cổ tay.
- **Dependencies:** Task 15
- **Files likely touched:**
  - Tất cả các module trong `gateway/`
- **Estimated scope:** M (Kiểm thử hệ thống)

---

## Checkpoint Cuối cùng: Nghiệm thu Toàn diện Hệ thống
- [ ] 100% tiêu chí nghiệm thu trong [`gateway/SPEC_GATEWAY.md`](file:///d:/Workspace/Projects/Wearable%20Device/gateway/SPEC_GATEWAY.md) được thỏa mãn.
- [ ] Bạn tự tin lập trình, cấu hình và gỡ lỗi (debug) trên cả Zephyr RTOS (NCS) và FreeRTOS (ESP-IDF).
