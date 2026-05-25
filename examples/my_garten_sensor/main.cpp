// Abgeleitet vom Simple Sensor Node, erweitert um:
// - SHT31 Luftfeuchte/Temperatur Sensor

#include "SensorMesh.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Sleep-Steuerung — Long-Press USR toggelt zwischen "schläft nach Senden" und "immer wach".
// Default: aktiviert (Energiesparmodus), erste Schlafphase verzögert sich auf FIRST_SLEEP_DELAY_MS.
bool     sleepEnabled = true;

#ifndef FIRST_SLEEP_DELAY_MS
#define FIRST_SLEEP_DELAY_MS 60000UL   // erste Schlafphase frühestens 60 s nach Boot
#endif

void toggleSleep() {
  sleepEnabled = !sleepEnabled;
  Serial.printf("[Sleep] mode %s\n", sleepEnabled ? "ENABLED" : "DISABLED (always awake)");
}

// 128-bit PSK des privaten Group-Channels "Garten" (16-byte hex = 32 Zeichen)
static const char GARTEN_CHANNEL_KEY_HEX[] = "111d26050e2b03985e5473808b6650e0";
#define GARTEN_CHANNEL_KEY_LEN 16

// ── Pin-Definitionen T114 ─────────────────────────────────────────────
// ACHTUNG: Pin 2 = PIN_TFT_RST, Pin 25 = SX126X_RESET — NICHT belegen!
// Frei sind u.a.: 5 (=AIN3), 6, 16, 18, 28..31, 32..36
#define PIN_DS18B20     13
#define PIN_SOIL_ADC     5   // AIN3
#define PIN_SOIL_VCC     6

#ifndef USER_BTN_PRESSED
#define USER_BTN_PRESSED LOW
#endif

// ── Display-Timeout nach Boot ─────────────────────────────────────────
#define DISPLAY_ON_BOOT_MS  3000UL   // 3 Sekunden sichtbar, dann aus

// ── Sleep nach Senden (Sekunden). 0 = deaktiviert
#ifndef SLEEP_AFTER_SEND_SECS
#define SLEEP_AFTER_SEND_SECS 0
#endif

// Flag: nach dem nächsten Senden in den Sleep gehen
static bool pending_sleep = false;

Adafruit_SHT31    sht31;
OneWire           ow(PIN_DS18B20);
DallasTemperature ds18(&ow);

// ── Display-Zustand ───────────────────────────────────────────────────
bool             displayOn       = true;
static uint32_t displayOnSince  = 0;
bool             displayBootAutoOff = false;
uint32_t        sendCount      = 0;
float           lastBattV      = -99.0f;
float           lastAirTemp    = -99.0f;
float           lastAirHum     = -1.0f;
float           lastSoilTemp   = -99.0f;
int             lastSoilRaw    = -1;

// ── Tasten-Entprellung ────────────────────────────────────────────────
// handled by UITask

// ════════════════════════════════════════════════════════════════════
class MyMesh : public SensorMesh {
public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms,
         mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
    : SensorMesh(board, radio, ms, rng, rtc, tables),
       battery_data(12*24, 5*60)    // 24 hours worth of battery data, every 5 minutes
  {
  }

protected:
  /* ========================== custom logic here ========================== */
  Trigger low_batt, critical_batt;
  TimeSeriesData  battery_data;

  void onSensorDataRead() override {
    float batt_voltage    = getVoltage(TELEM_CHANNEL_SELF);
    float airTemp  = sht31.readTemperature();
    float airHum   = sht31.readHumidity();

    if (isnan(airTemp)) airTemp = -99.0f;
    if (isnan(airHum))  airHum  =   0.0f;

    ds18.requestTemperatures();
    delay(750);
    float soilTemp = ds18.getTempCByIndex(0);
    if (soilTemp < -55.0f) soilTemp = -99.0f;

    int soilRaw = readSoilMoisture();

    Serial.printf("[Garten] Luft: %.1f°C  %.1f%%  Boden: %.1f°C"
                  "  Feuchte: %d  Akku: %.2fV\n",
                  airTemp, airHum, soilTemp, soilRaw, batt_voltage);

    battery_data.recordData(getRTCClock(), batt_voltage);   // record battery
    alertIf(batt_voltage < 3.4f, critical_batt, HIGH_PRI_ALERT, "Battery is critical!");
    alertIf(batt_voltage < 3.6f, low_batt,      LOW_PRI_ALERT,  "Battery is low");

    lastBattV    = batt_voltage;
    lastAirTemp  = airTemp;
    lastAirHum   = airHum;
    lastSoilTemp = soilTemp;
    lastSoilRaw  = soilRaw;

    sendChannelData(batt_voltage, airTemp, airHum, soilTemp, soilRaw);

    // markiere, dass nach dem Senden in den Sleep gewechselt werden soll
    pending_sleep = true;
  }

