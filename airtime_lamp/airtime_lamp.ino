// Airtime lamp: measures real 2.4GHz WiFi activity in promiscuous mode.
//
// Unlike a network scan, this counts every frame the radio hears --
// beacons, data from phones and laptops, even acknowledgements -- so it
// reflects how busy the air actually is rather than how many routers exist.

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WiFi.h>
#include <Wire.h>

extern "C" {
#include "user_interface.h"
}

// Four adjacent pins, D5-D8. D7 is held low as a software ground for the
// LED's common leg, which sits third of four on these LEDs.
const uint8_t PIN_BLUE = D5;
const uint8_t PIN_GREEN = D6;
const uint8_t PIN_GROUND = D7;
const uint8_t PIN_RED = D8;

// Of 1023. All channels' current returns through PIN_GROUND, and the pins
// are rated around 12mA with no series resistors fitted.
const int MAX_DUTY = 200;
const int RED_PERCENT = 45;

const uint8_t CHANNEL_MIN = 1;
const uint8_t CHANNEL_MAX = 13;
const uint8_t CHANNEL_COUNT = CHANNEL_MAX - CHANNEL_MIN + 1;

// Time spent listening on each channel before hopping.
const unsigned long DWELL_MS = 120;

// How fast a channel's energy decays once it goes quiet.
const float DECAY = 0.6f;

// Combined power, in dBm, mapped onto the 0-1 range the lamp displays.
const float QUIET_DBM = -95.0f;
const float BUSY_DBM = -35.0f;

const unsigned long SCREEN_MS = 8000;
const uint8_t SCREEN_COUNT = 3;
const uint8_t HISTORY_LEN = 128;

Adafruit_SSD1306 display(128, 64, &Wire, -1);

// Written by the packet callback, read by the main loop.
volatile uint32_t frames[CHANNEL_COUNT];
volatile int32_t rssi_total[CHANNEL_COUNT];

// Linear received power per channel, summed over the frames heard.
float energy[CHANNEL_COUNT];
float last_dbm = QUIET_DBM;
uint8_t history[HISTORY_LEN];
uint8_t history_head = 0;
uint8_t channel = CHANNEL_MIN;
uint8_t screen = 0;

/** Every frame the radio hears lands here, with its signal strength. */
void ICACHE_RAM_ATTR onPacket(uint8_t* buf, uint16_t len) {
  if(len < 12) return;
  int8_t rssi = (int8_t)buf[0];
  uint8_t index = channel - CHANNEL_MIN;
  frames[index]++;
  rssi_total[index] += rssi;
}

void setLed(float r, float g, float b) {
  const float scale[3] = {RED_PERCENT / 100.0f, 1.0f, 1.0f};
  const uint8_t pins[3] = {PIN_RED, PIN_GREEN, PIN_BLUE};
  const float values[3] = {r, g, b};
  for(int i = 0; i < 3; i++) {
    float v = constrain(values[i], 0.0f, 1.0f);
    analogWrite(pins[i], (int)(v * v * MAX_DUTY * scale[i]));
  }
}

/** Hue 0-1 to RGB, saturation and value fixed at full. */
void setHue(float h, float v) {
  float sector = h * 6.0f;
  int i = ((int)sector) % 6;
  float f = sector - (int)sector;
  float p = 0.0f, q = v * (1.0f - f), t = v * f;
  switch(i) {
    case 0: setLed(v, t, p); break;
    case 1: setLed(q, v, p); break;
    case 2: setLed(p, v, t); break;
    case 3: setLed(p, q, v); break;
    case 4: setLed(t, p, v); break;
    default: setLed(v, p, q); break;
  }
}

void hopChannel() {
  channel++;
  if(channel > CHANNEL_MAX) channel = CHANNEL_MIN;
  wifi_set_channel(channel);
}

/** Fold this dwell's frames into the channel's decaying energy.
 *
 * Each frame contributes its own received power, so both how much traffic
 * there is and how strong it is affect the result. Decibels cannot be
 * added, so the mean is converted to linear power before scaling.
 */
void accumulate(uint8_t index) {
  noInterrupts();
  uint32_t count = frames[index];
  int32_t rssi_sum = rssi_total[index];
  frames[index] = 0;
  rssi_total[index] = 0;
  interrupts();

  float power = 0.0f;
  if(count > 0) {
    float mean_rssi = (float)rssi_sum / (float)count;
    power = count * powf(10.0f, mean_rssi / 10.0f);
  }
  energy[index] = energy[index] * DECAY + power * (1.0f - DECAY);
}

