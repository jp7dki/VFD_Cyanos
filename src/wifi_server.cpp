#include "wifi_server.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPI.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include "rtc_rx8900.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;
WebServer server(80);
Preferences preferences;

extern SemaphoreHandle_t xWireMutex; // 実体は main.cpp にある
extern hw_timer_t *bam_timer; // display.h に extern
extern uint8_t dec2bcd(uint8_t val); // main.cpp に実装
extern uint8_t displayEffectMode; // main.cpp にある表示モード変数
extern uint32_t totalUptimeHours; // main.cpp にある累計時間
extern SemaphoreHandle_t xPrefsMutex; // declared in main.cpp
extern bool rtc_boot_ok; // main.cpp に追加した起動時 RTC 読み取りフラグ
// forward declarations
bool syncNTP();

// 最終 NTP 同期時刻（UNIX epoch）。0=未同期
static time_t last_ntp_sync = 0;
// NTP サーバ設定（カンマ区切りで複数可）。読み出しは WiFiTask 起動時に行う
static String ntp_pref = String("pool.ntp.org");

// GET /ntp -> {"ntp":"pool.ntp.org"}
void handleGetNtp(){
  if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
  preferences.begin("wifi", true);
  String v = preferences.getString("ntp", ntp_pref);
  preferences.end();
  if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);
  String json = String("{") + "\"ntp\":\"" + v + "\"}";
  server.send(200, "application/json", json);
}

// POST /ntp (form or body arg 'ntp') -> saves without restart
void handleSetNtp(){
  String v = server.arg("ntp");
  if(v.length() == 0){
    server.send(400, "text/plain", "ntp parameter required");
    return;
  }
  if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
  preferences.begin("wifi", false);
  preferences.putString("ntp", v);
  preferences.end();
  if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);
  ntp_pref = v;
  server.send(200, "text/plain", "ntp saved");
}

void handleRoot() {
  extern const char* htmlPage;
  server.send(200, "text/html", htmlPage);
}

void handleNotFound() {
  server.sendHeader("Location", String("http://") + apIP.toString(), true);
  server.send(302, "text/plain", "");
}

void handleSave() {
  timerAlarmDisable(bam_timer);
  digitalWrite(VFD_RCLK_PIN, LOW);
  SPI.beginTransaction(SPISettings(3000000, MSBFIRST, SPI_MODE0));
  SPI.transfer(0x00); SPI.transfer(0x00); SPI.transfer(0x00);
  SPI.endTransaction();
  digitalWrite(VFD_RCLK_PIN, HIGH);

  String newSSID = server.arg("ssid");
  String newPass = server.arg("pass");
  String newNtp = server.arg("ntp");

  if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
  preferences.begin("wifi", false);
  preferences.putString("ssid", newSSID);
  preferences.putString("pass", newPass);
  if(newNtp.length()>0) preferences.putString("ntp", newNtp);
  preferences.end();
  if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);


  server.send(200, "text/html", "<h2>Saved! Restarting...</h2>");
  delay(1000);
  ESP.restart();
}

// クライアント側のローカル日時を受け取りRTCへ設定する
// 受信フォーマット(plain text, CSV): "YYYY,MM,DD,hh,mm,ss,wday"
// wday: 0-6 (Sun=0)
void handleSyncTime(){
  String body = server.arg("plain");
  if(body.length() == 0){
    server.send(400, "text/plain", "empty body");
    return;
  }

  char buf[128];
  body.toCharArray(buf, sizeof(buf));
  int yr=0,mo=0,da=0,hh=0,mi=0,se=0,wd=0;
  int n = sscanf(buf, "%d,%d,%d,%d,%d,%d,%d", &yr,&mo,&da,&hh,&mi,&se,&wd);
  if(n < 6){
    server.send(400, "text/plain", "invalid format");
    return;
  }

  rtc_time t;
  t.bcd.year = dec2bcd((uint8_t)(yr - 2000));
  t.bcd.month = dec2bcd((uint8_t)mo);
  t.bcd.day = dec2bcd((uint8_t)da);
  t.bcd.weekday = 1 << (wd & 0x07);
  t.bcd.hour = dec2bcd((uint8_t)hh);
  t.bcd.min = dec2bcd((uint8_t)mi);
  t.bcd.sec = dec2bcd((uint8_t)se);

  extern SemaphoreHandle_t xWireMutex;
  if(xSemaphoreTake(xWireMutex, pdMS_TO_TICKS(2000)) == pdTRUE){
    rx8900_set_time(t);
    uint8_t ctrl1 = 0;
    rx8900_read_reg(0x0F, &ctrl1);
    rx8900_write_reg(0x0F, ctrl1 | 0x01);
    rx8900_write_reg(0x0F, ctrl1 & ~0x01);
    xSemaphoreGive(xWireMutex);
    // 手動同期であれば最終同期時刻を更新
    last_ntp_sync = time(NULL);
    server.send(200, "text/plain", "RTC updated");
  } else {
    server.send(500, "text/plain", "bus busy");
  }
}

