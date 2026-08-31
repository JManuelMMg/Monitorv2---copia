# Sistema de Monitoreo Multi-Sensor IoT (ESP32 + FastAPI)

Este sistema profesional recopila datos telem�tricos en tiempo real desde un nodo ESP32 equipado con tres sensores industriales (Temperatura/Humedad DHT11, Gas Metano MQ-4 y Proximidad por Ultrasonido AJ-SR04M) y los transmite mediante un puerto Serial (USB) o WiFi (HTTP REST) a una interfaz web interactiva con est�tica industrial cyberpunk.

---

## ??? Estructura del Proyecto

El proyecto est� organizado en las siguientes carpetas:
- **/arduino/mgas/mgas.ino**: Firmware estable y no bloqueante para el ESP32.
- **/web**: Servidor web FastAPI completo (c�digo del servidor, base de datos SQLite y frontend en tiempo real).
  - `/static`: Hojas de estilo CSS personalizadas y l�gica JavaScript.
  - `/templates`: Pantalla del dashboard Jinja2 HTML.
  - `main.py`: Punto de entrada del servidor FastAPI y WebSockets.
  - `database.py`: Gesti�n local de hist�ricos mediante base de datos SQLite.
  - `serial_handler.py`: Hilo en segundo plano para detecci�n y conexi�n serial autom�tica.
  - `requirements.txt`: Dependencias del sistema en Python.

---

## ?? Conexiones de Hardware (ESP32)

Realice los siguientes conexionados en su m�dulo ESP32:

| Sensor / Componente | Pin ESP32 | Descripci�n |
| :--- | :--- | :--- |
| **DHT11 (Temp/Hum)** | Pin 23 | Se�al de datos (SDA) |
| **MQ-4 (Gas Metano)** | Pin 34 | Salida anal�gica (A0) |
| **AJ-SR04M (Ultrasonido)** | Pin 5 | Trigger (TRIG) |
| **AJ-SR04M (Ultrasonido)** | Pin 18 | Echo (ECHO) |
| **LED de Estado** | Pin 2 | LED interno / LED de notificaci�n |

---

## ?? Gu�a de Inicio R�pido

### Paso 1: Configurar y Cargar el Firmware ESP32
1. Abra el archivo `arduino/mgas/mgas.ino` en el **Arduino IDE**.
2. Instale la biblioteca oficial de sensores de DHT si a�n no la tiene instalada (vaya a *Herramientas -> Administrar bibliotecas* y busque "DHT sensor library" de Adafruit).
3. Conecte su ESP32 mediante USB y c�rguele el c�digo.

*(Opcional: Si desea configurar el WiFi de forma predeterminada, lea la secci�n "Configuraci�n WiFi y Endpoint" m�s abajo).*

---

### Paso 2: Ejecutar el Servidor Web FastAPI
Aseg�rese de tener **Python 3.9 o superior** instalado en su sistema.

1. Abra una terminal en el directorio `/web` del proyecto:
   ```bash
   cd web
   ```
2. Cree e inicialice un entorno virtual de Python (Recomendado):
   ```bash
   python -m venv venv
   ```
   *En Windows (PowerShell):*
   ```powershell
   .\venv\Scripts\Activate.ps1
   ```
3. Instale las dependencias de Python:
   ```bash
   pip install -r requirements.txt
   ```
4. Inicie el servidor web:
   ```bash
   python -m uvicorn main:app --host 0.0.0.0 --port 8000 --reload
   ```

*Nota: Usar `--host 0.0.0.0` permite que cualquier dispositivo dentro de su misma red WiFi local (incluido el ESP32) se conecte a su servidor usando la IP local de su PC.*
### Configuración de intervalos de muestreo
El panel de control incluye ajustes de intervalo para lejía del MQ-4, lecturas del ultrasonido y envío de telemetría. Estos valores se envían al ESP32 con el comando JSON `set_intervals` y se almacenan localmente en el navegador.
---

### Paso 3: Abrir la Interfaz de Monitoreo
Abra su navegador web favorito y acceda a la URL:
```text
http://localhost:8000
```
La interfaz comenzar� a escuchar lecturas inmediatamente a trav�s de la conexi�n WebSocket.

---

## ?? Opciones de Conectividad (ESP32 ? Servidor)

El nodo IoT est� dise�ado para operar en dos modalidades diferentes o de manera combinada:

### Opci�n A: Conexi�n Directa por USB (Modo Serial)
* **C�mo funciona**: El ESP32 env�a lecturas formateadas en JSON a trav�s del puerto serial. El servidor FastAPI tiene un proceso interno que detecta autom�ticamente el puerto COM asignado al ESP32 al conectarse y empieza a procesar las tramas JSON.
* **Ventajas**: Cero configuraci�n de red requerida. Ideal para pruebas en laboratorios.

### Opci�n B: Conexi�n Inal�mbrica por Red Local (Modo WiFi)
* **C�mo funciona**: El ESP32 se conecta al WiFi de su hogar u oficina y env�a las tramas JSON directamente por peticiones HTTP POST a su computadora.
* **Ventajas**: Monitoreo a gran distancia inal�mbricamente.

#### Configuraci�n del WiFi y Servidor desde la Terminal Serial
Una vez cargado el firmware en el ESP32, usted puede configurar sus credenciales WiFi y el endpoint del servidor usando comandos JSON enviados al puerto serial desde el monitor de Arduino IDE (a velocidad `115200` y configurando *Retorno de carro* o *Nueva l�nea*):

1. **Configurar credenciales WiFi**:
   ```json
   {"cmd":"set_wifi", "ssid":"NombreDeTuRed", "pass":"TuContrasenaWiFi"}
   ```
2. **Configurar el enlace del servidor (IP de su PC)**:
   ```json
   {"cmd":"set_server", "url":"http://IP_DE_TU_PC:8000/api/readings"}
   ```
3. **Reiniciar el ESP32** para aplicar cambios y conectarse:
   ```json
   {"cmd":"reboot"}
   ```

*Nota: Esta configuraci�n se guardar� de forma permanente en la memoria interna flash del ESP32 (`Preferences`), por lo que el dispositivo recordar� la red incluso si es apagado o desconectado.*

---

## ??? Funcionalidades del Dashboard

1. **Monitoreo en Tiempo Real**: Visualice la temperatura, humedad, concentraci�n de gas metano y nivel de distancia en widgets interactivos con est�tica Cyberpunk.
2. **Sem�foro de Gas CH4**: El valor del gas metano se clasifica din�micamente y la tarjeta cambia de color y estado (`SEGURO`, `PRECAUCI�N`, `CR�TICO`).
3. **Gr�ficos Din�micos**: Tres gr�ficos avanzados potenciados por **Chart.js** con historial ajustable (1 hora, 6 horas y 24 horas).
4. **Alarmas Sonoras y Visuales**: Alarma instant�nea si el metano supera el umbral, si hay un obst�culo cerca del sensor de ultrasonido o si hay sobrecalentamiento. Los pitidos de alarma se generan de manera limpia y local usando la **Web Audio API** del navegador.
5. **Configuraci�n de Umbrales**: Ajuste las sensibilidades y l�mites de disparo de alarmas directamente desde el navegador de manera persistente (`localStorage`).
6. **Historial y Exportaci�n CSV**: Tabla din�mica con los �ltimos registros recibidos y bot�n para exportar todo el hist�rico de la base de datos SQLite directamente a un archivo CSV.
