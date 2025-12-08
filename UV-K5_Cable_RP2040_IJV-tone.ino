/*
 * Copyright (C) 2025- Hitoshi Kawaji <je1rav@gmail.com>
 * 
 * UV-K5_Cable_RP2040_IJV-tone.ino
 * 
 * UV-K5_Cable_RP2040_IJV-tone.ino is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 * 
 * UV-K5_Cable_RP2040_IJV-tone.ino is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
  
 * UV-K5_Cable_RP2040_IJV.ino uses a patched version 
 * https://github.com/je1rav/Adafruit_TinyUSB_Arduino/tree/je1rav-patch
 * of the modified Adafruit_TinyUSB_Arduino Library supporting USB Audio written by Phl Schatzmann.
 * https://github.com/pschatzmann/Adafruit_TinyUSB_Arduino/tree/Audio
 * MIT license, check LICENSE for more information
 * Copyright (c) 2024 Phl Schatzmann
*/

#define AUDIOSAMPLING 48000  // USB Audio sampling frequency
#define pin_SW 8  //pin for mode change switch (input)
#define pin_PTT 3  //pin for CW keying (input/output) for KD8CEC's firmware
#define pin_LED 16  //pin for NEOPIXEL LED (output)
#define pin_PPS 13  //pin from GPS_PPS (input)
#define pin_PWM 6 //pin for Mic (output)
uint8_t mode;  //0:FT8(GPS), 1:CW
bool pps_state = false; 

//EEPROM
#include <EEPROM.h>
int addr = 0;
int value;

// USB
//////////////////////////////////////////
#include "Adafruit_TinyUSB.h"

Adafruit_USBD_CDC USBSer2; // add one more USB Serial port
// USB Audio
Adafruit_USBD_Audio usb;
size_t sample_count_mic = 0;
size_t sample_count_spk = 0;
size_t now_sample_count_mic = 0;
size_t now_sample_count_spk = 0;
int16_t spk_buffer16[192];  //48 sampling (= 1 ms at 48000 Hz sampling) data sent from PC are recived (16bit stero; 48*2 = 96).
int16_t mic_buffer16[192];  //48 sampling date are written to PC in one packet (96 in stero).
bool cat_changed = false;
int16_t mic_counter = 0;
int16_t monodata[96];  
bool USBAudio_read;
int16_t USBAudio_read_length; 

// Microphone: generate data for USB
size_t readCB(uint8_t* data, size_t len, Adafruit_USBD_Audio& ref) {
  int16_t* data16 = (int16_t*)data;
  size_t samples = len / sizeof(int16_t);
  size_t result = 0;
  for (int j = 0; j < 96; j++) {
    data16[j] = mic_buffer16[j];
    result += sizeof(int16_t);
  }
  sample_count_mic += 96;
  return result;
}
// Speaker: receive data from USB and write them to the final destination
size_t writeCB(const uint8_t* data, size_t len, Adafruit_USBD_Audio& ref) {
  int16_t* data16 = (int16_t*)data;
  size_t samples = len / sizeof(int16_t);
  for (int i=0; i<samples; i++){
    spk_buffer16[i] = data16[i];
  }
  sample_count_spk += samples;
  USBAudio_read_length = samples /2;
  return len;
}

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

//Tone generator
#include "AudioTools.h"
AudioInfo info(8000, 1, 16);  // default pwm output pin = 6
SineWaveGenerator<int16_t> sineWave(1000); // subclass of SoundGenerator with max amplitude of 32000
GeneratedSoundStream<int16_t> sound(sineWave);  // Stream generated from sine wave
PWMAudioOutput pwm;                  
StreamCopy copier(pwm, sound);    // copy in to out
float audio_freq_correct = 1.06724;  //correction of frequncy genearated by PWM

//Transeiver 
int Tx_Status = 0; //0=RX, 1=TX
int Tx_Start = 0;  //0=RX, 1=TX
int not_TX_first = 0;
uint32_t Tx_last_mod_time;
uint32_t Tx_last_time;

//Audio signal frequency determination
int16_t mono_prev=0;  
int16_t mono_preprev=0;  
float delta_prev=0;
int16_t sampling=0;
int16_t cycle=0;
int32_t cycle_frequency[40];
int64_t audio_freq_prev = 1000000;