void handleStatus(){
  bool connected = (WiFi.status() == WL_CONNECTED);
  String ipStr = "";
  if(connected){ ipStr = WiFi.localIP().toString(); }
  uint8_t apClients = WiFi.softAPgetStationNum();
  unsigned long lastSync = (unsigned long)last_ntp_sync;
  // read ntp pref for status
  String ntp_show = ntp_pref;
  if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
  preferences.begin("wifi", true);
  ntp_show = preferences.getString("ntp", ntp_show);
  preferences.end();
  if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);
  String json = String("{") +
    "\"connected\":" + (connected?"true":"false") + "," +
    "\"ip\":\"" + ipStr + "\"," +
    "\"ap_clients\":" + String((int)apClients) + "," +
    "\"last_sync\":" + String((unsigned long)lastSync) + "," +
    "\"ntp\":\"" + ntp_show + "\"," +
    "\"rtc_ok\":" + (rtc_boot_ok?"true":"false") +
    "}";
  server.send(200, "application/json", json);
}

// Uptime handler
void handleGetUptime(){
  uint32_t h = __atomic_load_n(&totalUptimeHours, __ATOMIC_SEQ_CST);
  String json = String("{") + "\"uptime_hours\":" + String((unsigned)h) + "}";
  server.send(200, "application/json", json);
}

// NOTE: /uptime_inc (test-only) handler removed for production builds

// Brightness handlers
void handleGetBrightness(){
  // Brightness control removed; always report fixed 100%
  String json = String("{") + "\"brightness\":10," + "\"percent\":100}";
  server.send(200, "application/json", json);
}

void handleSetBrightness(){
  String v = server.arg("value");
  if(v.length() == 0) v = server.arg("b");
  if(v.length() == 0){
    server.send(400, "text/plain", "value required");
    return;
  }
  // Brightness control is disabled; inform client
  Serial.printf("handleSetBrightness: request value=%s -> ignored (fixed at 100%%)\n", v.c_str());
  server.send(403, "text/plain", "brightness control disabled; fixed at 100%");
}

void handleForceSync(){
  if (WiFi.status() != WL_CONNECTED) {
    server.send(400, "text/plain", "not connected to router");
    return;
  }
  bool ok = syncNTP();
  if (ok) server.send(200, "text/plain", "NTP sync OK");
  else server.send(500, "text/plain", "NTP sync failed");
}

// /mode?mode=<0-4> or /mode?m=<name>
void handleSetMode(){
  String m = server.arg("mode");
  if(m == "") m = server.arg("m");

  uint8_t newMode = 0xFF;
  if(m.length() == 0){
    server.send(400, "text/plain", "mode parameter required");
    return;
  }

  // 数字で指定された場合
  bool isNum = true;
  for (size_t i=0;i<m.length();i++) if(!isDigit(m[i])) { isNum = false; break; }
  if(isNum){
    int v = m.toInt();
    if(v >=0 && v <=4) newMode = (uint8_t)v;
  } else {
    String s = m;
    s.toLowerCase();
    if(s == "cut") newMode = 0;
    else if(s == "crossfade") newMode = 1;
    else if(s == "fade") newMode = 2;
    else if(s == "stroke") newMode = 3;
    else if(s == "roll") newMode = 4;
  }

  if(newMode == 0xFF){
    server.send(400, "text/plain", "invalid mode");
    return;
  }

  displayEffectMode = newMode;
  // persist display mode to Preferences so it survives power cycles
  if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
  preferences.begin("display", false);
  preferences.putUInt("mode", (uint32_t)newMode);
  preferences.end();
  if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);

  Serial.printf("handleSetMode: saved mode=%u\n", (unsigned)newMode);
  server.send(200, "text/plain", String("mode set to ") + String((int)newMode));
}

