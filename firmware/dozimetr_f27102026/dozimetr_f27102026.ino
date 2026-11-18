#include <Adafruit_GFX.h>
#include <SPI.h>
#include <Arduino_ST7789_Fast.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <avr/pgmspace.h>
#include <stddef.h>
#include <stdlib.h>
#include "config.h"
#include "boot.h"

Arduino_ST7789 lcd = Arduino_ST7789(PIN_TFT_DC, PIN_TFT_RST);
Adafruit_NeoPixel strip(1, PIN_LED, NEO_GRB + NEO_KHZ800);

struct Settings {
  uint8_t ver, brightness, sleep, anim, unit, graphStep;
  uint8_t sound, clicks, alert, ledMode, ledBright, lowBat, volume;
};
#define OFS(f) offsetof(Settings, f)
struct RowDef { const char* name; uint8_t off, n, fmt; };
Settings cfg, cfgBackup;

static const uint16_t sleepSec[5]  PROGMEM = {0, 10, 30, 60, 300};
static const uint8_t  graphSec[4]  PROGMEM = {2, 5, 10, 30};
static const uint8_t  lowBatPct[4] PROGMEM = {10, 15, 20, 30};

static void setDefaults() {
  cfg = Settings{CFG_VERSION, DEF_BRIGHTNESS, DEF_SLEEP, DEF_ANIM, DEF_UNIT, DEF_GRAPH_STEP,
                 DEF_SOUND, DEF_CLICKS, DEF_ALERT, DEF_LED_MODE, DEF_LED_BRIGHT, DEF_LOW_BAT, DEF_VOLUME};
}
static void saveCfg() { EEPROM.put(0, cfg); }
static void loadCfg() {
  EEPROM.get(0, cfg);
  if (cfg.ver != CFG_VERSION) { setDefaults(); saveCfg(); }
}

volatile uint8_t blkLevel = 0, buzLevel = PWM_LEVELS, buzOn = 0;
ISR(TIMER1_COMPA_vect) {
  static uint8_t ph;
  ph = (ph + 1) & (PWM_LEVELS - 1);
  if (ph < blkLevel) PORTB |= 1; else PORTB &= ~1;                 
  if (buzOn && ph < buzLevel) PORTD |= 0x10; else PORTD &= ~0x10;  
}
static void backlightInit() {
  DDRB |= 1;
  TCCR1A = 0;
  TCCR1B = (1 << WGM12) | (1 << CS11);               
  OCR1A = PWM_TICK_OCR;
  TIMSK1 = (1 << OCIE1A);
}
static uint8_t blkTarget() {                        
  uint8_t l = (uint16_t)(cfg.brightness + 1) * PWM_LEVELS / 10;
  return l ? l : 1;
}
static void backlightSet() { blkLevel = blkTarget(); }
static void backlightFade(uint8_t to) {              
  uint8_t l = blkLevel;
  while (l != to) { l += (l < to) ? 1 : -1; blkLevel = l; delay(FADE_STEP_MS); }
}
static void volumeApply() {                          
  uint8_t l = (uint16_t)(cfg.volume + 1) * PWM_LEVELS / 10;
  buzLevel = l ? l : 1;
}


static uint16_t blend(uint16_t c, uint8_t k) {       
  uint16_t r = ((c >> 11) & 31) * k / 255;
  uint16_t g = ((c >> 5) & 63) * k / 255;
  uint16_t b = (c & 31) * k / 255;
  return (r << 11) | (g << 5) | b;
}
static uint16_t gray(uint8_t v) { return COLOR565(v, v, v); }
static inline bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static void printCenter(int16_t cx, int16_t y, uint8_t sz, const __FlashStringHelper* s,
                        uint16_t fg, uint16_t bg) {
  lcd.setTextSize(sz);
  lcd.setTextColor(fg, bg);
  lcd.setCursor(cx - (int16_t)strlen_P((PGM_P)s) * 3 * sz, y);
  lcd.print(s);
}

volatile uint16_t pulseCnt = 0;
volatile bool clickFlag = false;
volatile uint32_t lastPulseUs = 0;
static void geigerISR() {
  uint32_t t = micros();
  if (t - lastPulseUs < GEIGER_DEAD_US) return;      
  lastPulseUs = t; pulseCnt++; clickFlag = true;
}

uint8_t  secBuf[GEIGER_WINDOW_S];
uint8_t  secIdx = 0, secFilled = 0;
float    cpm = 0, dose = 0;                          
uint8_t  zone = 0;                                   
uint16_t hist[GRAPH_POINTS];                         
uint8_t  histCnt = 0, graphTimer = 0;

static float unitVal(float usv, uint8_t u) {
  return u == 0 ? usv : (u == 1 ? usv * USV_TO_UR : usv * GEIGER_CPM_PER_USVH);
}
static uint8_t unitDec(float v, uint8_t u) {
  if (u == 2) return 0;
  if (u == 1) return v < 100 ? 1 : 0;
  return v < 10 ? 2 : (v < 100 ? 1 : 0);
}
static const __FlashStringHelper* unitName(uint8_t u) {
  return u == 0 ? F("usv/h") : (u == 1 ? F("ur/h") : F("cpm"));
}
static float graphStep(uint8_t u) {
  return u == 0 ? GRAPH_STEP_USV : (u == 1 ? GRAPH_STEP_UR : GRAPH_STEP_CPM);
}
static uint16_t zoneColor() { return zone == 0 ? COL_GOOD : (zone == 1 ? COL_WARN : COL_BAD); }

static void updateZone() {
  uint8_t r = dose > DOSE_WARN_MAX ? 2 : (dose > DOSE_GOOD_MAX ? 1 : 0);
  if (r < zone) {                                    
    float lim = (zone == 2) ? DOSE_WARN_MAX : DOSE_GOOD_MAX;
    if (dose > lim * ZONE_HYSTERESIS) r = zone;
  }
  zone = r;
}
static void histPush(uint16_t v) {
  if (histCnt < GRAPH_POINTS) hist[histCnt++] = v;
  else { memmove(hist, hist + 1, (GRAPH_POINTS - 1) * sizeof(uint16_t)); hist[GRAPH_POINTS - 1] = v; }
}

static const uint16_t batTab[11] PROGMEM = {3300, 3500, 3600, 3650, 3700, 3750, 3800, 3870, 3950, 4050, 4150};
uint16_t battMv = 0;
uint8_t  battPct = 0;
uint32_t battT = 0;

