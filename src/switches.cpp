#include "switches.h"
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Pin definitions
#define SW_PIN_A 21
#define SW_PIN_B 22
#define SW_PIN_C 23

static const uint8_t sw_pins[3] = { SW_PIN_A, SW_PIN_B, SW_PIN_C };
static bool stable_state[3];
static int32_t last_read[3];
static uint32_t last_change_ms[3];
static uint32_t debounce_ms = 50; // 50ms debounce

static switch_cb_t user_cb = NULL;

void switches_register_callback(switch_cb_t cb){
  user_cb = cb;
}

static void handle_state_change(int idx, bool pressed){
  if(user_cb) user_cb((SwitchId)idx, pressed);
  else {
    // default debug log
    Serial.printf("Switch %d %s\n", idx, pressed?"PRESSED":"RELEASED");
  }
}

static void SwitchTask(void *pvParameters){
  (void)pvParameters;
  for(;;){
    uint32_t now = millis();
    for(int i=0;i<3;i++){
      int v = digitalRead(sw_pins[i]); // HIGH when released, LOW when pressed (pull-up)
      if(v != last_read[i]){
        last_change_ms[i] = now;
        last_read[i] = v;
      } else {
        if((now - last_change_ms[i]) >= debounce_ms){
          bool pressed = (v == LOW);
          if(pressed != stable_state[i]){
            stable_state[i] = pressed;
            handle_state_change(i, pressed);
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void switches_init(){
  for(int i=0;i<3;i++){
    pinMode(sw_pins[i], INPUT_PULLUP);
    last_read[i] = digitalRead(sw_pins[i]);
    stable_state[i] = (last_read[i] == LOW);
    last_change_ms[i] = millis();
  }
  xTaskCreatePinnedToCore(SwitchTask, "SwitchTask", 1024, NULL, 1, NULL, 1);
}
