#include <Arduino.h>
#include <stdint.h>
#include "rtc_rx8900.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "display.h"
#include "wifi_server.h"

// RTC割り込みピン
#define RTC_INT_PIN 5

// 主要グローバルは必要に応じてここに置く
uint16_t lastTargetSegs[NUM_DIGITS] = {0};

uint64_t dispCount = 0;
TaskHandle_t ClockTaskHandle = NULL;
rtc_time currentTime;

SemaphoreHandle_t xTimeMutex;
SemaphoreHandle_t xWireMutex;
SemaphoreHandle_t xIntMutex;

enum EffectMode {
  EFFECT_CUT = 0,
  EFFECT_CROSSFADE = 1,
  EFFECT_FADE = 2,
  EFFECT_STROKE = 3,
  EFFECT_ROLL = 4
};

uint8_t displayEffectMode = EFFECT_ROLL;

// クロスフェード管理用変数
uint16_t currentSegs[NUM_DIGITS]  = {0}; 
uint16_t previousSegs[NUM_DIGITS] = {0}; 
uint8_t  fadeSteps[NUM_DIGITS]    = {50, 50, 50, 50, 50, 50, 50, 50, 50}; 
uint8_t fadeState[NUM_DIGITS] = {0};

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

// Captive Portal & 管理ページ
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body{font-family:sans-serif; text-align:center; margin:20px;}
    .card{display:inline-block; text-align:left; padding:16px; border:1px solid #ddd; border-radius:8px;}
    input, select, button{margin:8px 0; padding:8px; font-size:14px; width:100%;}
    h2{margin:0 0 12px 0}
  </style>
  <title>VFD Clock Setup</title>
</head>
<body>
  <div class="card">
    <h2>Wi‑Fi Setup</h2>
    <form action="/save" method="POST">
      <input name="ssid" placeholder="WiFi SSID" required>
      <input type="password" name="pass" placeholder="Password">
      <button type="submit">Save & Restart</button>
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
    <button id="setMode">Set Mode</button>
    <div id="result" style="margin-top:8px;color:#006;min-height:18px"></div>
    <div style="height:8px"></div>
    <button id="syncTime">Sync From Device</button>
    <div id="syncResult" style="margin-top:8px;color:#060;min-height:18px"></div>
    <div style="height:8px"></div>
    <button id="forceSync">Force Sync from NTP server</button>
    <div id="forceResult" style="margin-top:8px;color:#060;min-height:18px"></div>
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
        if(js.connected) forceBtn.disabled = false; else forceBtn.disabled = true;
      }).catch(function(){ forceBtn.disabled = true; });
    }
    forceBtn.addEventListener('click', function(){
      fetch('/force_sync', {method:'POST'}).then(function(r){ return r.text(); }).then(function(t){ forceRes.innerText = t; updateStatus(); }).catch(function(){ forceRes.innerText = 'Error'; });
    });
    updateStatus();
    setInterval(updateStatus, 5000);
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

    uint32_t digitBit = convShiftData(i, 0);
    uint32_t oldSegs = convShiftData(i, previousSegs[i]) & ~digitBit;
    uint32_t newSegs = convShiftData(i, currentSegs[i]) & ~digitBit;

    uint32_t commonSegs  = oldSegs & newSegs;
    uint32_t fadeOutSegs = oldSegs & ~newSegs;
    uint32_t fadeInSegs  = newSegs & ~oldSegs;

    for (int bit = 0; bit < BAM_RESOLUTION; bit++) {
      uint32_t layerData = 0;
      if (displayEffectMode == EFFECT_CROSSFADE) {
        if (MAX_BRIGHT & (1 << bit)) layerData |= commonSegs;
        if (oldBright  & (1 << bit)) layerData |= fadeOutSegs;
        if (newBright  & (1 << bit)) layerData |= fadeInSegs;
      } else {
        if (oldBright & (1 << bit)) layerData |= oldSegs;
        if (newBright & (1 << bit)) layerData |= newSegs;
      }
      if (layerData > 0) layerData |= digitBit;
      hiddenBuffer[i][bit] = layerData;
    }
  }
}

void ClockTask(void *pvParameters){
    while(1){
      if(xSemaphoreTake(xIntMutex, portMAX_DELAY) == pdTRUE){
          if(xSemaphoreTake(xWireMutex, portMAX_DELAY) == pdTRUE){
              rx8900_get_time(&currentTime);
              uint8_t flagReg = 0;
              rx8900_read_reg(0x0E, &flagReg);
              flagReg &= ~0x20; 
              rx8900_write_reg(0x0E, flagReg);
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

  // display 初期化を display モジュールに委譲
  display_init();

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
  rx8900_get_time(&currentTime);

  xTaskCreatePinnedToCore(ClockTask, "ClockTask", 1024*2, NULL, 5, &ClockTaskHandle, 1);
  attachInterrupt(digitalPinToInterrupt(RTC_INT_PIN), handleRtcInterrupt, FALLING);
  startWiFiTask();
}

void loop() {
  uint16_t targetSegs[NUM_DIGITS] = {0};
  uint8_t dispNum[NUM_DIGITS] = {0}; // ★追加: 各桁の純粋な数値(0-9)を保持する配列

  // 各桁の数値を分解して代入
  dispNum[8] = 0; targetSegs[8] = 0x00;
  dispNum[7] = currentTime.separate.sec1;  targetSegs[7] = convNumToSeg[dispNum[7]];
  dispNum[6] = currentTime.separate.sec2;  targetSegs[6] = convNumToSeg[dispNum[6]];
  dispNum[5] = currentTime.separate.min1;  targetSegs[5] = convNumToSeg[dispNum[5]] | SEG_DOT;
  dispNum[4] = currentTime.separate.min2;  targetSegs[4] = convNumToSeg[dispNum[4]];
  dispNum[3] = currentTime.separate.hour1; targetSegs[3] = convNumToSeg[dispNum[3]] | SEG_DOT;
  dispNum[2] = currentTime.separate.hour2; targetSegs[2] = convNumToSeg[dispNum[2]];
  dispNum[1] = 0; targetSegs[1] = 0x00;
  dispNum[0] = 0; targetSegs[0] = 0x00;

  // 1. 値が変化した時のトリガー処理
  for (uint8_t i = 0; i < NUM_DIGITS; i++) {
    if (targetSegs[i] != lastTargetSegs[i]) {
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
  updateDisplay();

  delay(20);
  dispCount++;
}