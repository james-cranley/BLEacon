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

/* ---------- Wi-Fi ---------- */
const char* WIFI_SSID = "JJC iPhone 16 Pro";
const char* WIFI_PASS = "Finals26";

/* ---------- Whitelist ---------- */
constexpr uint8_t WHITELIST[][6] = {
  { 0xF4, 0xB3, 0xB1, 0x77, 0xB4, 0xAD },
  { 0x1C, 0x3C, 0x78, 0xAC, 0x26, 0x1A },
  { 0xD0, 0x11, 0xE5, 0x8E, 0xC5, 0x13 }
};
const size_t NUM_DEV = sizeof(WHITELIST) / 6;

/* ---------- TFT wiring ---------- */
#define TFT_CS 42
#define TFT_DC 41
#define TFT_RST 39
#define TFT_BL 48
SPIClass hspi(HSPI);  // SCK 40, MOSI 45
Adafruit_ST7789 tft(&hspi, TFT_CS, TFT_DC, TFT_RST);

constexpr uint16_t COL_BG = ST77XX_BLACK;
constexpr uint16_t COL_TXT = ST77XX_WHITE;

/* ---------- Layout ---------- */
const int16_t X0 = 6;
const int16_t Y0 = 34;
const int16_t LH = 22;
const int nRows = 3;

/* ---------- Device state ---------- */
struct Device {
  char macStr[18];
  int16_t rssi;
  uint32_t lastSeenMs;
};
Device devices[NUM_DEV];

/* ---------- MAC to string ---------- */
void macToStr(const uint8_t* mac, char* buf) {
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---------- UTC string ---------- */
String utcNow() {
  time_t now = time(nullptr);
  if (now < 1577836800) return F("not available");
  tm t;
  gmtime_r(&now, &t);
  char b[20];
  strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &t);
  return b;
}

String utcDateName() {
  time_t now = time(nullptr);
  tm t;
  gmtime_r(&now, &t);
  char b[16];
  strftime(b, sizeof b, "%Y%m%d", &t);
  return b;
}

/* ---------- Print row ---------- */
void printRow(uint8_t idx, const String& text) {
  static String prev[8];
  if (text == prev[idx]) return;
  tft.setTextSize(2);
  tft.setTextColor(COL_BG, COL_BG);
  tft.setCursor(X0, Y0 + idx * LH);
  tft.print(prev[idx] + F("                   "));
  tft.setTextColor(COL_TXT, COL_BG);
  tft.setCursor(X0, Y0 + idx * LH);
  tft.print(text);
  prev[idx] = text;
}

void updateUTC() {
  static String prev;
  String nowStr = utcNow();
  if (nowStr == prev) return;
  printRow(0, String(F("UTC ")) + nowStr);
  prev = nowStr;
}

/* ---------- GAP callback ---------- */
void gapCB(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t* p) {
  if (ev != ESP_GAP_BLE_SCAN_RESULT_EVT) return;
  auto& r = p->scan_rst;
  if (r.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;
  for (size_t i = 0; i < NUM_DEV; i++) {
    if (!memcmp(r.bda, WHITELIST[i], 6)) {
      devices[i].rssi = r.rssi;
      devices[i].lastSeenMs = millis();
    }
  }
}

/* ---------- Wi-Fi ---------- */
bool wifiUp() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  return WiFi.isConnected();
}
void ntpSync() { configTime(0, 0, "pool.ntp.org"); }

/* ---------- SD Card ---------- */
#define MOUNT_POINT "/sdcard"
#define SDMMC_CLK  14
#define SDMMC_CMD  15
#define SDMMC_D0   16
#define SDMMC_D1   18
#define SDMMC_D2   17
#define SDMMC_D3   21

