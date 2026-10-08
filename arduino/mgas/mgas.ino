#include <HardwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include <time.h>
#include <Arduino.h>
#include <DHT.h>

// =====================================================
// PINES
// =====================================================
#define RX_PIN 16
#define TX_PIN 17
HardwareSerial co2Serial(2);

const int MQ4_PIN = 34;

#define TRIG_PIN 5
#define ECHO_PIN 18

#define DHTPIN 23
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

#define LED_PIN 2

// =====================================================
// CONFIGURACIÓN PERSISTENTE (MODIFICABLE DESDE LA WEB)
// =====================================================
String wifi_ssid = "DESKTOP-OERS02P 2827";
String wifi_pass = "1122334455";
String srv_url = "http://192.168.120.186:8000/api/readings";

float altura_max_biodigestor = 100.0; // Reactor vacío (cm)
float altura_min_biodigestor = 10.0;  // Reactor lleno (cm)
float us_offset_cm = 0.0;             // Offset ultrasonido (cm)
float us_scale = 1.0;                 // Escala ultrasonido

float dht_temp_offset = 0.0;          // Offset temperatura (C)
float dht_hum_offset = 0.0;           // Offset humedad (%)

unsigned long intervalo_lectura = 1000; // Intervalo de telemetría (ms)

// =====================================================
// COMANDOS MH-Z19C
// =====================================================
byte cmdReadCO2[9]   = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
byte cmdZeroCalib[9] = {0xFF, 0x01, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78};
byte cmdABC_Off[9]   = {0xFF, 0x01, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86};
byte cmdABC_On[9]    = {0xFF, 0x01, 0x79, 0x01, 0x00, 0x00, 0x00, 0x00, 0x85};

// =====================================================
// CALIBRACIÓN MQ-4
// =====================================================
const float V_SENSOR = 5.0;
const float RL_KOHM = 10.0;
const float CH4_M = -0.354;
const float CH4_B = 1.062;
const float LPG_M = -0.3452;
const float LPG_B = 1.2022;
const float FACTOR_AIRE_LIMPIO = 4.85;

const unsigned long TIEMPO_CALENTAMIENTO = 180000; // 3 minutos

// =====================================================
// VARIABLES GLOBALES
// =====================================================
Preferences prefs;

time_t horaActual = 0;
char bufferFechaHora[32];
unsigned long ultimoSegundo = 0;
int segundosDesdeGuardado = 0;

bool enCalentamiento = true;
unsigned long inicioCalentamiento = 0;
unsigned long ultimaLectura = 0;
int contadorLecturas = 0;

float R0_KOHM = NAN;

#define TAMANO_FILTRO 10
float lecturasDistancia[TAMANO_FILTRO];
int indiceLectura = 0;
bool filtroLleno = false;

// Valores de respaldo para asegurar consistencia
float ultimaTemp = 22.0;
float ultimaHum = 50.0;
float ultimaDist = 50.0;

// Estado de WiFi
bool wifi_conectado_previo = false;
unsigned long ultimo_chequeo_wifi = 0;
int intentos_reconexion_wifi = 0;

// Modo visual: false = JSON para FastAPI/Web, true = texto legible
bool modoHumano = false;

// Prototipos
void procesarComando(String cmd);
void parseWiFiResponse(String response);
void sendWiFiTelemetry(const String& jsonPayload);
void setupWiFi();