  int querySeriesData(uint32_t start_secs_ago, uint32_t end_secs_ago, MinMaxAvg dest[], int max_num) override {
    battery_data.calcMinMaxAvg(getRTCClock(), start_secs_ago, end_secs_ago, &dest[0], TELEM_CHANNEL_SELF, LPP_VOLTAGE);
    return 1;
  }

  bool handleCustomCommand(uint32_t sender_timestamp, char* command, char* reply) override {
    if (strcmp(command, "magic") == 0) {    // example 'custom' command handling
      strcpy(reply, "**Magic now done**");
      return true;   // handled
    }
    return false;  // not handled
  }
  /* ======================================================================= */
private:
  void sendChannelData(float battV, float airTemp, float airHum, float soilTemp, int soilRaw) {
    // GroupChannel "Garten" aufbauen: 128-bit secret + hash = sha256(secret, PATH_HASH_SIZE)
    mesh::GroupChannel channel;
    memset(channel.secret, 0, sizeof(channel.secret));
    mesh::Utils::fromHex(channel.secret, GARTEN_CHANNEL_KEY_LEN, GARTEN_CHANNEL_KEY_HEX);
    mesh::Utils::sha256(channel.hash, sizeof(channel.hash),
                        channel.secret, GARTEN_CHANNEL_KEY_LEN);

    // Payload-Format wie BaseChatMesh::sendGroupMessage:
    // [ts:4][TXT_TYPE_PLAIN:1]["<sender>: <text>\0"]
    uint8_t temp[5 + 128];
    uint32_t ts = getRTCClock()->getCurrentTime();
    memcpy(temp, &ts, 4);
    temp[4] = TXT_TYPE_PLAIN;

    int len = snprintf((char*)&temp[5], sizeof(temp) - 5,
                       ADVERT_NAME ": B=%.2f T=%.1f H=%.1f S=%.1f M=%d",
                       battV, airTemp, airHum, soilTemp, soilRaw);
    if (len <= 0) return;

    auto pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, temp, 5 + len);
    if (pkt) {
      sendFlood(pkt);
      sendCount++;
      // Serial.println("[Garten] Sent channel payload");
    }
  }

  static int readSoilMoisture() {
    pinMode(PIN_SOIL_VCC, OUTPUT);
    digitalWrite(PIN_SOIL_VCC, HIGH);
    delay(500);
    int val = analogRead(PIN_SOIL_ADC);
    digitalWrite(PIN_SOIL_VCC, LOW);
    return val;
  }
};

// ════════════════════════════════════════════════════════════════════
StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];

static void setDisplay(bool on) {
#ifdef DISPLAY_CLASS
  if (on) {
    if (!display.isOn()) {
      display.turnOn();
    }
    displayOn = true;
    displayBootAutoOff = false;
    ui_task.resetAutoOff();
    displayOnSince = millis();
  } else {
    display.clear();
    display.turnOff();   // ST7789: Backlight + Panel aus
    displayOn = false;
  }
#endif
}

#ifdef DISPLAY_CLASS
static uint32_t lastDisplayUpdate = 0;
#define LINE_H 16   // Zeilenhöhe in Pixel (Textgröße 1 = ~8px, Größe 2 = ~16px)

void renderSensorPage() {
  if (!displayOn) return;
  if (millis() - lastDisplayUpdate < 2000) return;
  lastDisplayUpdate = millis();

  // if (lastAirTemp < -90.0f) return;  // noch keine Daten

  char buf[32];
  int y = 4;  // Startposition oben

  display.startFrame();
  display.setTextSize(2);
  display.setColor(DisplayDriver::LIGHT);

  snprintf(buf, sizeof(buf), "%.1fC  %.0f%%", lastAirTemp, lastAirHum);
  display.setCursor(0, y); display.print("Luft:");
  y += LINE_H;
  display.setCursor(0, y); display.print(buf);
  y += LINE_H + 4;

  snprintf(buf, sizeof(buf), "%.1fC", lastSoilTemp);
  display.setCursor(0, y); display.print("Boden:");
  y += LINE_H;
  display.setCursor(0, y); display.print(buf);
  y += LINE_H + 4;

  snprintf(buf, sizeof(buf), "%d", lastSoilRaw);
  display.setCursor(0, y); display.print("Feuchte:");
  y += LINE_H;
  display.setCursor(0, y); display.print(buf);
  
  display.endFrame();
  Serial.println("[Display] Sensor page updated");
}
#endif