static void batteryRead() {
  uint32_t s = 0;
  for (uint8_t i = 0; i < 8; i++) s += analogRead(PIN_VBAT);
  float mv = (s / 8.0f) * ADC_REF_MV / 1023.0f * (BAT_R_TOP + BAT_R_BOTTOM) / BAT_R_BOTTOM;
  battMv = battMv ? (battMv * 3 + (uint16_t)mv) / 4 : (uint16_t)mv;
  uint8_t p = 0;
  if (battMv >= pgm_read_word(&batTab[10])) p = 100;
  else if (battMv > pgm_read_word(&batTab[0])) {
    uint8_t i = 0;
    while (battMv >= pgm_read_word(&batTab[i + 1])) i++;
    uint16_t a = pgm_read_word(&batTab[i]), b = pgm_read_word(&batTab[i + 1]);
    p = i * 10 + (uint32_t)(battMv - a) * 10 / (b - a);
  }
  battPct = p;
}
static uint16_t battColor() {
  if (battPct <= pgm_read_byte(&lowBatPct[cfg.lowBat])) return COL_BAD;
  return battPct < 40 ? COL_WARN : COL_GOOD;
}

#define RING_IDLE 255
static const uint16_t ringPat[RING_STEPS] PROGMEM = {RING_PATTERN};
uint8_t  ringStep = RING_IDLE;
uint32_t ringNext = 0, clickOff = 0;
bool     clickOn = false;
bool     alertOn = false, alertPhaseOn = false;
uint32_t alertT = 0;

static inline void buzzer(bool on) { buzOn = on; if (!on) PORTD &= ~0x10; }
static void ringStart(uint32_t now) {
  ringStep = 0; ringNext = now + pgm_read_word(&ringPat[0]); clickOn = false; buzzer(true);
}
static void ringStop() { ringStep = RING_IDLE; buzzer(false); }
static void buzzerService(uint32_t now) {
  if (ringStep != RING_IDLE) {
    if (!due(now, ringNext)) return;
    if (++ringStep >= RING_STEPS) { ringStop(); return; }
    buzzer(!(ringStep & 1));
    ringNext = now + pgm_read_word(&ringPat[ringStep]);
  } else if (clickOn && due(now, clickOff)) { clickOn = false; buzzer(false); }
}

bool     ledPulseOn = false, ledTestOn = false;
uint32_t ledPulseUntil = 0, ledTestUntil = 0, ledCur = 0xFFFFFFFF;

static void clickService(uint32_t now) {
  if (!clickFlag) return;
  clickFlag = false;
  if (cfg.sound && cfg.clicks && ringStep == RING_IDLE) { buzzer(true); clickOn = true; clickOff = now + CLICK_MS; }
  if (cfg.ledMode == 2) { ledPulseOn = true; ledPulseUntil = now + LED_PULSE_MS; }
}
static void alertService(uint32_t now) {
  bool c = (cfg.alert == 1) ? (zone == 2) : (cfg.alert == 2 && zone >= 1);
  if (c && !alertOn) { alertOn = true; alertPhaseOn = true; alertT = now; }
  else if (!c && alertOn) { alertOn = false; ringStop(); }
  if (!alertOn) return;
  if (due(now, alertT + (alertPhaseOn ? ALERT_ON_MS : ALERT_PAUSE_MS))) {
    alertPhaseOn = !alertPhaseOn; alertT = now;
    if (!alertPhaseOn) ringStop();
  }
  if (alertPhaseOn && cfg.sound && ringStep == RING_IDLE) ringStart(now);
}
static void ledTest() { ledTestOn = true; ledTestUntil = millis() + LED_TEST_MS; }
static void ledService(uint32_t now) {
  if (ledPulseOn && due(now, ledPulseUntil)) ledPulseOn = false;
  if (ledTestOn && due(now, ledTestUntil)) ledTestOn = false;
  bool on = ledTestOn;
  if (cfg.ledMode) on = on || ((alertOn && alertPhaseOn) ? ((now / LED_BLINK_MS) & 1) : (cfg.ledMode == 2 && ledPulseOn));
  uint32_t col = 0;
  if (on) {
    uint8_t k = (cfg.ledBright + 1) * 10;
    uint8_t r = zone == 0 ? 0 : 255, g = zone == 2 ? 0 : (zone == 1 ? 180 : 255);
    col = strip.Color(r * k / 100, g * k / 100, 0);
  }
  if (col != ledCur) { ledCur = col; strip.setPixelColor(0, col); strip.show(); }
}

enum { EV_NONE, EV_LEFT, EV_MID, EV_RIGHT, EV_LEFT_LONG, EV_MID_LONG, EV_RIGHT_LONG };
static const uint8_t btnPin[3] PROGMEM = {PIN_BTN_LEFT, PIN_BTN_MID, PIN_BTN_RIGHT};
bool     btnDown[3], btnLongDone[3];
uint32_t btnT[3], btnPressT[3];

static uint8_t buttonsPoll(uint32_t now) {
  for (uint8_t i = 0; i < 3; i++) {
    bool p = !digitalRead(pgm_read_byte(&btnPin[i]));
    if (p && !btnDown[i] && now - btnT[i] > BTN_DEBOUNCE_MS) {
      btnDown[i] = true; btnLongDone[i] = false; btnT[i] = btnPressT[i] = now;
    } else if (p && btnDown[i] && !btnLongDone[i] && now - btnPressT[i] >= BTN_LONG_MS) {
      btnLongDone[i] = true; return EV_LEFT_LONG + i;
    } else if (!p && btnDown[i] && now - btnT[i] > BTN_DEBOUNCE_MS) {
      btnDown[i] = false; btnT[i] = now;
      if (!btnLongDone[i]) return EV_LEFT + i;
    }
  }
  return EV_NONE;
}

enum { SCR_MAIN, SCR_MENU, SCR_PAGE0 };              
enum { PG_VIS, PG_SND, PG_MEAS, PG_PWR, PG_ABOUT, PG_COUNT };
enum { DF_ALL = 1, DF_RING = 2, DF_ICON = 4, DF_TOP = 8, DF_VALUE = 16, DF_STATS = 32, DF_GRAPH = 64 };
enum { BAR_VAL, BAR_ACT, BAR_APPLY, BAR_SAVED, BAR_MENU, BAR_NONE = 255 };

