#ifndef RTC_RX8900
#define RTC_RX8900

//-------------------------------------------------------
//- Header files include
//-------------------------------------------------------
#include <stdio.h>

//-------------------------------------------------------
//- macro definition
//-------------------------------------------------------
#define RTC_SDA_PIN 18
#define RTC_SCL_PIN 19
#define RTC_BAUD 400*1000       // 400kHz
#define RTC_ADDR 0x32           // RX8900 7-bit address

// register table
#define RX8900_REG_SEC 0x00
#define RX8900_REG_MIN 0x01
#define RX8900_REG_HOUR 0x02
#define RX8900_REG_WEEK 0x03
#define RX8900_REG_DAY 0x04
#define RX8900_REG_MONTH 0x05
#define RX8900_REG_YEAR 0x06
#define RX8900_REG_ALARM_MIN 0x08

//---- RTC structure --------------------
typedef struct rtcRx8900 RtcRx8900;

typedef struct{
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t weekday;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t dummy;
}rtc_time_bcd;

typedef struct{
    uint sec1 : 4;
    uint sec2 : 4;
    uint min1 : 4;
    uint min2 : 4;
    uint hour1 : 4;
    uint hour2 : 4;
    uint weekday : 8;
    uint day1 : 4;
    uint day2 : 4;
    uint month1 : 4;
    uint month2 : 4;
    uint year1 : 4;
    uint year2 : 4;
    uint dummy : 8;
}rtc_time_separate;

typedef union{
    rtc_time_bcd bcd;
    rtc_time_separate separate;
    uint64_t all;
}rtc_time;


//--------------------------------------
// function prototype
//--------------------------------------
int rx8900_read_reg(uint8_t addr, uint8_t *data);
int rx8900_read_reg_n(uint8_t addr, uint8_t *data, uint8_t data_num);
int rx8900_write_reg(uint8_t addr, uint8_t data);
void rx8900_init(void);
void rx8900_hard_init(void);
int rx8900_get_time(rtc_time *time);
int rx8900_set_time(rtc_time time);
bool rx8900_is_voltage_low(void);

#endif