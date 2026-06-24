#include <Arduino.h>
#include <stdint.h>
#include "rtc_rx8900.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "display.h"
#include "wifi_server.h"
#include <Preferences.h>
#include "switches.h"

// RTC割り込みピン
#define RTC_INT_PIN 5

// 主要グローバルは必要に応じてここに置く
uint16_t lastTargetSegs[NUM_DIGITS] = {0};

uint64_t dispCount = 0;
TaskHandle_t ClockTaskHandle = NULL;
rtc_time currentTime;
// 起動時に RTC の値を正常に読み込めたか（true=読み込み成功）
bool rtc_boot_ok = false;

// Uptime hours (cumulative powered-on hours). Persisted in Preferences.
volatile uint32_t totalUptimeHours = 0;
static Preferences uptimePrefs;
// Prefs queue for serializing NVS writes to a single task
typedef enum { PREF_SAVE_DISPLAY_MODE = 1, PREF_SAVE_UPTIME = 2 } PrefsCmdType;
typedef struct { uint8_t cmd; uint32_t value; } PrefsCmd;
QueueHandle_t prefsQueue = NULL;

// forward declaration for preferences mutex (defined later in file)
extern SemaphoreHandle_t xPrefsMutex;

void saveUptimeHours(){
  // enqueue save request for PrefsTask
  if(prefsQueue){
    PrefsCmd cmd = { .cmd = PREF_SAVE_UPTIME, .value = __atomic_load_n(&totalUptimeHours, __ATOMIC_SEQ_CST) };
    BaseType_t r = xQueueSend(prefsQueue, &cmd, 0);
    if(r != pdTRUE) Serial.println("Warning: prefsQueue send failed (uptime)");
  }
}

void loadUptimeHours(){
  if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
  uptimePrefs.begin("sys", false);
  totalUptimeHours = uptimePrefs.getUInt("uptime_h", 0);
  uptimePrefs.end();
  if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);
  Serial.printf("loadUptimeHours: totalUptimeHours=%u\n", (unsigned)totalUptimeHours);
}

// Load persisted display mode from Preferences
  // Load persisted display mode from Preferences
  // (moved below to be after EffectMode/displayEffectMode declaration)

void UptimeTask(void *pvParameters){
  (void)pvParameters;
  Serial.println("UptimeTask: started");
  // Wait for one hour intervals from boot: first delay is one hour.
  const TickType_t oneHour = pdMS_TO_TICKS(3600000UL);
  for(;;){
    vTaskDelay(oneHour);
    __atomic_add_fetch(&totalUptimeHours, 1, __ATOMIC_SEQ_CST);
    saveUptimeHours();
    Serial.printf("UptimeTask: incremented totalUptimeHours=%u\n", (unsigned)totalUptimeHours);
  }
}

SemaphoreHandle_t xTimeMutex;
SemaphoreHandle_t xWireMutex;
SemaphoreHandle_t xIntMutex;
SemaphoreHandle_t xPrefsMutex;

enum EffectMode {
  EFFECT_CUT = 0,
  EFFECT_CROSSFADE = 1,
  EFFECT_FADE = 2,
  EFFECT_STROKE = 3,
  EFFECT_ROLL = 4
};

uint8_t displayEffectMode = EFFECT_ROLL;
volatile bool displayModeDirty = false;
// If true, show Month(2)-Day(2)-space-Hour(2)-Min(2) layout instead of default
bool displayShowMDHM = false;

// Auto on/off config loaded from Preferences (also updated by wifi_server endpoints)
bool auto_enabled = false;
uint16_t auto_on_minutes = 0;
uint16_t auto_off_minutes = 0;

static bool last_display_state = true;

// クロスフェード管理用変数
uint16_t currentSegs[NUM_DIGITS]  = {0}; 
uint16_t previousSegs[NUM_DIGITS] = {0}; 
uint8_t  fadeSteps[NUM_DIGITS]    = {50, 50, 50, 50, 50, 50, 50, 50, 50}; 
uint8_t fadeState[NUM_DIGITS] = {0};

// Load persisted display mode from Preferences
void loadDisplayMode(){
  Preferences p;
  if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
  p.begin("display", true);
  uint32_t m = p.getUInt("mode", (uint32_t)EFFECT_ROLL);
  p.end();
  if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);
  if(m <= 4){
    displayEffectMode = (uint8_t)m;
  }
  Serial.printf("loadDisplayMode: loaded mode=%u\n", (unsigned)displayEffectMode);
}

// Load persisted auto on/off settings
void loadAutoSettings(){
  Preferences p;
  if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
  p.begin("auto", true);
  auto_enabled = p.getBool("enabled", false);
  auto_on_minutes = (uint16_t)p.getInt("on", 0);
  auto_off_minutes = (uint16_t)p.getInt("off", 0);
  p.end();
  if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);
  Serial.printf("loadAutoSettings: enabled=%d on=%u off=%u\n", (int)auto_enabled, (unsigned)auto_on_minutes, (unsigned)auto_off_minutes);
}