uint8_t  screen = SCR_MAIN, menuSel = 0, selRow = 0, dirty = DF_ALL, barMode = BAR_NONE;
uint8_t  snakePos = 0, wavePhase = 0;
bool     sleeping = false, battInfoDirty = true;
uint32_t snakeT = 0, bsT = 0, waveT = 0, lastAct = 0, secT = 0, savedUntil = 0;

#define GX       40
#define GY       160
#define GW       188
#define GH       68
#define RING_CELLS 236
#define ROW_H      17
#define ROW_PITCH  20

static void cellXY(uint8_t p, int16_t &x, int16_t &y) {
  if (p < 59)       { x = p;            y = 0; }
  else if (p < 118) { x = 59;           y = p - 59; }
  else if (p < 177) { x = 59 - (p - 118); y = 59; }
  else              { x = 0;            y = 59 - (p - 177); }
  x *= 4; y *= 4;
}
static void drawCell(uint8_t p, uint16_t c) {
  int16_t x, y; cellXY(p, x, y); lcd.fillRect(x, y, 4, 4, c);
}
static void drawRing() {
  uint16_t c = blend(zoneColor(), 70);
  lcd.fillRect(0, 0, 240, 4, c); lcd.fillRect(0, 236, 240, 4, c);
  lcd.fillRect(0, 0, 4, 240, c); lcd.fillRect(236, 0, 4, 240, c);
}
static void snakeDraw(bool full) {
  uint16_t zc = zoneColor();
  if (!full) drawCell((snakePos + RING_CELLS - SNAKE_LEN) % RING_CELLS, blend(zc, 70));
  for (uint8_t i = 0; i < SNAKE_LEN; i++)
    drawCell((snakePos + RING_CELLS - i) % RING_CELLS, blend(zc, 255 - i * (190 / SNAKE_LEN)));
}

static void drawCone(int16_t x, int16_t cy, int16_t len, int16_t r0, int16_t r1, uint16_t c) {
  for (int16_t i = 0; i < len; i++) {
    int16_t r = r0 + (r1 - r0) * i / len;
    lcd.drawFastVLine(x + i, cy - r, 2 * r + 1, c);
  }
}
static void drawStatusIcon() {
  uint16_t c = zoneColor();
  lcd.fillRect(72, 28, 88, 56, COL_BG);
  if (zone == 0) {                                    
    lcd.fillCircle(120, 56, 24, c);
    for (int8_t i = -1; i <= 1; i++) {
      lcd.drawLine(108, 57 + i, 116, 66 + i, COL_BG);
      lcd.drawLine(116, 66 + i, 135, 45 + i, COL_BG);
    }
  } else if (zone == 1) {                             
    for (int16_t y = 30; y <= 80; y++) { int16_t hw = (y - 30) * 28 / 50; lcd.drawFastHLine(120 - hw, y, 2 * hw + 1, c); }
    lcd.fillRect(117, 48, 6, 18, COL_BG);
    lcd.fillRect(117, 70, 6, 6, COL_BG);
  } else {                                            
    for (uint8_t r = 22; r <= 24; r++) lcd.drawCircle(120, 56, r, c);
    for (int8_t i = -1; i <= 1; i++) {
      lcd.drawLine(105 + i, 41, 135 + i, 71, c);
      lcd.drawLine(135 + i, 41, 105 + i, 71, c);
    }
  }
}
static void drawBatteryIcon(int16_t x, int16_t y, uint16_t c, uint8_t pct, uint16_t bg) {
  lcd.fillRect(x, y, 36, 16, bg);
  lcd.drawRect(x, y, 32, 16, COL_TEXT);
  lcd.fillRect(x + 32, y + 4, 3, 8, COL_TEXT);
  uint8_t n = (pct + 24) / 25;
  for (uint8_t i = 0; i < n; i++) lcd.fillRect(x + 3 + i * 7, y + 3, 6, 10, c);
}