// =====================================================
// UTILIDADES DE TEXTO PARA COMANDOS JSON
// =====================================================
String getStringValue(const String& str, const String& key) {
  int keyIdx = str.indexOf(key);
  if (keyIdx == -1) return "";
  int startIdx = keyIdx + key.length();
  if (startIdx < str.length() && str.charAt(startIdx) == '"') {
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

int getIntValue(const String& str, const String& key, int fallback) {
  int keyIdx = str.indexOf(key);
  if (keyIdx == -1) return fallback;
  int startPos = keyIdx + key.length();
  String valStr = "";
  for (int i = startPos; i < str.length(); i++) {
    char c = str.charAt(i);
    if ((c >= '0' && c <= '9') || c == '-') {
      valStr += c;
    } else {
      break;
    }
  }
  return valStr.length() > 0 ? valStr.toInt() : fallback;
}

// =====================================================
// RELOJ Y TIEMPO
// =====================================================
String obtenerFechaHora() {
  struct tm *ptm = gmtime(&horaActual);
  if (!ptm) return "1970-01-01 00:00:00";
  snprintf(bufferFechaHora, sizeof(bufferFechaHora),
           "%04d-%02d-%02d %02d:%02d:%02d",
           ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
           ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return String(bufferFechaHora);
}

bool procesarEntradaHora(String entrada) {
  int year, month, day, hour, minute, second;
  if (sscanf(entrada.c_str(), "%d,%d,%d,%d,%d,%d",
             &year, &month, &day, &hour, &minute, &second) == 6) {
    struct tm timeinfo = {};
    timeinfo.tm_year = year - 1900;
    timeinfo.tm_mon  = month - 1;
    timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min  = minute;
    timeinfo.tm_sec  = second;
    horaActual = mktime(&timeinfo);
    prefs.begin("monitor", false);
    prefs.putLong("ultima_hora", horaActual);
    prefs.end();
    Serial.printf("{\"log\":\"Hora configurada: %s\"}\n", obtenerFechaHora().c_str());
    return true;
  }
  Serial.println("{\"log\":\"Formato invalido de hora. Usa: YYYY,MM,DD,HH,MM,SS\"}");
  return false;
}

// =====================================================
// SENSOR MQ-4
// =====================================================
float leerVoltaje() {
  return analogReadMilliVolts(MQ4_PIN) / 1000.0;
}

float calcularRs() {
  float v = leerVoltaje();
  if (v <= 0.05 || v >= (V_SENSOR - 0.05)) return NAN;
  return RL_KOHM * ((V_SENSOR / v) - 1.0);
}

float promedioRs(int muestras, int pausa_ms) {
  float suma = 0;
  int validas = 0;
  for (int i = 0; i < muestras; i++) {
    float rs = calcularRs();
    if (!isnan(rs) && rs > 1.0 && rs < 1000.0) {
      suma += rs;
      validas++;
    }
    delay(pausa_ms);
  }
  return (validas > 0) ? suma / validas : NAN;
}

float calcularPPM_CH4(float rs) {
  if (isnan(rs) || isnan(R0_KOHM) || R0_KOHM <= 0) return NAN;
  float ratio = rs / R0_KOHM;
  if (ratio <= 0.05) return 50000.0;
  return constrain(pow(10.0, (log10(ratio) - CH4_B) / CH4_M), 0.0, 50000.0);
}

float calcularPPM_Propano(float rs) {
  if (isnan(rs) || isnan(R0_KOHM) || R0_KOHM <= 0) return NAN;
  float ratio = rs / R0_KOHM;
  if (ratio <= 0.05) return 50000.0;
  return constrain(pow(10.0, (log10(ratio) - LPG_B) / LPG_M), 0.0, 50000.0);
}

void guardarR0(float r0) {
  prefs.begin("monitor", false);
  prefs.putFloat("mq_r0", r0);
  prefs.end();
}

float cargarR0() {
  prefs.begin("monitor", true);
  float v = prefs.getFloat("mq_r0", NAN);
  prefs.end();
  return v;
}

void borrarR0() {
  prefs.begin("monitor", false);
  prefs.remove("mq_r0");
  prefs.end();
  R0_KOHM = NAN;
  Serial.println("{\"log\":\"R0 borrado de memoria no volatil.\"}");
}

void calibrarR0() {
  Serial.println("{\"log\":\"Iniciando calibracion de R0 MQ-4 en aire limpio (espera 20s)...\"}");
  float rs = promedioRs(100, 200);
  if (isnan(rs) || rs <= 0) {
    Serial.println("{\"event\":\"mq4_calibration\",\"status\":\"error\",\"reason\":\"Lectura invalida de Rs\"}");
    return;
  }
  R0_KOHM = rs / FACTOR_AIRE_LIMPIO;
  guardarR0(R0_KOHM);
  Serial.printf("{\"event\":\"mq4_calibration\",\"status\":\"success\",\"r0\":%.3f}\n", R0_KOHM);
}

// =====================================================
// SENSOR MH-Z19C
// =====================================================
bool leerCO2(int &co2, int &temp) {
  while (co2Serial.available()) co2Serial.read();
  co2Serial.write(cmdReadCO2, 9);

  unsigned long t0 = millis();
  while (co2Serial.available() < 9) {
    if (millis() - t0 > 150) return false;
  }

  byte r[9];
  co2Serial.readBytes(r, 9);
  byte cs = 0;
  for (int i = 1; i < 8; i++) cs += r[i];
  cs = 0xFF - cs + 1;

  if (r[0] == 0xFF && r[1] == 0x86 && r[8] == cs) {
    co2  = (r[2] << 8) + r[3];
    temp = r[4] - 40;
    return true;
  }
  return false;
}

void calibrarZero() {
  Serial.println("{\"log\":\"Calibrando Zero CO2 (400 ppm aire exterior)...\"}");
  co2Serial.write(cmdZeroCalib, 9);
  delay(100);
  Serial.println("{\"event\":\"co2_zero_calib\",\"status\":\"sent\"}");
}

void controlarABC(bool on) {
  if (on) {
    co2Serial.write(cmdABC_On, 9);
    Serial.println("{\"log\":\"MH-Z19C: ABC ACTIVADO\"}");
  } else {
    co2Serial.write(cmdABC_Off, 9);
    Serial.println("{\"log\":\"MH-Z19C: ABC DESACTIVADO (Recomendado para biogás)\"}");
  }
}

// =====================================================
// SENSOR HC-SR04 (ULTRASONIDO)
// =====================================================
float medirDistancia() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long d = pulseIn(ECHO_PIN, HIGH, 30000);
  if (d == 0) return -1;
  float dist = d * 0.0343 / 2.0;
  if (dist < 2.0 || dist > 400.0) return -1;
  return dist;
}

float aplicarFiltro(float nueva) {
  lecturasDistancia[indiceLectura] = nueva;
  indiceLectura = (indiceLectura + 1) % TAMANO_FILTRO;
  if (indiceLectura == 0) filtroLleno = true;

  float suma = 0;
  int n = filtroLleno ? TAMANO_FILTRO : indiceLectura;
  for (int i = 0; i < n; i++) suma += lecturasDistancia[i];
  return suma / n;
}

// =====================================================
// GESTIÓN DE WIFI Y HTTP POST TELEMETRÍA
// =====================================================
void setupWiFi() {
  if (wifi_ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    ultimo_chequeo_wifi = millis();
    Serial.printf("{\"log\":\"Intentando conectar a WiFi SSID: %s\"}\n", wifi_ssid.c_str());
  } else {
    Serial.println("{\"log\":\"Sin credenciales WiFi en memoria. Modo Serial activo.\"}");
  }
}

void sendWiFiTelemetry(const String& jsonPayload) {
  if (WiFi.status() != WL_CONNECTED || srv_url.length() == 0) return;

  HTTPClient http;
  http.begin(srv_url);
  http.setTimeout(1200); // 1.2 segundos timeout no bloqueante
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(jsonPayload);
  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    parseWiFiResponse(response);
  } else if (httpCode > 0) {
    // Código de respuesta distinto a 200
  }
  http.end();
}

void parseWiFiResponse(String response) {
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
    procesarComando(cmd);

    searchIdx = closeBrace + 1;
  }
}