// PrefsTask: centralizes NVS writes to avoid concurrent Preferences access
void PrefsTask(void *pvParameters){
  (void)pvParameters;
  Preferences p;
  PrefsCmd cmd;
  for(;;){
    if(xQueueReceive(prefsQueue, &cmd, portMAX_DELAY) == pdTRUE){
      if(cmd.cmd == PREF_SAVE_DISPLAY_MODE){
        p.begin("display", false);
        p.putUInt("mode", (uint32_t)cmd.value);
        p.end();
        Serial.println("PrefsTask: saved display mode");
      } else if(cmd.cmd == PREF_SAVE_UPTIME){
        p.begin("sys", false);
        p.putUInt("uptime_h", cmd.value);
        p.end();
        Serial.println("PrefsTask: saved uptime");
      }
    }
  }
}

// Switch handling: SWB cycles display effect mode when pressed.
void switch_handler(SwitchId id, bool pressed){
  // Ignore switch actions while display is blanked (off period).
  if(display_is_blank()){
    Serial.println("switch_handler: ignored while display blanked");
    return;
  }
  if(id == SW_B && pressed){
    uint8_t newMode = (displayEffectMode + 1) % 5;
    displayEffectMode = newMode;
    displayModeDirty = true; // defer flash write to main loop
    Serial.printf("switch_handler: SWB pressed -> mode=%u (deferred save)\n", (unsigned)newMode);
  } else if(id == SW_C && pressed){
    // Toggle alternate display: Month(2) Day(2) <space> Hour(2) Min(2)
    displayShowMDHM = !displayShowMDHM;
    Serial.printf("switch_handler: SWC pressed -> displayShowMDHM=%d\n", (int)displayShowMDHM);
  }
}

// ===== 追加: 7セグメントの書き順定義テーブル =====
// A=0x01, B=0x02, C=0x04, D=0x08, E=0x10, F=0x20, G=0x40
const uint8_t strokeOrder[10][7] = {
  {0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00}, // 0: F->E->D->C->B->A
  {0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}, // 1: B->C
  {0x01, 0x02, 0x40, 0x10, 0x08, 0x00, 0x00}, // 2: A->B->G->E->D
  {0x01, 0x02, 0x40, 0x04, 0x08, 0x00, 0x00}, // 3: A->B->G->C->D
  {0x20, 0x40, 0x02, 0x04, 0x00, 0x00, 0x00}, // 4: F->G->B->C
  {0x01, 0x20, 0x40, 0x04, 0x08, 0x00, 0x00}, // 5: A->F->G->C->D
  {0x01, 0x20, 0x10, 0x08, 0x04, 0x40, 0x00}, // 6: A->F->E->D->C->G
  {0x01, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00}, // 7: A->B->C
  {0x01, 0x02, 0x40, 0x10, 0x08, 0x04, 0x20}, // 8: A->B->G->E->D->C->F
  {0x02, 0x01, 0x20, 0x40, 0x04, 0x08, 0x00}  // 9: B->A->F->G->C->D
};

// 各数字が何画で構成されているか
const uint8_t strokeCount[10] = {6, 2, 5, 5, 4, 5, 6, 3, 7, 6};

// ===== ロールエフェクト用の「上から下」へのレイヤーマスク =====
// VFDを物理的に上から下へスライスした5層のデータです
const uint8_t rollMasks[5] = {
  0x01, // 1層目: A (一番上の横棒)
  0x22, // 2層目: F, B (上部の縦棒2本)
  0x40, // 3層目: G (真ん中の横棒)
  0x14, // 4層目: E, C (下部の縦棒2本)
  0x08  // 5層目: D (一番下の横棒)
};

uint8_t dec2bcd(uint8_t val){
  return ((val / 10) << 4) | (val % 10);
}

uint8_t bcd2dec(uint8_t b){
  return (uint8_t)(((b >> 4) * 10) + (b & 0x0F));
}

// date <-> days conversion (civil) (algorithm by Howard Hinnant)
static int64_t days_from_civil(int y, unsigned m, unsigned d){
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y-399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d-1;
  const unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int &y, unsigned &m, unsigned &d){
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
  y = (int)(yoe) + (int)era * 400;
  const unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
  const unsigned mp = (5*doy + 2)/153;
  d = doy - (153*mp+2)/5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);
}