static void drawSpeakerIcon(int16_t x, bool on) {
  uint16_t c = on ? COL_TEXT : COL_TEXT_DIM;
  lcd.fillRect(x, 13, 4, 8, c);
  drawCone(x + 4, 17, 9, 4, 9, c);
  if (on) { lcd.drawFastVLine(x + 16, 12, 10, c); lcd.drawFastVLine(x + 19, 9, 16, c); }
  else { lcd.drawLine(x - 2, 8, x + 20, 26, COL_BAD); lcd.drawLine(x - 1, 8, x + 21, 26, COL_BAD); }
}
static void drawLedIcon(int16_t x, bool on) {
  if (on) {
    lcd.fillCircle(x + 8, 15, 6, COL_WARN);
    lcd.fillRect(x + 5, 21, 7, 3, COL_WARN);
    lcd.fillRect(x + 6, 24, 5, 2, COL_TEXT_DIM);
  } else {
    lcd.drawCircle(x + 8, 15, 6, COL_TEXT_DIM);
    lcd.drawRect(x + 5, 21, 7, 3, COL_TEXT_DIM);
    lcd.drawRect(x + 6, 24, 5, 2, COL_TEXT_DIM);
    lcd.drawLine(x - 1, 8, x + 17, 26, COL_BAD); lcd.drawLine(x, 8, x + 18, 26, COL_BAD);
  }
}
static void drawTop() {
  lcd.fillRect(8, 6, 220, 22, COL_BG);                
  lcd.setTextSize(2); lcd.setTextColor(COL_TEXT_DIM, COL_BG);
  lcd.setCursor(10, 10); lcd.print(F("CPM ")); lcd.print((uint16_t)(cpm + 0.5f));
  drawLedIcon(126, cfg.ledMode != 0);
  drawSpeakerIcon(158, cfg.sound);
  drawBatteryIcon(190, 10, battColor(), battPct, COL_BG);
}
static void drawValue() {
  char b[12];
  float v = unitVal(dose, cfg.unit);
  dtostrf(v, 1, unitDec(v, cfg.unit), b);
  uint8_t vl = strlen(b), ul = cfg.unit == 0 ? 5 : (cfg.unit == 1 ? 4 : 3);
  int16_t x = (240 - (vl * 30 + 8 + ul * 18)) / 2;
  lcd.fillRect(8, 88, 224, 44, COL_BG);
  lcd.setTextColor(zoneColor(), COL_BG);
  lcd.setTextSize(5); lcd.setCursor(x, 90); lcd.print(b);
  lcd.setTextSize(3); lcd.setCursor(x + vl * 30 + 8, 106); lcd.print(unitName(cfg.unit));
}
static void drawStats() {
  float sum = 0, mx = 0;
  for (uint8_t i = 0; i < histCnt; i++) {
    float v = unitVal(hist[i] * 0.001f, cfg.unit);
    sum += v; if (v > mx) mx = v;
  }
  float avg = histCnt ? sum / histCnt : 0;
  for (uint8_t k = 0; k < 2; k++) {
    int16_t x = k ? 124 : 8; float v = k ? mx : avg; char b[12];
    dtostrf(v, 1, unitDec(v, cfg.unit), b);
    lcd.drawRect(x, 136, 108, 18, COL_PANEL_EDGE);
    lcd.fillRect(x + 1, 137, 106, 16, COL_BG);
    lcd.setTextSize(1); lcd.setTextColor(COL_TEXT, COL_BG);
    lcd.setCursor(x + 4, 141);
    lcd.print(k ? F("Max: ") : F("Avg: ")); lcd.print(b); lcd.print(unitName(cfg.unit));
  }
}
static int16_t graphPx(uint8_t i, float top) {
  int16_t h = unitVal(hist[i] * 0.001f, cfg.unit) * GH / top;
  return h > GH ? GH : h;
}
static void drawGraph() {
  uint8_t u = cfg.unit;
  float step = graphStep(u), mx = 0;
  for (uint8_t i = 0; i < histCnt; i++) { float v = unitVal(hist[i] * 0.001f, u); if (v > mx) mx = v; }
  float top = step * GRAPH_MIN_TOP_STEPS;
  while (top < mx) top += step;
  lcd.fillRect(6, GY - 6, 226, GH + 12, COL_BG);
  for (uint8_t i = 0; i < histCnt; i++) {            
    int16_t h0 = graphPx(i, top), h1 = (i + 1 < histCnt) ? graphPx(i + 1, top) : h0;
    for (uint8_t k = 0; k < 4; k++) {
      int16_t h = h0 + (h1 - h0) * k / 4, x = GX + i * 4 + k;
      if (h > 0) lcd.drawFastVLine(x, GY + GH - h, h, COL_GRAPH_FILL);
      lcd.drawPixel(x, GY + GH - h, COL_GRAPH_LINE);
      if (i + 1 >= histCnt) break;
    }
  }
  for (uint8_t j = 0; j <= 4; j++) {                  
    int16_t y = GY + j * GH / 4, x = GX + j * GW / 4;
    lcd.drawFastHLine(GX, y, GW, COL_GRID);
    lcd.drawFastVLine(x, GY, GH, COL_GRID);
    char b[10]; dtostrf(top * (4 - j) / 4, 4, u == 0 ? 2 : 0, b);
    lcd.setTextSize(1); lcd.setTextColor(COL_TEXT_DIM, COL_BG);
    lcd.setCursor(8, y - 3); lcd.print(b);
  }
  lcd.drawRect(GX - 1, GY - 1, GW + 2, GH + 3, COL_PANEL_EDGE);
}
static void mainRender() {
  if (dirty & DF_ALL) { lcd.fillScreen(COL_BG); dirty = 0xFF; }
  if (dirty & DF_RING)  { drawRing(); if (cfg.anim) snakeDraw(true); }
  if (dirty & DF_ICON)  drawStatusIcon();
  if (dirty & DF_TOP)   drawTop();
  if (dirty & DF_VALUE) drawValue();
  if (dirty & DF_STATS) drawStats();
  if (dirty & DF_GRAPH) drawGraph();
  dirty = 0;
}

static void sleepEnter() {
  if (cfg.anim) backlightFade(0);
  blkLevel = 0; sleeping = true;
}
static void wakeUp() {
  sleeping = false; lastAct = millis();
  dirty = DF_ALL; mainRender();                       
  if (cfg.anim) backlightFade(blkTarget()); else backlightSet();
}

/* ---- logo / boot ---- */
static void drawLogo(int16_t x, int16_t y, uint8_t sc, uint16_t c) {
  for (uint8_t j = 0; j < LOGO_H_PX; j++)
    for (uint8_t i = 0; i < LOGO_W; i++)
      if (pgm_read_byte(&logoBmp[j * 8 + (i >> 3)]) & (0x80 >> (i & 7)))
        lcd.fillRect(x + i * sc, y + j * sc, sc, sc, c);
}
static void bootAnimation() {
  lcd.fillScreen(COL_BG);
  if (!cfg.anim) {                                             
    drawLogo(56, 40, 2, gray(255));
    printCenter(120, 184, 2, F("delovoyarseni"), gray(255), COL_BG);
    delay(BOOT_HOLD_MS);
  } else {
    for (int8_t s = 0; s <= 2 * BOOT_FADE_STEPS; s++) {         
      int8_t t = s <= BOOT_FADE_STEPS ? s : 2 * BOOT_FADE_STEPS - s;
      uint16_t c = gray(t * 255 / BOOT_FADE_STEPS);
      uint32_t until = millis() + BOOT_STEP_MS;
      drawLogo(56, 40, 2, c);
      printCenter(120, 184, 2, F("delovoyarseni"), c, COL_BG);
      if (s == BOOT_FADE_STEPS) delay(BOOT_HOLD_MS);
      while (!due(millis(), until)) {}
    }
  }
  lcd.fillScreen(COL_BG);
}

static int16_t abX, abY, abW, abH;                  
static uint16_t abCol;
static uint8_t abPos = 0;

