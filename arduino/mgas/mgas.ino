#include <HardwareSerial.h>
#include <Preferences.h>
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
#define ALTURA_MAX_BIODIGESTOR 100.0
#define ALTURA_MIN_BIODIGESTOR 10.0

#define DHTPIN 23
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

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

// =====================================================
// INTERVALOS
// =====================================================
const unsigned long INTERVALO_LECTURA = 2000;   // Todo junto cada 2 segundos
const unsigned long TIEMPO_CALENTAMIENTO = 180000;

// =====================================================
// VARIABLES GLOBALES
// =====================================================
Preferences prefs;
Preferences memoria;

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

// =====================================================
// RELOJ
// =====================================================
String obtenerFechaHora() {
  struct tm *ptm = gmtime(&horaActual);
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
    prefs.putLong("ultima_hora", horaActual);
    Serial.print("✅ Hora configurada: ");
    Serial.println(obtenerFechaHora());
    return true;
  }
  Serial.println("❌ Formato inválido. Usa: YYYY,MM,DD,HH,MM,SS");
  return false;
}

void actualizarHora() {
  Serial.println("\n⚠️ Ingresa fecha y hora (YYYY,MM,DD,HH,MM,SS):");
  bool horaValida = false;
  while (!horaValida) {
    if (Serial.available()) {
      String entrada = Serial.readStringUntil('\n');
      entrada.trim();
      if (entrada.length() > 0) horaValida = procesarEntradaHora(entrada);
    }
    delay(50);
  }
}

// =====================================================
// MQ-4
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
  memoria.begin("mq4_calib", false);
  memoria.putFloat("R0", r0);
  memoria.end();
}

float cargarR0() {
  memoria.begin("mq4_calib", true);
  float v = memoria.getFloat("R0", NAN);
  memoria.end();
  return v;
}

void borrarR0() {
  memoria.begin("mq4_calib", false);
  memoria.clear();
  memoria.end();
  R0_KOHM = NAN;
  Serial.println("-> R0 borrado.");
}

void calibrarR0() {
  Serial.println("\n=== CALIBRACIÓN R0 (aire limpio) ===");
  for (int i = 5; i > 0; i--) {
    Serial.print(i); Serial.print("...");
    delay(1000);
  }
  Serial.println("\nMidiendo...");
  float rs = promedioRs(100, 200);
  if (isnan(rs) || rs <= 0) {
    Serial.println("❌ Error de lectura");
    return;
  }
  R0_KOHM = rs / FACTOR_AIRE_LIMPIO;
  guardarR0(R0_KOHM);
  Serial.print("✅ R0 guardado: ");
  Serial.print(R0_KOHM, 3);
  Serial.println(" kOhm\n");
}

// =====================================================
// MH-Z19C
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
  if (enCalentamiento) {
    Serial.println("⚠️ Espera a que termine el calentamiento");
    return;
  }
  Serial.println("⚠️ Calibrando Zero en 10 s (aire exterior)...");
  delay(10000);
  co2Serial.write(cmdZeroCalib, 9);
  delay(1000);
  Serial.println("✅ Calibración enviada\n");
}

void controlarABC(bool on) {
  if (enCalentamiento) {
    Serial.println("⚠️ Espera a que termine el calentamiento");
    return;
  }
  if (on) {
    co2Serial.write(cmdABC_On, 9);
    Serial.println("✅ ABC ACTIVADA\n");
  } else {
    co2Serial.write(cmdABC_Off, 9);
    Serial.println("🚫 ABC DESACTIVADA\n");
  }
  delay(1000);
}

