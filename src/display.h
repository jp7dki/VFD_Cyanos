#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "driver/timer.h"

// VFD / BAM 設定
#define VFD_DAT_PIN 16
#define VFD_CLK_PIN 2
#define VFD_RCLK_PIN 4
#define VFD_RSTN_PIN 15 
#define VFD_FILAMENT1_PIN 12
#define VFD_FILAMENT2_PIN 13

#define BAM_RESOLUTION  6
#define BASE_TIME_US  30
#define NUM_DIGITS 9

#define SEG_DASH 0x80
#define SEG_CONMA 0x100
#define SEG_DOT 0x200
#define SEG_HALF 0x400

extern volatile uint32_t bamBufferA[NUM_DIGITS][BAM_RESOLUTION];
extern volatile uint32_t bamBufferB[NUM_DIGITS][BAM_RESOLUTION];
extern volatile uint32_t (*activeBuffer)[BAM_RESOLUTION];
extern volatile uint32_t (*hiddenBuffer)[BAM_RESOLUTION];
extern hw_timer_t *bam_timer;

extern const uint16_t convNumToSeg[10];

void display_init();
uint32_t convShiftData(uint8_t digit, uint16_t segment);
void updateDisplay();
// Set brightness as percent (0-100). Returns true if exact level applied,
// false if snapped to a nearest allowed level.
bool display_set_brightness(uint8_t percent);
uint8_t display_get_brightness();
uint8_t display_get_bam_mask_for_digit(uint8_t digit);
uint8_t display_get_bam_mask_for_percent(uint8_t percent);
 