static uint8_t btnPerim() { return (abW / 2) * 2 + (abH / 2) * 2 - 4; }
static void btnCellDraw(uint8_t p, uint16_t c) {
  int16_t A = abW / 2, B = abH / 2, cx, cy;
  if (p < A)                  { cx = p;                         cy = 0; }
  else if (p < A + B - 1)     { cx = A - 1;                     cy = p - (A - 1); }
  else if (p < 2 * A + B - 2) { cx = A - 1 - (p - (A + B - 2)); cy = B - 1; }
  else                        { cx = 0;                         cy = B - 1 - (p - (2 * A + B - 3)); }
  lcd.fillRect(abX + cx * 2, abY + cy * 2, 2, 2, c);
}
static void btnSnakeDraw() {
  uint8_t P = btnPerim();
  for (uint8_t i = 0; i < BTN_SNAKE_LEN; i++)
    btnCellDraw((abPos + P - i) % P, blend(abCol, 255 - i * (150 / BTN_SNAKE_LEN)));
}
static void btnSnakeStep() {
  uint8_t P = btnPerim();
  for (uint8_t i = BTN_SNAKE_LEN - BTN_SNAKE_SPEED; i < BTN_SNAKE_LEN; i++)
    btnCellDraw((abPos + P - i) % P, blend(abCol, 110));
  abPos = (abPos + BTN_SNAKE_SPEED) % P;
  btnSnakeDraw();
}
static void drawActionBtn(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col,
                          const __FlashStringHelper* label, uint8_t sz, bool active, uint16_t txt) {
  uint16_t ring = blend(col, active ? 110 : 70);
  lcd.fillRect(x, y, w, h, blend(col, active ? 60 : 35));
  lcd.drawRect(x, y, w, h, ring); lcd.drawRect(x + 1, y + 1, w - 2, h - 2, ring);
  lcd.setTextSize(sz); lcd.setTextColor(txt);
  lcd.setCursor(x + (w - (int16_t)strlen_P((PGM_P)label) * 6 * sz) / 2, y + (h - 7 * sz) / 2);
  lcd.print(label);
  if (!active) return;
  abX = x; abY = y; abW = w; abH = h; abCol = col; abPos = 0;
  if (cfg.anim) btnSnakeDraw();
  else { lcd.drawRect(x, y, w, h, col); lcd.drawRect(x + 1, y + 1, w - 2, h - 2, col); }
}

static const __FlashStringHelper* pgTitle(uint8_t i) {
  switch (i) {
    case PG_VIS:  return F("Visuals");
    case PG_SND:  return F("Sound");
    case PG_MEAS: return F("Measure");
    case PG_PWR:  return F("Power");
    default:      return F("About");
  }
}
static void drawMenuIcon(uint8_t i, int16_t x, int16_t y) {
  uint16_t c = COL_TEXT;
  switch (i) {
    case PG_VIS:                                      
      lcd.drawCircle(x + 10, y + 10, 5, c);
      for (int8_t dx = -1; dx <= 1; dx++) for (int8_t dy = -1; dy <= 1; dy++)
        if (dx || dy) lcd.drawLine(x + 10 + dx * 7, y + 10 + dy * 7, x + 10 + dx * 10, y + 10 + dy * 10, c);
      break;
    case PG_SND:                                      
      lcd.fillRect(x + 2, y + 7, 5, 6, c);
      drawCone(x + 7, y + 10, 8, 3, 8, c);
      lcd.drawLine(x + 17, y + 5, x + 17, y + 15, c);
      break;
    case PG_MEAS:                                     
      lcd.drawFastVLine(x + 2, y + 2, 16, c); lcd.drawFastHLine(x + 2, y + 17, 17, c);
      lcd.drawLine(x + 4, y + 14, x + 8, y + 8, c); lcd.drawLine(x + 8, y + 8, x + 12, y + 12, c);
      lcd.drawLine(x + 12, y + 12, x + 18, y + 3, c);
      break;
    case PG_PWR:                                      
      lcd.drawRect(x, y + 4, 17, 12, c); lcd.fillRect(x + 17, y + 8, 3, 4, c);
      for (uint8_t k = 0; k < 3; k++) lcd.fillRect(x + 2 + k * 5, y + 6, 4, 8, c);
      break;
    default:                                         
      lcd.drawCircle(x + 10, y + 10, 9, c);
      lcd.fillRect(x + 9, y + 9, 2, 7, c); lcd.fillRect(x + 9, y + 5, 2, 2, c);
  }
}
#define MENU_Y0     42
#define MENU_PITCH  33
static void drawMenuRow(uint8_t i, bool sel) {
  int16_t y = MENU_Y0 + i * MENU_PITCH;
  lcd.fillRoundRect(8, y, 206, 30, 8, sel ? gray(90) : COL_PANEL);
  lcd.drawRoundRect(8, y, 206, 30, 8, sel ? COL_GOOD : COL_PANEL_EDGE);
  drawMenuIcon(i, 18, y + 5);
  lcd.setTextSize(2); lcd.setTextColor(COL_TEXT, sel ? gray(90) : COL_PANEL);
  lcd.setCursor(52, y + 8);
  lcd.print(pgTitle(i));
}
static void drawScrollbar() {
  lcd.fillRect(218, MENU_Y0, 16, 162, COL_UI_BG);
  lcd.drawRoundRect(220, MENU_Y0, 10, 162, 3, COL_PANEL_EDGE);
  lcd.fillRoundRect(222, MENU_Y0 + 2 + menuSel * 31, 6, 30, 2, COL_TEXT_DIM);
}
static void drawBtnBar(uint8_t mode) {
  static const int16_t bx[3] = {8, 70, 176}, bw[3] = {56, 100, 56};
  bool chev = (mode == BAR_MENU || mode == BAR_ACT);
  for (uint8_t i = 0; i < 3; i++) {
    int16_t cx = bx[i] + bw[i] / 2, cy = 223;
    if (i == 1) {
      if (mode == BAR_APPLY)      { drawActionBtn(bx[1], 212, bw[1], 22, COL_GOOD, F("Apply"), 2, true, COL_TEXT); continue; }
      if (mode == BAR_SAVED)      { drawActionBtn(bx[1], 212, bw[1], 22, COL_GOOD, F("Saved"), 2, false, COL_GOOD); continue; }
    }
    lcd.fillRoundRect(bx[i], 212, bw[i], 22, 5, COL_PANEL);
    lcd.drawRoundRect(bx[i], 212, bw[i], 22, 5, COL_PANEL_EDGE);
    if (i == 1) { printCenter(cx, 219, 1, F("Choose"), COL_TEXT, COL_PANEL); continue; }
    if (chev) {
      int8_t d = i ? -1 : 1;                           
      lcd.drawLine(cx - 8, cy + 3 * d, cx, cy - 3 * d, COL_TEXT);
      lcd.drawLine(cx, cy - 3 * d, cx + 8, cy + 3 * d, COL_TEXT);
    } else {
      lcd.drawFastHLine(cx - 8, cy, 17, COL_TEXT);
      if (i) lcd.drawFastVLine(cx, cy - 8, 17, COL_TEXT);
    }
  }
}
static void menuDraw() {
  lcd.fillScreen(COL_UI_BG);
  lcd.fillRoundRect(6, 4, 228, 32, 8, COL_PANEL);
  lcd.drawRoundRect(6, 4, 228, 32, 8, COL_PANEL_EDGE);
  printCenter(120, 10, 3, F("Settings"), COL_TEXT, COL_PANEL);
  for (uint8_t i = 0; i < PG_COUNT; i++) drawMenuRow(i, i == menuSel);
  drawScrollbar();
  barMode = BAR_MENU; drawBtnBar(BAR_MENU);
}

