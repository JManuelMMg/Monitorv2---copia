// ============================================================================
// ESP32 | DHT11 + MQ-4 + AJ-SR04M + MH-Z19C | Monitor de Sensores IoT v3.2
// ============================================================================
// CORRECCIONES:
// - ✅ MQ-4 con constantes correctas (Rs/R0 = 4.4)
// - ✅ MH-Z19C: Diagnóstico detallado de comunicación UART
// - ✅ MH-Z19C: Detección de lecturas clavadas (5000 PPM = error)
// - ✅ MH-Z19C: Verificación de bytes crudos
// ============================================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <DHT.h>

// ==================== CONFIGURACIÓN DE PINES ====================
#define DHTPIN 23
#define DHTTYPE DHT11
#define MQ4_PIN 34  // ⭐ MQ-4
#define TRIG_PIN 5
#define ECHO_PIN 18
#define STATUS_LED 2
#define MHZ19_RX_PIN 16  // RX del ESP32 -> TX del MH-Z19C
#define MHZ19_TX_PIN 17  // TX del ESP32 -> RX del MH-Z19C

// ==================== CONSTANTES DE SENSORES ====================
const float MQ4_RL = 10000.0;     // Resistencia de carga (10k ohms)
const float MQ4_VCC = 5.0;        // Voltaje de alimentación del MQ-4 (MIDE CON MULTÍMETRO)
const float US_MIN_CM = 2.0;
const float US_MAX_CM = 450.0;
const int US_BUFFER_SIZE = 15;
const float GAS_MIN_PRODUCTION_PPM = 50.0;
const unsigned long CO2_READ_INTERVAL = 5000;
const unsigned long CO2_WARMUP_MS = 180000;        // 3 min precalentamiento
const unsigned long CO2_FRESH_AIR_MIN_MS = 1200000; // 20 min aire fresco
const unsigned long MQ_WARMUP_DEFAULT_MS = 300000; // 5 min calentamiento
const unsigned long REBOOT_GRACE_MS = 250;

// ⭐ CURVA CH4 (ahora ajustable por calibración multipunto, ya no constante)
// Valores por defecto = curva genérica del datasheet, se sobreescriben si
// se ejecuta una calibración multipunto con gas de referencia (ver abajo).
float mq4_curve_a = 1012.7;
float mq4_curve_b = -2.786;
bool mq4_curve_is_custom = false;

// ⭐ Límites de Rs en aire limpio para MQ-4 (Rs/R0 = 4.4)
const float MQ4_CLEAN_AIR_RS_MIN = 5000.0;
const float MQ4_CLEAN_AIR_RS_MAX = 100000.0;

// ⭐ CALIBRACIÓN MULTIPUNTO DE LA CURVA (regresión log-log sobre PPM de referencia)
const int MQ4_CAL_MAX_POINTS = 8;
const int CAL_POINT_TARGET_SAMPLES = 20;              // 20 muestras
const unsigned long CAL_POINT_SAMPLE_INTERVAL_MS = 500; // cada 0.5s -> ~10s por punto
float mq4_cal_x[MQ4_CAL_MAX_POINTS];  // log(ratio compensado)
float mq4_cal_y[MQ4_CAL_MAX_POINTS];  // log(ppm de referencia)
int mq4_cal_point_count = 0;

bool is_adding_cal_point = false;
float pending_cal_ppm = 0.0;
unsigned long last_cal_point_sample = 0;
int cal_point_samples = 0;
float cal_point_rs_sum = 0.0;

// ==================== VARIABLES DE ESTADO Y CONFIGURACIÓN ====================
Preferences preferences;

String wifi_ssid = "";
String wifi_pass = "";
String srv_url = "http://192.168.1.100:8000/api/readings";
float MQ4_R0 = 15000.0;  // ⭐ MQ-4
float reactor_empty_cm = 200.0;
float reactor_full_cm = 20.0;
float us_offset_cm = 0.0;
float us_scale = 1.0;
unsigned long interval_mq = 200;
unsigned long interval_us = 50;
unsigned long interval_out = 1000;
unsigned long mq_warmup_ms = MQ_WARMUP_DEFAULT_MS;
bool co2_abc_enabled = false;
float dht_temp_offset = 0.0;
float dht_hum_offset = 0.0;

unsigned long last_mq_read = 0;
unsigned long last_us_read = 0;
unsigned long last_dht_read = 0;
unsigned long last_telemetry_out = 0;
unsigned long last_wifi_check = 0;
unsigned long last_co2_read = 0;

// ⭐ Estado de conexión WiFi (para diagnóstico expuesto en telemetría)
bool wifi_was_connected = false;
unsigned long wifi_connected_since = 0;
int wifi_reconnect_attempts = 0;

float us_buf[US_BUFFER_SIZE];
int us_write_idx = 0;
int us_sample_count = 0;
float latest_us_raw_cm = US_MAX_CM;
float latest_us_filtered_cm = US_MAX_CM;
bool latest_us_valid = false;

DHT dht(DHTPIN, DHTTYPE);
float dht_temp_buf[5];
float dht_hum_buf[5];
int dht_write_idx = 0;
int dht_sample_count = 0;
const unsigned long DHT_READ_INTERVAL = 2500;

float latest_rs = 15000.0;
int latest_co2_ppm = 0;  // ⭐ 0 por defecto, no -1

// ⭐ Diagnóstico MH-Z19C
int co2_error_count = 0;
int co2_last_valid_ppm = 0;
unsigned long co2_last_valid_time = 0;
bool co2_stuck_detected = false;
int co2_stuck_count = 0;
int co2_last_value = -1;

unsigned long co2_fresh_air_start = 0;
bool co2_in_fresh_air = false;

HardwareSerial co2Serial(2);

bool is_calibrating = false;
unsigned long calibration_start = 0;
unsigned long last_calibration_sample = 0;
int calibration_samples = 0;
float calibration_rs_sum = 0.0;
float calibration_rs_sq_sum = 0.0;
bool reboot_pending = false;
unsigned long reboot_requested_at = 0;

