#include <Wire.h>
#include "rtc_rx8900.h"

//--------------------------------------
// private function
//--------------------------------------
int rx8900_read_reg(uint8_t addr, uint8_t *data){

    Wire.beginTransmission(RTC_ADDR);
    Wire.write(addr);
    Wire.endTransmission(false); // Restartコンディション (OK)
    
    // ★修正1: 第3引数を true にして通信終了後にバスを開放する
    Wire.requestFrom((uint8_t)RTC_ADDR, (size_t)1, true);
    
    if(Wire.available()){
        *data = Wire.read();
        return 0; // 成功
    }

    return -1; // エラー
}

int rx8900_read_reg_n(uint8_t addr, uint8_t *data, uint8_t data_num){

    Wire.beginTransmission(RTC_ADDR);
    Wire.write(addr);
    Wire.endTransmission(false);
    
    // ★修正1: 第3引数を true にする
    // 型キャストも以前のワーニング対策として明示的に指定
    uint8_t bytesRead = Wire.requestFrom((uint8_t)RTC_ADDR, (size_t)data_num, true);
    
    // ★修正2: 実際に読み込めたバイト数だけループを回す
    for(size_t i=0; i < bytesRead; i++){
        data[i] = Wire.read();
    }

    // 予定していたバイト数が読み込めていれば0(成功)を返す
    return (bytesRead == data_num) ? 0 : -1;
}

int rx8900_write_reg(uint8_t addr, uint8_t data){

    uint8_t send_data[2] = {addr, data};
    uint8_t data_length = sizeof(send_data);
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(send_data, data_length);
    uint8_t error = Wire.endTransmission();
    return error; // 0なら成功
}

//--------------------------------------
// public function
//--------------------------------------
void rx8900_init(void)
{
    // メイン側で既にWire.begin()を呼んでいる場合は不要ですが、
    // 引数付きで再初期化する場合はこのままでもOKです。
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN, RTC_BAUD);
}

void rx8900_hard_init(void)
{
    rx8900_write_reg(0x0D, 0x08);
    rx8900_write_reg(0x0E, 0x00);
    rx8900_write_reg(0x0F, 0xC0);
    
    // ※BCD形式のデータであることに注意
    rx8900_write_reg(RX8900_REG_YEAR, 0x25);
    rx8900_write_reg(RX8900_REG_MONTH, 0x01);
    rx8900_write_reg(RX8900_REG_DAY, 0x01);
    rx8900_write_reg(RX8900_REG_WEEK, 0x01);
    rx8900_write_reg(RX8900_REG_HOUR, 0x00);
    rx8900_write_reg(RX8900_REG_MIN, 0x00);
    rx8900_write_reg(RX8900_REG_SEC, 0x00);
    
    rx8900_write_reg(0x0F, 0xE0);
}

int rx8900_get_time(rtc_time *time)
{
    uint8_t data[7];
    int result = rx8900_read_reg_n(RX8900_REG_SEC, data, 7);

    if (result == 0) { // 読み出し成功時のみ反映
        time->bcd.sec = data[0];
        time->bcd.min = data[1];
        time->bcd.hour = data[2];
        time->bcd.weekday = data[3];
        time->bcd.day = data[4];
        time->bcd.month = data[5];
        time->bcd.year = data[6];
        time->bcd.dummy = 0;
    }
    
    return result;
}

int rx8900_set_time(rtc_time time)
{
    // ★修正3: バーストライト（連続書き込み）に変更
    // 秒のアドレスから書き始めると、自動的にポインタがインクリメントされ、
    // 7バイト分が一瞬で連続して書き込まれるため時刻のズレが発生しません。
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(RX8900_REG_SEC);     // 開始アドレス
    Wire.write(time.bcd.sec);       // 0x00
    Wire.write(time.bcd.min);       // 0x01
    Wire.write(time.bcd.hour);      // 0x02
    Wire.write(time.bcd.weekday);   // 0x03
    Wire.write(time.bcd.day);       // 0x04
    Wire.write(time.bcd.month);     // 0x05
    Wire.write(time.bcd.year);      // 0x06
    return Wire.endTransmission();  // 0なら成功
}

bool rx8900_is_voltage_low(void){
    uint8_t result = 0;
    rx8900_read_reg(0x0E, &result);

    // VDET (Bit1) と VLF (Bit0) の両方を見るのが確実です
    if((result & 0x03) != 0){
        return true;
    }else{
        return false;
    }
}