void setup() {
  //Pin setting ----- 
  pinMode(pin_SW, INPUT_PULLUP); //SW (mode change)
  //UART pins
  Serial1.setTX(0);
  Serial1.setRX(1);
  Serial2.setTX(4);
  Serial2.setRX(5);

  //EEPROM
  EEPROM.begin(256);
  value = EEPROM.read(addr);
  if (value > 2) {
    value = 0;
    EEPROM.write(addr, value); 
    EEPROM.commit();
  }
  mode = value;

  Serial.begin(115200);

  // Manual begin() is required on core without built-in support e.g. mbed rp2040
  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
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

  //read the DC offset value of ADC input----- 
  delay(500);
  uint64_t ADC_sum = 0;
  for (int i=0; i<256; i++){
    adc_fifo_drain ();
    ADC_sum += adc();
  }
  ADC_offset = ADC_sum/256;
  
//NEOPIXEL LED  initialization-----
  pixels.begin();  // initialize the NEOPIXEL
  pixels.setPixelColor(0, colors[mode]);
  pixels.show();

  //Start USB device as both Audio Source and Sink
  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }

  switch (mode) {
    case 0: {  // Data cable with Audio
      //USB-Serial bridge for UV-K5
      //Serial1.begin(57600);
      pinMode(0, INPUT_PULLUP); // PPS
      //USBSer2.begin(115200);  //USB port2 
      Serial2.begin(9600);  //UART2 for GPS
      usb.setReadCallback(readCB); // Start USB device as Audio Source
      usb.setWriteCallback(writeCB);  // Start USB device as Audio Sink
      usb.begin(48000, 2, 16);  //48kHz stero 16bits
      pinMode(pin_PPS, INPUT); // PPS

    // start sine wave
      sineWave.begin(info, audio_freq_prev/100.00);

    // setup PWM output
      auto config = pwm.defaultConfig();
      config.copyFrom(info);
      config.resolution = 11;  // must be between 8 and 11 -> drives pwm frequency (8 is default)
      pwm.begin(config);
      break;
    }
  case 1: {  // CW control with Audio
      usb.setReadCallback(readCB); // Start USB device as Audio Source
      //usb.setWriteCallback(writeCB);  // Start USB device as Audio Sink
      usb.begin(48000, 2, 16);  //48kHz stero 16bits
      pinMode(pin_PTT, INPUT_PULLUP); // PTT; High impedance
      break;
    }
    default: break;
  }

   // If already enumerated, additional class driver begin() e.g msc, hid, midi won't take effect until re-enumeration
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

void loop() {
  #ifdef TINYUSB_NEED_POLLING_TASK
  // Manual call tud_task since it isn't called by Core's background
  TinyUSBDevice.task();
  #endif

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
      value = tmp_mode;
      EEPROM.write(addr, value);
      EEPROM.commit();
      delay (1000);
      rp2040.reboot();
    }
  }

  switch (mode) {
    case 0: {
      if (Tx_Start==0) receiving();
      else transmitting();
      if (Serial2.available()){
        USBSer2.write(Serial2.read());
      }
      if (USBSer2.available()){
        Serial2.write(USBSer2.read());
      }
      if (digitalRead(pin_PPS)==1){
        if (pps_state==false){
          pixels.setPixelColor(0, colors[mode+1]);
          pixels.show();
          pps_state = true;
        }
      }
      else {
        if (pps_state==true){
          pixels.setPixelColor(0, colors[mode+Tx_Start*2]);
          pixels.show();
          pps_state = false;
        }
      }
      break;
    }
    case 1: {
      if (Tx_Start==0) receiving();
      else transmitting();
      if (Serial.dtr()) {
        transmit();
      }
      else {
        receive();
      }
      break;
    }
    default: break;
  }
}

