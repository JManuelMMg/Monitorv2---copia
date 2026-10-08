import psycopg2
import psycopg2.extras
from datetime import datetime, timedelta, timezone
from pathlib import Path
import os
from dotenv import load_dotenv

load_dotenv(Path(__file__).with_name(".env"), override=False)
DATABASE_URL = os.getenv("DATABASE_URL")

def get_db():
    if not DATABASE_URL:
        raise RuntimeError("DATABASE_URL no está configurada")
    conn = psycopg2.connect(DATABASE_URL)
    conn.set_isolation_level(psycopg2.extensions.ISOLATION_LEVEL_AUTOCOMMIT)
    return conn

def init_db():
    try:
        with get_db() as conn:
            with conn.cursor() as cur:
                cur.execute("""
                    CREATE TABLE IF NOT EXISTS readings (
                        id SERIAL PRIMARY KEY,
                        timestamp TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
                        temp REAL,
                        hum REAL,
                        ppm REAL,
                        co2_ppm REAL,
                        dist REAL,
                        mq_r0 REAL,
                        uptime INTEGER,
                        wifi BOOLEAN,
                        rssi INTEGER
                    )
                """)
                cur.execute("ALTER TABLE readings ADD COLUMN IF NOT EXISTS co2_ppm REAL")
                conn.commit()
    except Exception as exc:
        print(f"[DB] No se pudo inicializar PostgreSQL: {exc}")

def save_reading(temp, hum, ppm, dist, mq_r0, uptime, wifi, rssi, co2_ppm=None):
    wifi_val = wifi if isinstance(wifi, bool) else bool(wifi)
    try:
        with get_db() as conn:
            with conn.cursor() as cur:
                cur.execute("""
                    INSERT INTO readings (temp, hum, ppm, co2_ppm, dist, mq_r0, uptime, wifi, rssi)
                    VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s)
                """, (temp, hum, ppm, co2_ppm, dist, mq_r0, uptime, wifi_val, rssi))
                conn.commit()
    except Exception as exc:
        print(f"[DB] No se pudo guardar lectura: {exc}")

def get_history(hours=24):
    limit_time = datetime.now(timezone.utc) - timedelta(hours=hours)
    try:
        with get_db() as conn:
            with conn.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
                cur.execute("""
                    SELECT timestamp, temp, hum, ppm, co2_ppm, dist, mq_r0, uptime, wifi, rssi
                    FROM readings
                    WHERE timestamp >= %s
                    ORDER BY timestamp ASC
                """, (limit_time,))
                rows = cur.fetchall()
                history = []
                for row in rows:
                    item = dict(row)
                    if isinstance(item.get("timestamp"), datetime):
                        item["timestamp"] = item["timestamp"].astimezone(timezone.utc).isoformat()
                    history.append(item)
                return history
    except Exception as exc:
        print(f"[DB] No se pudo leer historial: {exc}")
        return []

def get_latest_reading():
    try:
        with get_db() as conn:
            with conn.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
                cur.execute("""
                    SELECT timestamp, temp, hum, ppm, co2_ppm, dist, mq_r0, uptime, wifi, rssi
                    FROM readings
                    ORDER BY timestamp DESC
                    LIMIT 1
                """)
                row = cur.fetchone()
                if not row:
                    return None
                item = dict(row)
                if isinstance(item.get("timestamp"), datetime):
                    item["timestamp"] = item["timestamp"].astimezone(timezone.utc).isoformat()
                item["source"] = "WiFi" if item.get("wifi") else "Serial"
                return item
    except Exception as exc:
        print(f"[DB] No se pudo leer ultima lectura: {exc}")
        return None

def clear_history():
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM readings")
            conn.commit()
