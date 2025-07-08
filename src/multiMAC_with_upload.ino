/*
 * BLE Beacon Logger & Deferred Uploader (receiver-ID–based filenames)
 * ---------------------------------------------------------------
 *  ‣ Logs RSSI from a whitelist of BLE beacons to CSV on SD.
 *  ‣ Once per second appends: UTC, SSID, receiverID, beaconID, RSSI.
 *  ‣ Daily (configurable hour/minute) uploads any pending CSVs when Wi-Fi
 *    becomes available, then marks them as done.
 *  ‣ CSV filename pattern: <receiverID>_<YYYYMMDD>.csv
 *  ‣ Upload status shown on TFT row 2 as "UPL <ISO-8601>" or "UPL Never".
 *
 *  Hardware: ESP32-S3 + SD-card + ST7789 TFT.
 *  Author:   <you>
 * ----------------------------------------------------------------
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include "esp_gap_ble_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "time.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <HTTPClient.h>

/* ---------- User-adjustable constants ---------- */
const char *WIFI_SSID     = "JJC iPhone 16 Pro";
const char *WIFI_PASS     = "Finals26";
const char *ENDPOINT_URL  = "https://yourserver.com/upload";   // <-- set me
constexpr uint8_t UPLOAD_HOUR = 3;   // 0–23 UTC
constexpr uint8_t UPLOAD_MIN  = 0;   // 0–59 UTC

/* ---------- Logging window ---------- */
constexpr uint32_t LOG_WINDOW_MAX_MS = 10000;   // on-screen freshness

/* ---------- Beacon whitelist ---------- */
constexpr uint8_t WHITELIST[][6] = {
  {0xF4, 0xB3, 0xB1, 0x77, 0xB4, 0xAD},
  {0xF4, 0xB3, 0xB1, 0x82, 0xB8, 0x2D},
  {0x38, 0x39, 0x8F, 0x72, 0x97, 0x37}
};
const size_t NUM_DEV = sizeof(WHITELIST) / 6;

/* ---------- TFT wiring ---------- */
#define TFT_CS  42
#define TFT_DC  41
#define TFT_RST 39
#define TFT_BL  48
SPIClass hspi(HSPI);                 // SCK-40, MOSI-45 on ESP32-S3
Adafruit_ST7789 tft(&hspi, TFT_CS, TFT_DC, TFT_RST);

constexpr uint16_t COL_BG  = ST77XX_BLACK;
constexpr uint16_t COL_TXT = ST77XX_WHITE;

/* ---------- Layout ---------- */
const int16_t X0 = 6;
const int16_t Y0 = 34;
const int16_t LH = 22;
const int     nRows = 3;                       // number of beacon rows visible
const int     DEVICE_ROW_BASE = 4;             // Devices start at row index 4

/* ---------- Device state (screen only) ---------- */
struct Device {
  char     macStr[18];
  int16_t  rssi;
  uint32_t lastSeenMs;
};
Device devices[NUM_DEV];

/* ---------- Unique receiver ID ---------- */
String receiverID;

/* ---------- Ping queue ---------- */
struct PingEvent {
  uint8_t  devIdx;
  int16_t  rssi;
  uint32_t ms;
};
constexpr size_t QUEUE_SZ = 64;
PingEvent        q[QUEUE_SZ];
volatile size_t  qHead = 0, qTail = 0;
portMUX_TYPE     qMux  = portMUX_INITIALIZER_UNLOCKED;

/* ---------- Helper functions ---------- */
void macToStr(const uint8_t *mac, char *buf) {
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
String utcNow() {
  time_t now = time(nullptr);
  if (now < 1577836800) return F("not available");
  tm t; gmtime_r(&now, &t);
  char b[20]; strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &t);
  return b;
}
String utcDateName() {
  time_t now = time(nullptr);
  tm t; gmtime_r(&now, &t);
  char b[16]; strftime(b, sizeof b, "%Y%m%d", &t);
  return b;
}
bool afterUploadTimeUTC() {
  time_t now = time(nullptr);
  tm t; gmtime_r(&now, &t);
  return (t.tm_hour >  UPLOAD_HOUR) ||
         (t.tm_hour == UPLOAD_HOUR && t.tm_min >= UPLOAD_MIN);
}

