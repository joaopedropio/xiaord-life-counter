// MTG Life Counter - XIAO nRF52840 + Seeed Round Display for XIAO
//
// Controls:
//   - Drag your finger around the outer rim: clockwise = +1, counter-clockwise = -1
//   - Tap right of the number: +1   | hold: +10 every 0.6 s
//   - Tap left of the number:  -1   | hold: -10 every 0.6 s
//   - Tap the very center: menu (Reset / Standard 20 / Commander 40 / Back)
//   - Life and game mode are saved to internal flash and restored on the next boot.
//
// Setup (Arduino IDE / arduino-cli):
//   1. Board package: "Seeed nRF52 Boards" (NOT the mbed-enabled one)
//   2. Libraries: Seeed_GFX and Seeed_Arduino_FS (both from GitHub: Seeed-Studio)
//   3. Keep driver.h in the same folder as this sketch
//   4. Round Display slide switch ON, small DIP switch ON (backlight + battery read)
//
// Rendering: everything is drawn into a 240x240 framebuffer in RAM (TFT_eSprite,
// 115 KB) and only the changed rectangle is sent to the GC9A01 with one EasyDMA
// SPI transfer per row at 32 MHz. Seeed_GFX's own nRF52840 path sends every byte
// as a separate SPI transaction, which is far too slow for smooth updates.

#include <Wire.h>
#include <SPI.h>
#include <math.h>
#include "driver.h"
#include <TFT_eSPI.h>

// ---------- Tuning ----------
const int   STEP_DEGREES   = 15;    // degrees of rim travel per 1 life (24 per full lap)
const int   RIM_INNER_R    = 80;    // touches farther than this from center count as rim
const int   CENTER_R       = 30;    // touches closer than this open the menu
const unsigned long HOLD_MS = 600;  // hold left/right this long for -10/+10 (repeats)
const unsigned long MENU_TAP_MAX_MS = 600; // a center touch longer than this does nothing
const int   MAX_JUMP_DEG   = 60;    // bigger angle jumps between two reads are ignored (glitch / lift-off)
const unsigned long FRAME_MS   = 16; // redraw at most ~60 times/s; touch is read every loop
const unsigned long RELEASE_MS = 80; // touch ends after this long with no valid read
const unsigned long SAVE_DELAY_MS = 3000; // save to flash this long after the last change
const bool  INVERT_DIRECTION = false; // set true if clockwise subtracts on your unit
const bool  SPI_32MHZ = true;         // set false if the screen shows glitches (falls back to 16 MHz)
#define DEBUG 1                       // serial log at 115200 (0 to disable)

#if DEBUG
  #define DBG(...) do { Serial.printf(__VA_ARGS__); Serial.println(); } while (0)
#else
  #define DBG(...) do {} while (0)
#endif

// ---------- Hardware ----------
#define TOUCH_INT   D7
#define TOUCH_ADDR  0x2E
#define BACKLIGHT   D6
#define BATTERY_PIN A0

const int W = 240, H = 240;
const int CX = 120, CY = 120;

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite fb  = TFT_eSprite(&tft);   // full-screen framebuffer
uint16_t   *fbPix = nullptr;           // its pixels, already byte-swapped for SPI

// ---------- State ----------
int startingLife = 20;
int life = 20;

// what the current touch started on
enum Zone { Z_NONE, Z_RIM, Z_CENTER, Z_PLUS, Z_MINUS, Z_MENU };
Zone zone = Z_NONE;

// touch tracking
bool  touching = false;
int   touchX = 0, touchY = 0;           // where the current touch started
float lastAngle = 0;
float angleAccum = 0;
unsigned long touchStart = 0;
unsigned long lastTouchReadMs = 0;
unsigned long lastHoldMs = 0;           // last +10/-10 while holding
bool  holdFired = false;

// change indicator ("+3" / "-2")
int  pendingDelta = 0;
unsigned long lastChangeMs = 0;

// what needs redrawing on the next frame
bool lifeDirty = false;
bool deltaDirty = false;
int  wantMarkerX = -1, wantMarkerY = -1;
int  markerX = -1, markerY = -1;