// ==================== DECLARACIÓN DE FUNCIONES ====================
void loadPreferences();
void setupWiFi();
void readMQ4();
void readUltrasonic();
void readDHT();
void readCO2();
void checkCalibration();
void sendTelemetry();
void sendWiFiTelemetry(const String& jsonPayload);
void parseCommand(String cmd);
void parseWiFiResponse(String response);
String stripWhitespaceOutsideQuotes(const String& input);
void checkPendingReboot();
float getMedianDistance();
float getFilteredDistance();
float getAverageTemperature();
float getAverageHumidity();
float getMQ4PPM(float rs, float r0);
float getMQ4CompensationFactor();
float getReactorLevelPercent(float distance_cm);
bool isMQReady();
bool isCO2Ready();
int readMHZ19C();
bool sendMHZ19Command(byte commandId, byte dataByte);
bool waitForMHZ19Response(byte expectedCmd, byte* response, unsigned long timeoutMs);
bool setMHZ19ABC(bool enabled, bool persistState, const char* source);
bool calibrateMHZ19Zero(const char* source);
byte getMHZ19Checksum(const byte packet[]);
unsigned long getValueAfter(const String& str, int startPos);
float getFloatValue(const String& str, const String& key, float fallback);
String getStringValue(const String& str, const String& key);
float readMQ4Voltage();
void diagnoseMHZ19C();  // ⭐ NUEVO
void addMQ4CalibrationPoint(float known_ppm);
void checkAddCalPoint();
void fitMQ4Curve();
void resetMQ4Curve();

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(MQ4_PIN, INPUT);
  
  digitalWrite(STATUS_LED, LOW);
  analogSetAttenuation(ADC_11db);
  
  dht.begin();
  co2Serial.begin(9600, SERIAL_8N1, MHZ19_RX_PIN, MHZ19_TX_PIN);
  
  loadPreferences();
  setMHZ19ABC(co2_abc_enabled, false, "boot");
  setupWiFi();
  
  Serial.println("{\"log\":\"[INICIO] ESP32 Inicializado - Sistema IoT Monitor v3.2\"}");
  Serial.printf("{\"info\":\"sensor_config\",\"sensor\":\"MQ-4\",\"mq_vcc\":%.2f,\"co2_warmup_ms\":%lu,\"mq_warmup_ms\":%lu}\n",
                MQ4_VCC, CO2_WARMUP_MS, mq_warmup_ms);
  Serial.println("{\"log\":\"Verificando comunicación MH-Z19C...\"}");
  diagnoseMHZ19C();  // ⭐ Diagnóstico inicial
  Serial.println("{\"status\":\"ready\"}");
}

void loop() {
  readUltrasonic();
  readMQ4();
  readDHT();
  readCO2();
  checkCalibration();
  checkAddCalPoint();
  
  if (Serial.available() > 0) {
    String serialCmd = Serial.readStringUntil('\n');
    parseCommand(serialCmd);
  }
  
  if (millis() - last_telemetry_out >= interval_out) {
    last_telemetry_out = millis();
    sendTelemetry();
  }
  
  if (wifi_ssid.length() > 0) {
    bool now_connected = (WiFi.status() == WL_CONNECTED);

    // ⭐ Detecta el cambio de estado de inmediato (barato, no bloqueante),
    // en vez de esperar hasta 10s para enterarse de una conexion o caida.
    if (now_connected && !wifi_was_connected) {
      wifi_was_connected = true;
      wifi_connected_since = millis();
      wifi_reconnect_attempts = 0;
      Serial.printf(
        "{\"event\":\"wifi_status\",\"status\":\"connected\",\"ip\":\"%s\",\"rssi\":%d}\n",
        WiFi.localIP().toString().c_str(), WiFi.RSSI()
      );
    } else if (!now_connected && wifi_was_connected) {
      wifi_was_connected = false;
      Serial.println("{\"event\":\"wifi_status\",\"status\":\"disconnected\"}");
    }

    if (!now_connected && millis() - last_wifi_check >= 10000) {
      last_wifi_check = millis();
      wifi_reconnect_attempts++;
      Serial.printf(
        "{\"event\":\"wifi_status\",\"status\":\"reconnecting\",\"attempt\":%d,\"ssid\":\"%s\"}\n",
        wifi_reconnect_attempts, wifi_ssid.c_str()
      );
      WiFi.disconnect(false);
      WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    }
  }

  checkPendingReboot();
}

void loadPreferences() {
  preferences.begin("monitor", true);
  
  wifi_ssid = preferences.getString("wifi_ssid", "");
  wifi_pass = preferences.getString("wifi_pass", "");
  srv_url = preferences.getString("srv_url", "http://192.168.1.100:8000/api/readings");
  
  MQ4_R0 = preferences.getFloat("mq_r0", 15000.0);
  reactor_empty_cm = preferences.getFloat("rx_empty", 200.0);
  reactor_full_cm = preferences.getFloat("rx_full", 20.0);
  us_offset_cm = preferences.getFloat("us_offset", 0.0);
  us_scale = preferences.getFloat("us_scale", 1.0);
  if (us_scale < 0.5 || us_scale > 1.5) us_scale = 1.0;
  
  interval_mq = preferences.getULong("int_mq", 200);
  interval_us = preferences.getULong("int_us", 50);
  interval_out = preferences.getULong("int_out", 1000);
  mq_warmup_ms = preferences.getULong("mq_warmup", MQ_WARMUP_DEFAULT_MS);
  co2_abc_enabled = preferences.getBool("co2_abc", false);
  dht_temp_offset = preferences.getFloat("dht_t_off", 0.0);
  dht_hum_offset = preferences.getFloat("dht_h_off", 0.0);

  mq4_curve_a = preferences.getFloat("mq4_ca", 1012.7);
  mq4_curve_b = preferences.getFloat("mq4_cb", -2.786);
  mq4_curve_is_custom = preferences.getBool("mq4_custom", false);

  if (mq_warmup_ms < 60000 || mq_warmup_ms > 1800000) {
    mq_warmup_ms = MQ_WARMUP_DEFAULT_MS;
  }
  
  preferences.end();
  
  Serial.printf(
    "{\"event\":\"preferences_loaded\",\"mq_r0\":%.2f,\"co2_abc\":%s,\"dht_temp_offset\":%.2f,\"dht_hum_offset\":%.2f,\"mq_warmup_ms\":%lu}\n",
    MQ4_R0,
    co2_abc_enabled ? "true" : "false",
    dht_temp_offset,
    dht_hum_offset,
    mq_warmup_ms
  );
}

void setupWiFi() {
  if (wifi_ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    last_wifi_check = millis();
    Serial.println("{\"log\":\"Intentando conectar a WiFi SSID: " + wifi_ssid + "\"}");
  } else {
    Serial.println("{\"log\":\"Sin credenciales de WiFi en memoria. Modo Serial únicamente.\"}");
  }
}

float readMQ4Voltage() {
  int mv = analogReadMilliVolts(MQ4_PIN);
  return mv / 1000.0;
}

void readUltrasonic() {
  if (millis() - last_us_read >= interval_us) {
    last_us_read = millis();
    
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    unsigned long timeout_us = (unsigned long)((US_MAX_CM * 2.0) / 0.0343) + 3000;
    unsigned long duration = pulseIn(ECHO_PIN, HIGH, timeout_us);
    
    float dist = latest_us_filtered_cm;
    latest_us_valid = false;
    if (duration > 0) {
      float temp_c = getAverageTemperature();
      float sound_cm_per_us = (331.3 + (0.606 * temp_c)) / 10000.0;
      float raw_dist = (duration * sound_cm_per_us) / 2.0;
      dist = (raw_dist * us_scale) + us_offset_cm;
      latest_us_raw_cm = raw_dist;
    }
    
    if (duration == 0 || dist < US_MIN_CM || dist > US_MAX_CM) {
      return;
    }

    latest_us_valid = true;
    
    us_buf[us_write_idx] = dist;
    us_write_idx = (us_write_idx + 1) % US_BUFFER_SIZE;
    if (us_sample_count < US_BUFFER_SIZE) {
      us_sample_count++;
    }
    latest_us_filtered_cm = getFilteredDistance();
  }
}