void initSDCard() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_4BIT;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 4;
  slot_config.clk = (gpio_num_t)SDMMC_CLK;
  slot_config.cmd = (gpio_num_t)SDMMC_CMD;
  slot_config.d0  = (gpio_num_t)SDMMC_D0;
  slot_config.d1  = (gpio_num_t)SDMMC_D1;
  slot_config.d2  = (gpio_num_t)SDMMC_D2;
  slot_config.d3  = (gpio_num_t)SDMMC_D3;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024
  };

  sdmmc_card_t* card;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

  if (ret != ESP_OK) {
    Serial.printf("❌ Mount failed: %s\n", esp_err_to_name(ret));
  } else {
    Serial.println("✅ SD card mounted.");
  }
}

/* ---------- Setup ---------- */
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  hspi.begin(40, -1, 45);
  tft.init(240, 320);
  tft.setRotation(1);
  tft.setAddrWindow(0, 34, 320, 172);
  tft.fillScreen(COL_BG);

  for (size_t i = 0; i < NUM_DEV; i++) {
    macToStr(WHITELIST[i], devices[i].macStr);
    devices[i].rssi = -128;
    devices[i].lastSeenMs = 0;
  }

  printRow(0, F("UTC not available"));
  printRow(1, String(F("SSID ")) + WIFI_SSID);
  printRow(2, F("Devices"));

  if (wifiUp()) ntpSync();
  initSDCard();

  btStart();
  esp_bluedroid_init();
  esp_bluedroid_enable();
  esp_ble_gap_register_callback(gapCB);

  static esp_ble_scan_params_t scanParams = {
    BLE_SCAN_TYPE_ACTIVE, BLE_ADDR_TYPE_PUBLIC, BLE_SCAN_FILTER_ALLOW_ALL,
    0x50, 0x30, BLE_SCAN_DUPLICATE_DISABLE
  };
  esp_ble_gap_set_scan_params(&scanParams);
  esp_ble_gap_start_scanning(0);
}

/* ---------- Loop ---------- */
void loop() {
  static uint32_t lastTick = 0;
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    updateUTC();
  }

  static String prevSSID;
  if (WiFi.isConnected()) {
    String s = WiFi.SSID();
    if (s != prevSSID) {
      printRow(1, String(F("SSID ")) + s);
      prevSSID = s;
    }
  }

  static uint8_t scrollIdx = 0;
  static uint32_t lastScrollMs = 0;
  size_t nToShow = min(NUM_DEV, (size_t)nRows);

  if (NUM_DEV > nRows && millis() - lastScrollMs > 2000) {
    lastScrollMs = millis();
    scrollIdx = (scrollIdx + 1) % (NUM_DEV - nRows + 1);
  }
  if (NUM_DEV <= nRows) scrollIdx = 0;

  for (size_t i = 0; i < nRows; i++) {
    size_t devIdx = (i + scrollIdx) % NUM_DEV;
    int16_t rssi = (millis() - devices[devIdx].lastSeenMs > 7000) ? -128 : devices[devIdx].rssi;
    String line = String(devices[devIdx].macStr) + " " + (rssi == -128 ? "--" : String(rssi));
    printRow(3 + i, line);
  }

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 1000 && time(nullptr) > 1577836800) {
    lastLog = millis();
    String now = utcNow();
    String ssid = WiFi.SSID();
    String filename = String(MOUNT_POINT) + "/data/" + utcDateName() + ".csv";

    // Ensure directory exists
    mkdir((String(MOUNT_POINT) + "/data").c_str(), 0777);

    FILE* f = fopen(filename.c_str(), "a");
    if (f) {
      for (size_t i = 0; i < NUM_DEV; i++) {
        int16_t rssi = (millis() - devices[i].lastSeenMs > 7000) ? -128 : devices[i].rssi;
        String row = now + "," + ssid + "," + devices[i].macStr + "," + (rssi == -128 ? "NA" : String(rssi));
        fputs(row.c_str(), f);
        fputs("\n", f);
      }
      fclose(f);
    } else {
      Serial.println("Failed to open log file");
    }
  }

  delay(10);
}