bool menuOpen = false;

unsigned long lastBatteryMs = 0;

// persistence (struct must be defined before any function: Arduino puts
// auto-generated prototypes above the first function)
struct SavedState {                      // one 12-byte record in the flash log
  uint32_t seq;                          // increases with every save; newest wins
  int16_t  life;
  int16_t  startingLife;
  uint32_t check;                        // checksum over the other fields
};
bool saveDirty = false;
unsigned long saveDirtySince = 0;

// ================= Fast display flush =================
// Dirty rectangle (inclusive) accumulated since the last flush.
int dx0 = W, dy0 = H, dx1 = -1, dy1 = -1;

void markDirty(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  dx0 = min(dx0, max(x, 0));
  dy0 = min(dy0, max(y, 0));
  dx1 = max(dx1, min(x + w - 1, W - 1));
  dy1 = max(dy1, min(y + h - 1, H - 1));
}

void flush() {
  if (dx1 < dx0 || dy1 < dy0) return;
  int w = dx1 - dx0 + 1;

  tft.startWrite();                                   // CS low + SPI transaction
  SPI.setClockDivider(SPI_32MHZ ? SPI_CLOCK_DIV2 : SPI_CLOCK_DIV4);
  tft.setWindow(dx0, dy0, dx1, dy1);                  // leaves DC in data mode
  if (w == W) {
    // full-width rows are contiguous: one DMA transfer (split in 64 KB chunks by the core)
    SPI.transfer(fbPix + dy0 * W, (void *)nullptr, (size_t)w * (dy1 - dy0 + 1) * 2);
  } else {
    for (int y = dy0; y <= dy1; y++)
      SPI.transfer(fbPix + y * W + dx0, (void *)nullptr, (size_t)w * 2);
  }
  tft.endWrite();

  dx0 = W; dy0 = H; dx1 = -1; dy1 = -1;
}

// ================= Touch =================
// NOTE: the CHSC6X INT line PULSES low once per new report; it does NOT stay low
// while the finger is down. So INT only means "a new report is ready", and the
// end of a touch is detected by RELEASE_MS without any valid report.
bool touchReady() {
  return digitalRead(TOUCH_INT) == LOW;
}

// The core's Wire.requestFrom() busy-waits on the TWIM events with NO timeout, so
// a single lost/stretched transfer from the touch chip froze the whole sketch.
// This reads the touch directly on TWIM0 (the peripheral Wire set up) with a
// timeout, and recovers the bus if it gets stuck.
NRF_TWIM_Type *const TWIM = NRF_TWIM0;
const uint32_t I2C_TIMEOUT_US = 5000;   // a 5-byte read at 100 kHz takes ~0.6 ms
uint32_t i2cErrors = 0, i2cTimeouts = 0;

// Free a stuck bus: clock SCL up to 9 times until the slave releases SDA,
// send a STOP, then let Wire reconfigure the peripheral.
void i2cRecover() {
  TWIM->TASKS_STOP = 1;
  TWIM->ENABLE = TWIM_ENABLE_ENABLE_Disabled << TWIM_ENABLE_ENABLE_Pos;
  // open-drain emulation: OUTPUT LOW = pull down, INPUT_PULLUP = release
  pinMode(PIN_WIRE_SDA, INPUT_PULLUP);
  for (int i = 0; i < 9 && digitalRead(PIN_WIRE_SDA) == LOW; i++) {
    pinMode(PIN_WIRE_SCL, OUTPUT); digitalWrite(PIN_WIRE_SCL, LOW);
    delayMicroseconds(5);
    pinMode(PIN_WIRE_SCL, INPUT_PULLUP);
    delayMicroseconds(5);
  }
  // STOP: SDA low -> high while SCL is high
  pinMode(PIN_WIRE_SDA, OUTPUT); digitalWrite(PIN_WIRE_SDA, LOW);
  delayMicroseconds(5);
  pinMode(PIN_WIRE_SCL, INPUT_PULLUP);
  delayMicroseconds(5);
  pinMode(PIN_WIRE_SDA, INPUT_PULLUP);
  delayMicroseconds(5);
  Wire.begin();
}

