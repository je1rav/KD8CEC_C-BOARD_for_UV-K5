/*
 * Copyright (C) 2025- Hitoshi Kawaji <je1rav@gmail.com>
 * 
 * UV-K5_Cable_RP2040_IJV_mbed.ino
 * 
 * UV-K5_Cable_RP2040_IJV_mbed.ino is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 * 
 * UV-K5_Cable_RP2040_IJV_mbed.ino is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 * 
*/
#include "Arduino.h"
#include "mbed.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#define pin_SW 8  //pin for mode change switch (input)
#define pin_PTT 3  //pin for CW keying (input/output) for KD8CEC's firmware
#define pin_LED 16  //pin for NEOPIXEL LED (output)
#define pin_PPS 13  //pin from GPS_PPS (input)
#define pin_PWM 6 //pin for Mic (output)
uint8_t mode = 0;  //0:FT8(GPS), 1:CW
bool pps_state = false; 
bool Tx_Status = false;
bool Tx_Start = false;
uint8_t vox_delay;  
uint32_t monomaxsum = 0;
uint16_t monomaxprev = 0;
uint32_t Tx_last_time;

//EEPROM
#include "FlashIAPBlockDevice.h"
#include "KVStore.h"
#include "TDBStore.h"
// 512KB block device, starting 1MB inside the flash
FlashIAPBlockDevice bd(XIP_BASE + 1024*1024, 1024*512);
mbed::TDBStore eeprom(&bd);
uint8_t ram_buffer[2];
char data[2];

// USB
//////////////////////////////////////////
//#include "Adafruit_TinyUSB.h"
//Adafruit_USBD_CDC USBSer2; // add one more USB Serial port
UART Serial2(4, 5, 0, 0);

// USB Audio
#include "PluggableUSBAudio.h"
USBAudio audio(true, 48000, 2, 48000, 2);
static uint8_t readBuffer[192];  //48 sampling (=  0.5 ms at 48000 Hz sampling) data sent from PC are recived (16bit stero; 48*2*2).
int16_t writeBuffer16[96];       //48 sampling date are written to PC in one packet (stero).
uint16_t writeCounter=0;
uint16_t nBytes=0;
int16_t monodata[48];  
bool USBAudio_read;

//ADC
//////////////////////////////////////////
#include "hardware/adc.h"
int16_t ADC_offset = 0;  //ADC offset for Reciever
int16_t gain = 4;  //ADC gain for Reciever

//Adafruit_NeoPixel
//////////////////////////////////////////
#include <Adafruit_NeoPixel.h>  //Adafruit NeoPixel Library, https://github.com/adafruit/Adafruit_NeoPixel
Adafruit_NeoPixel pixels(1, pin_LED);
#define BRIGHTNESS 5  //max 255
uint32_t colors[] = {pixels.Color(0, 0, BRIGHTNESS), pixels.Color(0, BRIGHTNESS, 0), pixels.Color(BRIGHTNESS, 0, 0), pixels.Color(BRIGHTNESS, BRIGHTNESS, 0), pixels.Color(BRIGHTNESS, 0, BRIGHTNESS), pixels.Color(0, BRIGHTNESS, BRIGHTNESS), pixels.Color(BRIGHTNESS, BRIGHTNESS, BRIGHTNESS), pixels.Color(0, 0, 0)};

//PWMAudio
//ring buffer
//////////////////////////////////////////
#define BUFFER_SIZE_U16 64 
typedef struct {
    uint16_t buffer[BUFFER_SIZE_U16];
    volatile uint16_t head; 
    volatile uint16_t tail; 
} RingBuffer_U16;
RingBuffer_U16 myBuffer_U16;

void buffer_init_u16(RingBuffer_U16* buf) {
    buf->head = 0;
    buf->tail = 0;
}