enum { F_PCT, F_ONOFF, F_SLEEP, F_UNIT, F_GRAPH, F_ALERT, F_LIGHT, F_BATT, F_ACT };

static const char nBright[] PROGMEM = "Brightness";
static const char nLight[]  PROGMEM = "Light";
static const char nLightL[] PROGMEM = "Light brightness";
static const char nAnim[]   PROGMEM = "Animations";
static const char nSleep[]  PROGMEM = "Auto sleep";
static const char nSound[]  PROGMEM = "Sound";
static const char nVolume[] PROGMEM = "Volume";
static const char nClicks[] PROGMEM = "Clicks";
static const char nAlarm[]  PROGMEM = "Alarm";
static const char nRing[]   PROGMEM = "Ringtone";
static const char nUnits[]  PROGMEM = "Units";
static const char nGraph[]  PROGMEM = "Graph step";
static const char nLowBat[] PROGMEM = "Low battery";
static const char nReset[]  PROGMEM = "Reset Settings";
static const char nReboot[] PROGMEM = "Reboot";

static const RowDef rowDefs[] PROGMEM = {
  {nBright, OFS(brightness), 10, F_PCT},   {nLight,  OFS(ledMode),   3, F_LIGHT},    
  {nLightL, OFS(ledBright),  10, F_PCT},   {nAnim,   OFS(anim),     2, F_ONOFF},
  {nSleep,  OFS(sleep),       5, F_SLEEP},
  {nSound,  OFS(sound),       2, F_ONOFF}, {nVolume, OFS(volume),   10, F_PCT},      
  {nClicks, OFS(clicks),      2, F_ONOFF}, {nAlarm,  OFS(alert),     3, F_ALERT},
  {nRing,   0,                0, F_ACT},
  {nUnits,  OFS(unit),        3, F_UNIT},  {nGraph,  OFS(graphStep), 4, F_GRAPH},    
  {nLowBat, OFS(lowBat),      4, F_BATT},                                            
  {nReset,  0,                0, F_ACT},   {nReboot, 0,             0, F_ACT},       
};
static const uint8_t pgStart[PG_COUNT] PROGMEM = {0, 5, 10, 12, 13};
static const uint8_t pgCnt[PG_COUNT]   PROGMEM = {5, 5, 2, 1, 2};

static uint8_t pgN(uint8_t pg) { return pgm_read_byte(&pgCnt[pg]); }
static void rowDef(uint8_t pg, uint8_t r, RowDef &d) {
  memcpy_P(&d, &rowDefs[pgm_read_byte(&pgStart[pg]) + r], sizeof d);
}
static bool rowAct(uint8_t pg, uint8_t r) { RowDef d; rowDef(pg, r, d); return d.fmt == F_ACT; }
static uint8_t &rowVal(const RowDef &d) { return *((uint8_t*)&cfg + d.off); }
static int16_t rowsY(uint8_t pg) { return pg == PG_PWR ? 156 : 104; }

static void putNum(char *b, uint16_t n, char suf) {
  itoa(n, b, 10); uint8_t l = strlen(b); b[l] = suf; b[l + 1] = 0;
}
static void valStr(uint8_t fmt, uint8_t v, char *b) {
  switch (fmt) {
    case F_PCT:   putNum(b, (v + 1) * 10, '%'); break;
    case F_ONOFF: strcpy_P(b, v ? PSTR("On") : PSTR("Off")); break;
    case F_SLEEP: {
      uint16_t s = pgm_read_word(&sleepSec[v]);
      if (!s) strcpy_P(b, PSTR("Off")); else if (s >= 60) putNum(b, s / 60, 'm'); else putNum(b, s, 's');
      break; }
    case F_UNIT:  strcpy_P(b, (PGM_P)unitName(v)); break;
    case F_GRAPH: putNum(b, pgm_read_byte(&graphSec[v]), 's'); break;
    case F_ALERT: strcpy_P(b, v == 0 ? PSTR("Off") : (v == 1 ? PSTR("Danger") : PSTR("Warn+"))); break;
    case F_LIGHT: strcpy_P(b, v == 0 ? PSTR("Off") : (v == 1 ? PSTR("Alerts") : PSTR("Pulse"))); break;
    case F_BATT:  putNum(b, pgm_read_byte(&lowBatPct[v]), '%'); break;
    default:      strcpy_P(b, PSTR("Play"));
  }
}

static void drawAboutBtn(uint8_t r, bool sel) {
  RowDef d; rowDef(PG_ABOUT, r, d);
  drawActionBtn(r ? 124 : 10, 194, 106, 16, r ? COL_BLUE : COL_BAD,
                (const __FlashStringHelper*)d.name, 1, sel, sel ? COL_TEXT : COL_TEXT_DIM);
}
static void drawRow(uint8_t pg, uint8_t r, bool sel) {
  if (pg == PG_ABOUT) { drawAboutBtn(r, sel); return; }
  RowDef d; rowDef(pg, r, d);
  int16_t y = rowsY(pg) + r * ROW_PITCH;
  char b[8]; valStr(d.fmt, rowVal(d), b);
  uint16_t bg = sel ? COL_PANEL : COL_UI_BG;
  lcd.fillRoundRect(8, y, 206, ROW_H, 4, bg);
  lcd.drawRoundRect(8, y, 206, ROW_H, 4, sel ? COL_BLUE : COL_GRID);
  lcd.setTextSize(1); lcd.setTextColor(sel ? COL_TEXT : COL_TEXT_DIM, bg);
  lcd.setCursor(14, y + 5); lcd.print((const __FlashStringHelper*)d.name);
  lcd.setTextSize(2); lcd.setTextColor(sel ? COL_GOOD : COL_TEXT, bg);
  lcd.setCursor(208 - strlen(b) * 12, y + 2); lcd.print(b);
}
static void drawSlider(uint8_t pg) {                 
  if (pg == PG_ABOUT) return;
  int16_t y0 = rowsY(pg), h = pgN(pg) * ROW_PITCH - 3;
  RowDef d; rowDef(pg, selRow, d);
  lcd.fillRect(218, y0, 16, h, COL_UI_BG);
  lcd.drawRoundRect(220, y0, 12, h, 4, COL_PANEL_EDGE);
  if (d.fmt == F_ACT || d.n < 2) return;
  int16_t fh = 2 + (int32_t)(h - 8) * rowVal(d) / (d.n - 1);
  lcd.fillRect(223, y0 + h - 3 - fh, 6, fh, COL_GOOD);
}