// Waits for an event with a timeout. Returns false on timeout.
bool twimWait(volatile uint32_t &event, uint32_t t0) {
  while (!event) {
    if (micros() - t0 > I2C_TIMEOUT_US) return false;
  }
  return true;
}

bool i2cRead(uint8_t addr, uint8_t *dst, uint8_t n) {
  static uint8_t rx[8];                 // EasyDMA buffer must be in RAM
  if (n > sizeof(rx)) return false;

  TWIM->EVENTS_STOPPED = 0;
  TWIM->EVENTS_ERROR = 0;
  TWIM->EVENTS_LASTRX = 0;
  TWIM->ERRORSRC = TWIM->ERRORSRC;      // write-1-to-clear
  TWIM->ADDRESS = addr;
  TWIM->RXD.PTR = (uint32_t)rx;
  TWIM->RXD.MAXCNT = n;
  TWIM->SHORTS = TWIM_SHORTS_LASTRX_STOP_Msk; // send STOP automatically after the last byte
  TWIM->TASKS_RESUME = 1;
  TWIM->TASKS_STARTRX = 1;

  uint32_t t0 = micros();
  bool ok = true;
  while (!TWIM->EVENTS_STOPPED) {
    if (TWIM->EVENTS_ERROR) {           // NACK / overrun: stop and bail out
      TWIM->TASKS_STOP = 1;
      if (!twimWait(TWIM->EVENTS_STOPPED, micros())) { i2cTimeouts++; i2cRecover(); }
      i2cErrors++;
      ok = false;
      break;
    }
    if (micros() - t0 > I2C_TIMEOUT_US) {
      i2cTimeouts++;
      DBG("i2c timeout -> bus recovery");
      i2cRecover();
      ok = false;
      break;
    }
  }
  TWIM->SHORTS = 0;
  TWIM->EVENTS_STOPPED = 0;
  TWIM->EVENTS_ERROR = 0;
  if (!ok || TWIM->RXD.AMOUNT != n) return false;
  memcpy(dst, rx, n);
  return true;
}

bool readTouch(int &x, int &y) {
  uint8_t buf[5];
  if (!i2cRead(TOUCH_ADDR, buf, 5)) return false;
  if (buf[0] != 0x01) return false;
  x = buf[2];
  y = buf[4];
  return true;
}

// ================= Persistence (raw internal flash, append-only log) =================
// Two 4 KB flash pages hold a log of 12-byte records. A save appends one record
// to the active page (3 word writes, well under 1 ms, no erase). Only when that
// page is full is the OTHER page erased (~85 ms) and used next; the full page
// stays intact until then, so a power cut at any moment leaves the last good
// record readable. On boot the valid record with the highest seq wins.
//
// The pages are the last two of the area the linker reserves for InternalFS
// (0xED000-0xF4000, below the bootloader at 0xF4000). Nothing else in this
// sketch uses InternalFS, and firmware uploads never touch that area.
// Writes go straight to the NVMC; that is allowed because the SoftDevice (BLE)
// is not enabled by this sketch. If BLE is added later, this must use sd_flash_*.
const uint32_t PAGE_SIZE = 4096;
const uint32_t STATE_PAGES[2] = { 0xF2000, 0xF3000 };
const int SLOTS_PER_PAGE = PAGE_SIZE / sizeof(SavedState);   // 341

int      activePage = -1;   // page holding the newest record (-1: none yet)
int      nextSlot = 0;      // where the next record goes in activePage
bool     activeAppendable = false;
uint32_t lastSeq = 0;

uint32_t stateCheck(const SavedState &s) {
  uint32_t h = 0x4D544731;                        // "MTG1"
  h = (h ^ s.seq) * 0x9E3779B1u;
  h = (h ^ (uint16_t)s.life) * 0x9E3779B1u;
  h = (h ^ (uint16_t)s.startingLife) * 0x9E3779B1u;
  return h ^ (h >> 15);
}