// Captive Portal & 管理ページ
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body{font-family:sans-serif; text-align:center; margin:20px;}
    /* Unified card width and centered layout for consistent appearance */
    .card{display:block; text-align:left; padding:16px; border:1px solid #ddd; border-radius:8px; max-width:520px; margin:0 auto 16px auto;}
    input, select, button{margin:8px 0; padding:8px; font-size:14px; width:100%; box-sizing:border-box}
    h2{margin:0 0 12px 0}
    /* Slightly larger primary button look */
    button{background:#f0f0f0;border:0;border-radius:6px}
    button.primary{background:#007bff;color:#fff;padding:12px}
  </style>
  <title>VFD Clock Setup</title>
</head>
<body>
  <div class="card">
    <h2>Wi‑Fi Setup</h2>
    <form action="/save" method="POST">
      <input name="ssid" placeholder="WiFi SSID" required>
      <input type="password" name="pass" placeholder="Password">
      <input name="ntp" placeholder="NTP servers (comma separated)">
      <label for="tz">Timezone</label>
      <select name="tz" id="tz" style="width:100%">
        <option value="-720">UTC-12</option>
        <option value="-660">UTC-11</option>
        <option value="-600">UTC-10 (HST)</option>
        <option value="-540">UTC-9</option>
        <option value="-480">UTC-8 (PST)</option>
        <option value="-420">UTC-7 (MST)</option>
        <option value="-360">UTC-6 (CST)</option>
        <option value="-300">UTC-5 (EST)</option>
        <option value="-240">UTC-4</option>
        <option value="-180">UTC-3</option>
        <option value="-120">UTC-2</option>
        <option value="-60">UTC-1</option>
        <option value="0">UTC</option>
        <option value="60">UTC+1 (CET)</option>
        <option value="120">UTC+2 (EET)</option>
        <option value="180">UTC+3 (MSK)</option>
        <option value="210">UTC+3:30</option>
        <option value="240">UTC+4</option>
        <option value="270">UTC+4:30</option>
        <option value="300">UTC+5</option>
        <option value="330">UTC+5:30 (IST)</option>
        <option value="345">UTC+5:45 (NPT)</option>
        <option value="360">UTC+6</option>
        <option value="390">UTC+6:30</option>
        <option value="420">UTC+7</option>
        <option value="480">UTC+8</option>
        <option value="525">UTC+8:45</option>
        <option value="540" selected>UTC+9 (JST)</option>
        <option value="570">UTC+9:30</option>
        <option value="600">UTC+10</option>
        <option value="660">UTC+11</option>
        <option value="720">UTC+12</option>
      </select>
      <button type="submit">Save & Restart</button>
      <div style="height:8px"></div>
      <button type="button" id="saveNtp">Save NTP (no restart)</button>
      <div style="height:8px"></div>
      <button type="button" id="saveTz">Save Timezone (no restart)</button>
      <div id="saveTzResult" style="margin-top:8px;color:#060;min-height:18px"></div>
      <div id="saveNtpResult" style="margin-top:8px;color:#060;min-height:18px"></div>
    </form>
  </div>

  <div style="height:16px"></div>

  <div class="card">
    <h2>Display Management</h2>
    <label for="mode">Effect Mode</label>
    <select id="mode">
      <option value="cut">Cut</option>
      <option value="crossfade">Crossfade</option>
      <option value="fade">Fade</option>
      <option value="stroke">Stroke</option>
      <option value="roll">Roll</option>
    </select>
    <div style="height:8px"></div>
    <button id="setMode">Set Mode</button>
    <div id="result" style="margin-top:8px;color:#006;min-height:18px"></div>
    <div id="uptime" style="margin-top:8px;color:#060;min-height:18px">Uptime hours: -</div>
    <div style="height:8px"></div>
    <button id="syncTime">Sync From Device</button>
    <div id="syncResult" style="margin-top:8px;color:#060;min-height:18px"></div>
    <div style="height:8px"></div>
    <button id="forceSync">Force Sync from NTP server</button>
    <div id="forceResult" style="margin-top:8px;color:#060;min-height:18px"></div>
  </div>
  
  <div class="card">
    <h2>Auto On/Off</h2>
    <label><input type="checkbox" id="autoEnabled"> Enable Auto On/Off</label>
    <div style="height:8px"></div>
    <label for="autoOn">Auto On (HH:MM)</label>
    <input type="time" id="autoOn" name="auto_on">
    <label for="autoOff">Auto Off (HH:MM)</label>
    <input type="time" id="autoOff" name="auto_off">
    <div style="height:8px"></div>
    <button id="saveAuto">Save Auto Settings</button>
    <div id="saveAutoResult" style="margin-top:8px;color:#060;min-height:18px"></div>
  </div>

    <div class="card">
      <h2>Device Status</h2>
      <div id="statusConnected" style="margin-bottom:6px">Connected: -</div>
      <div id="statusIP" style="margin-bottom:6px">IP: -</div>
      <div id="statusAPClients" style="margin-bottom:6px">AP clients: -</div>
      <div id="statusLastSync" style="margin-bottom:6px">Last sync: -</div>
      <div id="statusRtcOk" style="margin-bottom:6px">RTC boot read OK: -</div>
    </div>
  <script>
    document.getElementById('setMode').addEventListener('click', function(){
      var m = document.getElementById('mode').value;
      fetch('/mode?m=' + encodeURIComponent(m)).then(function(resp){
        return resp.text();
      }).then(function(text){
        document.getElementById('result').innerText = text;
      }).catch(function(err){
        document.getElementById('result').innerText = 'Error';
      });
    });

    // Brightness control removed; display fixed at 100%

    document.getElementById('syncTime').addEventListener('click', function(){
      var d = new Date();
      var csv = d.getFullYear()+','+(d.getMonth()+1)+','+d.getDate()+','+d.getHours()+','+d.getMinutes()+','+d.getSeconds()+','+d.getDay();
      fetch('/sync_time', {
        method: 'POST',
        headers: {'Content-Type':'text/plain'},
        body: csv
      }).then(function(resp){ return resp.text(); })
      .then(function(text){ document.getElementById('syncResult').innerText = text; })
      .catch(function(){ document.getElementById('syncResult').innerText = 'Error'; });
    });

    // Force Sync handling: enable only when ESP is connected to router
    var forceBtn = document.getElementById('forceSync');
    var forceRes = document.getElementById('forceResult');
    function updateStatus(){
      fetch('/status').then(function(r){ return r.json(); }).then(function(js){
        if(js.connected) {
          forceBtn.disabled = false;
          document.getElementById('statusConnected').innerText = 'Connected: Yes';
        } else {
          forceBtn.disabled = true;
          document.getElementById('statusConnected').innerText = 'Connected: No';
        }
        // IP (device as STA)
        document.getElementById('statusIP').innerText = 'IP: ' + (js.ip? js.ip : '-');
        document.getElementById('statusAPClients').innerText = 'AP clients: ' + (typeof js.ap_clients !== 'undefined' ? js.ap_clients : '-');
        // last_sync is epoch seconds (0 means never)
        if(js.last_sync && js.last_sync > 0){
          var d = new Date(js.last_sync * 1000);
          document.getElementById('statusLastSync').innerText = 'Last sync: ' + d.toLocaleString();
        } else {
          document.getElementById('statusLastSync').innerText = 'Last sync: -';
        }
        document.getElementById('statusRtcOk').innerText = 'RTC boot read OK: ' + (js.rtc_ok? 'Yes' : 'No');
      }).catch(function(){
        forceBtn.disabled = true;
        document.getElementById('statusConnected').innerText = 'Connected: ?';
        document.getElementById('statusIP').innerText = 'IP: ?';
      });
    }
    forceBtn.addEventListener('click', function(){
      fetch('/force_sync', {method:'POST'}).then(function(r){ return r.text(); }).then(function(t){ forceRes.innerText = t; updateStatus(); }).catch(function(){ forceRes.innerText = 'Error'; });
    });
    updateStatus();
    setInterval(updateStatus, 5000);
    // Uptime fetcher: update uptime hours display every minute
    function fetchUptime(){
      fetch('/uptime').then(function(r){ return r.json(); }).then(function(js){
        var el = document.getElementById('uptime'); if(el) el.innerText = 'Uptime hours: ' + js.uptime_hours + ' h';
      }).catch(function(){});
    }
    setInterval(fetchUptime, 60000);
    fetchUptime();

    // Prefill NTP input with current setting
    fetch('/ntp').then(function(r){ return r.json(); }).then(function(js){
      try{
        if(js.ntp){
          var el = document.querySelector('input[name="ntp"]');
          if(el) el.value = js.ntp;
        }
      }catch(e){}
    }).catch(function(){});

    // Prefill timezone select with current setting
    fetch('/tz').then(function(r){ return r.json(); }).then(function(js){
      try{
        if(typeof js.tz_min !== 'undefined'){
          var el = document.querySelector('select[name="tz"]');
          if(el) el.value = js.tz_min;
        }
      }catch(e){}
    }).catch(function(){});

    // Save NTP (no restart) handler
    var saveNtpBtn = document.getElementById('saveNtp');
    var saveNtpRes = document.getElementById('saveNtpResult');
    if(saveNtpBtn){
      saveNtpBtn.addEventListener('click', function(){
        var v = document.querySelector('input[name="ntp"]').value || '';
        var body = 'ntp=' + encodeURIComponent(v);
        fetch('/ntp', {method:'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: body})
        .then(function(r){ return r.text(); })
        .then(function(t){ saveNtpRes.innerText = t; })
        .catch(function(){ saveNtpRes.innerText = 'Error saving NTP'; });
      });
    }

    var saveTzBtn = document.getElementById('saveTz');
    var saveTzRes = document.getElementById('saveTzResult');
    if(saveTzBtn){
      saveTzBtn.addEventListener('click', function(){
        var el = document.querySelector('select[name="tz"]');
        var v = el ? el.value : '';
        var body = 'tz=' + encodeURIComponent(v);
        fetch('/tz', {method:'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: body})
        .then(function(r){ return r.text(); })
        .then(function(t){ saveTzRes.innerText = t; })
        .catch(function(){ saveTzRes.innerText = 'Error saving TZ'; });
      });
    }

    // Prefill Auto On/Off
    fetch('/auto').then(function(r){ return r.json(); }).then(function(js){
      try{
        var cb = document.getElementById('autoEnabled');
        var onEl = document.getElementById('autoOn');
        var offEl = document.getElementById('autoOff');
        if(cb) cb.checked = !!js.enabled;
        if(typeof js.on !== 'undefined' && onEl){
          var hh = Math.floor(js.on/60); var mm = js.on % 60;
          onEl.value = (('0'+hh).slice(-2))+ ':' + (('0'+mm).slice(-2));
        }
        if(typeof js.off !== 'undefined' && offEl){
          var hh = Math.floor(js.off/60); var mm = js.off % 60;
          offEl.value = (('0'+hh).slice(-2))+ ':' + (('0'+mm).slice(-2));
        }
      }catch(e){}
    }).catch(function(){});

    var saveAutoBtn = document.getElementById('saveAuto');
    var saveAutoRes = document.getElementById('saveAutoResult');
    if(saveAutoBtn){
      saveAutoBtn.addEventListener('click', function(){
        var cb = document.getElementById('autoEnabled');
        var onEl = document.getElementById('autoOn');
        var offEl = document.getElementById('autoOff');
        var body = '';
        if(cb && cb.checked) body += 'enabled=1&';
        if(onEl && onEl.value) body += 'on=' + encodeURIComponent(onEl.value) + '&';
        if(offEl && offEl.value) body += 'off=' + encodeURIComponent(offEl.value) + '&';
        fetch('/auto', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: body})
        .then(function(r){ return r.text(); }).then(function(t){ saveAutoRes.innerText = t; }).catch(function(){ saveAutoRes.innerText = 'Error saving Auto'; });
      });
    }
  </script>

</body>
</html>
)rawliteral";

// Wi-Fi server/task moved to wifi_server.{h,cpp}

// renderDisplay(): 高レベルの描画合成をここで行い、display.hiddenBufferへ書き込みます
void renderDisplay() {
  extern volatile uint32_t (*hiddenBuffer)[BAM_RESOLUTION];
  const uint8_t MAX_BRIGHT = 50;

  // hiddenBuffer を直接操作するため、一旦ゼロクリア
  memset((void*)hiddenBuffer, 0, sizeof(bamBufferA));

  // global brightness percent (0-100) applied to all effects
  uint8_t globalPct = display_get_brightness();

  for (uint8_t i = 0; i < NUM_DIGITS; i++) {
    uint8_t newBright = 0;
    uint8_t oldBright = 0;

    switch (displayEffectMode) {
      case EFFECT_CUT:
        newBright = MAX_BRIGHT;
        oldBright = 0;
        break;
      case EFFECT_CROSSFADE:
        newBright = fadeSteps[i];
        oldBright = MAX_BRIGHT - fadeSteps[i];
        break;
      case EFFECT_FADE:
        if (fadeState[i] == 1) {
          oldBright = MAX_BRIGHT - fadeSteps[i];
          newBright = 0;
        } else if (fadeState[i] == 2) {
          oldBright = 0;
          newBright = fadeSteps[i];
        } else {
          oldBright = 0;
          newBright = MAX_BRIGHT;
        }
        break;
      case EFFECT_STROKE:
      case EFFECT_ROLL:
        newBright = MAX_BRIGHT;
        oldBright = 0;
        break;
    }

    // apply global brightness scaling
    if (globalPct < 100) {
      float scale = (float)globalPct / 100.0f;
      newBright = (uint8_t)((float)newBright * scale + 0.5f);
      oldBright = (uint8_t)((float)oldBright * scale + 0.5f);
    }

    uint32_t digitBit = convShiftData(i, 0);
    uint32_t oldSegs = convShiftData(i, previousSegs[i]) & ~digitBit;
    uint32_t newSegs = convShiftData(i, currentSegs[i]) & ~digitBit;

    uint32_t commonSegs  = oldSegs & newSegs;
    uint32_t fadeOutSegs = oldSegs & ~newSegs;
    uint32_t fadeInSegs  = newSegs & ~oldSegs;

    // Compute per-digit BAM masks from logical brightness levels
    extern uint8_t display_get_bam_mask_for_percent(uint8_t percent);
    uint8_t maskNew = display_get_bam_mask_for_percent((uint8_t)((newBright * 100) / MAX_BRIGHT));
    uint8_t maskOld = display_get_bam_mask_for_percent((uint8_t)((oldBright * 100) / MAX_BRIGHT));
    // Use union for common mask to avoid turning off bits that differ between
    // mask encodings for old/new brightness — prevents disappearing segments
    uint8_t maskCommon = maskOld | maskNew;

    uint8_t globalMask = display_get_bam_mask_for_percent(globalPct);
    for (int bit = 0; bit < BAM_RESOLUTION; bit++) {
      uint32_t layerData = 0;
      if (displayEffectMode == EFFECT_CROSSFADE) {
        if (maskCommon & (1 << bit)) layerData |= commonSegs;
        if (maskOld    & (1 << bit)) layerData |= fadeOutSegs;
        if (maskNew    & (1 << bit)) layerData |= fadeInSegs;
      } else {
        if (maskOld & (1 << bit)) layerData |= oldSegs;
        if (maskNew & (1 << bit)) layerData |= newSegs;
      }
      if (layerData) layerData |= digitBit;
      // Gate with global brightness mask so 0% forces display off regardless of MCPWM
      if (!(globalMask & (1 << bit))) layerData = 0;
      hiddenBuffer[i][bit] = layerData;
    }
  }
}

void ClockTask(void *pvParameters){
  while(1){
    if(xSemaphoreTake(xIntMutex, portMAX_DELAY) == pdTRUE){
      if(xSemaphoreTake(xWireMutex, portMAX_DELAY) == pdTRUE){
        // Read raw RTC time (BCD)
        rx8900_get_time(&currentTime);
        uint8_t flagReg = 0;
        rx8900_read_reg(0x0E, &flagReg);
        flagReg &= ~0x20; 
        rx8900_write_reg(0x0E, flagReg);

        // Apply timezone offset (tz_offset_minutes) to produce local displayed time.
        // Convert BCD->ymdhms, to epoch, add offset, convert back to components.
        int yr = 2000 + bcd2dec(currentTime.bcd.year);
        unsigned mo = bcd2dec(currentTime.bcd.month);
        unsigned da = bcd2dec(currentTime.bcd.day);
        unsigned hh = bcd2dec(currentTime.bcd.hour);
        unsigned mm = bcd2dec(currentTime.bcd.min);
        unsigned ss = bcd2dec(currentTime.bcd.sec);

        int64_t days = days_from_civil(yr, mo, da);
        int64_t epoch = days * 86400 + (int64_t)hh*3600 + (int64_t)mm*60 + (int64_t)ss;
        epoch += (int64_t)tz_offset_minutes * 60; // apply offset

        // Decompose epoch back to y/m/d/h/m/s
        int yy; unsigned mmn, dd;
        int64_t days2 = epoch / 86400;
        int64_t secs_of_day = epoch % 86400;
        if(secs_of_day < 0){ secs_of_day += 86400; days2 -= 1; }
        civil_from_days(days2, yy, mmn, dd);
        unsigned h2 = (unsigned)(secs_of_day / 3600);
        unsigned m2 = (unsigned)((secs_of_day % 3600) / 60);
        unsigned s2 = (unsigned)(secs_of_day % 60);

        // Store back into currentTime as BCD
        currentTime.bcd.year = dec2bcd((uint8_t)(yy - 2000));
        currentTime.bcd.month = dec2bcd((uint8_t)mmn);
        currentTime.bcd.day = dec2bcd((uint8_t)dd);
        // weekday is bitmask in RTC; compute tm_wday (0=Sun)
        // compute weekday via epoch mod 7: 1970-01-01 was Thu (4)
        int wday = (int)((4 + days2) % 7);
        if(wday < 0) wday += 7;
        currentTime.bcd.weekday = 1 << (wday & 0x07);
        currentTime.bcd.hour = dec2bcd((uint8_t)h2);
        currentTime.bcd.min = dec2bcd((uint8_t)m2);
        currentTime.bcd.sec = dec2bcd((uint8_t)s2);

        xSemaphoreGive(xWireMutex);
      }
    }
  }
}

void IRAM_ATTR handleRtcInterrupt(){
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(xIntMutex, &xHigherPriorityTaskWoken);
  if(xHigherPriorityTaskWoken == pdTRUE){
    portYIELD_FROM_ISR();
  }
}

void setup() {
  Serial.begin(115200);
  xTimeMutex = xSemaphoreCreateMutex();
  xWireMutex = xSemaphoreCreateMutex();
  xIntMutex = xSemaphoreCreateBinary();
  xPrefsMutex = xSemaphoreCreateMutex();
  prefsQueue = xQueueCreate(8, sizeof(PrefsCmd));
  if(prefsQueue) xTaskCreatePinnedToCore(PrefsTask, "PrefsTask", 1024*8, NULL, 3, NULL, 1);

  // display 初期化を display モジュールに委譲
  display_init();
  // switches (buttons) init
  switches_init();

  pinMode(RTC_INT_PIN, INPUT);

  rx8900_init();
  if(rx8900_is_voltage_low()){
    rx8900_hard_init();
    currentTime.bcd.year = 26;
    currentTime.bcd.month = 1;
    currentTime.bcd.day = 1;
    currentTime.bcd.hour = 0;
    currentTime.bcd.min = 0;
    currentTime.bcd.sec = 0;
    rx8900_set_time(currentTime);
  }
  // 起動時の読み取り結果をフラグに保持
  int r = rx8900_get_time(&currentTime);
  rtc_boot_ok = (r == 0);
  // load saved display mode before starting tasks so UI and rendering use it
  loadDisplayMode();

  xTaskCreatePinnedToCore(ClockTask, "ClockTask", 1024*2, NULL, 5, &ClockTaskHandle, 1);
  attachInterrupt(digitalPinToInterrupt(RTC_INT_PIN), handleRtcInterrupt, FALLING);

  // Register switch callback and decide whether to start WiFi.
  switches_register_callback(switch_handler);
  // If SWB was held at power-on, do not start WiFi (user requested)
  // Use immediate raw read to detect a button held during power-on.
  if(switches_was_held_at_boot(SW_B)){
    Serial.println("SWB held at boot - WiFi disabled by user");
  } else {
    startWiFiTask();
  }

  // load persisted uptime hours and start uptime task
  loadUptimeHours();
  // load auto on/off settings
  loadAutoSettings();
  // Increase stack for UptimeTask to avoid overflow during NVS/Preferences operations
  xTaskCreatePinnedToCore(UptimeTask, "UptimeTask", 1024*2, NULL, 1, NULL, 1);
}

void loop() {
  // Persist display mode if requested by switch handler (deferred to main loop)
  if(displayModeDirty){
    displayModeDirty = false;
    // enqueue save request for PrefsTask; fallback to direct save if queue missing
    if(prefsQueue){
      PrefsCmd cmd = { .cmd = PREF_SAVE_DISPLAY_MODE, .value = displayEffectMode };
      BaseType_t r = xQueueSend(prefsQueue, &cmd, 0);
      if(r != pdTRUE) Serial.println("Warning: prefsQueue send failed (display mode)");
    } else {
      Preferences p;
      if(xPrefsMutex) xSemaphoreTake(xPrefsMutex, portMAX_DELAY);
      p.begin("display", false);
      p.putUInt("mode", (uint32_t)displayEffectMode);
      p.end();
      if(xPrefsMutex) xSemaphoreGive(xPrefsMutex);
      Serial.printf("Saved display mode=%u to Preferences\n", (unsigned)displayEffectMode);
    }
  }
  uint16_t targetSegs[NUM_DIGITS] = {0};
  uint8_t dispNum[NUM_DIGITS] = {0}; // ★追加: 各桁の純粋な数値(0-9)を保持する配列

  // 各桁の数値を分解して代入
  if(displayShowMDHM){
    // Month(2), Day(2), space, Hour(2), Min(2)
    dispNum[0] = currentTime.separate.month2; targetSegs[0] = convNumToSeg[dispNum[0]];
    dispNum[1] = currentTime.separate.month1; targetSegs[1] = convNumToSeg[dispNum[1]];
    dispNum[2] = currentTime.separate.day2;   targetSegs[2] = convNumToSeg[dispNum[2]];
    dispNum[3] = currentTime.separate.day1;   targetSegs[3] = convNumToSeg[dispNum[3]];
    dispNum[4] = 0;                           targetSegs[4] = 0x00; // space
    dispNum[5] = currentTime.separate.hour2;  targetSegs[5] = convNumToSeg[dispNum[5]];
    dispNum[6] = currentTime.separate.hour1;  targetSegs[6] = convNumToSeg[dispNum[6]];
    dispNum[7] = currentTime.separate.min2;   targetSegs[7] = convNumToSeg[dispNum[7]];
    dispNum[8] = currentTime.separate.min1;   targetSegs[8] = convNumToSeg[dispNum[8]];
  } else {
    dispNum[8] = currentTime.separate.sec1;  targetSegs[8] = convNumToSeg[dispNum[8]];
    dispNum[7] = currentTime.separate.sec2;  targetSegs[7] = convNumToSeg[dispNum[7]];
    dispNum[6] = currentTime.separate.min1;  targetSegs[6] = convNumToSeg[dispNum[6]] | SEG_DOT;
    dispNum[5] = currentTime.separate.min2;  targetSegs[5] = convNumToSeg[dispNum[5]];
    dispNum[4] = currentTime.separate.hour1; targetSegs[4] = convNumToSeg[dispNum[4]] | SEG_DOT;
    dispNum[3] = currentTime.separate.hour2; targetSegs[3] = convNumToSeg[dispNum[3]];
    dispNum[2] = 0; targetSegs[2] = 0x00;
    dispNum[1] = currentTime.separate.day1;  targetSegs[1] = convNumToSeg[dispNum[1]];
    dispNum[0] = currentTime.separate.day2;  targetSegs[0] = convNumToSeg[dispNum[0]];
  }

  // 1. 値が変化した時のトリガー処理
  // NOTE: ドットは点滅用途で秒単位で変化するため、エフェクトトリガーの判定から除外する
  for (uint8_t i = 0; i < NUM_DIGITS; i++) {
    uint16_t maskedNew = (uint16_t)(targetSegs[i] & ~SEG_DOT);
    uint16_t maskedOld = (uint16_t)(lastTargetSegs[i] & ~SEG_DOT);
    if (maskedNew != maskedOld) {
      lastTargetSegs[i] = targetSegs[i];

      if (displayEffectMode == EFFECT_CUT) {
        previousSegs[i] = currentSegs[i];
        currentSegs[i] = targetSegs[i];
        fadeSteps[i] = 50;
      } 
      else if (displayEffectMode == EFFECT_CROSSFADE) {
        previousSegs[i] = currentSegs[i];
        currentSegs[i] = targetSegs[i];
        fadeSteps[i] = 0; 
      } 
      else if (displayEffectMode == EFFECT_FADE) {
        previousSegs[i] = currentSegs[i]; 
        fadeSteps[i] = 0;
        fadeState[i] = 1; 
      }
      else if (displayEffectMode == EFFECT_STROKE) {
        // ★追加: 書き順モードのトリガー
        previousSegs[i] = currentSegs[i];
        // 数字部分(0x7Fのマスク)だけを一度消去。コロンやドットなどの特殊セグメントは残す
        currentSegs[i] = targetSegs[i] & ~0x7F; 
        fadeSteps[i] = 0;
      }
      else if (displayEffectMode == EFFECT_ROLL){
        previousSegs[i] = currentSegs[i];
        fadeSteps[i] = 0;
        fadeState[i] = 1;
      }
    }
  }

  // 2. 毎フレームのアニメーション進行処理（50fps）
  for (uint8_t i = 0; i < NUM_DIGITS; i++) {
    if (displayEffectMode == EFFECT_CROSSFADE) {
      if (fadeSteps[i] < 50) {
        fadeSteps[i] += 2; 
        if (fadeSteps[i] > 50) fadeSteps[i] = 50;
      }
    } 
    else if (displayEffectMode == EFFECT_FADE) {
      if (fadeState[i] == 1) {
        fadeSteps[i] += 4; 
        if (fadeSteps[i] >= 50) {
          currentSegs[i] = targetSegs[i]; 
          fadeSteps[i] = 0;
          fadeState[i] = 2; 
        }
      } 
      else if (fadeState[i] == 2) {
        fadeSteps[i] += 4; 
        if (fadeSteps[i] >= 50) {
          fadeSteps[i] = 50;
          fadeState[i] = 0; 
        }
      }
    }
    else if (displayEffectMode == EFFECT_STROKE) {
      // ガード処理
      if(targetSegs[i] == 0x00){
        currentSegs[i] = 0x00;
        fadeSteps[i] = 50;
        continue;
      }
      
      // 書き順モードのフレーム進行
      if (fadeSteps[i] < 50) {
        fadeSteps[i] += 3; // ★値を大きくすると高速描画、小さくするとじっくり描画
        if (fadeSteps[i] > 50) fadeSteps[i] = 50;

        uint8_t num = dispNum[i];          // この桁の数字 (0-9)
        uint8_t totalDraw = strokeCount[num]; // この数字の総画数
        
        // 現在のステップ(0-50)から、いま何画目まで描画すべきかを計算 (0 ～ totalDraw)
        uint8_t currentDraw = (fadeSteps[i] * totalDraw) / 50;

        // 一旦、特殊セグメント（ドット等）だけにした状態から、現在の画数までセグメントを合成
        uint16_t tempSeg = targetSegs[i] & ~0x7F;
        for (uint8_t s = 0; s < currentDraw; s++) {
          tempSeg |= strokeOrder[num][s];
        }
        currentSegs[i] = tempSeg;
      }
    }
    else if (displayEffectMode == EFFECT_ROLL) {
      // ★追加: ロールエフェクトのフレーム進行処理
      if (fadeState[i] == 1) {
        // --- フェーズ1: ロールアウト（上から消える） ---
        fadeSteps[i] += 4; // アニメーション速度
        if (fadeSteps[i] >= 50) {
          fadeSteps[i] = 0;
          fadeState[i] = 2; // 消えきったらロールインへ移行
        } else {
          // 進行度(0-50)から、現在何層目まで消すかを算出 (0-5層)
          uint8_t layers = fadeSteps[i] / 10; 
          uint16_t tempSeg = previousSegs[i]; // 前の数字をベースにする

          // 上から順番に、指定された層の数だけマスクを反転させてANDを取り、消去していく
          for (uint8_t s = 0; s < layers && s < 5; s++) {
            tempSeg &= ~rollMasks[s]; 
          }
          currentSegs[i] = tempSeg;
        }
      } 
      else if (fadeState[i] == 2) {
        // --- フェーズ2: ロールイン（上から現れる） ---
        fadeSteps[i] += 4; 
        if (fadeSteps[i] >= 50) {
          fadeSteps[i] = 50;
          fadeState[i] = 0; // アニメーション完了
          currentSegs[i] = targetSegs[i];
        } else {
          uint8_t layers = fadeSteps[i] / 10;
          uint16_t tempSeg = targetSegs[i] & ~0x7F; // 特殊セグメント(ドット等)だけを残したベース
          
          // ターゲットの数字のうち、上から順番に指定された層の数だけORで追加していく
          for (uint8_t s = 0; s < layers && s < 5; s++) {
            tempSeg |= (targetSegs[i] & rollMasks[s]); 
          }
          currentSegs[i] = tempSeg;
        }
      }
    }
  }

  renderDisplay();
  // If alternate display active, blink the dot on hour ones digit (index 6)
  if(displayShowMDHM){
    if((currentTime.separate.sec1 & 0x1) == 0) currentSegs[6] |= SEG_DOT;
    else currentSegs[6] &= ~SEG_DOT;
  }
  updateDisplay();

  // Auto on/off enforcement: currentTime is already adjusted to tz in ClockTask
  if(auto_enabled){
    uint8_t ch = bcd2dec(currentTime.bcd.hour);
    uint8_t cm = bcd2dec(currentTime.bcd.min);
    uint16_t minutes = (uint16_t)ch * 60 + (uint16_t)cm;
    bool shouldShow;
    if(auto_on_minutes == auto_off_minutes){
      shouldShow = true; // degenerate -> always on
    } else if(auto_on_minutes < auto_off_minutes){
      shouldShow = (minutes >= auto_on_minutes && minutes < auto_off_minutes);
    } else {
      // wrap around midnight
      shouldShow = (minutes >= auto_on_minutes || minutes < auto_off_minutes);
    }
    if(shouldShow != last_display_state){
      if(shouldShow){
        // Make display visible again
        display_set_blank(false);
        display_set_enabled(true);
      } else {
        // Keep BAM running, but force blanking so nothing is visible
        display_set_blank(true);
        // Ensure display remains enabled so dynamic rendering continues
        display_set_enabled(true);
      }
      last_display_state = shouldShow;
    }
  } else {
    if(!last_display_state){ display_set_blank(false); display_set_enabled(true); last_display_state = true; }
  }

  delay(20);
  dispCount++;
}