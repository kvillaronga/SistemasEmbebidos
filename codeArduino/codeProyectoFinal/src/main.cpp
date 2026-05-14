/*
 * ============================================================
 *  SISTEMA DE RIEGO AUTOMÁTICO CON TECHO MÓVIL + OLED
 *  Controlador: ESP32-WROOM
 * ============================================================
 *  Sensores  : DHT11 (temperatura + humedad aire)
 *              YL-100 (humedad sustrato – salida analógica)
 *  Actuadores: Servo SG90 (techo – GPIO 25, PWM)
 *              Relé 1 canal (bomba de agua – GPIO 26)
 *  Display   : OLED SSD1306 0.96" 128x64 I2C
 *  IoT       : ThingSpeak vía MQTT (PubSubClient)
 *
 *  Librerías necesarias:
 *    - DHT sensor library  (Adafruit)
 *    - Adafruit Unified Sensor
 *    - Adafruit SSD1306
 *    - Adafruit GFX Library
 *    - ESP32Servo
 *    - PubSubClient (knolleary)
 *
 *  MAPA DE PINES:
 *    GPIO 4  → DHT11  DATA
 *    GPIO 34 → YL-100 AOUT (solo entrada ADC)
 *    GPIO 26 → Relé   IN   (trigger LOW → activa bomba)
 *    GPIO 25 → Servo  SEÑAL (PWM 50 Hz)
 *    GPIO 21 → OLED   SDA  (I2C)
 *    GPIO 22 → OLED   SCL  (I2C)
 *    3.3V    → DHT11 VCC, YL-100 VCC, OLED VCC
 *    5V      → Servo VCC, Relé VCC
 *    GND     → todos los GND
 *    Bomba alimentada a 6V desde fuente externa
 * ============================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ─── OLED ─────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── CONFIGURACIÓN Wi-Fi y ThingSpeak ─────────────────────
const char* WIFI_SSID    = "APTO_303_PLUS";
const char* WIFI_PASS    = "12352399031";

const char* MQTT_BROKER  = "mqtt3.thingspeak.com";
const int   MQTT_PORT    = 1883;
const char* MQTT_USER    = "HQkrLBsnMTwYGRc2JCc8BSU";
const char* MQTT_PASS    = "FTbF6Rr2h14YenrViSkMjxyk";
const char* MQTT_CLIENT  = "HQkrLBsnMTwYGRc2JCc8BSU";
const char* MQTT_CHANNEL = "channels/3382521/publish";

// ─── PINES ────────────────────────────────────────────────
#define PIN_DHT   4
#define PIN_YL100 34
#define PIN_RELE  26
#define PIN_SERVO 25

// ─── UMBRALES ─────────────────────────────────────────────
const float TEMP_CIERRA_TECHO = 28.0;
const float TEMP_ABRE_TECHO   = 25.0;

const int ADC_SUELO_SECO   = 2800;
const int ADC_SUELO_HUMEDO = 1500;

const unsigned long TIEMPO_MIN_BOMBA = 5000UL;

// ─── SERVO ────────────────────────────────────────────────
const int SERVO_ABIERTO = 0;
const int SERVO_CERRADO = 90;

// ─── INTERVALOS ───────────────────────────────────────────
const unsigned long INTERVALO_SENSOR = 5000UL;
const unsigned long INTERVALO_IOT    = 15000UL;
const unsigned long INTERVALO_OLED   = 2000UL;  // refresco pantalla

// ─── OBJETOS ──────────────────────────────────────────────
DHT dht(PIN_DHT, DHT11);
Servo servoTecho;
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ─── VARIABLES DE ESTADO ──────────────────────────────────
float temperatura     = 0.0;
float humedadAire     = 0.0;
int   adcSustrato     = 0;
float humedadSustrato = 0.0;
bool  bombaActiva     = false;
bool  techoCerrado    = false;

unsigned long tUltimaSensor = 0;
unsigned long tUltimaIot    = 0;
unsigned long tUltimaOled   = 0;

// Pantalla: alterna entre dos páginas de info
int  paginaOled = 0;
unsigned long tCambioPagina = 0;
const unsigned long INTERVALO_PAGINA = 4000UL;

unsigned long tInicioBomba = 0;

// ─── PROTOTIPOS ───────────────────────────────────────────
void conectarWifi();
void conectarMqtt();
void leerSensores();
void controlarTecho();
void controlarRiego();
void activarBomba();
void desactivarBomba();
void publicarThingSpeak();
int  adcAHumedad(int adcVal);
void iniciarOled();
void mostrarPantalla();
void paginaPrincipal();
void paginaEstado();
void barraProgreso(int x, int y, int ancho, int alto, int porcentaje);

// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Sistema de Riego Automático ===");

  // Relé apagado al inicio
  pinMode(PIN_RELE, OUTPUT);
  digitalWrite(PIN_RELE, HIGH);

  // Servo en posición abierta
  servoTecho.attach(PIN_SERVO, 500, 2400);
  servoTecho.write(SERVO_ABIERTO);
  techoCerrado = false;
  delay(500);

  // DHT11
  dht.begin();

  // OLED
  iniciarOled();

  // Wi-Fi y MQTT
  conectarWifi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
}

// ════════════════════════════════════════════════════════════
void loop() {
  if (!mqttClient.connected()) conectarMqtt();
  mqttClient.loop();

  unsigned long ahora = millis();

  // ── Sensores y control ────────────────────────────────
  if (ahora - tUltimaSensor >= INTERVALO_SENSOR) {
    tUltimaSensor = ahora;
    leerSensores();
    controlarTecho();
    controlarRiego();

    Serial.printf("[Sensores] Temp: %.1f°C | H.aire: %.0f%% | ADC: %d | H.suelo: %.0f%%\n",
                  temperatura, humedadAire, adcSustrato, humedadSustrato);
    Serial.printf("[Control]  Bomba: %s | Techo: %s\n",
                  bombaActiva ? "ON" : "OFF",
                  techoCerrado ? "CERRADO" : "ABIERTO");
  }

  // ── Pantalla OLED ─────────────────────────────────────
  if (ahora - tUltimaOled >= INTERVALO_OLED) {
    tUltimaOled = ahora;
    // Cambiar página cada INTERVALO_PAGINA ms
    if (ahora - tCambioPagina >= INTERVALO_PAGINA) {
      tCambioPagina = ahora;
      paginaOled = (paginaOled + 1) % 2;
    }
    mostrarPantalla();
  }

  // ── IoT ───────────────────────────────────────────────
  if (ahora - tUltimaIot >= INTERVALO_IOT) {
    tUltimaIot = ahora;
    publicarThingSpeak();
  }
}

// ════════════════════════════════════════════════════════════
//  OLED
// ════════════════════════════════════════════════════════════

void iniciarOled() {
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("[OLED] Error al iniciar – continúa sin pantalla");
    return;
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  // Pantalla de bienvenida
  oled.setTextSize(1);
  oled.setCursor(20, 10);
  oled.println("Sistema de Riego");
  oled.setCursor(30, 24);
  oled.println("Automatico v2");
  oled.drawRect(0, 0, 128, 64, SSD1306_WHITE);
  oled.drawLine(0, 40, 128, 40, SSD1306_WHITE);
  oled.setCursor(10, 48);
  oled.println("Iniciando...");
  oled.display();
  delay(2000);
  oled.clearDisplay();
}

void mostrarPantalla() {
  oled.clearDisplay();
  if (paginaOled == 0)
    paginaPrincipal();
  else
    paginaEstado();
  oled.display();
}

// ── Página 1: temperaturas y humedades con barras ─────────
void paginaPrincipal() {
  // Cabecera
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("Temp:");
  oled.setTextSize(1);
  oled.setCursor(36, 0);
  oled.printf("%.1f C", temperatura);

  oled.setCursor(80, 0);
  oled.print("1/2");

  // Línea divisoria
  oled.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Humedad del aire
  oled.setTextSize(1);
  oled.setCursor(0, 14);
  oled.print("Aire:");
  oled.setCursor(36, 14);
  oled.printf("%.0f%%", humedadAire);
  barraProgreso(0, 24, 128, 7, (int)humedadAire);

  // Humedad sustrato
  oled.setCursor(0, 35);
  oled.print("Suelo:");
  oled.setCursor(40, 35);
  oled.printf("%.0f%%", humedadSustrato);
  barraProgreso(0, 45, 128, 7, (int)humedadSustrato);

  // Indicador de página (puntos abajo)
  oled.fillCircle(60, 60, 2, SSD1306_WHITE);
  oled.drawCircle(68, 60, 2, SSD1306_WHITE);
}

// ── Página 2: estado de actuadores y Wi-Fi ────────────────
void paginaEstado() {
  oled.setTextSize(1);

  // Cabecera
  oled.setCursor(0, 0);
  oled.print("Estado sistema");
  oled.setCursor(104, 0);
  oled.print("2/2");
  oled.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Bomba
  oled.setCursor(0, 15);
  oled.print("Bomba:");
  if (bombaActiva) {
    oled.fillRoundRect(48, 13, 36, 11, 2, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(54, 15);
    oled.print("ON");
    oled.setTextColor(SSD1306_WHITE);
  } else {
    oled.drawRoundRect(48, 13, 40, 11, 2, SSD1306_WHITE);
    oled.setCursor(54, 15);
    oled.print("OFF");
  }

  // Techo
  oled.setCursor(0, 30);
  oled.print("Techo:");
  oled.setCursor(48, 30);
  oled.print(techoCerrado ? "CERRADO" : "ABIERTO");

  // Wi-Fi
  oled.setCursor(0, 45);
  oled.print("WiFi:");
  oled.setCursor(36, 45);
  if (WiFi.status() == WL_CONNECTED) {
    oled.print("OK");
    // Icono de señal simple
    oled.drawLine(56, 50, 56, 46, SSD1306_WHITE);
    oled.drawLine(59, 50, 59, 44, SSD1306_WHITE);
    oled.drawLine(62, 50, 62, 42, SSD1306_WHITE);
  } else {
    oled.print("Sin conexion");
  }

  // Temperatura umbral techo
  oled.setCursor(0, 57);
  oled.printf("Umbral: %.0fC cerrar/%.0fC abrir",
              TEMP_CIERRA_TECHO, TEMP_ABRE_TECHO);

  // Puntos de página
  oled.drawCircle(60, 60, 2, SSD1306_WHITE);
  oled.fillCircle(68, 60, 2, SSD1306_WHITE);
}

// ── Barra de progreso horizontal ──────────────────────────
void barraProgreso(int x, int y, int ancho, int alto, int porcentaje) {
  porcentaje = constrain(porcentaje, 0, 100);
  oled.drawRect(x, y, ancho, alto, SSD1306_WHITE);
  int relleno = (ancho - 2) * porcentaje / 100;
  if (relleno > 0)
    oled.fillRect(x + 1, y + 1, relleno, alto - 2, SSD1306_WHITE);
}

// ════════════════════════════════════════════════════════════
//  SENSORES Y CONTROL
// ════════════════════════════════════════════════════════════

void leerSensores() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperatura = t;
  if (!isnan(h)) humedadAire = h;

  long suma = 0;
  for (int i = 0; i < 5; i++) {
    suma += analogRead(PIN_YL100);
    delay(10);
  }
  adcSustrato     = suma / 5;
  humedadSustrato = adcAHumedad(adcSustrato);
}

int adcAHumedad(int adc) {
  adc = constrain(adc, ADC_SUELO_HUMEDO, ADC_SUELO_SECO);
  return map(adc, ADC_SUELO_SECO, ADC_SUELO_HUMEDO, 0, 100);
}

void controlarTecho() {
  if (!techoCerrado && temperatura >= TEMP_CIERRA_TECHO) {
    servoTecho.write(SERVO_CERRADO);
    techoCerrado = true;
    Serial.println("[Techo] CERRADO – temperatura alta");
  } else if (techoCerrado && temperatura <= TEMP_ABRE_TECHO) {
    servoTecho.write(SERVO_ABIERTO);
    techoCerrado = false;
    Serial.println("[Techo] ABIERTO – temperatura normal");
  }
}

void controlarRiego() {
  unsigned long ahora = millis();
  if (!bombaActiva) {
    if (humedadSustrato < 40)
      activarBomba();
  } else {
    bool tiempoOk    = (ahora - tInicioBomba) >= TIEMPO_MIN_BOMBA;
    bool sueloHumedo = humedadSustrato >= 70;
    if (tiempoOk && sueloHumedo)
      desactivarBomba();
  }
}

void activarBomba() {
  if (!bombaActiva) {
    digitalWrite(PIN_RELE, LOW);
    bombaActiva  = true;
    tInicioBomba = millis();
    Serial.println("[Bomba] ACTIVADA");
  }
}

void desactivarBomba() {
  if (bombaActiva) {
    digitalWrite(PIN_RELE, HIGH);
    bombaActiva = false;
    Serial.println("[Bomba] DESACTIVADA");
  }
}

// ════════════════════════════════════════════════════════════
//  Wi-Fi y MQTT
// ════════════════════════════════════════════════════════════

void conectarWifi() {
  // Mostrar en OLED mientras conecta
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 10);
  oled.print("Conectando WiFi...");
  oled.setCursor(0, 24);
  oled.print(WIFI_SSID);
  oled.display();

  Serial.printf("Conectando a Wi-Fi: %s ", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(".");
    intentos++;
  }

  oled.clearDisplay();
  oled.setCursor(0, 10);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi conectado. IP: %s\n", WiFi.localIP().toString().c_str());
    oled.print("WiFi conectado!");
    oled.setCursor(0, 24);
    oled.print(WiFi.localIP().toString());
  } else {
    Serial.println("\n[ERROR] Sin Wi-Fi.");
    oled.print("Sin WiFi.");
    oled.setCursor(0, 24);
    oled.print("Modo local activo");
  }
  oled.display();
  delay(2000);
}

void conectarMqtt() {
  int intentos = 0;
  while (!mqttClient.connected() && intentos < 5) {
    Serial.print("Conectando MQTT...");
    if (mqttClient.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS))
      Serial.println(" OK");
    else {
      Serial.printf(" Fallo (estado: %d). Reintento en 3s\n", mqttClient.state());
      delay(3000);
      intentos++;
    }
  }
}

void publicarThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[IoT] Sin Wi-Fi – publicación omitida");
    return;
  }
  if (!mqttClient.connected()) conectarMqtt();

  char payload[120];
  snprintf(payload, sizeof(payload),
           "field1=%.1f&field2=%.0f&field3=%.0f&field4=%d&field5=%d",
           temperatura, humedadAire, humedadSustrato,
           bombaActiva ? 1 : 0, techoCerrado ? 1 : 0);

  if (mqttClient.publish(MQTT_CHANNEL, payload))
    Serial.printf("[IoT] Publicado: %s\n", payload);
  else
    Serial.println("[IoT] Error al publicar");
}