const SavedState *slotPtr(int page, int slot) {
  return (const SavedState *)(STATE_PAGES[page] + slot * sizeof(SavedState));
}

bool slotErased(const SavedState *r) {
  const uint32_t *w = (const uint32_t *)r;
  return w[0] == 0xFFFFFFFF && w[1] == 0xFFFFFFFF && w[2] == 0xFFFFFFFF;
}

bool slotValid(const SavedState *r) {
  return r->seq != 0xFFFFFFFF && r->check == stateCheck(*r) &&
         (r->startingLife == 20 || r->startingLife == 40) &&
         r->life >= -999 && r->life <= 999;
}

void nvmcWait() { while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {} }

void flashErasePage(uint32_t addr) {
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos;
  nvmcWait();
  NRF_NVMC->ERASEPAGE = addr;
  nvmcWait();
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
  nvmcWait();
}

void flashWriteWords(uint32_t addr, const uint32_t *src, int n) {
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
  nvmcWait();
  for (int i = 0; i < n; i++) {
    ((volatile uint32_t *)addr)[i] = src[i];
    nvmcWait();
  }
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
  nvmcWait();
}

void loadState() {
  const SavedState *best = nullptr;
  for (int p = 0; p < 2; p++) {
    // valid records are written contiguously from slot 0
    int i = 0;
    const SavedState *newest = nullptr;
    while (i < SLOTS_PER_PAGE && slotValid(slotPtr(p, i))) { newest = slotPtr(p, i); i++; }
    if (newest && (!best || newest->seq > best->seq)) {
      best = newest;
      activePage = p;
      nextSlot = i;
      // appendable only if the slot after the last record is still blank
      activeAppendable = (i < SLOTS_PER_PAGE) && slotErased(slotPtr(p, i));
    }
  }
  if (!best) { DBG("no saved state"); return; }
  lastSeq = best->seq;
  life = best->life;
  startingLife = best->startingLife;
  DBG("loaded life=%d start=%d (page %d slot %d seq %lu)",
      life, startingLife, activePage, nextSlot - 1, lastSeq);
}

void saveState() {
  saveDirty = false;
  SavedState s = { lastSeq + 1, (int16_t)life, (int16_t)startingLife, 0 };
  s.check = stateCheck(s);
  unsigned long t0 = micros();

  if (activePage < 0 || !activeAppendable || nextSlot >= SLOTS_PER_PAGE) {
    // move to the other page (or page 0 the first time): erase it, start at slot 0
    activePage = (activePage < 0) ? 0 : 1 - activePage;
    flashErasePage(STATE_PAGES[activePage]);
    nextSlot = 0;
    activeAppendable = true;
  }
  uint32_t addr = (uint32_t)slotPtr(activePage, nextSlot);
  flashWriteWords(addr, (const uint32_t *)&s, sizeof(s) / 4);

  if (!slotValid(slotPtr(activePage, nextSlot))) {   // verify; on failure retry on the other page next time
    DBG("save verify FAILED at 0x%lx", addr);
    activeAppendable = false;
    saveDirty = true;
    saveDirtySince = millis();
    return;
  }
  lastSeq = s.seq;
  nextSlot++;
  DBG("saved life=%d start=%d (page %d slot %d seq %lu) in %lu us",
      life, startingLife, activePage, nextSlot - 1, lastSeq, micros() - t0);
}

void requestSave() {
  saveDirty = true;
  saveDirtySince = millis();
}

// ================= Drawing (into the framebuffer) =================
void drawRing() {
  // thin guide ring showing where the slider lives
  for (int r = 108; r <= 110; r++) fb.drawCircle(CX, CY, r, 0x39E7); // dark grey
}

