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

La conexión se carga desde `web/.env`, que está excluido de Git. Para cambiarla, edita ese archivo localmente con este formato; no pegues la URL real en documentación ni la subas al repositorio:

```dotenv
DATABASE_URL=postgresql://usuario:password@host/database?sslmode=require
```

Una variable `DATABASE_URL` definida en Windows tiene prioridad sobre el valor de `web/.env`.

## 3. Ejecutar backend y dashboard

Desde la carpeta `web`:

Si el ESP32 está conectado por USB, fija el puerto para que el backend no intente abrir otro COM:

```powershell
$env:ESP32_SERIAL_PORT="COM3"
```

Usa el COM que aparece para el ESP32 en el Administrador de dispositivos. Cierra Arduino Serial Monitor mientras el backend usa ese puerto.

Para usar solo WiFi y no abrir ningún puerto serial:

```powershell
$env:ESP32_SERIAL_PORT="disabled"
```

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

El ESP32 toma lecturas de DHT11 y MH-Z19C y publica telemetria aproximadamente cada segundo. El ESP32 envia lecturas por USB serial automaticamente; el backend tambien persiste como maximo una lectura por segundo. El backend detecta el puerto COM y las muestra en el dashboard.

## 6. Configurar WiFi del ESP32

La primera configuración necesita USB/Serial: un ESP sin WiFi todavía no
puede recoger comandos que el dashboard deje en la cola. Después de guardar
la red y la URL, el ESP puede funcionar sin cable.

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

### Operación solo WiFi y diagnóstico

1. Ejecuta FastAPI con `--host 0.0.0.0` y permite el puerto TCP 8000 en el
   firewall de Windows para redes privadas.
2. Arranca el backend con `ESP32_SERIAL_PORT="disabled"` o desconecta el USB;
   así queda claro que las lecturas entran por WiFi y no por el puerto COM.
3. Consulta `http://IP_DE_TU_PC:8000/api/devices`. El resultado muestra por
   ESP el último contacto, RSSI, IP, entregas correctas, fallos, descartes y
   profundidad de la cola de envío.

El firmware reintenta cada POST fallido hasta dos veces fuera del bucle de
sensores y conserva hasta 12 muestras. Si la red permanece caída se descartan
las más antiguas para priorizar el estado actual.

### Token opcional para proteger el endpoint

En la terminal que arranca el servidor define un token de al menos 16
caracteres antes de ejecutar Uvicorn:

```powershell
$env:ESP32_API_TOKEN="cambia-esto-por-un-secreto-largo"
```

Después envíalo una vez por Serial al ESP32:

```json
{"cmd":"set_api_token","token":"cambia-esto-por-un-secreto-largo"}
```

Con el token activo, el backend rechaza telemetría sin `X-API-Key`. Úsalo en
una red privada: HTTP sin TLS no protege el token ante alguien que capture el
tráfico. Para acceso fuera de la LAN, coloca el backend tras un proxy HTTPS o
usa un broker MQTT con TLS.