void readMQ4() {
  if (millis() - last_mq_read >= interval_mq) {
    last_mq_read = millis();
    
    float voltage_v = readMQ4Voltage();
    
    // El ADC del ESP32 solo lee hasta ~3.3V, así que el chequeo contra
    // MQ4_VCC (5.0V) nunca se alcanza en la práctica; se deja el límite
    // real del ADC como única validación.
    if (voltage_v < 0.05 || voltage_v > 3.3) {
      return;
    }
    
    latest_rs = ((MQ4_VCC / voltage_v) - 1.0) * MQ4_RL;
  }
}

void readDHT() {
  if (millis() - last_dht_read >= DHT_READ_INTERVAL) {
    last_dht_read = millis();
    
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    
    if (!isnan(t) && !isnan(h) && t > -40.0 && t < 80.0 && h >= 0.0 && h <= 100.0) {
      dht_temp_buf[dht_write_idx] = t;
      dht_hum_buf[dht_write_idx] = h;
      dht_write_idx = (dht_write_idx + 1) % 5;
      if (dht_sample_count < 5) {
        dht_sample_count++;
      }
    }
  }
}

// ⭐ CORREGIDO: Diagnóstico detallado y detección de lecturas clavadas
void readCO2() {
  if (millis() - last_co2_read < CO2_READ_INTERVAL) return;
  last_co2_read = millis();

  int ppm = readMHZ19C();
  
  if (ppm >= 0) {
    // ⭐ DETECCIÓN DE LECTURAS CLAVADAS
    if (ppm == co2_last_value) {
      co2_stuck_count++;
      if (co2_stuck_count >= 3) {  // 3 lecturas idénticas consecutivas
        co2_stuck_detected = true;
        Serial.printf("{\"log\":\"MH-Z19C: lectura clavada en %d PPM (%d veces)\"}\n", ppm, co2_stuck_count);
      }
    } else {
      co2_stuck_count = 0;
      co2_stuck_detected = false;
    }
    co2_last_value = ppm;
    
    // ⭐ Si está clavado en 5000, es probable que haya error de comunicación
    if (ppm == 5000 && co2_stuck_detected) {
      Serial.println("{\"log\":\"MH-Z19C: 5000 PPM clavado = posible error de comunicación\"}");
      co2_error_count++;
      if (co2_error_count >= 3) {
        Serial.println("{\"log\":\"MH-Z19C: reiniciando UART...\"}");
        co2Serial.end();
        delay(100);
        co2Serial.begin(9600, SERIAL_8N1, MHZ19_RX_PIN, MHZ19_TX_PIN);
        co2_error_count = 0;
        co2_stuck_count = 0;
      }
      return;  // No actualizar latest_co2_ppm
    }
    
    // ⭐ Solo actualizar si no está clavado en 5000
    if (!(ppm == 5000 && co2_stuck_detected)) {
      latest_co2_ppm = ppm;
      co2_last_valid_ppm = ppm;
      co2_last_valid_time = millis();
      
      if (ppm >= 380 && ppm <= 450) {
        if (!co2_in_fresh_air) {
          co2_in_fresh_air = true;
          co2_fresh_air_start = millis();
          Serial.printf("{\"event\":\"co2_fresh_air\",\"status\":\"detected\",\"ppm\":%d}\n", ppm);
        }
      } else {
        co2_in_fresh_air = false;
        co2_fresh_air_start = 0;
      }
    }
  } else {
    // ⭐ Lectura fallida
    co2_error_count++;
    Serial.printf("{\"log\":\"MH-Z19C: lectura fallida (error %d)\"}\n", co2_error_count);
    
    if (co2_error_count >= 5) {
      Serial.println("{\"log\":\"MH-Z19C: múltiples fallos, reiniciando UART...\"}");
      co2Serial.end();
      delay(100);
      co2Serial.begin(9600, SERIAL_8N1, MHZ19_RX_PIN, MHZ19_TX_PIN);
      co2_error_count = 0;
    }
  }
}

bool isCO2Ready() {
  return (millis() >= CO2_WARMUP_MS);
}