// The dot (r 90..106 from center) never touches the ring (r 108..110).
void drawMarker() {
  if (wantMarkerX == markerX && wantMarkerY == markerY) return;
  if (markerX >= 0) {
    fb.fillCircle(markerX, markerY, 9, TFT_BLACK);
    markDirty(markerX - 9, markerY - 9, 19, 19);
  }
  if (wantMarkerX >= 0) {
    fb.fillCircle(wantMarkerX, wantMarkerY, 8, TFT_ORANGE);
    markDirty(wantMarkerX - 9, wantMarkerY - 9, 19, 19);
  }
  markerX = wantMarkerX;
  markerY = wantMarkerY;
}

uint16_t lifeColor(int v) {
  if (v <= 0)  return TFT_RED;
  if (v <= 5)  return TFT_ORANGE;
  if (v <= 10) return TFT_YELLOW;
  return TFT_WHITE;
}

const int LIFE_W = 176, LIFE_H = 80;   // area cleared behind the big number

void drawLife() {
  fb.fillRect(CX - LIFE_W / 2, CY - LIFE_H / 2, LIFE_W, LIFE_H, TFT_BLACK);
  fb.setTextDatum(MC_DATUM);
  fb.setTextPadding(0);
  fb.setTextColor(lifeColor(life), TFT_BLACK);
  fb.drawString(String(life), CX, CY, 8);            // font 8 = large 75px digits
  markDirty(CX - LIFE_W / 2, CY - LIFE_H / 2, LIFE_W, LIFE_H);
}

const int DELTA_Y = CY - 62, DELTA_W = 90, DELTA_H = 30;

void drawDelta() {
  fb.fillRect(CX - DELTA_W / 2, DELTA_Y - DELTA_H / 2, DELTA_W, DELTA_H, TFT_BLACK);
  if (pendingDelta != 0) {
    String s = (pendingDelta > 0 ? "+" : "") + String(pendingDelta);
    fb.setTextDatum(MC_DATUM);
    fb.setTextPadding(0);
    fb.setTextColor(pendingDelta > 0 ? TFT_GREEN : TFT_RED, TFT_BLACK);
    fb.drawString(s, CX, DELTA_Y, 4);
  }
  markDirty(CX - DELTA_W / 2, DELTA_Y - DELTA_H / 2, DELTA_W, DELTA_H);
}

void drawStartLabel() {
  fb.fillRect(CX - 50, CY + 47, 100, 18, TFT_BLACK);
  fb.setTextDatum(MC_DATUM);
  fb.setTextPadding(0);
  fb.setTextColor(0x7BEF, TFT_BLACK); // grey
  fb.drawString(startingLife == 40 ? "Commander" : "Standard", CX, CY + 55, 2);
  markDirty(CX - 50, CY + 47, 100, 18);
}

// Approximate reading; battery is behind a 1/2 divider on A0.
int batteryPercent() {
  analogReference(AR_INTERNAL_2_4);
  analogReadResolution(12);
  long raw = 0;
  for (int i = 0; i < 16; i++) raw += analogRead(BATTERY_PIN);
  raw /= 16;
  float volts = raw * 2.4f / 4095.0f * 2.0f;
  int pct = (int)((volts - 3.3f) / (4.2f - 3.3f) * 100.0f);
  return constrain(pct, 0, 100);
}

void drawBattery() {
  if (menuOpen) return;                 // menu covers it; redrawn when the menu closes
  int pct = batteryPercent();
  fb.fillRect(CX - 25, CY + 72, 50, 18, TFT_BLACK);
  fb.setTextDatum(MC_DATUM);
  fb.setTextPadding(0);
  fb.setTextColor(pct < 20 ? TFT_RED : 0x7BEF, TFT_BLACK);
  fb.drawString(String(pct) + "%", CX, CY + 80, 2);
  markDirty(CX - 25, CY + 72, 50, 18);
}

void fullRedraw() {
  fb.fillSprite(TFT_BLACK);
  markerX = markerY = -1;
  drawRing();
  drawLife();
  drawDelta();
  drawStartLabel();
  drawBattery();
  drawMarker();
  markDirty(0, 0, W, H);
  lifeDirty = deltaDirty = false;
  flush();
}

