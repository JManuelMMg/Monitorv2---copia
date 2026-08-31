from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from pathlib import Path
import json
import asyncio
import time
import io
import csv
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
DB_SAVE_INTERVAL = 0  # 0 = guardar cada medicion recibida
global_loop = None

# Instantiate Serial Handler
serial_handler = SerialHandler(db_save_interval=DB_SAVE_INTERVAL)

def handle_incoming_data(data: Dict[str, Any]):
    global latest_sensor_data, global_loop
    latest_sensor_data.update(data)
    # Broadcast to all connected web clients in a thread-safe way
    if global_loop:
        asyncio.run_coroutine_threadsafe(broadcast_to_clients(latest_sensor_data), global_loop)

async def broadcast_to_clients(data: Dict[str, Any]):
    if not connected_clients:
        return
    message = json.dumps(data)
    # Broadcast to all clients
    tasks = [client.send_text(message) for client in connected_clients]
    await asyncio.gather(*tasks, return_exceptions=True)

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
    serial_handler.stop()

# HTTP POST Endpoint for WiFi ESP32
@app.post("/api/readings")
async def receive_readings(request: Request):
    global latest_sensor_data
    try:
        data = await request.json()
    except Exception:
        return {"status": "error", "message": "Invalid JSON"}

    if not all(key in data for key in ("temp", "hum", "ppm", "dist")):
        return {"status": "ignored", "message": "JSON does not contain a complete sensor reading"}
    
    data["timestamp"] = time.time()
    data["source"] = "WiFi"
    
    latest_sensor_data.update(data)
    
    # Broadcast to all web browsers
    await broadcast_to_clients(latest_sensor_data)
    
    # Save every received reading to PostgreSQL.
    save_reading(
        temp=data.get("temp", 20.0),
        hum=data.get("hum", 50.0),
        ppm=data.get("ppm", 0.0),
        co2_ppm=data.get("co2_ppm"),
        dist=data.get("dist", 100.0),
        mq_r0=data.get("mq_r0", 15000.0),
        uptime=data.get("uptime", 0),
        wifi=True,
        rssi=data.get("rssi", 0)
    )

    # Fetch and pop commands for ESP32
    commands = list(wifi_command_queue)
    wifi_command_queue.clear()
    
    return {"status": "ok", "commands": commands}

@app.get("/", response_class=HTMLResponse)
async def read_root(request: Request):
    return templates.TemplateResponse(request=request, name="index.html")

@app.get("/api/history")
async def get_sensor_history(hours: int = 24):
    history = get_history(hours=hours)
    return {"history": history}

@app.get("/api/latest")
async def get_latest_sensor_reading():
    if latest_sensor_data.get("timestamp"):
        return latest_sensor_data
    latest_from_db = get_latest_reading()
    if latest_from_db:
        return latest_from_db
    return latest_sensor_data

@app.get("/api/export")
async def export_csv(hours: int = 24):
    history = get_history(hours=hours)
    
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
    
    # Queue for WiFi POST response
    wifi_command_queue.append(command)
    
    return {
        "status": "queued",
        "serial_sent": serial_sent,
        "wifi_queued": True
    }

# Browser Client WebSocket Connection
@app.websocket("/ws/client")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    connected_clients.append(websocket)
    try:
        # Send current reading immediately
        await websocket.send_text(json.dumps(latest_sensor_data))
        while True:
            # Keep open and wait for incoming messages (ping/pong)
            await websocket.receive_text()
    except WebSocketDisconnect:
        if websocket in connected_clients:
            connected_clients.remove(websocket)
