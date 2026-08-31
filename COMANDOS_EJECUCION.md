# Comandos para ejecutar el sistema

Ejecuta estos comandos desde PowerShell en:

```powershell
cd "C:\Users\jmedi\OneDrive\Documents\proyectos\Monitor"
```

## 1. Preparar Python

```powershell
cd web
python -m venv venv
.\venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install -r requirements.txt
```

Esto instala tambien `websockets`, que FastAPI/Uvicorn necesita para que funcione `/ws/client` y el dashboard reciba datos en tiempo real sin caer al modo de sondeo.

Si PowerShell bloquea la activacion del entorno virtual:


```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
.\venv\Scripts\Activate.ps1
```

## 2. Configurar PostgreSQL

El proyecto ya tiene `DATABASE_URL` configurado en `web\database.py`.

Si quieres usar otra base de datos sin tocar codigo:

```powershell
$env:DATABASE_URL="postgresql://usuario:password@host/database?sslmode=require"
```

## 3. Ejecutar backend y dashboard

Desde la carpeta `web`:

```powershell
python -m uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

Si ya tenias el entorno creado antes de este cambio, reinstala dependencias:

```powershell
pip install -r requirements.txt
```

Abre:

```text
http://localhost:8000
```

## 4. Probar sin ESP32

En otra terminal, desde la raiz del proyecto:

```powershell
python test_esp32_sim.py
```

Tambien puedes mandar pocas lecturas de prueba:

```powershell
python -c "from test_esp32_sim import ESP32Simulator; s=ESP32Simulator(); s.run(duration=10)"
```

## 5. Cargar Arduino / ESP32

En Arduino IDE:

1. Abre `arduino\mgas\mgas.ino`.
2. Selecciona la placa `ESP32 Dev Module`.
3. Instala la libreria `DHT sensor library` de Adafruit.
4. Selecciona el puerto COM del ESP32.
5. Carga el sketch.
6. Abre Monitor Serial a `115200 baud`.

El ESP32 envia lecturas por USB serial automaticamente. El backend detecta el puerto COM y las muestra en el dashboard.

## 6. Configurar WiFi del ESP32

Puedes hacerlo desde el panel web o desde Monitor Serial:

```json
{"cmd":"set_wifi","ssid":"NombreDeTuRed","pass":"TuPassword"}
```

Configura la URL del backend usando la IP de tu PC:

```json
{"cmd":"set_server","url":"http://IP_DE_TU_PC:8000/api/readings"}
```

Reinicia el ESP32:

```json
{"cmd":"reboot"}
```

Para ver tu IP local en Windows:

```powershell
ipconfig
```

Busca la direccion IPv4 de tu adaptador WiFi o Ethernet.
