#include "display.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/mcpwm.h"
#include <SPI.h>

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

static uint8_t currentBrightness = 100; // fixed at 100% (0-100)
static bool allowedLevel[101];
// When true, ISR will drive zeros regardless of buffer contents so the
// dynamic rendering can continue updating buffers while the visible
// segments remain off to avoid residual images.
static volatile bool display_blank_state = false;
// Blink state for rightmost digit dot when blanking is active.
static volatile bool display_blink_state = false;
// Precomputed shift pattern for dot per digit to use from ISR.
static uint32_t dotShift[NUM_DIGITS];

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

bool display_set_brightness(uint8_t percent){
  // Brightness control disabled: keep at 100% always.
  (void)percent;
  currentBrightness = 100;
  // ensure PWM outputs are set to full duty
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 100.0f);
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 100.0f);
  mcpwm_set_duty_type(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
  mcpwm_set_duty_type(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, MCPWM_DUTY_MODE_0);
  Serial.println("display_set_brightness: disabled, fixed at 100%");
  return true;
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

  // Use direct binary representation of desired summed weight to produce
  // a monotonic mask. This avoids masks flipping between near-equal combos
  // that cause visible flicker during small changes.
  uint32_t target_sum = (uint32_t)currentBrightness * (uint32_t)total;
  // target_sum is percent*total, divide by 100 with rounding
  uint8_t desired = (uint8_t)((target_sum + 50) / 100);
  if(desired > total) desired = total;
  return desired; // desired interpreted as bitmask since weights are powers of two
}

// Compute a BAM mask for an arbitrary brightness percentage (0-100).
uint8_t display_get_bam_mask_for_percent(uint8_t percent){
  if(percent >= 100) return (uint8_t)((1<<BAM_RESOLUTION)-1);
  const uint8_t total = (1<<BAM_RESOLUTION) - 1; // 63
  uint32_t target_sum = (uint32_t)percent * (uint32_t)total;
  uint8_t desired = (uint8_t)((target_sum + 50) / 100);
  if(desired > total) desired = total;
  return desired;
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
  // If blanking is enabled, force no segments on to avoid residual images
  // while keeping BAM/dynamic rendering running in the background.
  if(display_blank_state){
    // If blink is active and this is the rightmost digit, show only the dot.
    if(display_blink_state && currentDigit == (NUM_DIGITS - 1)){
      outData = dotShift[currentDigit];
    } else {
      outData = 0;
    }
  }
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

  // Precompute shift patterns for the dot segment for each digit
  for(int d=0; d<NUM_DIGITS; ++d) dotShift[d] = convShiftData((uint8_t)d, SEG_DOT);

  // Start blink task to toggle the rightmost dot once per second while blanked
  xTaskCreatePinnedToCore([](void* pv){
    (void)pv;
    for(;;){
      if(display_blank_state) display_blink_state = !display_blink_state;
      else display_blink_state = false;
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }, "DispBlink", 2048, NULL, 1, NULL, 1);
}

static bool display_enabled_state = true;
// When true, ISR will drive zeros regardless of buffer contents so the
// dynamic rendering can continue updating buffers while the visible
// segments remain off to avoid residual images.
bool display_set_enabled(bool enabled){
  if(enabled == display_enabled_state) return display_enabled_state;
  if(!enabled){
    // Stop BAM ISR so we can safely drive the shift register directly
    if(bam_timer) timerAlarmDisable(bam_timer);
    // Ensure buffers contain zeros to avoid any residual data
    memset((void*)bamBufferA, 0, sizeof(bamBufferA));
    memset((void*)bamBufferB, 0, sizeof(bamBufferB));
    // Drive shift registers with zeros and latch to turn off all segments.
    // There are 24 bits per digit -> 3 bytes per digit. Shift zeros for all digits.
    digitalWrite(VFD_RCLK_PIN, LOW);
    SPI.begin();
    SPI.beginTransaction(SPISettings(3000000, MSBFIRST, SPI_MODE0));
    int totalBytes = NUM_DIGITS * 3;
    for(int i = 0; i < totalBytes; ++i) SPI.transfer(0x00);
    SPI.endTransaction();
    digitalWrite(VFD_RCLK_PIN, HIGH);
    // Turn off filaments
    digitalWrite(VFD_FILAMENT1_PIN, LOW);
    digitalWrite(VFD_FILAMENT2_PIN, LOW);
  } else {
    // Restore filaments and re-enable BAM timer
    digitalWrite(VFD_FILAMENT1_PIN, HIGH);
    digitalWrite(VFD_FILAMENT2_PIN, HIGH);
    if(bam_timer) timerAlarmEnable(bam_timer);
  }
  display_enabled_state = enabled;
  return display_enabled_state;
}

bool display_is_enabled(){
  return display_enabled_state;
}

// Enable/disable blanking while keeping BAM/filament state as-is.
bool display_set_blank(bool blank){
  display_blank_state = blank;
  return display_blank_state;
}

bool display_is_blank(){
  return display_blank_state;
}