/* ---------- TFT helpers ---------- */
void printRow(uint8_t idx, const String &text) {
  static String prev[10];
  if (text == prev[idx]) return;
  tft.setTextSize(2);
  tft.setTextColor(COL_BG, COL_BG);
  tft.setCursor(X0, Y0 + idx * LH);
  tft.print(prev[idx] + F("                    "));
  tft.setTextColor(COL_TXT, COL_BG);
  tft.setCursor(X0, Y0 + idx * LH);
  tft.print(text);
  prev[idx] = text;
}
void updateUTC() {
  static String prev;
  String nowStr = utcNow();
  if (nowStr != prev) {
    printRow(0, String(F("UTC ")) + nowStr);
    prev = nowStr;
  }
}
void updateUPL(const String &iso) {
  static String prev;
  String msg = String(F("UPL ")) + iso;
  if (msg != prev) {
    printRow(2, msg);
    prev = msg;
  }
}

/* ---------- BLE GAP callback ---------- */
void gapCB(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p) {
  if (ev != ESP_GAP_BLE_SCAN_RESULT_EVT) return;
  auto &r = p->scan_rst;
  if (r.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;

  for (size_t i = 0; i < NUM_DEV; i++) {
    if (!memcmp(r.bda, WHITELIST[i], 6)) {
      devices[i].rssi       = r.rssi;
      devices[i].lastSeenMs = millis();

      portENTER_CRITICAL_ISR(&qMux);
      size_t next = (qHead + 1) % QUEUE_SZ;
      if (next != qTail) {
        q[qHead] = { uint8_t(i), r.rssi, millis() };
        qHead    = next;
      }
      portEXIT_CRITICAL_ISR(&qMux);
      break;
    }
  }
}

/* ---------- Wi-Fi helpers ---------- */
bool wifiUp() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  return WiFi.isConnected();
}
void ntpSync() { configTime(0, 0, "pool.ntp.org"); }

/* ---------- SD-card ---------- */
#define MOUNT_POINT "/sdcard"
#define SDMMC_CLK  14
#define SDMMC_CMD  15
#define SDMMC_D0   16
#define SDMMC_D1   18
#define SDMMC_D2   17
#define SDMMC_D3   21
void initSDCard() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags        = SDMMC_HOST_FLAG_4BIT;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;
  slot.clk   = (gpio_num_t)SDMMC_CLK;
  slot.cmd   = (gpio_num_t)SDMMC_CMD;
  slot.d0    = (gpio_num_t)SDMMC_D0;
  slot.d1    = (gpio_num_t)SDMMC_D1;
  slot.d2    = (gpio_num_t)SDMMC_D2;
  slot.d3    = (gpio_num_t)SDMMC_D3;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  esp_vfs_fat_sdmmc_mount_config_t cfg = {
    .format_if_mount_failed = false,
    .max_files              = 5,
    .allocation_unit_size   = 16 * 1024
  };
  sdmmc_card_t *card;
  if (esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &cfg, &card) == ESP_OK)
    Serial.println(F("✅ SD card mounted."));
  else
    Serial.println(F("❌ SD card mount failed"));
}

