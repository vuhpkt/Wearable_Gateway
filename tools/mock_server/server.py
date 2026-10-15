#!/usr/bin/env python3
"""
VitalSync Mock Test Server & Live Telemetry Dashboard
=====================================================
Receives mock telemetry and raw waveforms from ESP32 Gateway locally without
polluting the production ThingsBoard Cloud or Vercel server.

Supports BOTH:
  - MQTT Broker on Port 1883 (v1/gateway/telemetry)
  - HTTP Server on Port 8080 (Web Dashboard & REST Ingest)

Zero external dependencies - uses standard Python library only.
"""

import sys
import json
import socket
import time
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

HTTP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
MQTT_PORT = 1883

# In-memory storage for 10 nodes
s_nodes_store = {
    f"Node{i}": {
        "name": f"Node{i}",
        "online": False,
        "last_seen_ts": 0,
        "ppgHeartRate": 0,
        "ecgHeartRate": 0,
        "spo2": 0,
        "temperature": 0.0,
        "steps": 0,
    } for i in range(10)
}

s_recent_logs = []
s_stats = {
    "packets_received": 0,
    "bytes_received": 0,
    "start_time": time.time()
}
s_lock = threading.Lock()

def get_local_ip():
    """Detect local LAN IP for display so ESP32 knows where to send."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip

def process_telemetry_payload(payload, source_tag="MQTT"):
    """Parse ThingsBoard Gateway payload format: {"Node0": [{"values": {...}}]}"""
    now_str = time.strftime("%H:%M:%S")
    with s_lock:
        s_stats["packets_received"] += 1
        parsed = False

        if isinstance(payload, dict):
            # 1. ThingsBoard Gateway format: {"Node0": [{"values": {...}}]}
            for node_key, entries in payload.items():
                if node_key in s_nodes_store and isinstance(entries, list):
                    for entry in entries:
                        vals = entry.get("values", {})
                        s_nodes_store[node_key]["online"] = True
                        s_nodes_store[node_key]["last_seen_ts"] = time.time()
                        if "ppgHeartRate" in vals: s_nodes_store[node_key]["ppgHeartRate"] = vals["ppgHeartRate"]
                        if "ecgHeartRate" in vals: s_nodes_store[node_key]["ecgHeartRate"] = vals["ecgHeartRate"]
                        if "spo2" in vals: s_nodes_store[node_key]["spo2"] = vals["spo2"]
                        if "temperature" in vals: s_nodes_store[node_key]["temperature"] = vals["temperature"]
                        if "steps" in vals: s_nodes_store[node_key]["steps"] = vals["steps"]
                    parsed = True
                    log_txt = f"{node_key} updated: HR={vals.get('ppgHeartRate') or vals.get('ecgHeartRate')} bpm, SpO2={vals.get('spo2')}%, Temp={vals.get('temperature')} C"
                    s_recent_logs.append({"time": now_str, "tag": source_tag, "text": log_txt})
                    print(f"[{now_str}] [{source_tag}] {log_txt}")

            # 2. Direct format: {"deviceName": "Node0", ...}
            if not parsed and "deviceName" in payload:
                dev = payload.get("deviceName", "")
                if dev in s_nodes_store:
                    s_nodes_store[dev]["online"] = True
                    s_nodes_store[dev]["last_seen_ts"] = time.time()
                    for k in ["ppgHeartRate", "ecgHeartRate", "spo2", "temperature", "steps"]:
                        if k in payload: s_nodes_store[dev][k] = payload[k]
                    log_txt = f"{dev} updated: HR={payload.get('ppgHeartRate')}, SpO2={payload.get('spo2')}%"
                    s_recent_logs.append({"time": now_str, "tag": source_tag, "text": log_txt})
                    print(f"[{now_str}] [{source_tag}] {log_txt}")
                    parsed = True

        if not parsed:
            s_recent_logs.append({"time": now_str, "tag": source_tag, "text": str(payload)[:100]})
            print(f"[{now_str}] [{source_tag}] {str(payload)[:100]}")

DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>VitalSync — Local Mock Gateway Dashboard</title>
  <style>
    :root {
      --bg: #0b0f19;
      --card-bg: #151d30;
      --card-border: #23314d;
      --text-main: #f1f5f9;
      --text-muted: #94a3b8;
      --cyan: #06b6d4;
      --green: #10b981;
      --amber: #f59e0b;
      --red: #ef4444;
      --purple: #8b5cf6;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: var(--text-main); padding: 24px; min-height: 100vh; }
    
    header {
      display: flex; justify-content: space-between; align-items: center;
      padding-bottom: 20px; border-bottom: 1px solid var(--card-border); margin-bottom: 24px;
    }
    .brand { display: flex; align-items: center; gap: 12px; }
    .badge-mock { background: rgba(16, 185, 129, 0.15); color: var(--green); border: 1px solid var(--green); padding: 4px 10px; border-radius: 999px; font-size: 12px; font-weight: 700; }
    .stats-bar { display: flex; gap: 20px; font-size: 14px; color: var(--text-muted); }
    .stat-val { color: var(--cyan); font-weight: 700; }

    .node-grid {
      display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 20px; margin-bottom: 30px;
    }
    .node-card {
      background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 14px;
      padding: 18px; transition: all 0.2s ease; position: relative; overflow: hidden;
    }
    .node-card.active { border-color: var(--cyan); box-shadow: 0 0 20px rgba(6, 182, 212, 0.15); }
    .card-head { display: flex; justify-content: space-between; align-items: center; margin-bottom: 14px; }
    .node-title { font-size: 17px; font-weight: 700; }
    .status-dot { width: 10px; height: 10px; border-radius: 50%; background: #475569; display: inline-block; }
    .status-dot.online { background: var(--green); box-shadow: 0 0 8px var(--green); }

    .vitals-row { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 12px; }
    .vital-box { background: rgba(11, 15, 25, 0.6); padding: 10px; border-radius: 8px; border: 1px solid rgba(255,255,255,0.05); }
    .vital-lbl { font-size: 11px; text-transform: uppercase; color: var(--text-muted); letter-spacing: 0.5px; }
    .vital-val { font-size: 20px; font-weight: 700; margin-top: 4px; }
    .val-hr { color: var(--cyan); }
    .val-spo2 { color: var(--green); }
    .val-temp { color: var(--amber); }
    .val-steps { color: var(--purple); }

    .console-box {
      background: #060911; border: 1px solid var(--card-border); border-radius: 12px;
      padding: 16px; font-family: monospace; font-size: 13px; max-height: 260px; overflow-y: auto;
    }
    .log-line { margin-bottom: 6px; line-height: 1.5; color: #cbd5e1; }
    .log-time { color: #64748b; }
    .log-tag { color: var(--cyan); font-weight: 700; }
  </style>
</head>
<body>

  <header>
    <div class="brand">
      <h2>🏥 VitalSync Gateway Mock Server</h2>
      <span class="badge-mock">MQTT :1883 + HTTP :8080</span>
    </div>
    <div class="stats-bar">
      <div>Packets: <span id="stat-pkts" class="stat-val">0</span></div>
      <div>Active Nodes: <span id="stat-nodes" class="stat-val">0/10</span></div>
    </div>
  </header>

  <h3 style="margin-bottom: 16px; color: var(--text-muted); font-size: 14px; text-transform: uppercase; letter-spacing: 1px;">Live Nodes Vitals Monitor (10 Wearable Nodes)</h3>
  <div class="node-grid" id="grid"></div>

  <h3 style="margin-bottom: 12px; color: var(--text-muted); font-size: 14px; text-transform: uppercase; letter-spacing: 1px;">Incoming API Feed (ThingsBoard Gateway Protocol)</h3>
  <div class="console-box" id="console"></div>

  <script>
    async function refresh() {
      try {
        const res = await fetch('/api/live_data');
        const data = await res.json();
        
        document.getElementById('stat-pkts').innerText = data.stats.packets_received;
        
        let onlineCount = 0;
        const grid = document.getElementById('grid');
        grid.innerHTML = '';
        
        for (const [k, node] of Object.entries(data.nodes)) {
          if (node.online) onlineCount++;
          const card = document.createElement('div');
          card.className = 'node-card' + (node.online ? ' active' : '');
          card.innerHTML = `
            <div class="card-head">
              <span class="node-title">${node.name}</span>
              <span class="status-dot ${node.online ? 'online' : ''}"></span>
            </div>
            <div class="vitals-row">
              <div class="vital-box">
                <div class="vital-lbl">Heart Rate</div>
                <div class="vital-val val-hr">${node.ppgHeartRate || node.ecgHeartRate || '--'} <small style="font-size:12px">bpm</small></div>
              </div>
              <div class="vital-box">
                <div class="vital-lbl">SpO₂</div>
                <div class="vital-val val-spo2">${node.spo2 || '--'} <small style="font-size:12px">%</small></div>
              </div>
            </div>
            <div class="vitals-row">
              <div class="vital-box">
                <div class="vital-lbl">Skin Temp</div>
                <div class="vital-val val-temp">${node.temperature ? Number(node.temperature).toFixed(1) : '--'} <small style="font-size:12px">°C</small></div>
              </div>
              <div class="vital-box">
                <div class="vital-lbl">Daily Steps</div>
                <div class="vital-val val-steps">${node.steps || 0}</div>
              </div>
            </div>
          `;
          grid.appendChild(card);
        }
        document.getElementById('stat-nodes').innerText = onlineCount + '/10';

        const con = document.getElementById('console');
        con.innerHTML = data.logs.map(l => `<div class="log-line"><span class="log-time">[${l.time}]</span> <span class="log-tag">${l.tag}</span>: ${l.text}</div>`).join('');
      } catch (e) {
        console.error("Refresh failed:", e);
      }
    }

    setInterval(refresh, 500);
    refresh();
  </script>
</body>
</html>
"""

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True

class MockServerHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        return

    def _send_json(self, status_code, data):
        body = json.dumps(data).encode('utf-8')
        self.send_response(status_code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ('/', '/index.html'):
            body = DASHBOARD_HTML.encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif self.path == '/api/live_data':
            now = time.time()
            with s_lock:
                for n in s_nodes_store.values():
                    if n["last_seen_ts"] > 0 and (now - n["last_seen_ts"]) > 15:
                        n["online"] = False

                self._send_json(200, {
                    "nodes": s_nodes_store,
                    "stats": s_stats,
                    "logs": s_recent_logs[-15:]
                })
        else:
            self._send_json(404, {"error": "Not Found"})

    def do_POST(self):
        content_len = int(self.headers.get('Content-Length', 0))
        post_data = self.rfile.read(content_len).decode('utf-8')
        try:
            payload = json.loads(post_data)
            process_telemetry_payload(payload, "HTTP_POST")
            self._send_json(200, {"status": "ok"})
        except Exception:
            self._send_json(400, {"error": "Invalid JSON format"})

# ====================================================================
# Embedded Lightweight MQTT Broker Thread (Port 1883)
# Implements MQTT 3.1.1 CONNECT, PUBLISH, PINGREQ, DISCONNECT
# ====================================================================
def decode_remaining_length(sock):
    multiplier = 1
    value = 0
    while True:
        b = sock.recv(1)
        if not b: return None
        encoded_byte = b[0]
        value += (encoded_byte & 127) * multiplier
        if (encoded_byte & 128) == 0:
            break
        multiplier *= 128
    return value

def handle_mqtt_client(client_sock, addr):
    client_sock.settimeout(60.0)
    try:
        while True:
            header_b = client_sock.recv(1)
            if not header_b: break
            pkt_type = header_b[0] >> 4
            qos = (header_b[0] >> 1) & 0x03

            rem_len = decode_remaining_length(client_sock)
            if rem_len is None: break

            raw_body = b""
            while len(raw_body) < rem_len:
                chunk = client_sock.recv(rem_len - len(raw_body))
                if not chunk: break
                raw_body += chunk

            if pkt_type == 1:  # CONNECT
                # Respond with CONNACK (Connection Accepted)
                connack = bytes([0x20, 0x02, 0x00, 0x00])
                client_sock.sendall(connack)
                print(f"[{time.strftime('%H:%M:%S')}] [MQTT] ESP32 Gateway connected from {addr[0]}:{addr[1]} ✓")

            elif pkt_type == 3:  # PUBLISH
                if len(raw_body) >= 2:
                    topic_len = (raw_body[0] << 8) | raw_body[1]
                    topic = raw_body[2:2 + topic_len].decode('utf-8', errors='ignore')
                    offset = 2 + topic_len
                    packet_id = 0
                    if qos > 0:
                        packet_id = (raw_body[offset] << 8) | raw_body[offset + 1]
                        offset += 2
                    payload_str = raw_body[offset:].decode('utf-8', errors='ignore')

                    try:
                        data = json.loads(payload_str)
                        process_telemetry_payload(data, f"MQTT:{topic}")
                    except Exception:
                        pass

                    if qos == 1:
                        # Send PUBACK
                        puback = bytes([0x40, 0x02, (packet_id >> 8) & 0xFF, packet_id & 0xFF])
                        client_sock.sendall(puback)

            elif pkt_type == 12:  # PINGREQ
                client_sock.sendall(bytes([0xD0, 0x00]))  # PINGRESP

            elif pkt_type == 14:  # DISCONNECT
                break

    except Exception:
        pass
    finally:
        client_sock.close()

def start_mqtt_broker():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind(('0.0.0.0', MQTT_PORT))
        server.listen(5)
        while True:
            client_sock, addr = server.accept()
            t = threading.Thread(target=handle_mqtt_client, args=(client_sock, addr), daemon=True)
            t.start()
    except Exception as e:
        print(f"[MQTT] Error starting broker on port {MQTT_PORT}: {e}")

def run_server():
    lan_ip = get_local_ip()

    # Start MQTT Broker in background thread
    mqtt_thread = threading.Thread(target=start_mqtt_broker, daemon=True)
    mqtt_thread.start()

    http_server = ThreadedHTTPServer(('0.0.0.0', HTTP_PORT), MockServerHandler)

    print("=" * 72)
    print("   [VitalSync] LOCAL MOCK TEST SERVER & DASHBOARD")
    print("=" * 72)
    print(f"   * Web Dashboard Browser URL : http://localhost:{HTTP_PORT}")
    print(f"   * MQTT Broker Endpoint      : mqtt://{lan_ip}:{MQTT_PORT}")
    print(f"   * Target Topic              : v1/gateway/telemetry")
    print("   * Safety Guarantee          : 100% Offline LAN — Zero Cloud Spam!")
    print("=" * 72)
    print("   Waiting for ESP32-S3 MQTT telemetry... (Press Ctrl+C to stop)\n")

    try:
        http_server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping Mock Server...")
        http_server.server_close()

if __name__ == '__main__':
    run_server()