bool buffer_write_u16(RingBuffer_U16* buf, uint16_t data) {
    noInterrupts(); 
    uint16_t next_head = (buf->head + 1) & (BUFFER_SIZE_U16 - 1);
    if (next_head == buf->tail) {
        interrupts(); 
        return false;
    }
    buf->buffer[buf->head] = data;
    buf->head = next_head;
    interrupts(); // 割り込みを有効に戻す
    return true;
}

bool buffer_read_u16(RingBuffer_U16* buf, uint16_t* data) {
    if (buf->head == buf->tail) {
        return false; 
    }
    *data = buf->buffer[buf->tail];
    buf->tail = (buf->tail + 1) & (BUFFER_SIZE_U16 - 1);
    return true;
}

//PWM
//////////////////////////////////////////
#include "hardware/pwm.h"
#include "hardware/irq.h"
uint slice_num; 
//const uint16_t rawData[] = { /* ... */ };
//const uint32_t rawDataSize = sizeof(rawData);
//volatile uint32_t audio_position = 0;
// -----------------------------------------------------------------
// PWM割り込みサービスルーチン (ISR)
// -----------------------------------------------------------------
void __isr __time_critical_func(pwm_handler)() {
    pwm_clear_irq(slice_num); // 割り込みフラグをクリア
    uint16_t receivedData;
    if (buffer_read_u16(&myBuffer_U16, &receivedData)) {
        uint16_t sample = (uint16_t)((int32_t)receivedData+32768);
        sample = sample >> 5; //16bit to 11bit
        pwm_set_gpio_level(pin_PWM, sample); 
    }
}

void setup() {
  //Pin setting ----- 
  pinMode(pin_SW, INPUT_PULLUP); //SW (mode change)

  Serial.begin(115200);

//EEPROM
  eeprom.init();
  mbed::KVStore::info_t info;
  if (eeprom.get_info("mydata", &info) != MBED_ERROR_ITEM_NOT_FOUND) {
    eeprom.get("mydata", ram_buffer, info.size);
    mode = ram_buffer[0] - '0'; 
    if (mode>2) {
      mode = 0;
      sprintf(data, "%d", mode);
      eeprom.reset();
      eeprom.set("mydata", data, strlen(data), 0);
    }
  } 
  else {
    sprintf(data, "%d", mode);
    eeprom.reset();
    eeprom.set("mydata", data, strlen(data), 0);
  }
  
  // ADC config
  adc_init();
  adc_gpio_init(26);           // ADC0 = GPIO26
  adc_select_input(0);
  adc_set_clkdiv(249.0f);

  // FIFO
  adc_fifo_setup(
    true,  // fifo effective
    true,  // DMA request enable
    0,     // DREQ
    false,
    false
  );
  adc_fifo_drain();
  adc_run(true);

  //NEOPIXEL LED  initialization-----
  pixels.begin();  // initialize the NEOPIXEL
  pixels.setPixelColor(0, colors[mode]);
  pixels.show();

  switch (mode) {
    case 0: {  // PTT with Audio & GPS
      pinMode(0, INPUT_PULLUP); // PTT init.
      Serial2.begin(9600);  //UART for GPS
      pinMode(pin_PPS, INPUT); // PPS
      break;
    }
  case 1: {  // CW control with Audio
      pinMode(0, INPUT_PULLUP); // PTT; High impedance
      break;
    }
  }

  //read the DC offset value of ADC input----- 
  delay(500);
  uint64_t ADC_sum = 0;
  for (int i=0; i<256; i++){
    adc_fifo_drain ();
    ADC_sum += adc();
  }
  ADC_offset = ADC_sum/256;

  // --- ring buffer init. ---
  buffer_init_u16(&myBuffer_U16);

  // --- PWM ---
    gpio_set_function(pin_PWM, GPIO_FUNC_PWM);
    slice_num = pwm_gpio_to_slice_num(pin_PWM);
    pwm_set_clkdiv_int_frac(slice_num, 1, 0); // PWMクロック分周器を1.0に設定
    uint16_t wrap_value = 2604; // 125,000kHz/48kHz = 約2604.16666, 実際の周波数は 48003.1Hzに近くなります
    pwm_set_wrap(slice_num, wrap_value); // WRAP値 (TOP（最大）値) を設定して、周期を決定する
    //pwm_set_enabled(slice_num, true);  // PWMスライスを有効にする
    //pwm_set_irq_enabled(slice_num, true); // このスライスからの割り込みを有効にする
    //irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_handler); // 割り込みハンドラを登録
    //irq_set_enabled(PWM_IRQ_WRAP, true); // 割り込みハンドラを有効にする

    adc_fifo_drain();
    adc_run(true);
}