/* ---------- Upload manifest ---------- */
#define MANIFEST MOUNT_POINT "/upload_done.txt"
bool wasUploaded(const String &fname) {
  FILE *f = fopen(MANIFEST, "r");
  if (!f) return false;
  char buf[40];
  while (fgets(buf, sizeof buf, f)) {
    if (fname == String(buf).trim()) {
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}
void rememberUploaded(const String &fname) {
  FILE *f = fopen(MANIFEST, "a");
  if (!f) return;
  fputs(fname.c_str(), f);
  fputs("\n", f);
  fclose(f);
}

/* ---------- File push ---------- */
bool pushFile(const String &path) {
  File file = SD.open(path, FILE_READ);
  if (!file) return false;

  int len = file.size();
  std::unique_ptr<uint8_t[]> buf(new uint8_t[len]);
  file.read(buf.get(), len);
  file.close();

  HTTPClient http;
  http.begin(ENDPOINT_URL);
  http.addHeader("Content-Type", "text/csv");
  int rc = http.POST(buf.get(), len);
  http.end();
  return rc == 200;
}

/* ---------- Setup ---------- */
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  uint64_t chip = ESP.getEfuseMac();
  char chipStr[18];
  sprintf(chipStr, "%02X:%02X:%02X:%02X:%02X:%02X",
          uint8_t(chip >> 40), uint8_t(chip >> 32),
          uint8_t(chip >> 24), uint8_t(chip >> 16),
          uint8_t(chip >> 8),  uint8_t(chip));
  receiverID = String(chipStr);

  hspi.begin(40, -1, 45);
  tft.init(240, 320);
  tft.setRotation(1);
  tft.setAddrWindow(0, 34, 320, 172);
  tft.fillScreen(COL_BG);

  for (size_t i = 0; i < NUM_DEV; i++) {
    macToStr(WHITELIST[i], devices[i].macStr);
    devices[i].rssi       = -128;
    devices[i].lastSeenMs = 0;
  }

  printRow(0, F("UTC not available"));
  printRow(1, String(F("SSID ")) + WIFI_SSID);
  updateUPL("Never");               // row 2
  // optional label: printRow(3, F("Devices"));

  if (wifiUp()) ntpSync();
  initSDCard();

  btStart();
  esp_bluedroid_init(); esp_bluedroid_enable();
  esp_ble_gap_register_callback(gapCB);

  static esp_ble_scan_params_t scan = {
    BLE_SCAN_TYPE_ACTIVE, BLE_ADDR_TYPE_PUBLIC, BLE_SCAN_FILTER_ALLOW_ALL,
    0x50, 0x30, BLE_SCAN_DUPLICATE_DISABLE
  };
  esp_ble_gap_set_scan_params(&scan);
  esp_ble_gap_start_scanning(0);
}

/* ---------- Main loop ---------- */
void loop() {
  /* 1 Hz UTC label */
  static uint32_t lastTick = 0;
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    updateUTC();
  }

  /* SSID row */
  static String prevSSID;
  if (WiFi.isConnected()) {
    String s = WiFi.SSID();
    if (s != prevSSID) {
      printRow(1, String(F("SSID ")) + s);
      prevSSID = s;
    }
  }

  /* Scroll display rows */
  static uint8_t  scrollIdx  = 0;
  static uint32_t lastScroll = 0;
  if (NUM_DEV > nRows && millis() - lastScroll > 2000) {
    lastScroll = millis();
    scrollIdx  = (scrollIdx + 1) % (NUM_DEV - nRows + 1);
  }
  if (NUM_DEV <= nRows) scrollIdx = 0;

  for (size_t i = 0; i < nRows; i++) {
    size_t  devIdx = (i + scrollIdx) % NUM_DEV;
    int16_t rssi   = (millis() - devices[devIdx].lastSeenMs > LOG_WINDOW_MAX_MS)
                     ? -128 : devices[devIdx].rssi;
    String  line   = String(devices[devIdx].macStr) + " " +
                     (rssi == -128 ? "--" : String(rssi));
    printRow(DEVICE_ROW_BASE + i, line);
  }

  /* Flush queue to CSV once per second */
  static uint32_t lastFlush = 0;
  if (millis() - lastFlush > 1000 && time(nullptr) > 1577836800) {
    lastFlush = millis();
    String now      = utcNow();
    String ssid     = WiFi.SSID();
    String dirPath  = String(MOUNT_POINT) + "/data";
    mkdir(dirPath.c_str(), 0777);
    String filename = dirPath + "/" + receiverID + "_" + utcDateName() + ".csv";

    FILE *f = fopen(filename.c_str(), "a");
    if (!f) { Serial.println("Failed to open log file"); return; }

    /* drain queue */
    while (true) {
      portENTER_CRITICAL(&qMux);
      bool empty = (qTail == qHead);
      PingEvent ev;
      if (!empty) {
        ev   = q[qTail];
        qTail = (qTail + 1) % QUEUE_SZ;
      }
      portEXIT_CRITICAL(&qMux);
      if (empty) break;

      String row = now + "," + ssid + "," + receiverID + "," +
                   devices[ev.devIdx].macStr + "," + String(ev.rssi);
      fputs(row.c_str(), f); fputs("\n", f);
    }
    fclose(f);
  }

  /* Upload supervisor (runs every 60 s when Wi-Fi & after time) */
  static uint32_t lastTry = 0;
  if (millis() - lastTry > 60000 && WiFi.isConnected() && afterUploadTimeUTC()) {
    lastTry = millis();
    File dir = SD.open("/sdcard/data");
    if (!dir) {
      Serial.println("❌ /data open fail");
    } else {
      while (File f = dir.openNextFile()) {
        String path = f.name();
        f.close();
        if (!path.endsWith(".csv")) continue;
        String fname = path.substring(path.lastIndexOf('/') + 1); // includes .csv
        if (wasUploaded(fname)) continue;

        Serial.println("📤 " + fname);
        if (pushFile(path)) {
          rememberUploaded(fname);
          updateUPL(utcNow());
          Serial.println("✅ upload ok");
        } else {
          Serial.println("❌ upload failed");
        }
      }
    }
  }
  delay(10);
}
