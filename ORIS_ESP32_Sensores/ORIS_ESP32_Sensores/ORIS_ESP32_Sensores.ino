/*
  AgroSmart - Firmware de ejemplo para ESP32 + sensores
  API local:
    GET  /api/sensores
    GET  /api/riego?estado=0|1

  IMPORTANTE:
  - Cambia SSID/CLAVE y calibra los sensores antes de usar lecturas.
  - NO conectes una bomba directamente a un GPIO.
  - Prueba inicialmente sin bomba conectada. El relé y su lógica varían;
    ajusta RELAY_ACTIVE_LOW a tu módulo.
  - ESP32 GPIO no tolera 5 V. El ECHO del HC-SR04 y cualquier salida
    analógica superior a 3.3 V necesitan adaptación de nivel/divisor.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <Wire.h>
#include <BH1750.h>

// ---------- Datos de Wi-Fi----------
const char* WIFI_SSID = "NOMBRE_DE_TU_WIFI";
const char* WIFI_PASSWORD = "CLAVE_DE_TU_WIFI";

// ---------- Pines para ESP32 DevKit ----------
#define DHT_PIN 4
#define DHT_TYPE DHT11
const int SOIL_PIN = 34;     // Salida analógica FC-28
const int MQ135_PIN = 35;    // Salida analógica del MQ-135
const int TRIG_PIN = 17;     // Trigger HC-SR04
const int ECHO_PIN = 18;     // Echo HC-SR04, requiere divisor a 3.3 V
const int RELAY_PIN = 26;    // Relé opcional; probar sin bomba al inicio

// Muchos relés son activos en LOW; cambiar a false si tu módulo es activo HIGH.
const bool RELAY_ACTIVE_LOW = true;
const int RELAY_ON = RELAY_ACTIVE_LOW ? LOW : HIGH;
const int RELAY_OFF = RELAY_ACTIVE_LOW ? HIGH : LOW;

// ---------- Calibración ----------
// Lee el FC-28 en seco y en suelo húmedo y reemplaza estos valores.
const int SOIL_DRY_RAW = 3000;
const int SOIL_WET_RAW = 1300;
// Altura útil del depósito en cm; ajusta a tu instalación.
const float TANK_HEIGHT_CM = 20.0;

DHT dht(DHT_PIN, DHT_TYPE);
BH1750 lightMeter;
WebServer server(80);
bool lightSensorAvailable = false;
bool relayRequestedOn = false;

String jsonNumberOrNull(float value, int decimals = 1) {
  if (!isfinite(value)) return "null";
  return String(value, decimals);
}

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // timeout de 30 ms para evitar bloquear demasiado tiempo
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return NAN;
  return (duration * 0.0343f) / 2.0f;
}

int soilMoisturePercent(int raw) {
  if (SOIL_DRY_RAW == SOIL_WET_RAW) return 0;
  float percentage = (float)(SOIL_DRY_RAW - raw) * 100.0f /
                     (float)(SOIL_DRY_RAW - SOIL_WET_RAW);
  if (percentage < 0) percentage = 0;
  if (percentage > 100) percentage = 100;
  return (int)roundf(percentage);
}

float waterLevelPercent(float distanceCm) {
  if (!isfinite(distanceCm) || TANK_HEIGHT_CM <= 0) return NAN;
  float percentage = (TANK_HEIGHT_CM - distanceCm) * 100.0f / TANK_HEIGHT_CM;
  if (percentage < 0) percentage = 0;
  if (percentage > 100) percentage = 100;
  return percentage;
}

void handleSensorData() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  int soilRaw = analogRead(SOIL_PIN);
  int soilPct = soilMoisturePercent(soilRaw);
  int airRaw = analogRead(MQ135_PIN);  // lectura ADC, NO equivale directamente a ppm

  float distanceCm = readDistanceCm();
  float waterPct = waterLevelPercent(distanceCm);

  float lux = NAN;
  if (lightSensorAvailable) {
    float measuredLux = lightMeter.readLightLevel();
    if (measuredLux >= 0) lux = measuredLux;
  }

  String json = "{";
  json += "\"temperature_c\":" + jsonNumberOrNull(temperature, 1) + ",";
  json += "\"humidity_pct\":" + jsonNumberOrNull(humidity, 1) + ",";
  json += "\"soil_moisture_pct\":" + String(soilPct) + ",";
  json += "\"soil_raw\":" + String(soilRaw) + ",";
  json += "\"water_level_pct\":" + jsonNumberOrNull(waterPct, 0) + ",";
  json += "\"water_distance_cm\":" + jsonNumberOrNull(distanceCm, 1) + ",";
  json += "\"air_raw\":" + String(airRaw) + ",";
  json += "\"light_lux\":" + jsonNumberOrNull(lux, 1) + ",";
  json += "\"relay_on\":" + String(relayRequestedOn ? "true" : "false");
  json += "}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleRelayRequest() {
  if (!server.hasArg("estado")) {
    server.send(400, "application/json",
                "{\"error\":\"Falta el parametro estado=0 o estado=1\"}");
    return;
  }

  String state = server.arg("estado");
  if (state != "0" && state != "1") {
    server.send(400, "application/json",
                "{\"error\":\"estado debe ser 0 o 1\"}");
    return;
  }

  relayRequestedOn = (state == "1");
  digitalWrite(RELAY_PIN, relayRequestedOn ? RELAY_ON : RELAY_OFF);

  String json = String("{\"ok\":true,\"relay_on\":") +
                (relayRequestedOn ? "true}" : "false}");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  relayRequestedOn = false;
  digitalWrite(RELAY_PIN, RELAY_OFF);  // estado seguro al arrancar

  analogReadResolution(12);  // ESP32 suele dar un rango ADC de 0 a 4095
  dht.begin();
  Wire.begin(21, 22);        // SDA=21, SCL=22 en muchas placas ESP32 DevKit
  lightSensorAvailable = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Wi-Fi conectado. IP del ESP32: http://");
  Serial.println(WiFi.localIP());
  Serial.println("Prueba esta ruta: http://IP_DEL_ESP32/api/sensores");

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain",
                "AgroSmart ESP32 OK. Usa /api/sensores");
  });
  server.on("/api/sensores", HTTP_GET, handleSensorData);
  server.on("/api/riego", HTTP_GET, handleRelayRequest);
  server.onNotFound([]() {
    server.send(404, "application/json", "{\"error\":\"Ruta no encontrada\"}");
  });
  server.begin();
  Serial.println("Servidor HTTP iniciado en el puerto 80.");
}

void loop() {
  server.handleClient();
  delay(2);
}
