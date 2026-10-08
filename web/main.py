from fastapi import BackgroundTasks, FastAPI, Request, WebSocket, WebSocketDisconnect
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
from typing import List, Dict, Any

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
    "source": "Ninguno"
}
connected_clients: List[WebSocket] = []
wifi_command_queue = []
global_loop = None
data_lock = threading.Lock()
persistence_lock = threading.Lock()
PERSIST_INTERVAL_SECONDS = 1
last_persistence_at = 0.0
recent_readings = {}

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
    return True


def persist_reading_if_due(data: Dict[str, Any]) -> None:
    global last_persistence_at
    now = time.monotonic()
    signature = tuple(data.get(key) for key in (
        "uptime", "temp", "hum", "ppm", "co2_ppm", "dist"
    ))

    with persistence_lock:
        expired = [key for key, seen_at in recent_readings.items() if now - seen_at > 10]
        for key in expired:
            del recent_readings[key]
        if signature in recent_readings:
            return
        recent_readings[signature] = now
        if now - last_persistence_at < PERSIST_INTERVAL_SECONDS:
            return
        last_persistence_at = now

    save_reading(
        temp=data.get("temp", 20.0),
        hum=data.get("hum", 50.0),
        ppm=data.get("ppm", 0.0),
        co2_ppm=data.get("co2_ppm"),
        dist=data.get("dist", 100.0),
        mq_r0=data.get("mq_r0", 15000.0),
        uptime=data.get("uptime", 0),
        wifi=bool(data.get("wifi", False)),
        rssi=data.get("rssi", 0)
    )

def handle_incoming_data(data: Dict[str, Any]):
    global global_loop
    if not _valid_reading(data):
        return
    with data_lock:
        latest_sensor_data.update(data)
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
    try:
        data = await request.json()
    except Exception:
        return {"status": "error", "message": "Invalid JSON"}

    if not _valid_reading(data):
        return {"status": "ignored", "message": "JSON does not contain a complete sensor reading"}
    
    data["timestamp"] = time.time()
    data["source"] = "WiFi"
    
    with data_lock:
        latest_sensor_data.update(data)
        snapshot = dict(latest_sensor_data)
    
    # Broadcast to all web browsers
    await broadcast_to_clients(snapshot)
    background_tasks.add_task(persist_reading_if_due, data)

    # Fetch and pop commands for ESP32
    commands = list(wifi_command_queue)
    wifi_command_queue.clear()
    
    return {"status": "ok", "commands": commands}

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

@app.get("/api/export")
async def export_csv(hours: int = 24):
    history = await asyncio.to_thread(get_history, hours=hours)
    
    output = io.StringIO()
    writer = csv.writer(output)
    
    # Header
    writer.writerow([
        "Timestamp (UTC)", "Temperatura (C)", "Humedad (%)", 
        "Gas Metano (PPM)", "CO2 (PPM)", "Distancia (cm)", "MQ4 R0", 
        "Uptime (s)", "WiFi Conectado", "Senal RSSI"
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
            row["rssi"]
        ])
        
    output.seek(0)
    return StreamingResponse(
        io.BytesIO(output.getvalue().encode("utf-8")),
        media_type="text/csv",
        headers={"Content-Disposition": f"attachment; filename=sensor_history_{hours}h.csv"}
    )

@app.post("/api/command")
async def send_command(command: Dict[str, Any]):
    # Try sending via Serial
    serial_sent = serial_handler.send_command(command)
    
    if not serial_sent:
        wifi_command_queue.append(command)
    
    return {
        "status": "queued",
        "serial_sent": serial_sent,
        "wifi_queued": not serial_sent
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
