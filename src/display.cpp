#include "display.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/mcpwm.h"

volatile uint32_t bamBufferA[NUM_DIGITS][BAM_RESOLUTION];
volatile uint32_t bamBufferB[NUM_DIGITS][BAM_RESOLUTION];

volatile uint32_t (*activeBuffer)[BAM_RESOLUTION] = bamBufferA;
volatile uint32_t (*hiddenBuffer)[BAM_RESOLUTION] = bamBufferB;

hw_timer_t *bam_timer = NULL;

portMUX_TYPE ptrMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

const uint16_t convNumToSeg[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static uint8_t currentBrightness = 50; // 0-100
static bool allowedLevel[101];

static void compute_allowed_levels(){
  const uint8_t weights[BAM_RESOLUTION] = {1,2,4,8,16,32};
  const uint8_t total = (1<<BAM_RESOLUTION) - 1; // 63
  for(int p=0;p<=100;p++) allowedLevel[p]=false;
  for(int p=0;p<=100;p++){
    // best mask for p
    uint8_t best_m = 0; uint32_t best_diff = UINT32_MAX; uint32_t target = (uint32_t)p * (uint32_t)total;
    int best_sum = 0;
    for(uint8_t m=0;m < (1<<BAM_RESOLUTION); m++){
      int sumW = 0; for(uint8_t b=0;b<BAM_RESOLUTION;b++) if(m & (1<<b)) sumW += weights[b];
      uint32_t diff = (sumW * 100 > target) ? (sumW * 100 - target) : (target - sumW * 100);
      if(diff < best_diff){ best_diff = diff; best_m = m; best_sum = sumW; }
    }
    // compare neighbors to detect stability
    int prev_sum = -1, next_sum = -1;
    if(p>0){ uint32_t t2 = (uint32_t)(p-1) * (uint32_t)total; uint32_t bd=UINT32_MAX; int s=0; for(uint8_t m=0;m < (1<<BAM_RESOLUTION); m++){ int sumW=0; for(uint8_t b=0;b<BAM_RESOLUTION;b++) if(m & (1<<b)) sumW+=weights[b]; uint32_t diff=(sumW*100>t2)?(sumW*100-t2):(t2-sumW*100); if(diff<bd){bd=diff; s=sumW;}} prev_sum=s; }
    if(p<100){ uint32_t t2 = (uint32_t)(p+1) * (uint32_t)total; uint32_t bd=UINT32_MAX; int s=0; for(uint8_t m=0;m < (1<<BAM_RESOLUTION); m++){ int sumW=0; for(uint8_t b=0;b<BAM_RESOLUTION;b++) if(m & (1<<b)) sumW+=weights[b]; uint32_t diff=(sumW*100>t2)?(sumW*100-t2):(t2-sumW*100); if(diff<bd){bd=diff; s=sumW;}} next_sum=s; }
    bool stable = true;
    if(prev_sum!=-1 && abs(best_sum - prev_sum) > 1) stable = false;
    if(next_sum!=-1 && abs(best_sum - next_sum) > 1) stable = false;
    if(stable) allowedLevel[p] = true;
  }
}

void display_set_brightness(uint8_t percent){
  if(percent > 100) percent = 100;
  // snap to nearest allowed level
  if(!allowedLevel[percent]){
    int best = -1; int bestDist = 1000;
    for(int d=0; d<=100; d++) if(allowedLevel[d]){
      int dist = abs(d - (int)percent);
      if(dist < bestDist){ bestDist = dist; best = d; }
    }
    if(best >= 0) percent = (uint8_t)best;
  }
  currentBrightness = percent;
  Serial.printf("display_set_brightness: snapped to %u%% (MCPWM fixed)\n", (unsigned)currentBrightness);
}

uint8_t display_get_brightness(){
  return currentBrightness;
}

// N-frame bundling: precompute N masks per digit so average over N frames
// approximates requested brightness while minimizing per-frame quantization error.
uint8_t display_get_bam_mask_for_digit(uint8_t digit){
  // Simple single-frame selection: choose mask whose weight sum best matches
  // requested brightness percentage. This is the pre-N-frame method.
  const uint8_t weights[BAM_RESOLUTION] = {1,2,4,8,16,32};
  const uint8_t total = (1<<BAM_RESOLUTION) - 1; // 63

  if(digit >= NUM_DIGITS) digit = digit % NUM_DIGITS;
  if (currentBrightness >= 100) return (uint8_t)((1<<BAM_RESOLUTION)-1);

  // find best mask by brute-force (64 combinations)
  uint8_t best_mask = 0;
  uint32_t best_diff = UINT32_MAX;
  uint32_t target = (uint32_t)currentBrightness * (uint32_t)total; // compare against sumW*100
  for(uint8_t m=0; m < (1<<BAM_RESOLUTION); m++){
    uint32_t sumW = 0;
    for(uint8_t b=0; b<BAM_RESOLUTION; b++) if(m & (1<<b)) sumW += weights[b];
    uint32_t diff = (sumW * 100 > target) ? (sumW * 100 - target) : (target - sumW * 100);
    if(diff < best_diff){ best_diff = diff; best_mask = m; }
  }
  return best_mask;
}


uint32_t convShiftData(uint8_t digit, uint16_t segment){
  uint32_t disp_data = 0x00000000;
  uint8_t digitShiftTable[9] = {0, 4, 7, 8, 9, 10, 12, 15, 19};

  disp_data |= (0x00000001 << digitShiftTable[digit]);

  if((segment&0x01)!=0) disp_data |= 0x0000040000;    
  if((segment&0x02)!=0) disp_data |= 0x0000020000;    
  if((segment&0x04)!=0) disp_data |= 0x0000010000;    
  if((segment&0x08)!=0) disp_data |= 0x0000000040;    
  if((segment&0x10)!=0) disp_data |= 0x0000000020;    
  if((segment&0x20)!=0) disp_data |= 0x0000000004;    
  if((segment&0x40)!=0) disp_data |= 0x0000000008;    
  if((segment&0x80)!=0) disp_data |= 0x0000000002;    
  if((segment&0x100)!=0) disp_data |= 0x0000000800;   
  if((segment&0x200)!=0) disp_data |= 0x0000002000;   
  if((segment&0x400)!=0) disp_data |= 0x0000004000;   

  return disp_data;
}

// High-level rendering is implemented in main.cpp (renderDisplay),
// display module only provides low-level buffer management and ISR.

void updateDisplay(){
  portENTER_CRITICAL(&ptrMux);
  volatile uint32_t (*temp)[BAM_RESOLUTION] = activeBuffer;
  activeBuffer = hiddenBuffer;
  hiddenBuffer = temp;
  portEXIT_CRITICAL(&ptrMux);
}

// シンプルにISRへ渡すためのラッパー
void IRAM_ATTR onBAMTimer(){
  portENTER_CRITICAL_ISR(&timerMux);
  static uint8_t currentDigit = 0;
  static uint8_t currentBit = 0;

  GPIO.out_w1tc = (1 << VFD_RCLK_PIN);
  GPIO.out_w1tc = (1 << VFD_DAT_PIN);
  for (int8_t i = 0; i < 24; i++) {
    GPIO.out_w1ts = (1 << VFD_CLK_PIN);
    GPIO.out_w1tc = (1 << VFD_CLK_PIN);
  }
  GPIO.out_w1ts = (1 << VFD_RCLK_PIN);
  ets_delay_us(2);

  uint32_t outData = activeBuffer[currentDigit][currentBit];
  GPIO.out_w1tc = (1 << VFD_RCLK_PIN);
  for (int8_t i = 23; i >= 0; i--) {
    if (outData & (1 << i)) {
      GPIO.out_w1ts = (1 << VFD_DAT_PIN);
    } else {
      GPIO.out_w1tc = (1 << VFD_DAT_PIN);
    }
    GPIO.out_w1ts = (1 << VFD_CLK_PIN);
    GPIO.out_w1tc = (1 << VFD_CLK_PIN);
  }
  GPIO.out_w1ts = (1 << VFD_RCLK_PIN);

  uint64_t nextDelay = BASE_TIME_US * (1 << currentBit);
  timerAlarmWrite(bam_timer, nextDelay, true);

  currentBit++;
  if (currentBit >= BAM_RESOLUTION){
    currentBit = 0;
    currentDigit++;
    if(currentDigit > 8) currentDigit = 0;
  }
  portEXIT_CRITICAL_ISR(&timerMux);
}

void display_init(){
  pinMode(VFD_DAT_PIN, OUTPUT);
  pinMode(VFD_CLK_PIN, OUTPUT);
  pinMode(VFD_RCLK_PIN, OUTPUT);
  pinMode(VFD_RSTN_PIN, OUTPUT);
  pinMode(VFD_FILAMENT1_PIN, OUTPUT);
  pinMode(VFD_FILAMENT2_PIN, OUTPUT);

  digitalWrite(VFD_DAT_PIN, LOW);
  digitalWrite(VFD_CLK_PIN, LOW);
  digitalWrite(VFD_RCLK_PIN, LOW);
  digitalWrite(VFD_RSTN_PIN, LOW);
  digitalWrite(VFD_FILAMENT1_PIN, LOW);
  digitalWrite(VFD_FILAMENT2_PIN, HIGH);
  delay(10);
  digitalWrite(VFD_RSTN_PIN, HIGH);

  memset((void*)bamBufferA, 0, sizeof(bamBufferA));
  memset((void*)bamBufferB, 0, sizeof(bamBufferB));

  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, VFD_FILAMENT1_PIN);
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0B, VFD_FILAMENT2_PIN);
  mcpwm_config_t pwm_config;
  pwm_config.frequency = 20000;
  pwm_config.cmpr_a = 50.0;
  pwm_config.cmpr_b = 50.0;
  pwm_config.counter_mode = MCPWM_UP_COUNTER;
  pwm_config.duty_mode = MCPWM_DUTY_MODE_0;
  mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_config);
  mcpwm_deadtime_enable(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_ACTIVE_HIGH_COMPLIMENT_MODE, 20, 20);

  bam_timer = timerBegin(0, 80, true);
  timerAttachInterrupt(bam_timer, &onBAMTimer, true);
  timerAlarmWrite(bam_timer, BASE_TIME_US, true);
  timerAlarmEnable(bam_timer);
  // Apply initial brightness to MCPWM outputs
  // compute allowed levels before accepting brightness
  compute_allowed_levels();
  display_set_brightness(currentBrightness);
}
