# WEARABLE SERVER — API CONTRACT & SPECIFICATION

> **Authoritative Reference:** Derived from [`quang-nguyen-161/Wearable_Server`](https://github.com/quang-nguyen-161/Wearable_Server)  
> **Target Dashboard:** Next.js 14 (`wearable-server.vercel.app`) & ThingsBoard Cloud (`thingsboard.cloud`)  
> **Architecture Pattern:** Dual-Protocol Pipeline (MQTT Gateway for Vitals + HTTPS REST for Waveforms)

---

## 1. Kiến Trúc Tổng Quan (Dual-Protocol Architecture)

Hệ thống truyền thông từ Gateway ESP32-S3 lên Cloud được phân tách làm hai luồng độc lập nhằm tối ưu hóa băng thông mạng và tránh bị khóa Rate Limit:

```
┌────────────────────────────────────────────────────────────────────────┐
│                        ESP32-S3 GATEWAY NODE                           │
│                                                                        │
│  [ Core 1: UART / Real-time DSP ]                                      │
│    - Thu thập ECG (250Hz) & PPG                                        │
│    - Giải mã Vitals (HR, SpO2, Temp, Steps)                            │
│                                                                        │
│  [ Core 0: Network & Cloud Dispatcher ]                                │
│    ├── STREAM 1: Vital Parameters ────▶ MQTT Gateway (Port 1883)       │
│    │             (15s chu kỳ hoặc batch 1s)    mqtt.thingsboard.cloud  │
│    │                                                                   │
│    └── STREAM 2: Raw Waveforms ───────▶ HTTPS POST (Port 443)          │
│                  (Batch 50 mẫu / 500ms)        /api/telemetry/ingest   │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
           ┌────────────────────────┴────────────────────────┐
           ▼                                                 ▼
┌───────────────────────────────┐     ┌──────────────────────────────────┐
│      THINGSBOARD CLOUD        │     │     NEXT.JS WEB DASHBOARD        │
│   (mqtt.thingsboard.cloud)    │     │  (wearable-server.vercel.app)    │
│                               │     │                                  │
│ - Lưu trữ dữ liệu chuỗi TG    │◀────│ - Lắng nghe WebSocket trực tiếp  │
│ - Quản lý phân cấp 10 Node    │REST │ - Hiển thị thẻ chỉ số VitalCard  │
│ - Kích hoạt Alarm Rules       │     │ - Vẽ biểu đồ sóng TrendChart     │
└───────────────────────────────┘     └──────────────────────────────────┘
```

---

## 2. Stream 1: MQTT Gateway API (Vital Parameters)

### 2.1. Thông số Kết nối Broker
- **Host:** `mqtt.thingsboard.cloud` (hoặc Mock Broker local)
- **Port:** `1883` (TCP Plain MQTT)
- **Client ID:** Bất kỳ chuỗi định danh duy nhất (ví dụ `esp32_gateway_01`)
- **Username:** `GATEWAY_ACCESS_TOKEN` (Lấy từ ThingsBoard Gateway Device Credentials)
- **Password:** *Không cần (để trống)*
- **QoS:** `0` (At most once - tiết kiệm băng thông) hoặc `1`

### 2.2. MQTT Topic
```text
v1/gateway/telemetry
```

### 2.3. Cấu trúc Payload JSON (ThingsBoard Gateway Protocol)
Payload gửi lên là một JSON Object gom nhóm theo tên thiết bị con (`Node0`, `Node1`, ... `Node9`):

```json
{
  "Node0": [
    {
      "ts": 1712839200000,
      "values": {
        "ppgHeartRate": 72.0,
        "ecgHeartRate": 72.5,
        "spo2": 98.2,
        "temperature": 36.6,
        "steps": 1250
      }
    }
  ],
  "Node1": [
    {
      "ts": 1712839200000,
      "values": {
        "ppgHeartRate": 85.0,
        "ecgHeartRate": 84.8,
        "spo2": 96.5,
        "temperature": 37.1,
        "steps": 3410
      }
    }
  ]
}
```

> [!NOTE]
> - Trường `ts` (Unix epoch timestamp in milliseconds) là tùy chọn. Nếu bỏ qua, ThingsBoard Server sẽ tự động gán thời gian nhận gói tin.
> - Khi gửi lên topic này, ThingsBoard tự động tạo quan hệ `GATEWAY_ID Manages NodeX`.

### 2.4. Danh mục Key Vitals & Ngưỡng Cảnh Báo (Alert Thresholds)
Trích xuất trực tiếp từ file cấu hình [`pages/index.js`](https://github.com/quang-nguyen-161/Wearable_Server/blob/main/pages/index.js#L30-L71):

| Key | Tên Chỉ Số | Đơn vị | Dải Bình Thường | Cảnh Báo (Warn) | Nguy Cấp (Danger Alert) |
|---|---|---|---|---|---|
| `ppgHeartRate` | Nhịp tim quang học (PPG) | `bpm` | 60 – 100 | 50 – 59 hoặc 101 – 120 | < 40 hoặc > 130 |
| `ecgHeartRate` | Nhịp tim điện tâm đồ (ECG) | `bpm` | 60 – 100 | 50 – 59 hoặc 101 – 120 | < 40 hoặc > 130 |
| `spo2` | Độ bão hòa Oxy trong máu | `%` | 95 – 100 | 90 – 94 | < 88 |
| `temperature` | Nhiệt độ bề mặt da | `°C` | 36.1 – 37.2 | 35.5 – 36.0 hoặc 37.3 – 38.5 | < 35.0 hoặc > 39.5 |
| `systolic` | Huyết áp tâm thu (tùy chọn) | `mmHg` | 90 – 120 | 121 – 139 | ≥ 140 |
| `diastolic` | Huyết áp tâm trương (tùy chọn) | `mmHg` | 60 – 80 | 81 – 89 | ≥ 90 |
| `steps` | Số bước chân trong ngày | `count` | 0 – 50000 | N/A | N/A |

---

## 3. Stream 2: HTTPS REST Ingest API (Raw ECG & PPG Waveforms)

### 3.1. Thông số Endpoint
- **URL:** `https://wearable-server.vercel.app/api/telemetry/ingest` (hoặc Local Mock Server)
- **Method:** `POST`
- **Headers:**
  ```http
  Content-Type: application/json
  x-tb-token: <THINGSBOARD_JWT_TOKEN>
  ```
  *(Nếu gửi qua Local Mock Server, header `x-tb-token` có thể bỏ qua).*

### 3.2. Cấu trúc Payload JSON Ingest
```json
{
  "deviceName": "Node0",
  "ts": 1712839200500,
  "ecg_batch": "[2048, 2055, 2120, 2450, 1980, ...]",
  "ppg_batch": "[2048, 2060, 2085, 2110, 2130, ...]"
}
```

### 3.3. Quy tắc Căn chỉnh Thời gian của Server (Timestamp Alignment)
Server tại `pages/api/telemetry/ingest.js` sẽ bóc tách chuỗi mảng JSON và nội suy mốc thời gian từng mẫu sóng theo công thức:

$$\text{sampleTs}_i = \text{batchTs} - (N - 1 - i) \times \Delta t$$

Với:
- Tần số lấy mẫu: $f_s = 250\text{ Hz}$ $\Rightarrow \Delta t = 4\text{ ms}$.
- Số mẫu mỗi mẻ (Batch size): $N = 50\text{ mẫu}$ $\Rightarrow$ Độ dài mỗi mẻ: $50 \times 4\text{ ms} = 200\text{ ms}$.

---

## 4. Cơ Chế Bảo Vệ Rate Limit & Zero Heap Allocation

### 4.1. Rate Limit Protection
- ThingsBoard Cloud giới hạn số request/giây trên mỗi account.
- **Quy tắc bắt buộc:**
  1. Không bao giờ gửi từng mẫu riêng lẻ.
  2. Gom bản tin Vitals của toàn bộ các Node đang hoạt động vào **1 gói MQTT duy nhất mỗi 1 giây** (Batching 1s).
  3. Gửi sóng ECG/PPG theo mẻ $N=50$ mẫu mỗi $500\text{ms}$ đến $1000\text{ms}$.

### 4.2. Zero Dynamic Allocation (Chuẩn C Nhúng)
Tuyệt đối không dùng `cJSON_CreateObject()` hay `malloc()` trong vòng lặp thời gian thực. Sử dụng buffer tĩnh:

```c
#define BATCH_JSON_BUFFER_SIZE  1024U
static char s_json_buffer[BATCH_JSON_BUFFER_SIZE];

/* Sử dụng snprintf nối chuỗi an toàn không phân mảnh RAM */
int len = snprintf(s_json_buffer, sizeof(s_json_buffer),
    "{\"Node%u\":[{\"values\":{\"ppgHeartRate\":%u,\"ecgHeartRate\":%u,\"spo2\":%u,\"temperature\":%.1f}}]}",
    node_id, ppg_hr, ecg_hr, spo2, temp);
```

---

## 5. Môi Trường Giả Lập Cục Bộ (Local Mock Testing)

Để kiểm thử toàn bộ quá trình đóng gói và gửi dữ liệu từ ESP32 mà **không làm bẩn dữ liệu trên server thật**, hệ thống hỗ trợ Local Mock Test:
- ESP32 gửi HTTP/MQTT tới IP của máy tính lập trình viên trên cùng mạng Wi-Fi (ví dụ `http://192.168.1.x:8080`).
- Local Mock Server hiển thị giao diện trực quan xác nhận từng gói tin nhận được theo đúng định dạng trên.