// =====================================================
// HC-SR04
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
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  co2Serial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  for (int i = 0; i < TAMANO_FILTRO; i++) lecturasDistancia[i] = ALTURA_MAX_BIODIGESTOR;

  dht.begin();

  Serial.println("=========================================");
  Serial.println("  Sistema Completo - Salida en 1 línea");
  Serial.println("  MH-Z19C + MQ-4 + HC-SR04 + DHT11");
  Serial.println("=========================================");

  // Reloj
  prefs.begin("reloj", false);
  if (prefs.isKey("ultima_hora")) {
    horaActual = prefs.getLong("ultima_hora");
    Serial.print("✅ Hora cargada: ");
    Serial.println(obtenerFechaHora());
  } else {
    Serial.println("⚠️ PRIMERA VEZ - Configura fecha/hora (YYYY,MM,DD,HH,MM,SS):");
    bool ok = false;
    while (!ok) {
      if (Serial.available()) {
        String e = Serial.readStringUntil('\n');
        e.trim();
        if (e.length() > 0) ok = procesarEntradaHora(e);
      }
      delay(50);
    }
  }

  // R0
  R0_KOHM = cargarR0();
  if (!isnan(R0_KOHM) && R0_KOHM > 0) {
    Serial.print("-> R0 cargado: ");
    Serial.print(R0_KOHM, 3);
    Serial.println(" kOhm");
  } else {
    Serial.println("-> Sin R0. Usa 'c' para calibrar");
  }

  Serial.println("\nComandos: 2=Zero CO2 | 3=ABC Off | 4=ABC On | 5=Hora | c=Calibrar R0 | b=Borrar R0");
  
  inicioCalentamiento = millis();
  Serial.println("⏳ Calentando MH-Z19C (3 min)...\n");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  unsigned long ahora = millis();

  // Reloj
  if (ahora - ultimoSegundo >= 1000) {
    horaActual++;
    ultimoSegundo += 1000;
    segundosDesdeGuardado++;
    if (segundosDesdeGuardado >= 3600) {
      prefs.putLong("ultima_hora", horaActual);
      segundosDesdeGuardado = 0;
    }
  }

  // Calentamiento
  if (enCalentamiento) {
    if (ahora - inicioCalentamiento >= TIEMPO_CALENTAMIENTO) {
      enCalentamiento = false;
      Serial.println("✅ CO2 listo. Lecturas unificadas iniciadas.\n");
      ultimaLectura = ahora;
    } else {
      static unsigned long ultimoAviso = 0;
      if (ahora - ultimoAviso >= 30000) {
        Serial.print("   ⏳ Faltan ");
        Serial.print((TIEMPO_CALENTAMIENTO - (ahora - inicioCalentamiento)) / 1000);
        Serial.println(" s");
        ultimoAviso = ahora;
      }
    }
  }

  // ========== LECTURA UNIFICADA CADA 2 SEGUNDOS ==========
  if (!enCalentamiento && (ahora - ultimaLectura >= INTERVALO_LECTURA)) {
    ultimaLectura = ahora;
    contadorLecturas++;

    String fecha = obtenerFechaHora();

    // ----- 1. CO2 (formato original) -----
    int co2 = -1, tempCO2 = -1;
    bool okCO2 = leerCO2(co2, tempCO2);

    // ----- 2. MQ-4 (formato original) -----
    float ratio = NAN, ch4 = NAN, prop = NAN;
    String estadoGas = "SIN R0";
    if (!isnan(R0_KOHM) && R0_KOHM > 0) {
      float rs = promedioRs(5, 10);
      if (!isnan(rs)) {
        ratio = rs / R0_KOHM;
        ch4 = calcularPPM_CH4(rs);
        prop = calcularPPM_Propano(rs);
        if (ratio > 3.8)      estadoGas = "SEGURO (Aire limpio)";
        else if (ratio > 1.8) estadoGas = "PRECAUCIÓN (Gas detectado)";
        else if (ratio > 1.0) estadoGas = "ALERTA (Concentración alta)";
        else                  estadoGas = "PELIGRO (Fuga severa)";
      } else {
        estadoGas = "Lectura no válida";
      }
    }

    // ----- 3. HC-SR04 (formato original) -----
    float dist = -1, nivel = -1;
    String estadoNivel = "Error";
    float cruda = medirDistancia();
    if (cruda >= 0) {
      dist = aplicarFiltro(cruda);
      float rango = ALTURA_MAX_BIODIGESTOR - ALTURA_MIN_BIODIGESTOR;
      nivel = ((ALTURA_MAX_BIODIGESTOR - dist) / rango) * 100.0;
      if (nivel < 0) nivel = 0;
      if (nivel > 100) nivel = 100;

      if (nivel >= 90)      estadoNivel = "🔴 ¡ALERTA! NIVEL CRÍTICO";
      else if (nivel >= 75) estadoNivel = "🟠 Nivel Alto";
      else if (nivel >= 50) estadoNivel = "🟡 Nivel Medio";
      else if (nivel >= 25) estadoNivel = "🟢 Nivel Normal";
      else                  estadoNivel = "⚪ Nivel Bajo";
    }

    // ----- 4. DHT11 (formato original) -----
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    float hic = NAN;
    bool okDHT = !isnan(h) && !isnan(t);
    if (okDHT) hic = dht.computeHeatIndex(t, h, false);

    // ========== IMPRESIÓN EN UNA SOLA LÍNEA ==========
    // Conservando el formato original de cada sensor
    Serial.print("["); Serial.print(fecha); Serial.print("] #"); Serial.print(contadorLecturas);
    
    // CO2
    Serial.print(" | CO2: ");
    if (okCO2) {
      Serial.print(co2); Serial.print(" ppm | Temp: "); Serial.print(tempCO2); Serial.print(" °C");
    } else {
      Serial.print("ERROR");
    }

    // MQ-4
    Serial.print(" | Rs/R0: ");
    if (!isnan(ratio)) {
      Serial.print(ratio, 3);
      Serial.print(" | CH4: "); Serial.print(ch4, 1); Serial.print(" ppm");
      Serial.print(" | Propano: "); Serial.print(prop, 1); Serial.print(" ppm");
      Serial.print(" | Estado: "); Serial.print(estadoGas);
    } else {
      Serial.print("--- | CH4: --- | Propano: --- | Estado: "); Serial.print(estadoGas);
    }

    // HC-SR04
    Serial.print(" | 📏 Distancia: ");
    if (dist >= 0) {
      Serial.print(dist, 1); Serial.print(" cm | Nivel: ");
      Serial.print(nivel, 1); Serial.print("% | "); Serial.print(estadoNivel);
    } else {
      Serial.print("--- | Nivel: --- | "); Serial.print(estadoNivel);
    }

    // DHT11
    Serial.print(" | 💧 Humedad: ");
    if (okDHT) {
      Serial.print(h, 1); Serial.print(" % | 🌡️ Temp: ");
      Serial.print(t, 1); Serial.print(" °C | 🔥 Sensación: ");
      Serial.print(hic, 1); Serial.print(" °C");
    } else {
      Serial.print("--- % | 🌡️ Temp: --- °C | 🔥 Sensación: --- °C");
    }

    Serial.println();  // Fin de la línea única
  }

  // Comandos
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n' || c == ' ') return;
    switch (c) {
      case '2': calibrarZero(); break;
      case '3': controlarABC(false); break;
      case '4': controlarABC(true); break;
      case '5': actualizarHora(); break;
      case 'c': case 'C': calibrarR0(); break;
      case 'b': case 'B': borrarR0(); break;
      default: Serial.println("Comando no válido (2,3,4,5,c,b)");
    }
  }
}