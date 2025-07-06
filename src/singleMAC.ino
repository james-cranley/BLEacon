/*********************************************************************
  Tracks the presence of a single BLE beacon
  User supplies: MAC, SSID/paswword (for time)
  Board : Waveshare ESP32-S3-LCD-1.47 (ST7789 – glass 172 × 320)
*********************************************************************/
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include "esp_gap_ble_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "time.h"

/* ---------- Wi-Fi ---------- */
const char* WIFI_SSID = "James's Wi-Fi Network";
const char* WIFI_PASS = "Finals26";

/* ---------- Shelly MAC ---------- */
constexpr uint8_t SHELLY_MAC[6] = {0xF4,0xB3,0xB1,0x77,0xB4,0xAD};

/* ---------- TFT wiring ---------- */
#define TFT_CS   42
#define TFT_DC   41
#define TFT_RST  39
#define TFT_BL   48
SPIClass hspi(HSPI);                 // SCK 40, MOSI 45
Adafruit_ST7789 tft(&hspi, TFT_CS, TFT_DC, TFT_RST);

constexpr uint16_t COL_BG  = ST77XX_BLACK;
constexpr uint16_t COL_TXT = ST77XX_WHITE;
constexpr uint16_t COL_BAR = ST77XX_GREEN;

/* ---------- BLE scan params ---------- */
static esp_ble_scan_params_t scanParams = {
  BLE_SCAN_TYPE_ACTIVE, BLE_ADDR_TYPE_PUBLIC, BLE_SCAN_FILTER_ALLOW_ALL,
  0x50, 0x30, BLE_SCAN_DUPLICATE_DISABLE
};

/* ---------- shared state ---------- */
volatile int16_t  g_rssi = -128;        // -128 = none yet
volatile uint32_t g_lastAdvMs = 0;

/* ---------- layout ---------- */
const int16_t X0 = 6;
const int16_t Y0 = 34;                  // first row
const int16_t LH = 22;                  // row height
const int16_t BAR_W = 10*5 + 4*4;
const int16_t BAR_X = 320 - 6 - BAR_W;
const int16_t BAR_BASE_Y = Y0 + 3*LH + 16;

/* ---------- helpers ---------- */
uint8_t bars(int r){
  return r>-50?5:r>-60?4:r>-70?3:r>-80?2:r>-90?1:0;
}
String utcNow(){
  time_t now = time(nullptr);
  if(now < 1577836800) return F("time not available");
  tm t; gmtime_r(&now,&t);
  char b[20]; strftime(b,sizeof b,"%Y-%m-%d %H:%M:%S",&t);
  return b;          // length = 19
}
void drawBarGlyph(uint8_t lvl){
  const uint8_t w=10,g=4,h[5]={8,16,24,32,40};
  tft.fillRect(BAR_X, BAR_BASE_Y-40, BAR_W, 40, COL_BG);   // clear glyph area
  for(uint8_t i=0;i<5;i++){
    int16_t bx = BAR_X + i*(w+g);
    tft.drawRect(bx, BAR_BASE_Y-h[i], w, h[i], COL_TXT);
    if(i<lvl) tft.fillRect(bx+1, BAR_BASE_Y-h[i]+1, w-2, h[i]-2, COL_BAR);
  }
}

/* ---------- incremental row printer ---------- */
void printRow(uint8_t idx, const String& text){
  static String prev[4];
  if(text == prev[idx]) return;
  tft.setTextSize(2);
  tft.setTextColor(COL_BG, COL_BG);      // erase old
  tft.setCursor(X0, Y0 + idx*LH);
  tft.print(prev[idx] + F("   "));
  tft.setTextColor(COL_TXT, COL_BG);     // draw new
  tft.setCursor(X0, Y0 + idx*LH);
  tft.print(text);
  prev[idx] = text;
}

/* ---------- UTC row ---------- */
void updateUTC(){
  static String prev;
  String nowStr = utcNow();
  if(nowStr == prev) return;
  tft.setTextSize(2);
  tft.setTextColor(COL_TXT, COL_BG);
  tft.setCursor(X0, Y0);
  tft.print(F("UTC "));
  tft.print(nowStr);
  prev = nowStr;
}

/* ---------- GAP callback ---------- */
void gapCB(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t* p){
  if(ev != ESP_GAP_BLE_SCAN_RESULT_EVT) return;
  auto& r = p->scan_rst;
  if(r.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;
  if(memcmp(r.bda, SHELLY_MAC, 6)) return;
  g_rssi      = r.rssi;
  g_lastAdvMs = millis();
}

/* ---------- Wi-Fi ---------- */
bool wifiUp(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<15000) delay(250);
  return WiFi.isConnected();
}
void ntpSync(){ configTime(0,0,"pool.ntp.org"); }

/* ---------- setup ---------- */
void setup(){
  Serial.begin(115200); delay(300);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);

  hspi.begin(40,-1,45);
  tft.init(240,320); tft.setRotation(1);          // landscape
  tft.setAddrWindow(0,34,320,172);                // offset to glass
  tft.fillScreen(COL_BG);
  tft.setTextSize(2); tft.setTextColor(COL_TXT, COL_BG);

  /* static labels */
  tft.setCursor(X0, Y0);         tft.print(F("UTC "));
  tft.setCursor(X0, Y0+LH);      tft.print(F("MAC  F4:B3:B1:77:B4:AD"));
  tft.setCursor(X0, Y0+2*LH);    tft.print(F("SSID "));
  tft.setCursor(X0, Y0+3*LH);    tft.print(F("RSSI -- dBm"));
  drawBarGlyph(0);

  if(wifiUp()) ntpSync();

  btStart(); esp_bluedroid_init(); esp_bluedroid_enable();
  esp_ble_gap_register_callback(gapCB);
  esp_ble_gap_set_scan_params(&scanParams);
  esp_ble_gap_start_scanning(0);
}

/* ---------- loop ---------- */
void loop(){
  /* UTC refresh */
  static uint32_t lastTick=0;
  if(millis()-lastTick >= 1000){
    lastTick = millis();
    updateUTC();
  }

  /* RSSI row & bars */
  static int16_t prevRSSI = -128;
  if(g_rssi != prevRSSI){
    prevRSSI = g_rssi;
    String txt = (g_rssi == -128)
                   ? String(F("RSSI -- dBm"))
                   : String("RSSI ") + g_rssi + F(" dBm");
    printRow(3, txt);
    drawBarGlyph(g_rssi==-128 ? 0 : bars(g_rssi));
  }

  /* SSID once Wi-Fi connects */
  static String prevSSID;
  if(WiFi.isConnected()){
    String s = WiFi.SSID();
    if(s != prevSSID){
      printRow(2, String(F("SSID ")) + s);
      prevSSID = s;
    }
  }

  /* retry Wi-Fi every 30 s */
  static uint32_t lastWiFiTry=0;
  if(!WiFi.isConnected() && millis()-lastWiFiTry>30000){
    lastWiFiTry=millis();
    if(wifiUp()) ntpSync();
  }

  delay(10);
}
