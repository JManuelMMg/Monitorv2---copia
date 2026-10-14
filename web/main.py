from fastapi import BackgroundTasks, FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from pathlib import Path
import json
import asyncio
import time
import io
import csv
import math
import threading
import os
import hmac
from collections import defaultdict, deque
from typing import Deque, List, Dict, Any

from database import init_db, save_reading, get_history, get_latest_reading
from serial_handler import SerialHandler

app = FastAPI(title="Industrial IoT Multi-Sensor Monitor")
BASE_DIR = Path(__file__).resolve().parent

# Mount static files and templates
app.mount("/static", StaticFiles(directory=BASE_DIR / "static"), name="static")
templates = Jinja2Templates(directory=BASE_DIR / "templates")

# Global states
latest_sensor_data = {
    "temp": 20.0,
    "hum": 50.0,
    "ppm": 0.0,
    "co2_ppm": None,
    "dist": 100.0,
    "mq_ready": False,
    "mq_r0": 15000.0,
    "wifi": False,
    "rssi": 0,
    "uptime": 0,
    "timestamp": 0,
    "source": "Ninguno",
    "device_id": None
}
connected_clients: List[WebSocket] = []
# Las colas por dispositivo evitan que un ESP consuma comandos destinados a
# otro. La clave "legacy" conserva compatibilidad con sketches anteriores.
wifi_command_queues: Dict[str, Deque[Dict[str, Any]]] = defaultdict(deque)
command_lock = threading.Lock()
MAX_COMMANDS_PER_DEVICE = 50
MAX_TELEMETRY_BYTES = 8 * 1024
ESP32_API_TOKEN = os.getenv("ESP32_API_TOKEN", "")
global_loop = None
data_lock = threading.Lock()
persistence_lock = threading.Lock()
PERSIST_INTERVAL_SECONDS = 1
last_persistence_at = {}
recent_readings = {}
device_status: Dict[str, Dict[str, Any]] = {}

# Instantiate Serial Handler
serial_handler = SerialHandler()


def _valid_reading(data: Any) -> bool:
    if not isinstance(data, dict):
        return False
    for key in ("temp", "hum", "ppm", "dist", "co2_ppm", "mq_r0", "uptime", "rssi"):
        if key not in data or data[key] is None:
            continue
        value = data[key]
        if isinstance(value, bool):
            return False
        try:
            if not math.isfinite(float(value)):
                return False
        except (TypeError, ValueError):
            return False
    if "wifi" in data and not isinstance(data["wifi"], bool):
        return False
    if not all(key in data for key in ("temp", "hum", "ppm", "dist")):
        return False
    # device_id is optional for backward compatibility
    if "device_id" in data and data["device_id"] is not None:
        if not isinstance(data["device_id"], str) or len(data["device_id"]) > 17:
            return False
    return True


def _ready_for_persistence(data: Dict[str, Any]) -> bool:
    readiness_flags = (
        "mq_ready",
        "mq_calibrated",
        "mq_valid",
        "co2_ready",
        "co2_valid",
    )
    return all(data.get(flag) is True for flag in readiness_flags)


def _queue_key(device_id: Any) -> str:
    return device_id if isinstance(device_id, str) and device_id else "legacy"


def _record_device_status(data: Dict[str, Any], source: str) -> None:
    device_id = _queue_key(data.get("device_id"))
    device_status[device_id] = {
        "device_id": data.get("device_id"),
        "source": source,
        "last_seen": data.get("timestamp", time.time()),
        "ip": data.get("ip"),
        "rssi": data.get("rssi"),
        "wifi": data.get("wifi"),
        "wifi_tx_ok": data.get("wifi_tx_ok", 0),
        "wifi_tx_failed": data.get("wifi_tx_failed", 0),
        "wifi_tx_dropped": data.get("wifi_tx_dropped", 0),
        "wifi_queue_depth": data.get("wifi_queue_depth", 0),
    }


def _take_wifi_commands(device_id: Any) -> List[Dict[str, Any]]:
    """Devuelve solo los comandos pendientes para el ESP que hizo el POST."""
    key = _queue_key(device_id)
    with command_lock:
        commands = list(wifi_command_queues[key])
        wifi_command_queues[key].clear()
    return commands