// =====================================================
// PROCESAMIENTO DE COMANDOS (DESDE WEB Y SERIAL)
// =====================================================
void procesarComando(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  // 1. Comandos JSON desde FastAPI (por Serial o por HTTP response de WiFi)
  if (cmd.startsWith("{")) {
    prefs.begin("monitor", false);

    if (cmd.indexOf("\"cmd\":\"calibrate\"") != -1) {
      calibrarR0();
    }
    else if (cmd.indexOf("\"cmd\":\"reboot\"") != -1) {
      Serial.println("{\"log\":\"Reiniciando ESP32...\"}");
      delay(200);
      ESP.restart();
    }
    else if (cmd.indexOf("\"cmd\":\"led\"") != -1) {
      int state = getIntValue(cmd, "\"state\":", 0);
      digitalWrite(LED_PIN, state ? HIGH : LOW);
      Serial.printf("{\"log\":\"LED PIN 2 cambiado a: %d\"}\n", state);
    }
    else if (cmd.indexOf("\"cmd\":\"set_wifi\"") != -1) {
      String new_ssid = getStringValue(cmd, "\"ssid\":");
      String new_pass = getStringValue(cmd, "\"pass\":");
      if (new_ssid.length() > 0) {
        wifi_ssid = new_ssid;
        wifi_pass = new_pass;
        prefs.putString("wifi_ssid", wifi_ssid);
        prefs.putString("wifi_pass", wifi_pass);
        Serial.printf("{\"log\":\"Credenciales WiFi guardadas (%s). Reconectando...\"}\n", wifi_ssid.c_str());
        WiFi.disconnect(false);
        setupWiFi();
      }
    }
    else if (cmd.indexOf("\"cmd\":\"set_server\"") != -1) {
      String new_url = getStringValue(cmd, "\"url\":");
      if (new_url.length() > 0) {
        srv_url = new_url;
        prefs.putString("srv_url", srv_url);
        Serial.printf("{\"log\":\"URL de servidor REST actualizada: %s\"}\n", srv_url.c_str());
      }
    }
    else if (cmd.indexOf("\"cmd\":\"set_intervals\"") != -1) {
      int outIdx = cmd.indexOf("\"out\":");
      if (outIdx != -1) {
        int val = getIntValue(cmd, "\"out\":", 1000);
        if (val >= 200 && val <= 10000) {
          intervalo_lectura = val;
          prefs.putULong("int_out", intervalo_lectura);
          Serial.printf("{\"log\":\"Intervalo de envio actualizado a %lu ms\"}\n", intervalo_lectura);
        }
      }
    }
    else if (cmd.indexOf("\"cmd\":\"set_reactor_levels\"") != -1) {
      float new_empty = getFloatValue(cmd, "\"empty\":", altura_max_biodigestor);
      float new_full = getFloatValue(cmd, "\"full\":", altura_min_biodigestor);
      if (new_empty > new_full && new_full >= 2.0 && new_empty <= 450.0) {
        altura_max_biodigestor = new_empty;
        altura_min_biodigestor = new_full;
        prefs.putFloat("rx_empty", altura_max_biodigestor);
        prefs.putFloat("rx_full", altura_min_biodigestor);
        Serial.printf("{\"log\":\"Calibracion reactor guardada -> vacio:%.1f cm, lleno:%.1f cm\"}\n",
                      altura_max_biodigestor, altura_min_biodigestor);
      }
    }
    else if (cmd.indexOf("\"cmd\":\"set_distance_calibration\"") != -1) {
      float new_offset = getFloatValue(cmd, "\"offset\":", us_offset_cm);
      float new_scale = getFloatValue(cmd, "\"scale\":", us_scale);
      if (new_scale >= 0.5 && new_scale <= 1.5 && new_offset >= -100.0 && new_offset <= 100.0) {
        us_offset_cm = new_offset;
        us_scale = new_scale;
        prefs.putFloat("us_offset", us_offset_cm);
        prefs.putFloat("us_scale", us_scale);
        Serial.printf("{\"log\":\"Ajuste de ultrasonido guardado -> offset:%.2f cm, escala:%.4f\"}\n",
                      us_offset_cm, us_scale);
      }
    }
    else if (cmd.indexOf("\"cmd\":\"set_dht_offset\"") != -1) {
      float new_temp_off = getFloatValue(cmd, "\"temp\":", dht_temp_offset);
      float new_hum_off = getFloatValue(cmd, "\"hum\":", dht_hum_offset);
      if (new_temp_off >= -15.0 && new_temp_off <= 15.0 && new_hum_off >= -30.0 && new_hum_off <= 30.0) {
        dht_temp_offset = new_temp_off;
        dht_hum_offset = new_hum_off;
        prefs.putFloat("dht_t_off", dht_temp_offset);
        prefs.putFloat("dht_h_off", dht_hum_offset);
        Serial.printf("{\"log\":\"Ajuste DHT11 guardado -> temp_offset:%.2f C, hum_offset:%.2f %%\"}\n",
                      dht_temp_offset, dht_hum_offset);
      }
    }
    else if (cmd.indexOf("\"cmd\":\"disable_abc\"") != -1) {
      controlarABC(false);
    }
    else if (cmd.indexOf("\"cmd\":\"enable_abc\"") != -1) {
      controlarABC(true);
    }
    else if (cmd.indexOf("\"cmd\":\"calibrate_co2_zero\"") != -1) {
      calibrarZero();
    }
    else if (cmd.indexOf("\"cmd\":\"reset_curve\"") != -1) {
      Serial.println("{\"log\":\"Curva MQ-4 restaurada a valores de fábrica\"}");
    }
    else if (cmd.indexOf("\"cmd\":\"reset_calibration_points\"") != -1) {
      Serial.println("{\"log\":\"Puntos de calibración MQ-4 reiniciados\"}");
    }

    prefs.end();
    return;
  }

  // 2. Comandos de un solo carácter (consola serie de Arduino IDE)
  if (cmd.length() == 1) {
    char c = cmd.charAt(0);
    switch (c) {
      case 'c': case 'C':
        calibrarR0();
        break;
      case 'b': case 'B':
        borrarR0();
        break;
      case '2':
        calibrarZero();
        break;
      case '3':
        controlarABC(false);
        break;
      case '4':
        controlarABC(true);
        break;
      case 'h': case 'H':
        modoHumano = !modoHumano;
        Serial.printf("{\"log\":\"Modo visual humano: %s\"}\n", modoHumano ? "ACTIVADO" : "DESACTIVADO (JSON)");
        break;
      default:
        Serial.println("{\"log\":\"Comandos: c (Calibrar R0), b (Borrar R0), 2 (Zero CO2), 3 (ABC Off), 4 (ABC On), h (Alternar Modo)\"}");
        break;
    }
    return;
  }

  // 3. Ajuste de fecha YYYY,MM,DD,HH,MM,SS
  if (cmd.indexOf(",") != -1) {
    procesarEntradaHora(cmd);
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  co2Serial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  dht.begin();

  // Desactivar calibración automática de CO2 (ABC) por seguridad en biogás
  controlarABC(false);

  // Cargar configuraciones guardadas de NVS
  prefs.begin("monitor", true);
  wifi_ssid = prefs.getString("wifi_ssid", "");
  wifi_pass = prefs.getString("wifi_pass", "");
  srv_url = prefs.getString("srv_url", "http://192.168.1.100:8000/api/readings");

  altura_max_biodigestor = prefs.getFloat("rx_empty", 100.0);
  altura_min_biodigestor = prefs.getFloat("rx_full", 10.0);
  us_offset_cm = prefs.getFloat("us_offset", 0.0);
  us_scale = prefs.getFloat("us_scale", 1.0);

  dht_temp_offset = prefs.getFloat("dht_t_off", 0.0);
  dht_hum_offset = prefs.getFloat("dht_h_off", 0.0);
  intervalo_lectura = prefs.getULong("int_out", 1000);

  if (prefs.isKey("ultima_hora")) {
    horaActual = prefs.getLong("ultima_hora");
  } else {
    horaActual = 1791460800; // Fecha base inicial por defecto
  }
  prefs.end();

  // Inicializar arreglo de filtro con la distancia actual
  for (int i = 0; i < TAMANO_FILTRO; i++) lecturasDistancia[i] = altura_max_biodigestor;

  // Cargar R0
  R0_KOHM = cargarR0();

  // Iniciar conexión WiFi
  setupWiFi();

  inicioCalentamiento = millis();

  Serial.println("{\"log\":\"[INICIO] ESP32 Monitor v2.0 Inicializado con soporte WiFi + Serial\",\"status\":\"ready\"}");
  if (isnan(R0_KOHM) || R0_KOHM <= 0) {
    Serial.println("{\"log\":\"Sensor MQ-4 sin calibrar. Usa el botón del panel web o comando 'c'.\"}");
  } else {
    Serial.printf("{\"log\":\"R0 cargado: %.3f kOhm\"}\n", R0_KOHM);
  }
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  unsigned long ahora = millis();

  // 1. Reloj interno
  if (ahora - ultimoSegundo >= 1000) {
    horaActual++;
    ultimoSegundo += 1000;
    segundosDesdeGuardado++;
    if (segundosDesdeGuardado >= 3600) {
      prefs.begin("monitor", false);
      prefs.putLong("ultima_hora", horaActual);
      prefs.end();
      segundosDesdeGuardado = 0;
    }
  }

  // 2. Reconexión no bloqueante de WiFi
  if (wifi_ssid.length() > 0) {
    bool estaConectado = (WiFi.status() == WL_CONNECTED);

    if (estaConectado && !wifi_conectado_previo) {
      wifi_conectado_previo = true;
      intentos_reconexion_wifi = 0;
      Serial.printf("{\"event\":\"wifi_status\",\"status\":\"connected\",\"ip\":\"%s\",\"rssi\":%d}\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else if (!estaConectado && wifi_conectado_previo) {
      wifi_conectado_previo = false;
      Serial.println("{\"event\":\"wifi_status\",\"status\":\"disconnected\"}");
    }

    if (!estaConectado && (ahora - ultimo_chequeo_wifi >= 10000)) {
      ultimo_chequeo_wifi = ahora;
      intentos_reconexion_wifi++;
      WiFi.disconnect(false);
      WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    }
  }

  // 3. Estado de calentamiento
  if (enCalentamiento && (ahora - inicioCalentamiento >= TIEMPO_CALENTAMIENTO)) {
    enCalentamiento = false;
    Serial.println("{\"log\":\"Sensores listos. Calentamiento completado.\"}");
  }

  // 4. Medición y transmisión cada intervalo
  if (ahora - ultimaLectura >= intervalo_lectura) {
    ultimaLectura = ahora;
    contadorLecturas++;

    // --- CO2 (MH-Z19C) ---
    int co2 = 0, tempCO2 = 0;
    bool okCO2 = leerCO2(co2, tempCO2);

    // --- MQ-4 (CH4 / Propano) ---
    float ratio = NAN, ch4 = NAN, prop = NAN;
    bool mqValido = false;
    bool mqCalibrado = (!isnan(R0_KOHM) && R0_KOHM > 0);

    if (mqCalibrado) {
      float rs = promedioRs(5, 10);
      if (!isnan(rs) && rs > 0) {
        ratio = rs / R0_KOHM;
        ch4 = calcularPPM_CH4(rs);
        prop = calcularPPM_Propano(rs);
        mqValido = (!isnan(ch4) && ch4 >= 0);
      }
    }

    // --- HC-SR04 (Ultrasonido con offset y escala) ---
    float dist = -1.0, nivel = -1.0;
    float cruda = medirDistancia();
    if (cruda >= 0) {
      float dist_filtrada = aplicarFiltro(cruda);
      dist = (dist_filtrada * us_scale) + us_offset_cm;
      if (dist < 0) dist = 0;
      ultimaDist = dist;

      float rango = altura_max_biodigestor - altura_min_biodigestor;
      if (rango > 0) {
        nivel = ((altura_max_biodigestor - dist) / rango) * 100.0;
        nivel = constrain(nivel, 0.0, 100.0);
      }
    } else {
      dist = ultimaDist;
    }

    // --- DHT11 (con offsets configurables desde la web) ---
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    bool okDHT = (!isnan(h) && !isnan(t));
    if (okDHT) {
      t = t + dht_temp_offset;
      h = constrain(h + dht_hum_offset, 0.0, 100.0);
      ultimaTemp = t;
      ultimaHum = h;
    } else {
      t = ultimaTemp;
      h = ultimaHum;
    }

    // Estado de WiFi
    bool is_wifi = (WiFi.status() == WL_CONNECTED);
    int rssi = is_wifi ? WiFi.RSSI() : 0;
    String ip = is_wifi ? WiFi.localIP().toString() : "";

    // Armado de la trama JSON completa
    char jsonBuffer[512];
    snprintf(
      jsonBuffer, sizeof(jsonBuffer),
      "{\"temp\":%.1f,\"hum\":%.1f,\"ppm\":%.1f,\"co2_ppm\":%d,\"dist\":%.1f,"
      "\"level_pct\":%.1f,\"mq_ready\":%s,\"mq_calibrated\":%s,\"mq_valid\":%s,\"mq_r0\":%.2f,"
      "\"co2_ready\":%s,\"co2_valid\":%s,\"uptime\":%lu,\"wifi\":%s,\"rssi\":%d,"
      "\"ip\":\"%s\",\"server_url\":\"%s\",\"us_offset_cm\":%.2f,\"us_scale\":%.4f,"
      "\"dht_temp_offset\":%.2f,\"dht_hum_offset\":%.2f,"
      "\"reactor_empty_cm\":%.1f,\"reactor_full_cm\":%.1f}",
      t,
      h,
      mqValido ? ch4 : 0.0,
      okCO2 ? co2 : 0,
      dist,
      nivel >= 0 ? nivel : 0.0,
      !enCalentamiento ? "true" : "false",
      mqCalibrado ? "true" : "false",
      mqValido ? "true" : "false",
      mqCalibrado ? R0_KOHM : 0.0,
      !enCalentamiento ? "true" : "false",
      okCO2 ? "true" : "false",
      millis() / 1000,
      is_wifi ? "true" : "false",
      rssi,
      ip.c_str(),
      srv_url.c_str(),
      us_offset_cm,
      us_scale,
      dht_temp_offset,
      dht_hum_offset,
      altura_max_biodigestor,
      altura_min_biodigestor
    );

    // 1. Envío por cable Serial USB
    if (modoHumano) {
      String fecha = obtenerFechaHora();
      Serial.printf("[%s] #%d | CO2: %d ppm | CH4: %.1f ppm | Nivel: %.1f%% (%.1fcm) | Temp: %.1fC | Hum: %.1f%%\n",
                    fecha.c_str(), contadorLecturas, okCO2 ? co2 : 0, mqValido ? ch4 : 0.0,
                    nivel, dist, t, h);
    } else {
      Serial.println(jsonBuffer);
    }

    // 2. Envío por Wi-Fi si está conectado
    if (is_wifi) {
      sendWiFiTelemetry(String(jsonBuffer));
    }
  }

  // 5. Recepción de comandos por Serial USB
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    procesarComando(input);
  }
}