// ================= Menu =================
enum MenuItem { M_RESET, M_STANDARD, M_COMMANDER, M_BACK, M_COUNT };
const char *MENU_LABELS[M_COUNT] = { "Reiniciar", "Standard 20", "Commander 40", "Voltar" };
const int MENU_W = 150, MENU_H = 34, MENU_GAP = 6;
const int MENU_TOP = CY - (M_COUNT * MENU_H + (M_COUNT - 1) * MENU_GAP) / 2;

int menuItemY(int i) { return MENU_TOP + i * (MENU_H + MENU_GAP); }

void drawMenu() {
  fb.fillSprite(TFT_BLACK);
  markerX = markerY = -1;
  fb.setTextDatum(MC_DATUM);
  fb.setTextPadding(0);
  for (int i = 0; i < M_COUNT; i++) {
    int y = menuItemY(i);
    bool current = (i == M_STANDARD && startingLife == 20) || (i == M_COMMANDER && startingLife == 40);
    uint16_t border = (i == M_RESET) ? TFT_ORANGE : (current ? TFT_GREEN : 0x7BEF);
    fb.fillRoundRect(CX - MENU_W / 2, y, MENU_W, MENU_H, 10, 0x18E3); // very dark grey
    fb.drawRoundRect(CX - MENU_W / 2, y, MENU_W, MENU_H, 10, border);
    fb.setTextColor(TFT_WHITE, 0x18E3);
    fb.drawString(MENU_LABELS[i], CX, y + MENU_H / 2, 2);
  }
  markDirty(0, 0, W, H);
  flush();
}

int menuItemAt(int x, int y) {
  if (abs(x - CX) > MENU_W / 2) return -1;
  for (int i = 0; i < M_COUNT; i++) {
    int top = menuItemY(i);
    if (y >= top - MENU_GAP / 2 && y < top + MENU_H + MENU_GAP / 2) return i;
  }
  return -1;
}

void openMenu() {
  DBG("menu open");
  menuOpen = true;
  wantMarkerX = wantMarkerY = -1;
  drawMenu();
}

void closeMenu() {
  menuOpen = false;
  fullRedraw();
}

void resetGame(int start) {
  startingLife = start;
  life = start;
  pendingDelta = 0;
  requestSave();
}

void onMenuTap(int x, int y) {
  int item = menuItemAt(x, y);
  DBG("menu tap item=%d", item);
  switch (item) {
    case M_RESET:     resetGame(startingLife); break;
    case M_STANDARD:  resetGame(20); break;
    case M_COMMANDER: resetGame(40); break;
    case M_BACK:      break;
    default:          return;           // tapped outside the buttons: keep the menu open
  }
  closeMenu();
}

// ================= Logic =================
void changeLife(int d) {
  life += d;
  pendingDelta += d;
  lastChangeMs = millis();
  lifeDirty = deltaDirty = true;
  requestSave();
}

float angleOf(int x, int y) {
  return atan2f((float)(y - CY), (float)(x - CX)) * 180.0f / PI; // clockwise positive
}

int distOf(int x, int y) {
  int ddx = x - CX, ddy = y - CY;
  return (int)sqrtf((float)(ddx * ddx + ddy * ddy));
}

Zone zoneOf(int x, int y) {
  if (menuOpen) return Z_MENU;
  int d = distOf(x, y);
  if (d >= RIM_INNER_R) return Z_RIM;
  if (d < CENTER_R)     return Z_CENTER;
  return (x >= CX) ? Z_PLUS : Z_MINUS;
}

void onTouchStart(int x, int y) {
  touching = true;
  touchX = x; touchY = y;
  touchStart = millis();
  lastHoldMs = touchStart;
  holdFired = false;
  angleAccum = 0;
  zone = zoneOf(x, y);
  lastAngle = angleOf(x, y);
  DBG("touch start x=%d y=%d dist=%d zone=%d", x, y, distOf(x, y), zone);
}

