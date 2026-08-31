# Plan de Implementación: Monitor de Sensores IoT (ESP32 + FastAPI)

Este plan describe la arquitectura y los cambios propuestos para mejorar el firmware del ESP32 (DHT11 + MQ-4 + AJ-SR04M) y crear una interfaz web de monitoreo en tiempo real usando FastAPI, SQLite y Chart.js con un diseño cyberpunk/industrial premium.

## Resumen del Proyecto

El sistema consta de dos partes principales:
1. **Firmware ESP32 (`arduino/mgas/mgas.ino`)**:
   - Lectura optimizada y no bloqueante de sensores.
   - Envío de datos formateados en JSON a través de Serial (USB) y WiFi (WebSocket Client).
   - Recepción de comandos en JSON para calibración de MQ-4, cambio de intervalos, control de LED, etc.
   - Almacenamiento no volátil de la calibración ($R_0$) y configuración mediante la librería `Preferences`.
2. **Servidor Web Python (`web/main.py` y frontend)**:
   - Framework FastAPI de alto rendimiento.
   - Hilo/tarea en segundo plano para escaneo y comunicación automática bidireccional por puerto Serial (usando `pyserial`).
   - Servidor WebSocket para recibir datos de ESP32 (si se usa por WiFi) y transmitir datos en tiempo real a los navegadores abiertos.
   - Base de datos SQLite ligera para el registro histórico de lecturas.
   - Interfaz web interactiva con estética oscura Cyberpunk/Industrial, gráficos dinámicos con Chart.js, alertas visuales y sonoras (sintetizadas mediante la Web Audio API) y exportación de datos a CSV.

---

## Cambios Propuestos

### 1. Firmware ESP32 (`/arduino/mgas/mgas.ino`)

Optimizaremos el firmware para eliminar las funciones bloqueantes (`delay` en bucles) que actualmente provocan retardos importantes y pérdida de paquetes de comunicación.

- **Lectura de Ultrasonido No Bloqueante**: 
  Implementaremos un buffer circular (rolling window) para las muestras del sensor ultrasónico AJ-SR04M. En lugar de tomar 9 lecturas consecutivas bloqueando el bucle con `delay(12)`, se tomará una sola muestra cada 20 ms. Cuando se solicite el valor actual de distancia, se calculará la mediana de las últimas 9 muestras para filtrar ruido y rebotes sin bloquear el procesador.
- **Formato de Transmisión**: 
  Los datos se estructurarán como una cadena JSON compacta enviada por `Serial` y opcionalmente por un cliente WebSocket.
- **Librería de Preferencias**: 
  Usaremos `Preferences.h` para almacenar permanentemente el valor de calibración `MQ4_R0` del sensor de gas y los intervalos de muestreo.
- **Comandos Soportados**:
  - `{"cmd":"calibrate"}`: Ejecuta una rutina de calibración del sensor MQ-4 en aire limpio y recalcula $R_0$.
  - `{"cmd":"set_intervals", "mq": ms, "us": ms, "out": ms}`: Modifica los intervalos de lectura en tiempo de ejecución.
  - `{"cmd":"reboot"}`: Reinicia el ESP32.
  - `{"cmd":"led", "state": 1|0}`: Enciende o apaga manualmente el LED de estado.

### 2. Backend FastAPI (`/web/`)

Crearemos un servidor web robusto y tolerante a fallos:

- `web/main.py`: Código del servidor FastAPI.
- `web/serial_handler.py`: Controlador que busca de forma persistente y automática el puerto COM del ESP32, se reconecta en caso de desconexión y procesa las líneas JSON entrantes.
- `web/database.py`: Manejo de SQLite para almacenar el histórico de datos de manera eficiente (p. ej., registrando una muestra cada 5 segundos para evitar el crecimiento excesivo).
- `web/requirements.txt`: Dependencias del proyecto (`fastapi`, `uvicorn`, `pyserial`, `jinja2`).

### 3. Frontend (`/web/templates/index.html` y `/web/static/`)

Implementaremos una interfaz moderna y llamativa con una estética de monitoreo industrial oscuro:

- **Estilo Visual**: Colores oscuros profundos con acentos de color vibrantes (verde cian, naranja de advertencia, rojo de peligro), sombras brillantes (neon glow), tipografía de alta legibilidad (`Orbitron` e `Inter` de Google Fonts) y diseño adaptivo.
- **Gráficas en Tiempo Real (Chart.js)**:
  - Gráfico 1: Temperatura y Humedad en eje doble Y.
  - Gráfico 2: Concentración de CH4 (Metano) en PPM con áreas sombreadas de peligro.
  - Gráfico 3: Historial de distancia.
- **Indicador de Nivel de Tanque/Distancia**: Un widget animado que simula el llenado de un tanque de almacenamiento según la lectura del sensor de distancia.
- **Panel de Control**: Formularios interactivos para enviar comandos al ESP32 (calibrar, cambiar intervalos de muestreo, reiniciar, etc.) y configurar los umbrales de alerta del navegador de manera local.
- **Sistema de Alertas**:
  - Alerta de Gas: Cuando CH4 supera el límite configurable.
  - Alerta de Proximidad: Si la distancia es menor al límite.
  - Alerta de Temperatura: Fuera del rango seguro.
  - Generación de sonido mediante Web Audio API en el navegador para evitar depender de archivos de audio locales.

---

## Arquitectura de Directorios

Crearemos la siguiente estructura limpia en el espacio de trabajo:

```text
Monitor/
├── arduino/
│   └── mgas/
│       └── mgas.ino         <- Firmware ESP32 mejorado y optimizado
├── web/
│   ├── static/
│   │   ├── css/
│   │   │   └── style.css    <- Estilos premium cyberpunk
│   │   └── js/
│   │       └── app.js       <- Lógica de WebSockets, gráficos y audio API
│   ├── templates/
│   │   └── index.html       <- Plantilla Jinja2 del dashboard
│   ├── main.py              <- Servidor FastAPI y WebSocket
│   ├── database.py          <- Gestión de SQLite
│   ├── serial_handler.py    <- Lector serial en segundo plano
│   └── requirements.txt     <- Dependencias de Python
└── README.md                <- Instrucciones de uso e instalación
```

---

## Plan de Verificación

### Pruebas de Software (Backend)
- Validación de que la reconexión serial funciona correctamente al desconectar y volver a conectar el cable USB del ESP32.
- Verificación del almacenamiento de datos en la base de datos SQLite.
- Validación del endpoint de exportación a CSV.

### Pruebas de Interfaz (Frontend)
- Simulación de datos extremos (alta temperatura, gas elevado, distancia baja) para comprobar el disparo de las alarmas visuales y el sintetizador de audio.
- Verificación de la adaptabilidad responsiva en pantallas móviles y de escritorio.

---
> [!IMPORTANT]
> **Revisión del usuario requerida**:
> Por favor, revisa el plan de implementación. Una vez aprobado, comenzaré a desarrollar los componentes.