// ════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();                               // 1. Board zuerst (initialisiert SPI + Pins)

#ifdef DISPLAY_CLASS
  if (display.begin()) {                       // 2. Display danach initialisieren
    display.startFrame();
  #ifdef ST7789
    display.setTextSize(2);
  #endif
    display.drawTextCentered(display.width() / 2, 28, "Loading...");
    display.endFrame();
  } else {
    Serial.println("===== Display FEHLER — begin() false");
  }
#else
  Serial.println("===== Kein Display definiert");
#endif

  // Sensoren                                   // 3. I2C für Sensoren
  Wire.begin();
  if (!sht31.begin(0x44)) {
    Serial.println("FEHLER: SHT31 nicht gefunden!");
  }
  ds18.begin();


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
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Sensor ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;


  the_mesh.begin(fs);

  sensors.begin();

  {
    NodePrefs* prefs = the_mesh.getNodePrefs();
    Serial.printf("LoRa parameters: freq=%.3f MHz, bw=%.1f kHz, sf=%u, cr=%u, tx=%d dBm\n",
                  prefs->freq, prefs->bw, prefs->sf, prefs->cr,
                  prefs->tx_power_dbm);
  }

#ifdef DISPLAY_CLASS
  Serial.println("===== Starting UI Task");
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif
}

// ════════════════════════════════════════════════════════════════════
// LOOP
// ════════════════════════════════════════════════════════════════════
void loop() {
  // ── Display nach 3 s automatisch ausschalten (nur beim Boot) ─────
#ifdef DISPLAY_CLASS
  if (displayOn && displayBootAutoOff && displayOnSince > 0 &&
      (millis() - displayOnSince) >= DISPLAY_ON_BOOT_MS) {
    setDisplay(false);
    displayOnSince = 0;   // einmalig, nicht wiederholen
    displayBootAutoOff = false;
    Serial.println("[Display] Automatisch ausgeschaltet nach 3s");
  }
#endif

  // ── Serial CLI ────────────────────────────────────────────────────
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
    }
    Serial.print(c);
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
  // renderSensorPage();
#endif
  // Sleep nach Senden:
  // - sleepEnabled=false (Long-Press): nicht schlafen
  // - Erste FIRST_SLEEP_DELAY_MS nach Boot: Sensor bleibt erreichbar
  // Echter Timer-Sleep auf nRF52: FreeRTOS-delay() = tickless idle (RTC1-basiert).
  // board.sleep() ist hier nutzlos (ignoriert die Sekunden, ist nur WFE).
#if SLEEP_AFTER_SEND_SECS > 0
  if (pending_sleep && sleepEnabled && millis() >= FIRST_SLEEP_DELAY_MS) {
    // 1) LoRa-TX leerlaufen lassen (SF8/BW62.5/CR5 ~1-2 s Luftzeit)
    Serial.printf("[Sleep] flushing TX, then deep-sleeping %u seconds\n", (unsigned)SLEEP_AFTER_SEND_SECS);
    pending_sleep = false;
    uint32_t flush_until = millis() + 2500;
    while ((int32_t)(millis() - flush_until) < 0) {
      the_mesh.loop();
      delay(10);
    }
  #ifdef DISPLAY_CLASS
    // 2) Display ausschalten — beim Aufwachen bleibt es aus bis Button-Press
    if (display.isOn()) {
      display.clear();
      display.turnOff();
      displayOn = false;
    }
  #endif
    // 3) LoRa-Chip in Schlafmodus (~µA statt ~600 µA Standby)
    radio_driver.powerOff();
    Serial.flush();
    // 4) Echter Timer-Sleep via FreeRTOS tickless idle
    delay((uint32_t)SLEEP_AFTER_SEND_SECS * 1000UL);
    // 5) Aufwachen: Radio neu initialisieren, damit das_mesh.loop() wieder TX/RX kann
    if (!radio_init()) {
      Serial.println("[Sleep] WARN: radio_init() after wake failed");
    }
    NodePrefs* p = the_mesh.getNodePrefs();
    radio_set_params(p->freq, p->bw, p->sf, p->cr);
    radio_set_tx_power(p->tx_power_dbm);
    Serial.println("[Sleep] woke up, radio re-initialised");
  } else if (pending_sleep && !sleepEnabled) {
    pending_sleep = false;
    Serial.println("[Sleep] skipped: sleep mode disabled");
  }
  // sonst: noch in der Boot-Schonzeit, pending_sleep bleibt stehen
#endif
  rtc_clock.tick();
}