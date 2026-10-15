# VitalSync Local Mock Test Server

A zero-dependency local mock test server and real-time web dashboard to test ESP32 telemetry transmission without sending junk data to production ThingsBoard Cloud or Vercel.

## Quick Start

1. Start the mock server:
   ```bash
   python gateway/tools/mock_server/server.py 8080
   ```
2. Open your web browser:
   - Dashboard: `http://localhost:8080`
3. Configure your ESP32-S3 firmware to send telemetry to:
   - `http://<YOUR_LAN_IP>:8080/api/v1/gateway/telemetry`

## Features

- **Live Grid:** Displays 10 Node cards (`Node0` .. `Node9`) with real-time HR, SpO2, Skin Temp, and Steps.
- **ThingsBoard Gateway Protocol:** Parses standard ThingsBoard Gateway JSON payload:
  ```json
  {
    "Node0": [{"values": {"ppgHeartRate": 72, "ecgHeartRate": 72, "spo2": 98, "temperature": 36.6}}]
  }
  ```
- **Terminal Console:** Shows raw payloads as they arrive from the ESP32.
- **Safety:** 100% isolated to your local machine. No external cloud credentials or internet connection required.
