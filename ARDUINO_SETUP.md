# 🔧 CONFIGURACIÓN COMPLETA ESP32 + SISTEMA IoT

## 📋 RESUMEN DE INTEGRACIÓN

El código Arduino está completamente adaptado para funcionar con:
- ✅ Backend FastAPI (puerto 8000)
- ✅ Base de datos PostgreSQL Neon
- ✅ Dashboard cyberpunk en tiempo real
- ✅ Comunicación serial + WiFi
- ✅ Sincronización de comandos bidireccional

---

## 🔌 CONEXIONES DE HARDWARE

### Pines ESP32
| Sensor | Pin GPIO | Descripción |
|--------|----------|-------------|
| DHT11 (DATA) | GPIO 23 | Temperatura/Humedad |
| MQ-4 (A0) | GPIO 34 (ADC) | Sensor de Gas Metano |
| Ultrasonido TRIG | GPIO 5 | Trigger ultrasónico |
| Ultrasonido ECHO | GPIO 18 | Echo ultrasónico |
| LED Status | GPIO 2 | LED indicador |
| GND | GND | Tierra común |
| 5V | 5V | Alimentación |

### Diagrama de conexión
```
ESP32         |    DHT11      |  MQ-4   |  AJ-SR04M  |  LED
----------    |    -----      |  -----  |  --------  |  ---
GPIO 23 ------+--- DATA       |         |            |
GPIO 34 ------|-------------- +-- ADC  |            |
GPIO 5  ------|-------------- |------+----- TRIG    |
GPIO 18 ------|-------------- |------+----- ECHO    |
GPIO 2  ------|-------------- |------+-------------- +-- Ánodo
GND ------+---+-- GND --------+-- GND +-- GND ------|-- Cátodo (vía 330Ω)
5V -------+---+-- VCC --------+-- VCC +-- VCC
```

---

## 💾 INSTALACIÓN EN ARDUINO IDE

### 1. Librerías Requeridas
Instala estas librerías en Arduino IDE (`Sketch → Include Library → Manage Libraries`):
- **DHT sensor library** (Adafruit) v1.4.4+
- **WiFi** (incluida en ESP32)
- **HTTPClient** (incluida en ESP32)
- **Preferences** (incluida en ESP32)

### 2. Configurar Placa
1. Abre `Preferences` en Arduino IDE
2. Agrega URL: `https://dl.espressif.com/dl/package_esp32_index.json`
3. Selecciona: `Herramientas → Placa → esp32 → ESP32 Dev Module`
4. **Configuración de puerto:**
   - Serial Baud Rate: **115200**
   - Flash Mode: DIO
   - Flash Frequency: 80 MHz
   - Flash Size: 4MB
   - Partition Scheme: Default

### 3. Cargar Firmware
```
1. Conecta ESP32 por USB
2. Abre: arduino/mgas/mgas.ino
3. Presiona Upload (Ctrl+U)
4. Espera: "Hard resetting via RTS pin..."
```

---

## 🚀 INICIALISACIÓN Y CONFIGURACIÓN

### Fase 1: Serial Communication (Primeras veces)
1. Abre **Monitor Serial** (115200 baud)
2. Verás:
   ```json
   {"log":"[INICIO] ESP32 Inicializado - Sistema IoT Monitor v2.0"}
   {"status":"ready"}
   {"log":"Configuracion cargada de Preferences."}
   ```

3. Comienza a enviar lecturas cada 1 segundo:
   ```json
   {"temp":22.5,"hum":45.3,"ppm":120.5,"dist":45.2,"mq_ready":true,"mq_r0":14800.5,"wifi":false,"rssi":0,"uptime":125}
   ```

### Fase 2: Conexión al Backend Local

**Opción A: Serial + FastAPI en localhost**
```bash
# Terminal 1: Ejecutar FastAPI (ya está corriendo)
cd web
python -m uvicorn main:app --host 0.0.0.0 --port 8000

# Terminal 2: Conectar ESP32 por USB
# En Monitor Serial verás datos cada 1 segundo
```

**Dashboard:** http://localhost:8000

### Fase 3: Configuración WiFi (Opcional)
Si tienes WiFi disponible, configura desde el dashboard:

1. En **Panel de Control → RED & SERVIDOR**
2. Ingresa SSID y contraseña
3. Haz clic en "Guardar WiFi"
4. ESP32 se reiniciará automáticamente
5. Verificar en Monitor Serial:
   ```json
   {"log":"Intentando conectar a WiFi SSID: [TuSSID]"}
   ```

---

## 📊 FLUJO DE DATOS

### Arquitectura de Datos
```
┌─────────────────────────────────────────────────────────────┐
│                        ESP32 (mgas.ino)                      │
├─────────────────────────────────────────────────────────────┤
│  • DHT11 (Temp+Humedad)                                     │
│  • MQ-4  (Gas Metano PPM)                                   │
│  • AJ-SR04M (Distancia/Nivel)                               │
│  • Actuadores (LED, Comandos)                               │
└──────────┬──────────────────────────────────┬────────────────┘
           │ JSON/Serial                      │ JSON/HTTP POST
           ▼                                  ▼
    ┌─────────────────┐              ┌──────────────────┐
    │   FastAPI       │              │  WiFi (opcional) │
    │  main.py        │◄─────────────┤                  │
    │                 │              │  /api/readings   │
    └────────┬────────┘              └──────────────────┘
             │ SQL
             ▼
    ┌──────────────────────┐
    │  PostgreSQL (Neon)   │
    │  Database: mgasalg   │
    │  Table: readings     │
    └──────────┬───────────┘
             │
             ▼
    ┌──────────────────────┐
    │  Dashboard Web       │
    │  http://localhost:8000 │
    │  (Templates + Static) │
    │  (JavaScript + Charts)│
    └──────────────────────┘
```