// ⭐ CORREGIDO: Diagnóstico detallado de bytes
int readMHZ19C() {
  byte command[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
  byte response[9];

  // Limpiar buffer
  while (co2Serial.available() > 0) {
    co2Serial.read();
  }

  co2Serial.write(command, 9);
  co2Serial.flush();

  unsigned long start = millis();
  int index = 0;
  while (millis() - start < 300) {  // ⭐ Aumentado a 300ms
    if (co2Serial.available() > 0) {
      response[index++] = co2Serial.read();
      if (index == 9) break;
    }
  }

  if (index != 9) {
    Serial.printf("{\"log\":\"MH-Z19C: timeout, recibidos %d bytes de 9\"}\n", index);
    return -1;
  }
  
  if (response[0] != 0xFF || response[1] != 0x86) {
    Serial.printf("{\"log\":\"MH-Z19C: header inválido: 0x%02X 0x%02X (esperado 0xFF 0x86)\"}\n", 
                  response[0], response[1]);
    return -1;
  }
  
  if (getMHZ19Checksum(response) != response[8]) {
    Serial.printf("{\"log\":\"MH-Z19C: checksum incorrecto, calculado: 0x%02X, recibido: 0x%02X\"}\n",
                  getMHZ19Checksum(response), response[8]);
    return -1;
  }

  int ppm = (int)response[2] * 256 + (int)response[3];
  
  // ⭐ Log detallado para diagnóstico
  if (ppm == 5000) {
    Serial.printf("{\"log\":\"MH-Z19C: bytes crudos: %02X %02X %02X %02X %02X %02X %02X %02X %02X\"}\n",
                  response[0], response[1], response[2], response[3], response[4],
                  response[5], response[6], response[7], response[8]);
  }
  
  return ppm;
}

// ⭐ NUEVO: Diagnóstico completo del MH-Z19C
void diagnoseMHZ19C() {
  byte command[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
  byte response[9];
  
  Serial.println("{\"log\":\"MH-Z19C: enviando comando de lectura...\"}");
  
  while (co2Serial.available() > 0) {
    co2Serial.read();
  }
  
  co2Serial.write(command, 9);
  co2Serial.flush();
  
  unsigned long start = millis();
  int index = 0;
  while (millis() - start < 500) {
    if (co2Serial.available() > 0) {
      response[index++] = co2Serial.read();
      if (index == 9) break;
    }
  }
  
  if (index == 0) {
    Serial.println("{\"log\":\"MH-Z19C: ⚠️ SIN RESPUESTA - Verifica cables TX/RX y alimentación\"}");
    Serial.println("{\"log\":\"MH-Z19C: Cableado correcto: ESP32 RX (GPIO16) -> MH-Z19C TX, ESP32 TX (GPIO17) -> MH-Z19C RX\"}");
    return;
  }
  
  Serial.printf("{\"log\":\"MH-Z19C: %d bytes recibidos\"}\n", index);
  
  if (index < 9) {
    Serial.println("{\"log\":\"MH-Z19C: ⚠️ Respuesta incompleta\"}");
    return;
  }
  
  Serial.printf("{\"log\":\"MH-Z19C: Respuesta: %02X %02X %02X %02X %02X %02X %02X %02X %02X\"}\n",
                response[0], response[1], response[2], response[3], response[4],
                response[5], response[6], response[7], response[8]);
  
  if (response[0] == 0xFF && response[1] == 0x86) {
    int ppm = (int)response[2] * 256 + (int)response[3];
    Serial.printf("{\"log\":\"MH-Z19C: ✅ Comunicación OK, CO2: %d PPM\"}\n", ppm);
    
    if (ppm == 5000) {
      Serial.println("{\"log\":\"MH-Z19C: ⚠️ 5000 PPM = sensor en precalentamiento o error\"}");
    }
  } else {
    Serial.println("{\"log\":\"MH-Z19C: ⚠️ Respuesta inválida - sensor no responde correctamente\"}");
  }
}

bool sendMHZ19Command(byte commandId, byte dataByte) {
  byte packet[9] = {0xFF, 0x01, commandId, dataByte, 0x00, 0x00, 0x00, 0x00, 0x00};
  packet[8] = getMHZ19Checksum(packet);

  while (co2Serial.available() > 0) {
    co2Serial.read();
  }

  return co2Serial.write(packet, 9) == 9;
}

bool waitForMHZ19Response(byte expectedCmd, byte* response, unsigned long timeoutMs) {
  unsigned long start = millis();
  int index = 0;
  
  while (millis() - start < timeoutMs && index < 9) {
    if (co2Serial.available() > 0) {
      response[index++] = co2Serial.read();
    }
  }
  
  if (index != 9) return false;
  if (response[0] != 0xFF || response[1] != expectedCmd) return false;
  if (getMHZ19Checksum(response) != response[8]) return false;
  
  return true;
}

bool setMHZ19ABC(bool enabled, bool persistState, const char* source) {
  sendMHZ19Command(0x79, enabled ? 0xA0 : 0x00);
  
  byte response[9];
  bool confirmed = waitForMHZ19Response(0x79, response, 200);
  
  if (confirmed) {
    co2_abc_enabled = enabled;
    if (persistState) {
      preferences.begin("monitor", false);
      preferences.putBool("co2_abc", co2_abc_enabled);
      preferences.end();
    }
  }

  Serial.printf(
    "{\"event\":\"co2_abc\",\"status\":\"%s\",\"enabled\":%s,\"source\":\"%s\"}\n",
    confirmed ? "applied" : "uart_response_failed",
    enabled ? "true" : "false",
    source
  );

  return confirmed;
}

bool calibrateMHZ19Zero(const char* source) {
  if (!co2_in_fresh_air || (millis() - co2_fresh_air_start) < CO2_FRESH_AIR_MIN_MS) {
    unsigned long elapsed = co2_in_fresh_air ? (millis() - co2_fresh_air_start) : 0;
    Serial.printf(
      "{\"event\":\"co2_zero_calibration\",\"status\":\"rejected\",\"reason\":\"need_fresh_air_20min\",\"elapsed_ms\":%lu,\"required_ms\":%lu}\n",
      elapsed, CO2_FRESH_AIR_MIN_MS
    );
    return false;
  }
  
  bool sent = sendMHZ19Command(0x87, 0x00);
  
  byte response[9];
  bool confirmed = waitForMHZ19Response(0x87, response, 200);
  
  Serial.printf(
    "{\"event\":\"co2_zero_calibration\",\"status\":\"%s\",\"source\":\"%s\"}\n",
    (sent && confirmed) ? "confirmed" : "uart_failed",
    source
  );
  return sent && confirmed;
}

byte getMHZ19Checksum(const byte packet[]) {
  byte checksum = 0;
  for (int i = 1; i < 8; i++) {
    checksum += packet[i];
  }
  checksum = 0xFF - checksum;
  checksum += 1;
  return checksum;
}

void checkCalibration() {
  if (!is_calibrating) return;
  
  if (millis() - last_calibration_sample >= 500) {
    last_calibration_sample = millis();
    
    float voltage_v = readMQ4Voltage();
    
    if (voltage_v < 0.05 || voltage_v >= MQ4_VCC) {
      return;
    }
    
    float current_rs = ((MQ4_VCC / voltage_v) - 1.0) * MQ4_RL;
    calibration_rs_sum += current_rs;
    calibration_rs_sq_sum += current_rs * current_rs;
    calibration_samples++;
    
    Serial.printf(
      "{\"event\":\"mq4_calibrating\",\"sample\":%d,\"current_rs\":%.1f,\"voltage\":%.3f}\n",
      calibration_samples, current_rs, voltage_v
    );
    
    if (calibration_samples >= 100) {
      float avg_rs = calibration_rs_sum / (float)calibration_samples;
      float variance = (calibration_rs_sq_sum / (float)calibration_samples) - (avg_rs * avg_rs);
      float std_dev = variance > 0.0 ? sqrt(variance) : 0.0;
      float cv_pct = (avg_rs > 0.0) ? (std_dev / avg_rs) * 100.0 : 0.0;

      if (avg_rs < MQ4_CLEAN_AIR_RS_MIN || avg_rs > MQ4_CLEAN_AIR_RS_MAX) {
        Serial.printf(
          "{\"event\":\"mq4_calibration\",\"status\":\"FAILED\",\"reason\":\"not_clean_air\",\"avg_rs\":%.1f,\"expected_range\":\"%.0f-%.0f\"}\n",
          avg_rs, MQ4_CLEAN_AIR_RS_MIN, MQ4_CLEAN_AIR_RS_MAX
        );
        is_calibrating = false;
        return;
      }

      // ⭐ Rechaza calibraciones tomadas en condiciones inestables (corriente de
      // aire, movimiento cerca del sensor, gas presente durante el "aire limpio")
      if (cv_pct > 25.0) {
        Serial.printf(
          "{\"event\":\"mq4_calibration\",\"status\":\"FAILED\",\"reason\":\"lecturas_inestables\",\"cv_pct\":%.1f,\"maximo_permitido\":25.0}\n",
          cv_pct
        );
        is_calibrating = false;
        return;
      }
      
      // ⭐ Para MQ-4: Rs/R0 = 4.4 en aire limpio
      MQ4_R0 = avg_rs / 4.4;
      
      preferences.begin("monitor", false);
      preferences.putFloat("mq_r0", MQ4_R0);
      preferences.end();
      
      Serial.printf(
        "{\"event\":\"mq4_calibration\",\"status\":\"completed\",\"samples\":%d,\"duration_ms\":%lu,\"avg_rs\":%.2f,\"cv_pct\":%.1f,\"mq_r0\":%.2f}\n",
        calibration_samples,
        millis() - calibration_start,
        avg_rs,
        cv_pct,
        MQ4_R0
      );
      is_calibrating = false;
    }
  }
}

// ⭐ Inicia la captura de UN punto de calibración de curva con gas de
// concentración conocida (bolsa/cilindro de referencia, o comparación
// simultánea con un analizador de CH4 ya calibrado).
void addMQ4CalibrationPoint(float known_ppm) {
  if (known_ppm <= 0.0) {
    Serial.println("{\"event\":\"mq4_curve_point\",\"status\":\"rejected\",\"reason\":\"ppm_invalido\"}");
    return;
  }
  if (!isMQReady()) {
    Serial.println("{\"event\":\"mq4_curve_point\",\"status\":\"rejected\",\"reason\":\"warmup\"}");
    return;
  }
  if (is_calibrating || is_adding_cal_point) {
    Serial.println("{\"event\":\"mq4_curve_point\",\"status\":\"rejected\",\"reason\":\"otra_calibracion_en_curso\"}");
    return;
  }
  if (mq4_cal_point_count >= MQ4_CAL_MAX_POINTS) {
    Serial.printf(
      "{\"event\":\"mq4_curve_point\",\"status\":\"rejected\",\"reason\":\"buffer_lleno\",\"maximo\":%d}\n",
      MQ4_CAL_MAX_POINTS
    );
    return;
  }

  is_adding_cal_point = true;
  pending_cal_ppm = known_ppm;
  last_cal_point_sample = millis();
  cal_point_samples = 0;
  cal_point_rs_sum = 0.0;

  Serial.printf(
    "{\"event\":\"mq4_curve_point\",\"status\":\"started\",\"ppm_referencia\":%.1f,\"sample_interval_ms\":%lu,\"target_samples\":%d}\n",
    known_ppm, CAL_POINT_SAMPLE_INTERVAL_MS, CAL_POINT_TARGET_SAMPLES
  );
}

// ⭐ Muestreo no bloqueante de un punto de calibración (se llama desde loop())
void checkAddCalPoint() {
  if (!is_adding_cal_point) return;
  if (millis() - last_cal_point_sample < CAL_POINT_SAMPLE_INTERVAL_MS) return;
  last_cal_point_sample = millis();

  float voltage_v = readMQ4Voltage();
  if (voltage_v < 0.05 || voltage_v >= MQ4_VCC) return;

  float current_rs = ((MQ4_VCC / voltage_v) - 1.0) * MQ4_RL;
  cal_point_rs_sum += current_rs;
  cal_point_samples++;

  Serial.printf(
    "{\"event\":\"mq4_curve_point_sampling\",\"sample\":%d,\"rs\":%.1f}\n",
    cal_point_samples, current_rs
  );

  if (cal_point_samples >= CAL_POINT_TARGET_SAMPLES) {
    float avg_rs = cal_point_rs_sum / (float)cal_point_samples;
    float ratio = avg_rs / MQ4_R0;
    float compensated_ratio = ratio / getMQ4CompensationFactor();

    if (ratio <= 0.0 || compensated_ratio <= 0.0) {
      Serial.println("{\"event\":\"mq4_curve_point\",\"status\":\"failed\",\"reason\":\"ratio_invalido\"}");
      is_adding_cal_point = false;
      return;
    }

    mq4_cal_x[mq4_cal_point_count] = log(compensated_ratio);
    mq4_cal_y[mq4_cal_point_count] = log(pending_cal_ppm);
    mq4_cal_point_count++;

    Serial.printf(
      "{\"event\":\"mq4_curve_point\",\"status\":\"captured\",\"ppm_referencia\":%.1f,\"avg_rs\":%.2f,\"ratio\":%.4f,\"puntos_totales\":%d}\n",
      pending_cal_ppm, avg_rs, ratio, mq4_cal_point_count
    );
    is_adding_cal_point = false;
  }
}

// ⭐ Ajusta A y B de la curva PPM = A * ratio^B por regresión lineal en
// escala log-log sobre los puntos de referencia capturados. Reemplaza la
// curva genérica del datasheet por una curva propia del sensor + instalación.
void fitMQ4Curve() {
  int n = mq4_cal_point_count;
  if (n < 2) {
    Serial.printf(
      "{\"event\":\"mq4_curve_fit\",\"status\":\"rejected\",\"reason\":\"puntos_insuficientes\",\"puntos\":%d,\"minimo\":2}\n",
      n
    );
    return;
  }

  float sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
  for (int i = 0; i < n; i++) {
    sum_x += mq4_cal_x[i];
    sum_y += mq4_cal_y[i];
    sum_xy += mq4_cal_x[i] * mq4_cal_y[i];
    sum_xx += mq4_cal_x[i] * mq4_cal_x[i];
  }

  float mean_x = sum_x / n;
  float mean_y = sum_y / n;
  float denom = sum_xx - n * mean_x * mean_x;

  if (fabs(denom) < 1e-9) {
    Serial.println("{\"event\":\"mq4_curve_fit\",\"status\":\"rejected\",\"reason\":\"puntos_sin_variacion\"}");
    return;
  }

  float b = (sum_xy - n * mean_x * mean_y) / denom;
  float log_a = mean_y - b * mean_x;
  float a = exp(log_a);

  float ss_tot = 0.0, ss_res = 0.0;
  for (int i = 0; i < n; i++) {
    float y_pred = log_a + b * mq4_cal_x[i];
    ss_res += (mq4_cal_y[i] - y_pred) * (mq4_cal_y[i] - y_pred);
    ss_tot += (mq4_cal_y[i] - mean_y) * (mq4_cal_y[i] - mean_y);
  }
  float r_squared = (ss_tot > 1e-9) ? (1.0 - (ss_res / ss_tot)) : 1.0;

  mq4_curve_a = a;
  mq4_curve_b = b;
  mq4_curve_is_custom = true;

  preferences.begin("monitor", false);
  preferences.putFloat("mq4_ca", mq4_curve_a);
  preferences.putFloat("mq4_cb", mq4_curve_b);
  preferences.putBool("mq4_custom", true);
  preferences.end();

  Serial.printf(
    "{\"event\":\"mq4_curve_fit\",\"status\":\"completed\",\"puntos\":%d,\"a\":%.4f,\"b\":%.4f,\"r2\":%.4f}\n",
    n, mq4_curve_a, mq4_curve_b, r_squared
  );

  if (r_squared < 0.90) {
    Serial.println(
      "{\"log\":\"Advertencia: R2 menor a 0.90. Revisa las concentraciones de referencia y repite con puntos mas separados entre si.\"}"
    );
  }

  mq4_cal_point_count = 0;
}

// ⭐ Restaura la curva genérica del datasheet (rollback de seguridad)
void resetMQ4Curve() {
  mq4_curve_a = 1012.7;
  mq4_curve_b = -2.786;
  mq4_curve_is_custom = false;

  preferences.begin("monitor", false);
  preferences.putFloat("mq4_ca", mq4_curve_a);
  preferences.putFloat("mq4_cb", mq4_curve_b);
  preferences.putBool("mq4_custom", false);
  preferences.end();

  Serial.println("{\"event\":\"mq4_curve_reset\",\"status\":\"done\",\"a\":1012.70,\"b\":-2.786}");
}

void checkPendingReboot() {
  if (!reboot_pending) return;
  if (millis() - reboot_requested_at < REBOOT_GRACE_MS) return;

  reboot_pending = false;
  ESP.restart();
}

float getMedianDistance() {
  return getFilteredDistance();
}

float getFilteredDistance() {
  if (us_sample_count == 0) return US_MAX_CM;
  
  float temp[US_BUFFER_SIZE];
  for (int i = 0; i < us_sample_count; i++) {
    temp[i] = us_buf[i];
  }
  
  for (int i = 0; i < us_sample_count - 1; i++) {
    for (int j = 0; j < us_sample_count - i - 1; j++) {
      if (temp[j] > temp[j+1]) {
        float t = temp[j];
        temp[j] = temp[j+1];
        temp[j+1] = t;
      }
    }
  }
  
  if (us_sample_count < 5) {
    return temp[us_sample_count / 2];
  }

  float sum = 0.0;
  int count = 0;
  for (int i = 1; i < us_sample_count - 1; i++) {
    sum += temp[i];
    count++;
  }
  return count > 0 ? (sum / (float)count) : temp[us_sample_count / 2];
}

float getAverageTemperature() {
  float avg = 20.0;
  if (dht_sample_count == 0) {
    float t = dht.readTemperature();
    avg = isnan(t) ? 20.0 : t;
  } else {
    float sum = 0.0;
    for (int i = 0; i < dht_sample_count; i++) {
      sum += dht_temp_buf[i];
    }
    avg = sum / (float)dht_sample_count;
  }

  return avg + dht_temp_offset;
}

float getAverageHumidity() {
  float avg = 50.0;
  if (dht_sample_count == 0) {
    float h = dht.readHumidity();
    avg = isnan(h) ? 50.0 : h;
  } else {
    float sum = 0.0;
    for (int i = 0; i < dht_sample_count; i++) {
      sum += dht_hum_buf[i];
    }
    avg = sum / (float)dht_sample_count;
  }

  avg += dht_hum_offset;
  if (avg < 0.0) avg = 0.0;
  else if (avg > 100.0) avg = 100.0;

  return avg;
}

// ⭐ CONSTANTES CORRECTAS PARA MQ-4
float getMQ4PPM(float rs, float r0) {
  if (r0 <= 0.0) return 0.0;
  float ratio = rs / r0;
  if (ratio <= 0.0) return 0.0;
  
  float compensation = getMQ4CompensationFactor();
  float compensated_ratio = ratio / compensation;
  if (compensated_ratio <= 0.0) return 0.0;

  float ppm = mq4_curve_a * pow(compensated_ratio, mq4_curve_b);
  
  if (ppm < 0.0) ppm = 0.0;
  if (ppm > 15000.0) ppm = 15000.0;
  
  return ppm;
}

float getMQ4CompensationFactor() {
  float temp = getAverageTemperature();
  float hum = getAverageHumidity();

  float temp_factor = 1.0 + ((temp - 20.0) * 0.010);
  float hum_factor = 1.0 + ((hum - 50.0) * 0.0025);
  float factor = temp_factor * hum_factor;

  if (factor < 0.70) factor = 0.70;
  if (factor > 1.30) factor = 1.30;

  return factor;
}

float getReactorLevelPercent(float distance_cm) {
  float span = reactor_empty_cm - reactor_full_cm;
  if (span < 1.0) return 0.0;

  float level = ((reactor_empty_cm - distance_cm) / span) * 100.0;
  if (level < 0.0) level = 0.0;
  if (level > 100.0) level = 100.0;
  return level;
}

bool isMQReady() {
  return (millis() >= mq_warmup_ms);
}

void sendTelemetry() {
  float current_temp = getAverageTemperature();
  float current_hum = getAverageHumidity();
  float current_dist = getMedianDistance();
  float current_level_pct = getReactorLevelPercent(current_dist);
  float current_ppm = getMQ4PPM(latest_rs, MQ4_R0);
  bool mq_ready = isMQReady();
  bool co2_ready = isCO2Ready();
  bool gas_production_ok = (!mq_ready) ? true : (current_ppm >= GAS_MIN_PRODUCTION_PPM);
  bool is_wifi = (WiFi.status() == WL_CONNECTED);
  int rssi = is_wifi ? WiFi.RSSI() : 0;
  unsigned long uptime = millis() / 1000;
  String ip = is_wifi ? WiFi.localIP().toString() : "";
  
  // ⭐ CO2 se envía SIEMPRE, 0 si hay error
  int co2_to_send = latest_co2_ppm;
  if (co2_to_send < 0) co2_to_send = 0;
  
  String payload = "{";
  payload += "\"temp\":" + String(current_temp, 1) + ",";
  payload += "\"hum\":" + String(current_hum, 1) + ",";
  payload += "\"ppm\":" + String(current_ppm, 1) + ",";
  payload += "\"co2_ppm\":" + String(co2_to_send) + ",";
  payload += "\"dist\":" + String(current_dist, 1) + ",";
  payload += "\"level_pct\":" + String(current_level_pct, 1) + ",";
  payload += "\"mq_ready\":" + String(mq_ready ? "true" : "false") + ",";
  payload += "\"co2_ready\":" + String(co2_ready ? "true" : "false") + ",";
  payload += "\"gas_ok\":" + String(gas_production_ok ? "true" : "false") + ",";
  payload += "\"mq_r0\":" + String(MQ4_R0, 1) + ",";
  payload += "\"mq_rs\":" + String(latest_rs, 1) + ",";
  payload += "\"mq4_curve_a\":" + String(mq4_curve_a, 3) + ",";
  payload += "\"mq4_curve_b\":" + String(mq4_curve_b, 4) + ",";
  payload += "\"mq4_curve_custom\":" + String(mq4_curve_is_custom ? "true" : "false") + ",";
  payload += "\"mq4_curve_points\":" + String(mq4_cal_point_count) + ",";
  payload += "\"co2_abc\":" + String(co2_abc_enabled ? "true" : "false") + ",";
  payload += "\"dht_temp_offset\":" + String(dht_temp_offset, 2) + ",";
  payload += "\"dht_hum_offset\":" + String(dht_hum_offset, 2) + ",";
  payload += "\"mq_warmup_ms\":" + String(mq_warmup_ms) + ",";
  payload += "\"reactor_empty_cm\":" + String(reactor_empty_cm, 1) + ",";
  payload += "\"reactor_full_cm\":" + String(reactor_full_cm, 1) + ",";
  payload += "\"dist_raw_cm\":" + String(latest_us_raw_cm, 1) + ",";
  payload += "\"dist_valid\":" + String(latest_us_valid ? "true" : "false") + ",";
  payload += "\"us_offset_cm\":" + String(us_offset_cm, 2) + ",";
  payload += "\"us_scale\":" + String(us_scale, 4) + ",";
  payload += "\"wifi\":" + String(is_wifi ? "true" : "false") + ",";
  payload += "\"wifi_configured\":" + String(wifi_ssid.length() > 0 ? "true" : "false") + ",";
  payload += "\"wifi_reconnect_attempts\":" + String(wifi_reconnect_attempts) + ",";
  payload += "\"wifi_connected_since_s\":" + String(wifi_was_connected ? (millis() - wifi_connected_since) / 1000 : 0) + ",";
  payload += "\"rssi\":" + String(rssi) + ",";
  payload += "\"ip\":\"" + ip + "\",";
  payload += "\"server_url\":\"" + srv_url + "\",";
  payload += "\"uptime\":" + String(uptime);
  payload += "}";
  
  Serial.println(payload);
  
  if (is_wifi) {
    sendWiFiTelemetry(payload);
  }
}

void sendWiFiTelemetry(const String& jsonPayload) {
  HTTPClient http;
  http.begin(srv_url);
  http.setTimeout(1500);
  http.addHeader("Content-Type", "application/json");
  
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode > 0) {
    if (httpResponseCode == HTTP_CODE_OK) {
      String response = http.getString();
      parseWiFiResponse(response);
    } else {
      Serial.printf("{\"log\":\"HTTP POST response code: %d\"}\n", httpResponseCode);
    }
  } else {
    Serial.printf("{\"log\":\"Fallo HTTP POST: %s\"}\n", http.errorToString(httpResponseCode).c_str());
  }
  http.end();
}

void parseWiFiResponse(String response) {
  response = stripWhitespaceOutsideQuotes(response);
  
  int commandsIdx = response.indexOf("\"commands\":[");
  if (commandsIdx == -1) return;
  
  int startPos = commandsIdx + 11;
  int endPos = response.lastIndexOf("]");
  if (endPos == -1 || endPos <= startPos) return;
  
  String commandsList = response.substring(startPos + 1, endPos);
  
  int searchIdx = 0;
  while (searchIdx < commandsList.length()) {
    int openBrace = commandsList.indexOf('{', searchIdx);
    if (openBrace == -1) break;
    int closeBrace = commandsList.indexOf('}', openBrace);
    if (closeBrace == -1) break;
    
    String cmd = commandsList.substring(openBrace, closeBrace + 1);
    parseCommand(cmd);
    
    searchIdx = closeBrace + 1;
  }
}

// ⭐ Normaliza espacios SOLO fuera de comillas, para no destruir espacios
// legitimos dentro de valores (SSID, contraseña, URL). El indexOf literal de
// parseCommand asume "cmd":"led" sin espacios alrededor de los dos puntos,
// pero un SSID como "Wifi Medina" tiene que sobrevivir intacto.
String stripWhitespaceOutsideQuotes(const String& input) {
  String result;
  result.reserve(input.length());
  bool inQuotes = false;

  for (int i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    if (c == '"') {
      inQuotes = !inQuotes;
      result += c;
      continue;
    }
    if (c == '\r' || c == '\n') {
      continue;  // fin de linea, siempre se descarta
    }
    if (!inQuotes && c == ' ') {
      continue;  // espacio estructural (fuera de un valor entre comillas)
    }
    result += c;
  }
  return result;
}

void parseCommand(String cmd) {
  cmd = stripWhitespaceOutsideQuotes(cmd);
  
  if (cmd.indexOf("\"cmd\":\"led\"") != -1) {
    int stateIdx = cmd.indexOf("\"state\":");
    if (stateIdx != -1) {
      char stateChar = cmd.charAt(stateIdx + 8);
      int state = stateChar - '0';
      if (state == 1) {
        digitalWrite(STATUS_LED, HIGH);
        Serial.println("{\"log\":\"LED encendido manualmente.\"}");
      } else if (state == 0) {
        digitalWrite(STATUS_LED, LOW);
        Serial.println("{\"log\":\"LED apagado manualmente.\"}");
      }
    }
  } 
  else if (cmd.indexOf("\"cmd\":\"calibrate\"") != -1) {
    if (is_calibrating) {
      Serial.println("{\"event\":\"mq4_calibration\",\"status\":\"ignored\",\"reason\":\"already_running\"}");
    } else if (!isMQReady()) {
      Serial.printf(
        "{\"event\":\"mq4_calibration\",\"status\":\"rejected\",\"reason\":\"warmup\",\"uptime_ms\":%lu,\"required_warmup_ms\":%lu}\n",
        millis(),
        mq_warmup_ms
      );
    } else {
      is_calibrating = true;
      calibration_start = millis();
      last_calibration_sample = millis();
      calibration_samples = 0;
      calibration_rs_sum = 0.0;
      calibration_rs_sq_sum = 0.0;
      Serial.println(
        "{\"event\":\"mq4_calibration\",\"status\":\"started\",\"sample_interval_ms\":500,\"target_samples\":100,\"clean_air_ratio\":4.4}"
      );
    }
  }
  else if (cmd.indexOf("\"cmd\":\"add_calibration_point\"") != -1) {
    float known_ppm = getFloatValue(cmd, "\"ppm\":", -1.0);
    addMQ4CalibrationPoint(known_ppm);
  }
  else if (cmd.indexOf("\"cmd\":\"fit_calibration_curve\"") != -1) {
    fitMQ4Curve();
  }
  else if (cmd.indexOf("\"cmd\":\"reset_calibration_points\"") != -1) {
    mq4_cal_point_count = 0;
    Serial.println("{\"event\":\"mq4_curve_points\",\"status\":\"cleared\"}");
  }
  else if (cmd.indexOf("\"cmd\":\"reset_curve\"") != -1) {
    resetMQ4Curve();
  }
  else if (cmd.indexOf("\"cmd\":\"disable_abc\"") != -1) {
    setMHZ19ABC(false, true, "command");
  }
  else if (cmd.indexOf("\"cmd\":\"enable_abc\"") != -1) {
    setMHZ19ABC(true, true, "command");
  }
  else if (cmd.indexOf("\"cmd\":\"calibrate_co2_zero\"") != -1) {
    calibrateMHZ19Zero("command");
  }
  else if (cmd.indexOf("\"cmd\":\"diagnose_co2\"") != -1) {  // ⭐ NUEVO comando
    diagnoseMHZ19C();
  }
  else if (cmd.indexOf("\"cmd\":\"reboot\"") != -1) {
    reboot_pending = true;
    reboot_requested_at = millis();
    Serial.println("{\"event\":\"system_reboot\",\"status\":\"scheduled\",\"delay_ms\":250}");
  } 
  else if (cmd.indexOf("\"cmd\":\"set_intervals\"") != -1) {
    int mqIdx = cmd.indexOf("\"mq\":");
    int usIdx = cmd.indexOf("\"us\":");
    int outIdx = cmd.indexOf("\"out\":");
    
    if (mqIdx != -1 && usIdx != -1 && outIdx != -1) {
      unsigned long new_mq = getValueAfter(cmd, mqIdx + 5);
      unsigned long new_us = getValueAfter(cmd, usIdx + 5);
      unsigned long new_out = getValueAfter(cmd, outIdx + 5);
      
      if (new_mq >= 10 && new_us >= 10 && new_out >= 100) {
        interval_mq = new_mq;
        interval_us = new_us;
        interval_out = new_out;
        
        preferences.begin("monitor", false);
        preferences.putULong("int_mq", interval_mq);
        preferences.putULong("int_us", interval_us);
        preferences.putULong("int_out", interval_out);
        preferences.end();
        
        Serial.printf("{\"log\":\"Intervalos actualizados -> MQ:%lu, US:%lu, OUT:%lu\"}\n", interval_mq, interval_us, interval_out);
      }
    }
  } 
  else if (cmd.indexOf("\"cmd\":\"set_reactor_levels\"") != -1) {
    int emptyIdx = cmd.indexOf("\"empty\":");
    int fullIdx = cmd.indexOf("\"full\":");

    if (emptyIdx != -1 && fullIdx != -1) {
      unsigned long new_empty = getValueAfter(cmd, emptyIdx + 8);
      unsigned long new_full = getValueAfter(cmd, fullIdx + 7);

      if (new_empty > new_full && new_full >= US_MIN_CM && new_empty <= US_MAX_CM) {
        reactor_empty_cm = (float)new_empty;
        reactor_full_cm = (float)new_full;

        preferences.begin("monitor", false);
        preferences.putFloat("rx_empty", reactor_empty_cm);
        preferences.putFloat("rx_full", reactor_full_cm);
        preferences.end();

        Serial.printf("{\"log\":\"Calibracion reactor actualizada -> vacio:%0.1f cm, lleno:%0.1f cm\"}\n", reactor_empty_cm, reactor_full_cm);
      } else {
        Serial.println("{\"log\":\"Calibracion reactor invalida. Usa empty > full, dentro de 2 a 450 cm.\"}");
      }
    }
  }
  else if (cmd.indexOf("\"cmd\":\"set_distance_calibration\"") != -1) {
    float new_offset = getFloatValue(cmd, "\"offset\":", us_offset_cm);
    float new_scale = getFloatValue(cmd, "\"scale\":", us_scale);

    if (new_scale >= 0.5 && new_scale <= 1.5 && new_offset >= -100.0 && new_offset <= 100.0) {
      us_offset_cm = new_offset;
      us_scale = new_scale;

      preferences.begin("monitor", false);
      preferences.putFloat("us_offset", us_offset_cm);
      preferences.putFloat("us_scale", us_scale);
      preferences.end();

      Serial.printf("{\"log\":\"Calibracion AJ-SR04M actualizada -> offset:%0.2f cm, escala:%0.4f\"}\n", us_offset_cm, us_scale);
    } else {
      Serial.println("{\"log\":\"Calibracion AJ-SR04M invalida. Offset -100..100 cm, escala 0.5..1.5.\"}");
    }
  }
  else if (cmd.indexOf("\"cmd\":\"set_dht_offset\"") != -1) {
    float new_temp_offset = getFloatValue(cmd, "\"temp\":", dht_temp_offset);
    float new_hum_offset = getFloatValue(cmd, "\"hum\":", dht_hum_offset);

    if (new_temp_offset >= -15.0 && new_temp_offset <= 15.0 && new_hum_offset >= -30.0 && new_hum_offset <= 30.0) {
      dht_temp_offset = new_temp_offset;
      dht_hum_offset = new_hum_offset;

      preferences.begin("monitor", false);
      preferences.putFloat("dht_t_off", dht_temp_offset);
      preferences.putFloat("dht_h_off", dht_hum_offset);
      preferences.end();

      Serial.printf(
        "{\"event\":\"dht_offset\",\"status\":\"updated\",\"temp_offset\":%.2f,\"hum_offset\":%.2f}\n",
        dht_temp_offset,
        dht_hum_offset
      );
    } else {
      Serial.println("{\"event\":\"dht_offset\",\"status\":\"rejected\",\"reason\":\"out_of_range\"}");
    }
  }
  else if (cmd.indexOf("\"cmd\":\"set_wifi\"") != -1) {
    String new_ssid = getStringValue(cmd, "\"ssid\":");
    String new_pass = getStringValue(cmd, "\"pass\":");
    
    if (new_ssid.length() > 0) {
      preferences.begin("monitor", false);
      preferences.putString("wifi_ssid", new_ssid);
      preferences.putString("wifi_pass", new_pass);
      preferences.end();
      
      wifi_ssid = new_ssid;
      wifi_pass = new_pass;
      Serial.printf("{\"log\":\"SSID/PASS actualizado (%s). Conectando WiFi...\"}\n", new_ssid.c_str());
      WiFi.disconnect(false);
      setupWiFi();
    }
  } 
  else if (cmd.indexOf("\"cmd\":\"set_server\"") != -1) {
    String new_url = getStringValue(cmd, "\"url\":");
    if (new_url.length() > 0) {
      new_url.replace("\"", "");
      
      preferences.begin("monitor", false);
      preferences.putString("srv_url", new_url);
      preferences.end();
      
      srv_url = new_url;
      Serial.printf("{\"log\":\"URL de servidor actualizada: %s\"}\n", srv_url.c_str());
    }
  }
}

unsigned long getValueAfter(const String& str, int startPos) {
  String valStr = "";
  for (int i = startPos; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c >= '0' && c <= '9') {
      valStr += c;
    } else {
      break;
    }
  }
  return valStr.length() > 0 ? valStr.toInt() : 0;
}

float getFloatValue(const String& str, const String& key, float fallback) {
  int keyIdx = str.indexOf(key);
  if (keyIdx == -1) return fallback;

  int startPos = keyIdx + key.length();
  String valStr = "";
  for (int i = startPos; i < str.length(); i++) {
    char c = str.charAt(i);
    if ((c >= '0' && c <= '9') || c == '-' || c == '.') {
      valStr += c;
    } else {
      break;
    }
  }
  return valStr.length() > 0 ? valStr.toFloat() : fallback;
}

String getStringValue(const String& str, const String& key) {
  int keyIdx = str.indexOf(key);
  if (keyIdx == -1) return "";
  
  int startIdx = keyIdx + key.length();
  if (str.charAt(startIdx) == '"') {
    startIdx++;
  }
  
  int endIdx = -1;
  for (int i = startIdx; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == '"' || c == ',' || c == '}') {
      endIdx = i;
      break;
    }
  }
  
  if (endIdx != -1 && endIdx > startIdx) {
    return str.substring(startIdx, endIdx);
  }
  return "";
}