/** Combined power across the band as a 0-1 level, via dBm. */
float totalActivity() {
  float total = 0.0f;
  for(int i = 0; i < CHANNEL_COUNT; i++) total += energy[i];

  last_dbm = (total > 0.0f) ? 10.0f * log10f(total) : QUIET_DBM;
  float level = (last_dbm - QUIET_DBM) / (BUSY_DBM - QUIET_DBM);
  return constrain(level, 0.0f, 1.0f);
}

void drawTotal(float level) {
  display.setCursor(0, 0);
  display.print(F("Airtime activity"));

  display.setCursor(0, 14);
  display.print((int)last_dbm);
  display.print(F(" dBm  ch"));
  int best = 0;
  for(int i = 1; i < CHANNEL_COUNT; i++)
    if(energy[i] > energy[best]) best = i;
  display.print(best + CHANNEL_MIN);
  display.print(F(" "));
  display.print((int)(level * 100));
  display.print(F("%"));

  display.drawRect(0, 26, 128, 8, SSD1306_WHITE);
  display.fillRect(1, 27, (int)(level * 126), 6, SSD1306_WHITE);

  for(int x = 0; x < HISTORY_LEN; x++) {
    uint8_t value = history[(history_head + x) % HISTORY_LEN];
    int h = value * 24 / 255;
    if(h) display.drawFastVLine(x, 62 - h, h, SSD1306_WHITE);
  }
}

void drawChannels() {
  display.setCursor(0, 0);
  display.print(F("Activity by channel"));
  int width = 128 / CHANNEL_COUNT;
  float peak = 0.0f;
  for(int i = 0; i < CHANNEL_COUNT; i++)
    if(energy[i] > peak) peak = energy[i];
  for(int i = 0; i < CHANNEL_COUNT; i++) {
    int h = (peak > 0.0f) ? (int)((energy[i] / peak) * 40.0f) : 0;
    if(h) display.fillRect(i * width, 52 - h, width - 1, h, SSD1306_WHITE);
  }
  display.drawFastHLine(0, 53, 128, SSD1306_WHITE);
  display.setCursor(0, 56);
  display.print(F("1"));
  display.setCursor(5 * width, 56);
  display.print(F("6"));
  display.setCursor(10 * width, 56);
  display.print(F("11"));
}

void drawLive() {
  display.setCursor(0, 0);
  display.print(F("Listening ch "));
  display.print(channel);
  display.setCursor(0, 20);
  display.print(F("power this channel:"));
  display.setCursor(0, 34);
  display.setTextSize(2);
  float e = energy[channel - CHANNEL_MIN];
  display.print((int)(e > 0.0f ? 10.0f * log10f(e) : QUIET_DBM));
  display.setTextSize(1);
  display.print(F(" dBm"));
}

void setup() {
  pinMode(PIN_GROUND, OUTPUT);
  digitalWrite(PIN_GROUND, LOW);
  analogWriteRange(1023);
  analogWriteFreq(1000);
  setLed(0, 0, 0);

  // Wire.begin takes SDA first: SDA is GPIO5/D1, SCL is GPIO4/D2.
  Wire.begin(D1, D2);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // No screen: flash red slowly so the failure is visible.
    while(true) {
      setLed(1, 0, 0);
      delay(400);
      setLed(0, 0, 0);
      delay(400);
    }
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("Starting..."));
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_set_opmode(STATION_MODE);
  wifi_set_channel(channel);
  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(onPacket);
  wifi_promiscuous_enable(1);
}

void loop() {
  static unsigned long last_screen = 0;
  unsigned long start = millis();

  while(millis() - start < DWELL_MS) delay(1);
  accumulate(channel - CHANNEL_MIN);

  float level = totalActivity();
  history[history_head] = (uint8_t)(level * 255);
  history_head = (history_head + 1) % HISTORY_LEN;

  setHue(0.66f - level * 0.66f, 0.15f + level * 0.85f);

  if(millis() - last_screen > SCREEN_MS) {
    screen = (screen + 1) % SCREEN_COUNT;
    last_screen = millis();
  }

  display.clearDisplay();
  if(screen == 0) drawTotal(level);
  else if(screen == 1) drawChannels();
  else drawLive();
  display.display();

  hopChannel();
}