def persist_reading_if_due(data: Dict[str, Any]) -> None:
    if not _ready_for_persistence(data):
        return

    global last_persistence_at
    now = time.monotonic()
    signature = tuple(data.get(key) for key in (
        "device_id", "uptime", "temp", "hum", "ppm", "co2_ppm", "dist"
    ))

    with persistence_lock:
        expired = [key for key, seen_at in recent_readings.items() if now - seen_at > 10]
        for key in expired:
            del recent_readings[key]
        expired_devices = [
            device_id for device_id, saved_at in last_persistence_at.items()
            if now - saved_at > 10
        ]
        for device_id in expired_devices:
            del last_persistence_at[device_id]
        if signature in recent_readings:
            return
        recent_readings[signature] = now
        device_id = data.get("device_id") or "legacy"
        if now - last_persistence_at.get(device_id, 0.0) < PERSIST_INTERVAL_SECONDS:
            return
        last_persistence_at[device_id] = now

    save_reading(
        temp=data.get("temp", 20.0),
        hum=data.get("hum", 50.0),
        ppm=data.get("ppm", 0.0),
        co2_ppm=data.get("co2_ppm"),
        dist=data.get("dist", 100.0),
        mq_r0=data.get("mq_r0", 15000.0),
        uptime=data.get("uptime", 0),
        wifi=bool(data.get("wifi", False)),
        rssi=data.get("rssi", 0),
        device_id=data.get("device_id")
    )

def handle_incoming_data(data: Dict[str, Any]):
    global global_loop
    if not _valid_reading(data):
        return
    with data_lock:
        latest_sensor_data.update(data)
        _record_device_status(data, "Serial")
        snapshot = dict(latest_sensor_data)
    # Broadcast to all connected web clients in a thread-safe way
    if global_loop:
        asyncio.run_coroutine_threadsafe(broadcast_to_clients(snapshot), global_loop)
        asyncio.run_coroutine_threadsafe(asyncio.to_thread(persist_reading_if_due, data), global_loop)

async def broadcast_to_clients(data: Dict[str, Any]):
    if not connected_clients:
        return
    message = json.dumps(data)
    clients = list(connected_clients)
    results = await asyncio.gather(
        *(asyncio.wait_for(client.send_text(message), timeout=0.5) for client in clients),
        return_exceptions=True
    )
    for client, result in zip(clients, results):
        if isinstance(result, BaseException) and client in connected_clients:
            connected_clients.remove(client)

@app.on_event("startup")
async def startup_event():
    global global_loop
    global_loop = asyncio.get_running_loop()
    init_db()
    
    # Bind serial handler callback and start
    serial_handler.on_data_received = handle_incoming_data
    serial_handler.start()

@app.on_event("shutdown")
async def shutdown_event():
    global global_loop
    serial_handler.stop()
    global_loop = None

# HTTP POST Endpoint for WiFi ESP32
@app.post("/api/readings")
async def receive_readings(request: Request, background_tasks: BackgroundTasks):
    global latest_sensor_data
    if ESP32_API_TOKEN:
        supplied_token = request.headers.get("X-API-Key", "")
        if not hmac.compare_digest(supplied_token, ESP32_API_TOKEN):
            raise HTTPException(status_code=401, detail="Invalid ESP32 API token")

    try:
        content_length = int(request.headers.get("content-length", "0"))
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid Content-Length")
    if content_length > MAX_TELEMETRY_BYTES:
        raise HTTPException(status_code=413, detail="Telemetry payload too large")

    try:
        raw_body = await request.body()
        if len(raw_body) > MAX_TELEMETRY_BYTES:
            raise HTTPException(status_code=413, detail="Telemetry payload too large")
        data = json.loads(raw_body)
    except HTTPException:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise HTTPException(status_code=400, detail="Invalid JSON")

    if not _valid_reading(data):
        raise HTTPException(status_code=422, detail="JSON does not contain a complete sensor reading")

    header_device_id = request.headers.get("X-Device-ID")
    if header_device_id and data.get("device_id") and header_device_id != data["device_id"]:
        raise HTTPException(status_code=400, detail="Device ID header does not match payload")
    
    data["timestamp"] = time.time()
    data["source"] = "WiFi"
    
    with data_lock:
        latest_sensor_data.update(data)
        _record_device_status(data, "WiFi")
        snapshot = dict(latest_sensor_data)
    
    # Broadcast to all web browsers
    await broadcast_to_clients(snapshot)
    background_tasks.add_task(persist_reading_if_due, data)

    commands = _take_wifi_commands(data.get("device_id"))
    
    return {"status": "ok", "commands": commands, "server_time": time.time()}