void loop() {
  if (digitalRead(pin_SW)==0) {
    uint8_t tmp_mode = mode;
    while (digitalRead(pin_SW)==0) {
      tmp_mode++;
      tmp_mode %= 2;
      pixels.setPixelColor(0, colors[tmp_mode]);
      pixels.show();
      delay (500);
    }
    if (tmp_mode != mode) {
      sprintf(data, "%d", tmp_mode);
      eeprom.reset();
      eeprom.set("mydata", data, strlen(data), 0);
      delay (1000);
      watchdog_reboot(0, 0, 0);
    }
  }

  switch (mode) {
    case 0: {
      if (Tx_Start==0) receiving();
      else transmitting();
      if (Serial2.available()){
        Serial.write(Serial2.read());
      }
      if (Serial.available()){
        Serial2.write(Serial.read());
      }
      if (digitalRead(pin_PPS)==1){
        if (pps_state==false){
          pixels.setPixelColor(0, colors[mode + 1]);
          pixels.show();
          pps_state = true;
        }
      }
      else {
        if (pps_state==true){
          pixels.setPixelColor(0, colors[mode + Tx_Start*2]);
          pixels.show();
          pps_state = false;
        }
      }
      break;
    }
    case 1: {
      if (Tx_Start==0) receiving_cw();
      else transmitting_cw();
      if (Serial.dtr()) {
        Tx_Start = 1;
      }
      else {
        Tx_Start = 0;;
      }
      break;
    }
    default: break;
  }
}

void transmitting(){
  if (USBAudio_read) 
  {
    uint16_t monomax = 0;
    for (int i=0; i<USBAudio_read; i++){
      if (abs(monodata[i]) > monomax) monomax = abs(monodata[i]);
    }
    monomaxsum += monomax;
    monomaxprev = monomax;
    vox_delay ++;
    if (vox_delay > 9){
     vox_delay = 0;
     //if (monomaxsum < 16384){  
      if (monomaxsum < 16){  
      Tx_Start = 0;
      receive();
      return;
     }
     monomaxsum = 0;
    }
    Tx_last_time = millis();
  }
  else if (millis()-Tx_last_time > 10) {
    Tx_Start = 0;
    receive();
    return;
  }
  transmit();
  USBAudioRead();
}

void receiving() {
  USBAudioRead();  // read in the USB Audio buffer (myRawBuffer) to check the transmitting
  if (USBAudio_read) 
  {
    uint16_t monomax = 0;
    for (int i=0; i<USBAudio_read; i++){
      if (abs(monodata[i]) > monomax) monomax = abs(monodata[i]);
    }
    //if (monomax > 16384)
    if (monomax > 16)
    {                       // VOX if singanl exceeds 50% of 2^15
      Tx_Start = 1;
      return;
    }
  }
  int16_t rx_adc = (adc() - ADC_offset)* gain; //read ADC data (48kHz sampling)
  USBAudioWrite(rx_adc, rx_adc);
}

void transmitting_cw(){
  if (USBAudio_read) {
  }
  transmit();
  USBAudioRead();
}

void receiving_cw() {
  receive_cw();
  USBAudioRead();  // read in the USB Audio buffer (myRawBuffer) to check the transmitting
  int16_t rx_adc = (adc() - ADC_offset)* gain; //read ADC data (48kHz sampling)
  USBAudioWrite(rx_adc, rx_adc);
}