### Secuencia de Lecturas
1. **Cada 50ms**: Lectura Ultrasonido (buffer circular 9 muestras)
2. **Cada 200ms**: Lectura MQ-4 (cálculo de Rs)
3. **Cada 2.5s**: Lectura DHT11 (promedio 5 muestras)
4. **Cada 1000ms**: Envío completo de telemetría
   - Serial + HTTP POST (si WiFi conectado)
   - Guardado en PostgreSQL (throttled cada 5s)

---

## 🎮 COMANDOS DISPONIBLES

El dashboard envía comandos automáticamente. También puedes enviar por Serial:

### Calibrar MQ-4 (5 segundos)
```json
{"cmd":"calibrate"}
```
Respuesta esperada:
```json
{"log":"Iniciando calibracion MQ-4 (5 segundos)..."}
{"log":"Calibracion completada. Nuevo R0: 15200.50"}
```

### Cambiar Intervalos de Muestreo
```json
{"cmd":"set_intervals","mq":200,"us":50,"out":1000}
```
Parámetros:
- `mq`: Intervalo MQ-4 (ms) - mínimo 10ms
- `us`: Intervalo Ultrasonido (ms) - mínimo 10ms
- `out`: Intervalo de envío (ms) - mínimo 100ms

### Control de LED
```json
{"cmd":"led","state":1}
```
- `state:1` = LED encendido
- `state:0` = LED apagado

### Reiniciar ESP32
```json
{"cmd":"reboot"}
```

### Configurar WiFi
```json
{"cmd":"set_wifi","ssid":"MiRed","pass":"Contraseña"}
```

### Cambiar URL de servidor
```json
{"cmd":"set_server","url":"http://192.168.1.100:8000/api/readings"}
```

---

## 🔍 DEBUGGING

### Monitor Serial Esperado
```
[INICIO] ESP32 Inicializado - Sistema IoT Monitor v2.0
{"status":"ready"}
{"log":"Configuracion cargada de Preferences."}
{"log":"Sin credenciales de WiFi en memoria. Modo Serial únicamente."}
{"temp":22.5,"hum":45.3,"ppm":120.5,"dist":45.2,"mq_ready":true,"mq_r0":14800.5,"wifi":false,"rssi":0,"uptime":1}
{"temp":22.5,"hum":45.3,"ppm":120.5,"dist":45.2,"mq_ready":true,"mq_r0":14800.5,"wifi":false,"rssi":0,"uptime":2}
...
```

### Problemas Comunes

**Problema:** No aparece nada en Monitor Serial
- **Solución:** Verifica baud rate es 115200, reinicia ESP32 (botón RESET)

**Problema:** Readings con valores `-nan` o muy altos
- **Solución:** Espera 60 segundos (precalentamiento MQ-4), verifica conexiones

**Problema:** WiFi no conecta
- **Solución:** Verifica SSID y contraseña, revisa Monitor Serial para error de conexión

**Problema:** Dashboard no recibe datos
- **Solución:** 
  1. Verifica que FastAPI está corriendo (`http://0.0.0.0:8000`)
  2. Comprueba que ESP32 está conectado por USB
  3. Abre Monitor Serial para verificar transmisión

---

## 📈 RENDIMIENTO ESPERADO

| Métrica | Valor |
|---------|-------|
| Tasa de lectura | 1 lectura/segundo |
| Latencia serial | <100ms |
| Latencia WiFi HTTP | <500ms |
| Precalentamiento MQ-4 | 60 segundos (hasta mq_ready:true) |
| Tiempo de calibración | 5 segundos |
| Precisión temperatura | ±2°C |
| Precisión humedad | ±5% |
| Rango distancia | 2-450 cm |
| Resolución PPM | 0.1 PPM |

---

## 🔐 ALMACENAMIENTO PERSISTENTE

El ESP32 guarda en flash memory (Preferences):
- `wifi_ssid` - SSID de WiFi
- `wifi_pass` - Contraseña WiFi
- `srv_url` - URL del servidor
- `mq_r0` - Valor de calibración MQ-4
- `int_mq` - Intervalo MQ-4
- `int_us` - Intervalo Ultrasonido
- `int_out` - Intervalo de envío

Para resetear: Envía comando `reset_preferences` (si lo agregas) o borra manualmente:
```cpp
preferences.begin("monitor", false);
preferences.clear();
preferences.end();
```

---

## ✅ CHECKLIST DE FUNCIONAMIENTO

- [ ] Arduino IDE instalado con ESP32 board
- [ ] Todas las librerías instaladas
- [ ] Hardware conectado correctamente
- [ ] Firmware cargado en ESP32
- [ ] Monitor Serial muestra startup messages
- [ ] FastAPI corriendo en puerto 8000
- [ ] Dashboard accesible en localhost:8000
- [ ] Datos visibles en dashboard (tiempo real)
- [ ] PostgreSQL Neon conectado
- [ ] Comandos enviados desde dashboard funcionan

---

## 🎯 PRÓXIMOS PASOS

1. **Conecta el ESP32 y abre el Monitor Serial**
2. **Verifica que recibe datos (1 lectura/segundo)**
3. **Abre http://localhost:8000 en el navegador**
4. **Deberías ver datos en tiempo real en el dashboard**
5. **(Opcional) Configura WiFi desde el dashboard**

¡Listo! Tu sistema IoT profesional está operativo. 🚀

---

**Versión:** 2.0 | **Fecha:** Mayo 2026 | **Estado:** ✅ Producción