void onTouchMove(int x, int y) {
  if (zone != Z_RIM) return;
  float a = angleOf(x, y);
  float d = a - lastAngle;
  if (d > 180)  d -= 360;
  if (d < -180) d += 360;
  lastAngle = a;
  if (fabsf(d) > MAX_JUMP_DEG) {      // glitch or finger lifted/replaced: re-anchor, don't count
    DBG("jump ignored %.0f deg", d);
    return;
  }
  if (INVERT_DIRECTION) d = -d;
  angleAccum += d;

  while (angleAccum >= STEP_DEGREES)  { changeLife(+1); angleAccum -= STEP_DEGREES; }
  while (angleAccum <= -STEP_DEGREES) { changeLife(-1); angleAccum += STEP_DEGREES; }

  // marker dot snapped onto the ring
  float rad = a * PI / 180.0f;
  wantMarkerX = CX + (int)(98 * cosf(rad));
  wantMarkerY = CY + (int)(98 * sinf(rad));
}

// Called every loop while touching, even with no new report (finger held still).
void checkHold() {
  if (!touching || (zone != Z_PLUS && zone != Z_MINUS)) return;
  if (millis() - lastHoldMs >= HOLD_MS) {
    lastHoldMs = millis();
    holdFired = true;
    changeLife(zone == Z_PLUS ? +10 : -10);
  }
}

void onTouchEnd() {
  unsigned long held = millis() - touchStart;
  DBG("touch end zone=%d held=%lu life=%d", zone, held, life);
  touching = false;
  wantMarkerX = wantMarkerY = -1;
  switch (zone) {
    case Z_PLUS:   if (!holdFired) changeLife(+1); break;
    case Z_MINUS:  if (!holdFired) changeLife(-1); break;
    case Z_CENTER: if (held < MENU_TAP_MAX_MS) openMenu(); break;
    case Z_MENU:   onMenuTap(touchX, touchY); break;
    default: break;
  }
  zone = Z_NONE;
}

// ================= Arduino =================
void setup() {
#if DEBUG
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10); // wait up to 3 s for the monitor
#endif
  DBG("boot");
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(BACKLIGHT, OUTPUT);
  digitalWrite(BACKLIGHT, HIGH);
  Wire.begin();

  loadState();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  fb.setColorDepth(16);
  fbPix = (uint16_t *)fb.createSprite(W, H);
  if (!fbPix) {
    DBG("framebuffer alloc FAILED");
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("no RAM for framebuffer", 20, 110, 2);
    while (true) delay(1000);
  }

  unsigned long t1 = micros();
  fullRedraw();
  DBG("full frame in %lu us", micros() - t1);
}

void loop() {
#if DEBUG
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat > 2000) {
    lastBeat = millis();
    DBG("alive t=%lu life=%d touching=%d i2c err=%lu timeouts=%lu",
        millis(), life, touching, i2cErrors, i2cTimeouts);
  }
#endif
  int x, y;
  if (touchReady() && readTouch(x, y)) {
    lastTouchReadMs = millis();
    if (!touching) onTouchStart(x, y);
    else           onTouchMove(x, y);
  } else if (touching && millis() - lastTouchReadMs > RELEASE_MS) {
    onTouchEnd();
  }
  checkHold();

  // clear the "+3" indicator 2 s after the last change
  if (pendingDelta != 0 && millis() - lastChangeMs > 2000) {
    pendingDelta = 0;
    deltaDirty = true;
  }

  // refresh battery every 30 s
  if (millis() - lastBatteryMs > 30000) {
    lastBatteryMs = millis();
    drawBattery();
  }

  // save to flash once things have been quiet for a moment (and not mid-touch)
  if (saveDirty && !touching && millis() - saveDirtySince > SAVE_DELAY_MS) saveState();

  // one frame: draw what changed into the framebuffer, then push the dirty rect
  static unsigned long lastFrame = 0;
  if (!menuOpen && millis() - lastFrame >= FRAME_MS) {
    lastFrame = millis();
    if (lifeDirty)  { lifeDirty = false;  drawLife(); }
    if (deltaDirty) { deltaDirty = false; drawDelta(); }
    drawMarker();
    flush();
  }

  delay(2);
}