void transmit(){
  if (Tx_Status==0){
    pixels.setPixelColor(0, colors[2]);  //red LED
    pixels.show();
    pinMode(0, OUTPUT);
    digitalWrite(0, LOW);  // LOW (PTT PUSH)
    adc_run(false);                         //stop ADC free running
    buffer_init_u16(&myBuffer_U16);
    pwm_set_enabled(slice_num, true);  // PWMスライスを有効にする
    pwm_set_irq_enabled(slice_num, true); // このスライスからの割り込みを有効にする
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_handler); // 割り込みハンドラを登録
    irq_set_enabled(PWM_IRQ_WRAP, true); // 割り込みハンドラを有効にする
    Tx_Status=1;
  }
}

void transmit_cw(){
  if (Tx_Status==0){
    pixels.setPixelColor(0, colors[2]);  //red LED
    pixels.show();
    pinMode(0, OUTPUT);
    digitalWrite(0, LOW);  // LOW (PTT PUSH)
    adc_run(false);                         //stop ADC free running
    Tx_Status=1;
  }
}

void receive(){
  if (Tx_Status==1){
    pixels.setPixelColor(0, colors[mode]);
    pixels.show();
    digitalWrite(0, HIGH);  // LOW 
    pinMode(0, INPUT_PULLUP);  // High impedance (PTT OFF)
    // initialization of monodata[]
    for (int i = 0; i < 48; i++) {
      monodata[i] = 0;
    } 
    pwm_set_enabled(slice_num, false);  // PWMスライスを無効にする
    pwm_set_irq_enabled(slice_num, false); // このスライスからの割り込みを無効にする
    irq_set_enabled(PWM_IRQ_WRAP, false); // 割り込みハンドラを無効にする
    // initializaztion of ADC data write counter
    adc_fifo_drain ();                     //initialization of adc fifo
    adc_run(true);                         //start ADC free running
    Tx_Status=0;
  }
}

void receive_cw(){
  if (Tx_Status==1){
    pixels.setPixelColor(0, colors[mode]);
    pixels.show();
    digitalWrite(0, HIGH);  // HIHG
    pinMode(0, INPUT_PULLUP);  // High impedance (PTT OFF)
    // initializaztion of ADC data write counter
    adc_fifo_drain ();                     //initialization of adc fifo
    adc_run(true);                         //start ADC free running
    Tx_Status=0;
  }
}

int16_t adc() {
  int64_t adc = 0;
  for (int i=0;i<4;i++){                    // 192kHz/4 = 48kHz_
    adc += adc_fifo_get_blocking() -1532 ;   // read from ADC fifo (offset about 1.5 V)
  }  
  int16_t division = 1;
  return (int16_t)(adc/division);    // if the audio input is too large, please reduce the adc output with increasing the "division".
}

void USBAudioRead() {
  USBAudio_read = audio.read(readBuffer, sizeof(readBuffer));
  int32_t monosum=0;
  if (USBAudio_read) {
    for (int i = 0; i < 48 ; i++) {
      int16_t outL = (readBuffer[4*i]) + (readBuffer[4*i+1] << 8);
      int16_t outR = (readBuffer[4*i+2]) + (readBuffer[4*i+3] << 8);
      int16_t mono = (outL+outR)/2;
      monosum += mono;
      monodata[i] = mono;
      buffer_write_u16(&myBuffer_U16, mono); //write to ring buffer
    }
  }
  if (monosum == 0) USBAudio_read=0;
}

void USBAudioWrite(int16_t left,int16_t right) {
  if(nBytes>191){
    uint8_t *writeBuffer =  (uint8_t *)writeBuffer16;
    audio.write(writeBuffer, 192);
    writeCounter =0;
    nBytes =0;
  }
  writeBuffer16[writeCounter]=left;
  writeCounter++;
  nBytes+=2;
  writeBuffer16[writeCounter]=right;
  writeCounter++;
  nBytes+=2;
}