bool syncNTP() {
  // Parse ntp_pref (comma separated) and call configTime with up to 3 servers
  String list = ntp_pref;
  if(list.length() == 0){ list = String("pool.ntp.org"); }
  // split by comma
  std::vector<String> servers;
  int start = 0;
  for(int i=0;i<=list.length();i++){
    if(i==list.length() || list.charAt(i)==','){
      String token = list.substring(start, i);
      token.trim();
      if(token.length()>0) servers.push_back(token);
      start = i+1;
    }
  }
  if(servers.size() == 0) servers.push_back(String("pool.ntp.org"));
  if(servers.size() == 1) configTime(9 * 3600, 0, servers[0].c_str());
  else if(servers.size() == 2) configTime(9 * 3600, 0, servers[0].c_str(), servers[1].c_str());
  else configTime(9 * 3600, 0, servers[0].c_str(), servers[1].c_str(), servers[2].c_str());
  Serial.println("Waiting for NTP time sync...");
  struct tm timeinfo;
  bool syncSuccess = false;
  for (int i = 0; i < 3; i++) {
    if (getLocalTime(&timeinfo, 5000)) {
      syncSuccess = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  if (!syncSuccess) {
    Serial.println("NTP Sync Failed.");
    return false;
  }
  Serial.println("ESP32 Internal Clock Synced with NTP.");
  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint32_t current_ms = tv.tv_usec / 1000;
  uint32_t wait_ms = 1000 - current_ms;
  vTaskDelay(pdMS_TO_TICKS(wait_ms));
  time_t now = tv.tv_sec + 1;
  struct tm *t_exact = localtime(&now);

  rtc_time t;
  t.bcd.year    = dec2bcd(t_exact->tm_year - 100);
  t.bcd.month   = dec2bcd(t_exact->tm_mon + 1);
  t.bcd.day     = dec2bcd(t_exact->tm_mday);
  t.bcd.weekday = 1 << t_exact->tm_wday;
  t.bcd.hour    = dec2bcd(t_exact->tm_hour);
  t.bcd.min     = dec2bcd(t_exact->tm_min);
  t.bcd.sec     = dec2bcd(t_exact->tm_sec);

  extern SemaphoreHandle_t xWireMutex;
  if(xSemaphoreTake(xWireMutex, portMAX_DELAY) == pdTRUE) {
    rx8900_set_time(t);
    uint8_t ctrl1 = 0;
    rx8900_read_reg(0x0F, &ctrl1);
    rx8900_write_reg(0x0F, ctrl1 | 0x01);
    rx8900_write_reg(0x0F, ctrl1 & ~0x01);
    xSemaphoreGive(xWireMutex);
    Serial.printf("RTC Perfectly Synced! Time: %02d:%02d:%02d\n", t_exact->tm_hour, t_exact->tm_min, t_exact->tm_sec);
    // NTP による同期成功として時刻を記録
    last_ntp_sync = time(NULL);
    return true;
  }
  return false;
}

// WiFiTask 本体（AP + STA, WebServer, DNS, NTP 管理）
void WiFiTask(void *pvParameters) {
  WiFi.persistent(false);

  if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
  preferences.begin("wifi", false);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  preferences.end();
  if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);

  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_AP_STA);
  vTaskDelay(pdMS_TO_TICKS(50));

  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("VFD_Clock_Setup", "12345678");
  Serial.println("Access Point Started.");

  dnsServer.start(DNS_PORT, "*", apIP);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/mode", HTTP_GET, handleSetMode);
  server.on("/mode", HTTP_POST, handleSetMode);
  server.on("/brightness", HTTP_GET, handleGetBrightness);
  server.on("/brightness", HTTP_POST, handleSetBrightness);
  server.on("/uptime", HTTP_GET, handleGetUptime);
  server.on("/sync_time", HTTP_POST, handleSyncTime);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/ntp", HTTP_GET, handleGetNtp);
  server.on("/ntp", HTTP_POST, handleSetNtp);
  server.on("/force_sync", HTTP_POST, handleForceSync);
  server.onNotFound(handleNotFound);
  server.begin();

  unsigned long lastSyncMillis = 0;
  bool forceSync = true;
  const unsigned long syncIntervalInterval = 86400000UL;

  if (ssid != "") {
    Serial.printf("Connecting to Router: %s\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    Serial.println("No Router SSID set. Running in AP-Only mode.");
    forceSync = false;
  }

  // Load NTP preference
  if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
  preferences.begin("wifi", true);
  ntp_pref = preferences.getString("ntp", ntp_pref);
  preferences.end();
  if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);

  while (1) {
    dnsServer.processNextRequest();
    server.handleClient();

    if (ssid != "" && (forceSync || (millis() - lastSyncMillis >= syncIntervalInterval))) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Router connected. Triggering periodic NTP sync...");
        if (syncNTP()) {
          lastSyncMillis = millis();
          forceSync = false;
        } else {
          lastSyncMillis = millis() - syncIntervalInterval + 3600000UL;
          forceSync = false;
        }
      } else {
        static unsigned long lastReconnectAttempt = 0;
        if (millis() - lastReconnectAttempt > 30000) {
          Serial.println("Router disconnected. Reconnecting...");
          WiFi.begin(ssid.c_str(), pass.c_str());
          lastReconnectAttempt = millis();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void startWiFiTask(){
  xTaskCreatePinnedToCore(WiFiTask, "WiFiTask", 1024 * 8, NULL, 2, NULL, 0);
}
