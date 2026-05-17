#include "SensorMesh.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

// ── Sensor-Bibliotheken ───────────────────────────────────────────────
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ── Pin-Definitionen T114 ─────────────────────────────────────────────
#define PIN_DS18B20   13   // 1-Wire DQ
#define PIN_SOIL_ADC   2   // AIN0 — Bodenfeuchte
#define PIN_SOIL_VCC  25   // schaltbare Sensor-Versorgung
// Akku-ADC ist intern via getVoltage(TELEM_CHANNEL_SELF) verfügbar

Adafruit_SHT31    sht31;
OneWire           ow(PIN_DS18B20);
DallasTemperature ds18(&ow);

// ── Hilfsfunktionen ───────────────────────────────────────────────────
static int readSoilMoisture() {
  pinMode(PIN_SOIL_VCC, OUTPUT);
  digitalWrite(PIN_SOIL_VCC, HIGH);
  delay(500);
  int val = analogRead(PIN_SOIL_ADC);
  digitalWrite(PIN_SOIL_VCC, LOW);
  return val;
}

// ════════════════════════════════════════════════════════════════════
class MyMesh : public SensorMesh {
public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms,
         mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
    : SensorMesh(board, radio, ms, rng, rtc, tables),
      battery_data(12*24, 5*60)
  { }

protected:
  Trigger low_batt, critical_batt;
  TimeSeriesData battery_data;

  void onSensorDataRead() override {
    float battV = getVoltage(TELEM_CHANNEL_SELF);

    // ── SHT31: Lufttemperatur & Luftfeuchtigkeit ──────────────────
    float airTemp = sht31.readTemperature();
    float airHum  = sht31.readHumidity();
    if (isnan(airTemp)) airTemp = -99.0f;
    if (isnan(airHum))  airHum  =   0.0f;

    // ── DS18B20: Bodentemperatur ──────────────────────────────────
    ds18.requestTemperatures();
    delay(750);
    float soilTemp = ds18.getTempCByIndex(0);
    if (soilTemp < -55.0f) soilTemp = -99.0f;

    // ── Kapazitive Bodenfeuchte ───────────────────────────────────
    int soilRaw = readSoilMoisture();

    // ── Debug-Ausgabe ─────────────────────────────────────────────
    Serial.printf("[Garten] Luft: %.1f°C  %.1f%%  Boden: %.1f°C"
                  "  Feuchte: %d  Akku: %.2fV\n",
                  airTemp, airHum, soilTemp, soilRaw, battV);

    // ── Akku-Alarme (aus Original übernommen) ─────────────────────
    battery_data.recordData(getRTCClock(), battV);
    alertIf(battV < 3.4f, critical_batt, HIGH_PRI_ALERT, "Battery is critical!");
    alertIf(battV < 3.6f, low_batt,      LOW_PRI_ALERT,  "Battery is low");
  }

  int querySeriesData(uint32_t start_secs_ago, uint32_t end_secs_ago,
                      MinMaxAvg dest[], int max_num) override {
    battery_data.calcMinMaxAvg(getRTCClock(), start_secs_ago, end_secs_ago,
                               &dest[0], TELEM_CHANNEL_SELF, LPP_VOLTAGE);
    return 1;
  }

  bool handleCustomCommand(uint32_t sender_timestamp,
                           char* command, char* reply) override {
    return false;  // keine Custom-Commands
  }
};

// ════════════════════════════════════════════════════════════════════
StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(),
                fast_rng, rtc_clock, tables);

void halt() { while (1); }

static char command[160];

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

  // ── Sensoren initialisieren ───────────────────────────────────────
  Wire.begin();
  if (!sht31.begin(0x44)) {
    Serial.println("FEHLER: SHT31 nicht gefunden!");
  }
  ds18.begin();

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.print("Garten Node...");
    display.endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_get_rng_seed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif

  if (!store.load("_main", the_mesh.self_id)) {
    the_mesh.self_id = radio_new_identity();
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 ||
                          the_mesh.self_id.pub_key[0] == 0xFF)) {
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Sensor ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  Serial.println();

  command[0] = 0;
  sensors.begin();
  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') { command[len++] = c; command[len] = 0; }
    Serial.print(c);
  }
  if (len == sizeof(command)-1) command[sizeof(command)-1] = '\r';

  if (len > 0 && command[len-1] == '\r') {
    command[len-1] = 0;
    char reply[160];
    the_mesh.handleCommand(0, command, reply);
    if (reply[0]) { Serial.print("  -> "); Serial.println(reply); }
    command[0] = 0;
  }

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
}