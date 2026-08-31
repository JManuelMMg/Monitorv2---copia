import serial
import serial.tools.list_ports
import threading
import json
import time
from database import save_reading

class SerialHandler:
    def __init__(self, baudrate=115200, db_save_interval=5):
        self.baudrate = baudrate
        self.db_save_interval = db_save_interval
        self.serial_port = None
        self.running = False
        self.thread = None
        self.last_db_save = 0
        self.latest_data = {}
        self.on_data_received = None  # Callback to broadcast WebSocket data
        self.lock = threading.Lock()

    def start(self):
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
        ports = serial.tools.list_ports.comports()
        for port in ports:
            # Look for common ESP32 / USB-Serial chips
            desc = port.description.lower()
            if any(x in desc for x in ["silicon labs", "ch340", "usb serial", "cp210", "mbed"]):
                return port.device
        # Fallback to the first available COM port if any exists
        if ports:
            return ports[0].device
        return None

    def _run(self):
        while self.running:
            port = self._find_port()
            if not port:
                # Silently wait, avoid flooding logs
                time.sleep(3)
                continue

            try:
                print(f"[Serial] Connecting to {port}...")
                self.serial_port = serial.Serial(port, self.baudrate, timeout=1)
                print(f"[Serial] Connected to {port}")
                
                self.serial_port.reset_input_buffer()
                self.serial_port.reset_output_buffer()

                while self.running:
                    if not self.serial_port.is_open:
                        break
                    
                    line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                    if not line:
                        continue

                    if line.startswith("{") and line.endswith("}"):
                        try:
                            data = json.loads(line)
                            if not all(key in data for key in ("temp", "hum", "ppm", "dist")):
                                print(f"[ESP32 Log]: {line}")
                                continue

                            data["timestamp"] = time.time()
                            data["source"] = "Serial"
                            
                            self.latest_data = data
                            
                            # Invoke callback
                            if self.on_data_received:
                                self.on_data_received(self.latest_data)

                            # Save every valid reading so PostgreSQL matches the dashboard stream.
                            save_reading(
                                temp=data.get("temp", 0.0),
                                hum=data.get("hum", 0.0),
                                ppm=data.get("ppm", 0.0),
                                co2_ppm=data.get("co2_ppm"),
                                dist=data.get("dist", 0.0),
                                mq_r0=data.get("mq_r0", 0.0),
                                uptime=data.get("uptime", 0),
                                wifi=data.get("wifi", False),
                                rssi=data.get("rssi", 0)
                            )
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
                time.sleep(3)
