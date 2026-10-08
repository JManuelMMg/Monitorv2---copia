import serial
import serial.tools.list_ports
import threading
import json
import time
import os
from typing import Any, Callable, Dict, Optional

class SerialHandler:
    def __init__(self, baudrate=115200, port=None):
        self.baudrate = baudrate
        self.configured_port = port or os.getenv("ESP32_SERIAL_PORT")
        self.serial_port = None
        self.running = False
        self.thread = None
        self.latest_data = {}
        self.on_data_received: Optional[Callable[[Dict[str, Any]], None]] = None
        self.lock = threading.Lock()

    def start(self):
        if self.running:
            return
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.serial_port and self.serial_port.is_open:
            try:
                self.serial_port.close()
            except:
                pass
        if self.thread and self.thread is not threading.current_thread():
            self.thread.join(timeout=2)

    def send_command(self, cmd_json):
        if self.serial_port and self.serial_port.is_open:
            try:
                cmd_str = json.dumps(cmd_json) + "\n"
                with self.lock:
                    self.serial_port.write(cmd_str.encode('utf-8'))
                print(f"[Serial] Sent command to ESP32: {cmd_str.strip()}")
                return True
            except Exception as e:
                print(f"[Serial] Error sending command: {e}")
        return False

    def _find_port(self):
        if self.configured_port and self.configured_port.strip().lower() in {"off", "none", "disabled"}:
            return None
        if self.configured_port:
            return self.configured_port

        ports = serial.tools.list_ports.comports()
        for port in ports:
            # Look for common ESP32 / USB-Serial chips
            desc = port.description.lower()
            if any(x in desc for x in ["esp32", "silicon labs", "ch340", "usb serial", "cp210", "mbed"]):
                return port.device
        return None

    def _run(self):
        retry_delay = 2
        while self.running:
            port = self._find_port()
            if not port:
                time.sleep(3)
                continue

            try:
                print(f"[Serial] Connecting to {port}...")
                self.serial_port = serial.Serial(port, self.baudrate, timeout=1)
                print(f"[Serial] Connected to {port}")
                
                self.serial_port.reset_input_buffer()
                self.serial_port.reset_output_buffer()
                retry_delay = 2

                while self.running:
                    if not self.serial_port.is_open:
                        break
                    
                    line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                    if not line:
                        continue

                    if line.startswith("{") and line.endswith("}"):
                        try:
                            data = json.loads(line)
                            if not isinstance(data, dict):
                                print(f"[Serial] Ignorando JSON que no es un objeto: {line}")
                                continue
                            if not all(key in data for key in ("temp", "hum", "ppm", "dist")):
                                print(f"[ESP32 Log]: {line}")
                                continue

                            data["timestamp"] = time.time()
                            data["source"] = "Serial"
                            
                            self.latest_data = data
                            
                            # Invoke callback
                            if self.on_data_received:
                                self.on_data_received(self.latest_data)

                        except json.JSONDecodeError:
                            print(f"[ESP32 Log]: {line}")
                    else:
                        print(f"[ESP32 Output]: {line}")

            except Exception as e:
                print(f"[Serial] Connection error on {port}: {e}")
                if self.serial_port:
                    try:
                        self.serial_port.close()
                    except:
                        pass
                self.serial_port = None
                if self.running:
                    time.sleep(retry_delay)
                    retry_delay = min(retry_delay * 2, 30)

            if self.serial_port and not self.serial_port.is_open:
                self.serial_port = None
                if self.running:
                    time.sleep(retry_delay)
                    retry_delay = min(retry_delay * 2, 30)