void transmitting(){
  int64_t audio_freq;
  copier.copy();
  if (USBAudio_read) {
    mono_prev = 0;  // for calculation by the single packet 
    mono_preprev = mono_prev;  // for calculation by the single packet 
    delta_prev = 0;
    for (int i=0;i< USBAudio_read_length ;i++){
      int16_t mono = monodata[i];
      if ((mono_prev < 0) && (mono >= 0)) {
        if ((mono == 0) && (((float)mono_prev * 1.8 - (float)mono_preprev < 0.0) || ((float)mono_prev * 2.02 - (float)mono_preprev > 0.0))) {    //Detect the sudden drop to zero due to the end of transmission
          Tx_Start = 0;
          break;
        }
        int16_t difference = mono - mono_prev;
        // x=0付近のsin関数をテーラー展開の第1項まで(y=xで近似）
        float delta = (float)mono_prev / (float)difference;
        if (delta_prev != 0){
          float period = (1.0 + delta_prev) + (float)sampling - delta;
          audio_freq = AUDIOSAMPLING*100.0/period; // in 0.01Hz    
          if ((audio_freq>270000) && (audio_freq<350500)){
            cycle_frequency[cycle]=audio_freq;
            cycle++;
          }
        }
        delta_prev = delta;
        sampling=0;
        mono_preprev = mono_prev;
        mono_prev = mono;     
      }
      else if ((not_TX_first == 1) && (mono_prev == 0) && (mono == 0)) {        //Detect non-transmission
        Tx_Start = 0;
        break;
      }
      else {
        sampling++;
        mono_preprev = mono_prev;
        mono_prev = mono;
      }
    }
    if (Tx_Start == 0){
      cycle = 0;
      receive();
      return;
    }
    if ((cycle > 0) && (millis() - Tx_last_mod_time > 10)){  //the frequency changes every 10mS
      audio_freq = 0;
      int n_audio_freq = 0;
      //eliminate distortion caused by imperfection in USB audio of the modified Adafruit_TinyUSB_Arduino Library (at 2025/07/22)
      for (int i = 0; i < cycle - 1; i++){         //sorting data
        for (int j = i + 1; j < cycle; j++) {
          if (cycle_frequency[i] > cycle_frequency[j]) {
          int temp = cycle_frequency[i];
          cycle_frequency[i] = cycle_frequency[j];
          cycle_frequency[j] = temp;
          }
        }
      }
      for (int i=cycle/10;i<9*cycle/10;i++){    //using only the central 4/5 data
      //for (int i=0;i<cycle;i++){    //using all data
          audio_freq += cycle_frequency[i];
          n_audio_freq ++;
      }
      audio_freq = audio_freq / n_audio_freq;
      if (abs(audio_freq - audio_freq_prev) > 100 ){
        sineWave.end();
        sineWave.begin(info, (audio_freq-200000)/100.0*audio_freq_correct);  //transmittion freqency shift (-2000Hz)
        audio_freq_prev = audio_freq;
      }
      if ((audio_freq > 279900) && (audio_freq < 350000)) transmit();  //limit the audio frequency  
      cycle = 0;
      Tx_last_mod_time = millis();
    }
    not_TX_first = 1;
    Tx_last_time = millis();
  }
  else if (millis()-Tx_last_time > 50) {  // If USBaudio data is not received for more than 50 ms during transmission, the system moves to receive. 
    Tx_Start = 0;
    cycle = 0;
    receive();
    return;
  }  
  USBAudioRead();
}

void receiving() {
  receive();
  USBAudioRead();  // read in the USB Audio buffer (myRawBuffer) to check the transmitting
  if (USBAudio_read) 
  {
    uint16_t monomax = 0;
    for (int i=0; i<USBAudio_read_length; i++){
      if (abs(monodata[i]) > monomax) monomax = abs(monodata[i]);
    }
    if (monomax > 22938)
    {                       // VOX if singanl exceeds 70% of 2^15
      Tx_Start = 1;
      not_TX_first = 0;
      return;
    }
  }
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
    // initializaztion of ADC data write counter
    mic_counter=0;
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
  if (sample_count_spk > now_sample_count_spk){
    USBAudio_read = 1;
    now_sample_count_spk = sample_count_spk;
  }
  else USBAudio_read = 0;
  int32_t monosum=0;
  if (USBAudio_read) {
    //for (int i = 0; i < USBAudio_read_length ; i++) {
      for (int i = 0; i < 48 ; i++) {
      int16_t outL = spk_buffer16[2*i];
      int16_t outR = spk_buffer16[2*i+1];
      int16_t mono = (outL+outR)/2;
      monosum += mono;
      monodata[i] = mono;
    }
  }
  if (monosum == 0) USBAudio_read=0;
}

void USBAudioWrite(int16_t left,int16_t right) {
  if (sample_count_mic > now_sample_count_mic){
    mic_counter = 0;
    now_sample_count_mic = sample_count_mic;
  }
  mic_buffer16[mic_counter] = left;
  mic_buffer16[mic_counter+1] = right;
  mic_counter+=2;
  if (mic_counter>96) mic_counter = 0;
}