static void drawHero(uint8_t pg) {
  if (pg > PG_MEAS) return;
  lcd.fillRect(8, 28, 224, 74, COL_UI_BG);
  if (pg == PG_VIS) {                                 
    uint16_t c = blend(COL_WARN, 90 + cfg.brightness * 18);
    lcd.fillCircle(120, 65, 22, c);
  } else if (pg == PG_SND) {                          
    uint16_t c = cfg.sound ? COL_TEXT : COL_TEXT_DIM;
    lcd.fillRect(26, 58, 10, 16, c);
    drawCone(36, 66, 18, 8, 22, c);
    for (uint8_t i = 0; i < 10; i++) {
      int16_t h = 8 + i * 5;
      lcd.fillRect(70 + i * 15, 92 - h, 10, h, (cfg.sound && i <= cfg.volume) ? COL_GOOD : COL_PANEL);
    }
    if (!cfg.sound) { lcd.drawLine(22, 44, 58, 90, COL_BAD); lcd.drawLine(23, 44, 59, 90, COL_BAD); }
  } else {                                             
    printCenter(120, 44, 3, unitName(cfg.unit), zoneColor(), COL_UI_BG);
  }
}

#define BX 28
#define BY 34
#define BW 164
#define BH 64
static void drawBatteryInfo() {
  char b[8];
  lcd.fillRect(8, 104, 206, 46, COL_UI_BG);
  lcd.setTextSize(2); lcd.setTextColor(COL_TEXT, COL_UI_BG);
  lcd.setCursor(14, 106); lcd.print(F("Level: ")); lcd.print(battPct); lcd.print('%');
  dtostrf(battMv / 1000.0f, 1, 2, b);
  lcd.setTextSize(1);
  lcd.setCursor(14, 126); lcd.print(F("Current voltage: ")); lcd.print(b); lcd.print('V');
  lcd.setCursor(14, 138); lcd.print(F("Live remain: ~"));
  lcd.print((uint32_t)battPct * BAT_CAPACITY_MAH / 100 / BAT_CURRENT_MA); lcd.print('h');
}
static void drawBottle() {
  const int16_t ix = BX + 4, iy = BY + 4, iw = BW - 8, ih = BH - 8;
  int16_t fw = (int32_t)iw * battPct / 100;
  for (int16_t y = 0; y < ih; y++) {
    uint8_t p = (y * 5 + wavePhase) & 15;
    int16_t e = fw + (p < 8 ? p : 16 - p) - 4;
    e = constrain(e, 0, iw);
    if (e) lcd.drawFastHLine(ix, iy + y, e, battColor());
    if (e < iw) lcd.drawFastHLine(ix + e, iy + y, iw - e, COL_UI_BG);
  }
}
static void drawBottleFrame() {
  lcd.drawRoundRect(BX, BY, BW, BH, 12, COL_TEXT);
  lcd.drawRoundRect(BX + 1, BY + 1, BW - 2, BH - 2, 11, COL_TEXT);
  lcd.fillRoundRect(BX + BW, BY + 20, 12, 24, 4, COL_TEXT);
}

static void refreshBar() {
  uint8_t pg = screen - SCR_PAGE0;
  uint8_t m = memcmp(&cfg, &cfgBackup, sizeof cfg) ? BAR_APPLY : (rowAct(pg, selRow) ? BAR_ACT : BAR_VAL);
  if (m != barMode) { barMode = m; drawBtnBar(m); }
}

static void pageDraw(uint8_t pg) {
  lcd.fillScreen(COL_UI_BG);
  lcd.setTextSize(2); lcd.setTextColor(COL_TEXT, COL_UI_BG);
  lcd.setCursor(8, 8); lcd.print(F("Settings > ")); lcd.print(pgTitle(pg));
  if (pg == PG_PWR) { drawBatteryInfo(); drawBottleFrame(); drawBottle(); }
  if (pg == PG_ABOUT) {
    drawLogo(56, 28, 2, COL_TEXT);
    lcd.setTextSize(1); lcd.setTextColor(COL_TEXT, COL_UI_BG);
    lcd.setCursor(24, 162); lcd.print(F("Made by Zhuravlev Lab"));
    lcd.setCursor(24, 172); lcd.print(F("Firmware: ")); lcd.print(F(FW_VERSION));
    lcd.setCursor(24, 182); lcd.print(F("Github: ")); lcd.print(F(GITHUB_NAME));
  }
  drawHero(pg);
  for (uint8_t r = 0; r < pgN(pg); r++) drawRow(pg, r, r == selRow);
  drawSlider(pg);
  barMode = BAR_NONE; refreshBar();
}

static void adjustRow(uint8_t pg, uint8_t r, int8_t dir) {
  RowDef d; rowDef(pg, r, d);
  uint8_t &v = rowVal(d);
  v = (v + d.n + dir) % d.n;
  switch (d.off) {
    case OFS(brightness): backlightSet(); break;
    case OFS(volume):     volumeApply(); buzzer(true); clickOn = true; clickOff = millis() + VOLUME_TEST_MS; break;
    case OFS(ledMode):
    case OFS(ledBright):  ledTest(); break;
  }
}
static void reboot() {
  cli(); TIMSK1 = 0; asm volatile("jmp 0");
}
static void goMenu() { screen = SCR_MENU; menuDraw(); }
static void goMain() { screen = SCR_MAIN; dirty = DF_ALL; }
static void enterPage(uint8_t pg) {
  cfgBackup = cfg; screen = SCR_PAGE0 + pg; selRow = 0; abPos = 0; pageDraw(pg);
}
static void leavePage() {                              
  cfg = cfgBackup; backlightSet(); volumeApply();
  goMenu();
}
static void applyCfg() {
  saveCfg(); cfgBackup = cfg;
  barMode = BAR_SAVED; savedUntil = millis() + SAVED_MS; drawBtnBar(BAR_SAVED);
}
static void doAction(uint8_t pg, uint8_t r) {
  if (pg == PG_SND) { ringStart(millis()); return; }         
  if (r == 0) { setDefaults(); saveCfg(); cfgBackup = cfg; backlightSet(); volumeApply(); goMenu(); }
  else reboot();
}