@app.get("/", response_class=HTMLResponse)
async def read_root(request: Request):
    return templates.TemplateResponse(request=request, name="index.html")

@app.get("/api/history")
async def get_sensor_history(hours: int = 24):
    history = await asyncio.to_thread(get_history, hours=hours)
    return {"history": history}

@app.get("/api/latest")
async def get_latest_sensor_reading():
    with data_lock:
        snapshot = dict(latest_sensor_data)
    if snapshot.get("timestamp"):
        return snapshot
    latest_from_db = await asyncio.to_thread(get_latest_reading)
    if latest_from_db:
        return latest_from_db
    return latest_sensor_data


@app.get("/api/devices")
async def get_devices():
    """Estado de entrega para diagnóstico y operación con varios ESP32."""
    with data_lock:
        return {"devices": list(device_status.values())}

@app.get("/api/export")
async def export_csv(hours: int = 24):
    history = await asyncio.to_thread(get_history, hours=hours)
    
    output = io.StringIO()
    writer = csv.writer(output)
    
    # Header
    writer.writerow([
        "Timestamp (UTC)", "Temperatura (C)", "Humedad (%)", 
        "Gas Metano (PPM)", "CO2 (PPM)", "Distancia (cm)", "MQ4 R0", 
        "Uptime (s)", "WiFi Conectado", "Senal RSSI", "Device ID"
    ])
    
    for row in history:
        writer.writerow([
            row["timestamp"],
            row["temp"],
            row["hum"],
            row["ppm"],
            row.get("co2_ppm"),
            row["dist"],
            row["mq_r0"],
            row["uptime"],
            "Si" if row["wifi"] else "No",
            row["rssi"],
            row.get("device_id", "")
        ])
        
    output.seek(0)
    return StreamingResponse(
        io.BytesIO(output.getvalue().encode("utf-8")),
        media_type="text/csv",
        headers={"Content-Disposition": f"attachment; filename=sensor_history_{hours}h.csv"}
    )

@app.post("/api/command")
async def send_command(command: Dict[str, Any]):
    command = dict(command)
    target_device_id = command.pop("device_id", None)
    if target_device_id is not None and (
        not isinstance(target_device_id, str) or len(target_device_id) > 17
    ):
        raise HTTPException(status_code=422, detail="Invalid device_id")
    if not isinstance(command.get("cmd"), str) or not command["cmd"]:
        raise HTTPException(status_code=422, detail="A non-empty cmd is required")

    # Try sending via Serial
    serial_sent = serial_handler.send_command(command)
    
    if not serial_sent:
        # La UI añade device_id. Para clientes antiguos se dirige al último
        # ESP visto, en vez de entregar el comando al primer ESP que consulte.
        if not target_device_id:
            with data_lock:
                target_device_id = latest_sensor_data.get("device_id")
        key = _queue_key(target_device_id)
        with command_lock:
            queue = wifi_command_queues[key]
            if len(queue) >= MAX_COMMANDS_PER_DEVICE:
                queue.popleft()
            queue.append(command)
    
    return {
        "status": "queued",
        "serial_sent": serial_sent,
        "wifi_queued": not serial_sent,
        "device_id": target_device_id if not serial_sent else None,
    }

# Browser Client WebSocket Connection
@app.websocket("/ws/client")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    connected_clients.append(websocket)
    try:
        # Send current reading immediately
        with data_lock:
            snapshot = dict(latest_sensor_data)
        await websocket.send_text(json.dumps(snapshot))
        while True:
            # Keep open and wait for incoming messages (ping/pong)
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        if websocket in connected_clients:
            connected_clients.remove(websocket)
