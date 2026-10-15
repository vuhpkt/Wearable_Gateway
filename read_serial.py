#!/usr/bin/env python3
"""
Wearable Gateway - Persistent Serial Monitor Tool
Tu dong ket noi va lien tuc theo doi cong COM cua nRF52840 / ESP32.
Tu dong ket noi lai khi chip Reset hoac rut ra cam lai.
"""

import sys
import time
import serial
import serial.tools.list_ports
from datetime import datetime

DEFAULT_BAUD = 115200

def find_default_port():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        return None

    # Uu tien tim cong co chu 'USB Serial' hoac 'CDC' hoac 'nRF'
    for p in ports:
        desc = (p.description or "").lower()
        if "usb serial" in desc or "cdc" in desc or "nordic" in desc:
            return p.device

    return ports[-1].device

def list_all_ports():
    ports = list(serial.tools.list_ports.comports())
    print("Danh sach cac cong COM hien co tren may:")
    for idx, p in enumerate(ports):
        print(f"  [{idx + 1}] {p.device}: {p.description}")
    return ports

def is_port_available(port_name):
    return any(p.device.upper() == port_name.upper() for p in serial.tools.list_ports.comports())

def main():
    print("==================================================")
    print("      Wearable Gateway - Persistent Monitor       ")
    print(" (Tu dong ket noi lai khi chip Reset / Rut cam)   ")
    print("==================================================")

    # 1. Chon cong COM
    target_port = None
    if len(sys.argv) > 1:
        target_port = sys.argv[1].upper()
    else:
        auto_port = find_default_port()
        ports = list_all_ports()
        print()
        if auto_port:
            choice = input(f"Tu dong phat hien cong [{auto_port}]. Nhan Enter de dung, hoac nhap so/ten cong: ").strip()
            if not choice:
                target_port = auto_port
            elif choice.isdigit() and 1 <= int(choice) <= len(ports):
                target_port = ports[int(choice) - 1].device
            else:
                target_port = choice.upper()
        else:
            if not ports:
                print("[!] Khong tim thay cong COM nao. Vui long cam kit vao may tinh!")
                return
            choice = input("Chon so thu tu hoac nhap ten cong (vi du COM11): ").strip()
            if choice.isdigit() and 1 <= int(choice) <= len(ports):
                target_port = ports[int(choice) - 1].device
            else:
                target_port = choice.upper()

    print(f"\n[+] Da chon theo doi cong: {target_port} (Baudrate: {DEFAULT_BAUD})")
    print("[+] Nhan Ctrl+C bat ky luc nao de dung script.\n")

    # 2. Vong lap giam sat lien tuc (Persistent Monitor Loop)
    while True:
        ser = None
        try:
            # Cho den khi cong COM xuat hien tren he thong
            waiting_notified = False
            while not is_port_available(target_port):
                if not waiting_notified:
                    print(f"[*] Dang doi cong {target_port} xuat hien...", end="", flush=True)
                    waiting_notified = True
                print(".", end="", flush=True)
                time.sleep(0.5)

            if waiting_notified:
                print(f"\n[+] Phat hien {target_port}! Dang mo ket noi...")
                time.sleep(0.4)  # Cho driver CDC ACM Windows on dinh

            # Thu mo cong Serial
            ser = serial.Serial(
                port=target_port,
                baudrate=DEFAULT_BAUD,
                timeout=0.5,
                rtscts=False,
                dsrdtr=False
            )
            ser.dtr = True
            ser.rts = True
            ser.reset_input_buffer()

            print(f"[+] Ket noi thanh cong toi {target_port}! Dang doc log...")
            print("--------------------------------------------------------------------")

            # Vong lap doc log tu cong
            while True:
                line = ser.readline()
                if line:
                    try:
                        decoded = line.decode('utf-8', errors='replace').rstrip('\r\n')
                    except Exception:
                        decoded = str(line)

                    now = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    print(f"[{now}] {decoded}")

        except (serial.SerialException, OSError, PermissionError) as e:
            now = datetime.now().strftime("%H:%M:%S")
            print(f"\n[{now}] [!] Mat ket noi {target_port} (Chip dang Reset hoac ngat ket noi). Dang cho ket noi lai...")
            if ser and ser.is_open:
                try:
                    ser.close()
                except Exception:
                    pass
            time.sleep(0.8)

        except KeyboardInterrupt:
            print("\n--------------------------------------------------------------------")
            print("[+] Da ngat ket noi Serial. Tam biet!")
            if ser and ser.is_open:
                ser.close()
            break

if __name__ == "__main__":
    main()