static void pageEvent(uint8_t ev) {
  uint8_t pg = screen - SCR_PAGE0, n = pgN(pg), old = selRow;
  if (ev == EV_MID_LONG) { leavePage(); return; }
  bool act = rowAct(pg, selRow);
  if (ev == EV_MID) {
    if (barMode == BAR_APPLY) { applyCfg(); return; }
    if (act) { doAction(pg, selRow); return; }
    selRow = (selRow + 1) % n;
  } else if (ev == EV_LEFT || ev == EV_RIGHT) {
    int8_t d = ev == EV_RIGHT ? 1 : -1;
    if (act) selRow = (selRow + n + d) % n;
    else { adjustRow(pg, selRow, d); drawRow(pg, selRow, true); drawHero(pg); }
  } else return;
  if (old != selRow) { drawRow(pg, old, false); drawRow(pg, selRow, true); }
  drawSlider(pg);
  refreshBar();
}
static void onEvent(uint8_t ev) {
  if (screen == SCR_MAIN) {
    switch (ev) {
      case EV_LEFT:       if (!alertOn) sleepEnter(); break;                      
      case EV_LEFT_LONG:  cfg.sound ^= 1; saveCfg(); dirty |= DF_TOP; break;
      case EV_MID:        menuSel = 0; goMenu(); break;
      case EV_MID_LONG: {                                                        
        static uint8_t lastMode = DEF_LED_MODE ? DEF_LED_MODE : 2;
        if (cfg.ledMode) { lastMode = cfg.ledMode; cfg.ledMode = 0; }
        else { cfg.ledMode = lastMode; ledTest(); }
        saveCfg(); dirty |= DF_TOP; break; }
      case EV_RIGHT:      cfg.unit = (cfg.unit + 1) % 3; saveCfg(); dirty |= DF_VALUE | DF_STATS | DF_GRAPH; break;
      case EV_RIGHT_LONG: cfg.anim ^= 1; saveCfg(); dirty |= DF_RING; break;     
    }
  } else if (screen == SCR_MENU) {
    uint8_t old = menuSel;
    if (ev == EV_LEFT)  menuSel = (menuSel + PG_COUNT - 1) % PG_COUNT;
    if (ev == EV_RIGHT) menuSel = (menuSel + 1) % PG_COUNT;
    if (old != menuSel) { drawMenuRow(old, false); drawMenuRow(menuSel, true); drawScrollbar(); }
    if (ev == EV_MID) enterPage(menuSel);
    if (ev == EV_MID_LONG) goMain();
  } else pageEvent(ev);
}

static void secondTick() {
  noInterrupts(); uint16_t p = pulseCnt; pulseCnt = 0; interrupts();
  secBuf[secIdx] = p > 255 ? 255 : p;
  secIdx = (secIdx + 1) % GEIGER_WINDOW_S;
  if (secFilled < GEIGER_WINDOW_S) secFilled++;
  uint16_t sum = 0;
  for (uint8_t i = 0; i < secFilled; i++) sum += secBuf[i];
  uint8_t div = secFilled < GEIGER_MIN_S ? GEIGER_MIN_S : secFilled;
  cpm = sum * 60.0f / div;
  dose = cpm / GEIGER_CPM_PER_USVH;
  uint8_t oldZone = zone; updateZone();
  dirty |= DF_VALUE | DF_TOP;
  if (zone != oldZone) dirty |= DF_RING | DF_ICON;
  if (++graphTimer >= pgm_read_byte(&graphSec[cfg.graphStep])) {
    graphTimer = 0;
    float m = dose * 1000.0f;
    histPush(m > 65535.0f ? 65535 : (uint16_t)m);
    dirty |= DF_GRAPH | DF_STATS;
  }
}

void setup() {
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BTN_LEFT, INPUT_PULLUP); pinMode(PIN_BTN_MID, INPUT_PULLUP); pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);
  pinMode(PIN_GEIGER, GEIGER_PULLUP ? INPUT_PULLUP : INPUT);
  delay(30);
  uint8_t edge = GEIGER_EDGE_MODE == 1 ? FALLING : (GEIGER_EDGE_MODE == 2 ? RISING
                 : (digitalRead(PIN_GEIGER) ? FALLING : RISING));
  strip.begin(); strip.show();
  backlightInit();
  lcd.begin();
  lcd.fillScreen(COL_BG);
  loadCfg();
  analogReference(INTERNAL); analogRead(PIN_VBAT);
  batteryRead();
  backlightSet(); volumeApply();
  bootAnimation();
  attachInterrupt(digitalPinToInterrupt(PIN_GEIGER), geigerISR, edge);
  secT = battT = lastAct = millis();
  goMain();
}

void loop() {
  uint32_t now = millis();

  buzzerService(now);
  clickService(now);
  if (now - secT >= 1000) { secT += 1000; secondTick(); }
  if (now - battT >= BAT_PERIOD_MS) { battT = now; batteryRead(); dirty |= DF_TOP; battInfoDirty = true; }
  alertService(now);
  ledService(now);

  uint8_t ev = buttonsPoll(now);
  if (ev) {
    lastAct = now;
    if (sleeping) wakeUp();                          
    else onEvent(ev);
  }
  uint16_t sl = pgm_read_word(&sleepSec[cfg.sleep]);
  if (!sleeping && sl && screen == SCR_MAIN && now - lastAct > (uint32_t)sl * 1000UL && !alertOn) sleepEnter();
  if (sleeping && alertOn) wakeUp();
  if (sleeping) return;

  if (screen == SCR_MAIN) {
    mainRender();
    if (cfg.anim && now - snakeT >= SNAKE_STEP_MS) {
      snakeT = now; snakePos = (snakePos + 1) % RING_CELLS; snakeDraw(false);
    }
  } else if (screen == SCR_PAGE0 + PG_PWR) {
    if (battInfoDirty) { battInfoDirty = false; drawBatteryInfo(); }
    if (now - waveT >= WAVE_STEP_MS) { waveT = now; wavePhase = (wavePhase + 1) & 15; drawBottle(); }
  }
  if (screen >= SCR_PAGE0) {
    if (barMode == BAR_SAVED && due(now, savedUntil)) refreshBar();
    if (cfg.anim && (barMode == BAR_APPLY || screen == SCR_PAGE0 + PG_ABOUT) && now - bsT >= BTN_SNAKE_STEP_MS) {
      bsT = now; btnSnakeStep();
    }
  }
}
