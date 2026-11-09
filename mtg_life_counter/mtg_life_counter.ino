// MTG Life Counter - XIAO nRF52840 + Seeed Round Display for XIAO
//
// Controls:
//   - Drag your finger around the outer rim: clockwise = +1, counter-clockwise = -1
//   - Tap right of the number: +1   | hold: +10 every 0.6 s
//   - Tap left of the number:  -1   | hold: -10 every 0.6 s
//   - Tap the very center: menu (Reset / 20 / 40 / Dice / Back)
//   - Dice: pick d4..d20, it rolls and shows the result until you tap the screen
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

const uint16_t GREY = 0x7BEF;
const int W = 240, H = 240;
const int CX = 120, CY = 120;

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite fb  = TFT_eSprite(&tft);   // full-screen framebuffer
uint16_t   *fbPix = nullptr;           // its pixels, already byte-swapped for SPI

// ---------- State ----------
int startingLife = 20;
int life = 20;
int brightness = 100;                   // backlight %, BRIGHT_MIN..100
const int BRIGHT_MIN = 1;
const int BRIGHT_STEP_DEG = 5;          // rim travel per 1% brightness (72% per lap)
bool brightDirty = false;

// screen auto-off: backlight off + panel asleep after this long without a touch
const int SLEEP_SECS[] = { 0, 5, 10, 15, 30, 60, 120, 300 };     // 0 = never
const char *SLEEP_LABELS[] = { "nunca", "5 s", "10 s", "15 s", "30 s", "1 min", "2 min", "5 min" };
const int SLEEP_OPTIONS = sizeof(SLEEP_SECS) / sizeof(SLEEP_SECS[0]);
int  sleepIdx = 4;                      // default 30 s
const int MENU_MIN_SLEEP_S = 15;        // floor for the auto-off outside the life screen
bool asleep = false;
unsigned long lastActivityMs = 0;

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

// which screen is showing; everything but S_LIFE handles taps on touch end
enum Screen { S_LIFE, S_MENU, S_DICE_PICK, S_DICE_RESULT, S_BRIGHT };
Screen screen = S_LIFE;

unsigned long lastBatteryMs = 0;

// persistence (struct must be defined before any function: Arduino puts
// auto-generated prototypes above the first function)
struct SavedState {                      // one 16-byte record in the flash log
  uint32_t seq;                          // increases with every save; newest wins
  int16_t  life;
  uint8_t  startingLife;                 // 20 or 40
  uint8_t  brightness;                   // backlight %, 1..100
  uint8_t  sleep5s;                      // auto-off time / 5 s (0 = never)
  uint8_t  reserved[3];                  // 0xFF; room for future settings
  uint32_t check;                        // checksum over the other fields
};
static_assert(sizeof(SavedState) == 16, "SavedState must be 16 bytes (4-byte flash words)");
bool saveDirty = false;
// menu buttons: rectangles used both for drawing and for hit-testing
struct Btn { int x, y, w, h; const char *label; };
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
// Two 4 KB flash pages hold a log of 16-byte records. A save appends one record
// to the active page (4 word writes, well under 1 ms, no erase). Only when that
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
const int SLOTS_PER_PAGE = PAGE_SIZE / sizeof(SavedState);   // 256

int      activePage = -1;   // page holding the newest record (-1: none yet)
int      nextSlot = 0;      // where the next record goes in activePage
bool     activeAppendable = false;
uint32_t lastSeq = 0;

// Hashes every word before `check`. The seed changed from "MTG1" when the record
// grew from 12 to 16 bytes, so old records never pass as new ones.
uint32_t stateCheck(const SavedState &s) {
  uint32_t h = 0x4D544732;                        // "MTG2"
  const uint32_t *w = (const uint32_t *)&s;
  for (unsigned i = 0; i < sizeof(SavedState) / 4 - 1; i++) h = (h ^ w[i]) * 0x9E3779B1u;
  return h ^ (h >> 15);
}

// position of `secs` in SLEEP_SECS, or -1
int sleepIndexOf(int secs) {
  for (int i = 0; i < SLEEP_OPTIONS; i++) if (SLEEP_SECS[i] == secs) return i;
  return -1;
}

const SavedState *slotPtr(int page, int slot) {
  return (const SavedState *)(STATE_PAGES[page] + slot * sizeof(SavedState));
}

bool slotErased(const SavedState *r) {
  const uint32_t *w = (const uint32_t *)r;
  for (unsigned i = 0; i < sizeof(SavedState) / 4; i++) if (w[i] != 0xFFFFFFFF) return false;
  return true;
}

bool slotValid(const SavedState *r) {
  return r->seq != 0xFFFFFFFF && r->check == stateCheck(*r) &&
         (r->startingLife == 20 || r->startingLife == 40) &&
         r->brightness >= BRIGHT_MIN && r->brightness <= 100 && sleepIndexOf(r->sleep5s * 5) >= 0 &&
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
  brightness = best->brightness;
  sleepIdx = sleepIndexOf(best->sleep5s * 5);
  DBG("loaded life=%d start=%d (page %d slot %d seq %lu)",
      life, startingLife, activePage, nextSlot - 1, lastSeq);
}

void saveState() {
  saveDirty = false;
  SavedState s = { lastSeq + 1, (int16_t)life, (uint8_t)startingLife, (uint8_t)brightness,
                   (uint8_t)(SLEEP_SECS[sleepIdx] / 5), { 0xFF, 0xFF, 0xFF }, 0 };
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
  DBG("saved life=%d start=%d bright=%d sleep=%ds (page %d slot %d seq %lu) in %lu us",
      life, startingLife, brightness, SLEEP_SECS[sleepIdx], activePage, nextSlot - 1, lastSeq, micros() - t0);
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

// Approximate reading; battery is behind a 1/2 divider on A0.
int batteryPercent() {
  analogReference(AR_INTERNAL_2_4);
  analogReadResolution(12);
  long raw = 0;
  for (int i = 0; i < 16; i++) raw += analogRead(BATTERY_PIN);
  raw /= 16;
  float volts = raw * 2.4f / 4095.0f * 2.0f;
  DBG("battery raw=%ld mV=%d vbus=%d", raw, (int)(volts * 1000),
      (int)((NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0));
  int pct = (int)((volts - 3.3f) / (4.2f - 3.3f) * 100.0f);
  return constrain(pct, 0, 100);
}

// USB power present. The Round Display's charger has no status line to the MCU,
// so "USB connected" is the best "charging" signal available.
bool usbPowered() {
  return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}
bool shownUsb = false;

// small lightning bolt centered on (bx, by), 14 px tall
void drawBolt(int bx, int by, uint16_t color) {
  fb.fillTriangle(bx + 3, by - 7, bx - 4, by + 1, bx + 1, by + 1, color);
  fb.fillTriangle(bx - 1, by - 1, bx + 4, by - 1, bx - 3, by + 7, color);
}

void drawBattery() {
  if (screen != S_LIFE) return;         // menu covers it; redrawn when the menu closes
  int pct = batteryPercent();
  shownUsb = usbPowered();
  fb.fillRect(CX - 40, CY + 72, 80, 18, TFT_BLACK);
  fb.setTextDatum(MC_DATUM);
  fb.setTextPadding(0);
  fb.setTextColor(pct < 20 && !shownUsb ? TFT_RED : GREY, TFT_BLACK);
  String txt = String(pct) + "%";
  int tx = shownUsb ? CX + 8 : CX;      // make room for the bolt
  fb.drawString(txt, tx, CY + 80, 2);
  if (shownUsb) drawBolt(tx - fb.textWidth(txt, 2) / 2 - 9, CY + 80, TFT_YELLOW);
  markDirty(CX - 40, CY + 72, 80, 18);
}

void fullRedraw() {
  fb.fillSprite(TFT_BLACK);
  markerX = markerY = -1;
  drawRing();
  drawLife();
  drawDelta();
  drawBattery();
  drawMarker();
  markDirty(0, 0, W, H);
  lifeDirty = deltaDirty = false;
  flush();
}

// ================= Menu & dice =================
const uint16_t BTN_FILL = 0x18E3;       // very dark grey

void drawButton(const Btn &b, uint16_t border) {
  fb.fillRoundRect(b.x, b.y, b.w, b.h, 10, BTN_FILL);
  fb.drawRoundRect(b.x, b.y, b.w, b.h, 10, border);
  fb.setTextDatum(MC_DATUM);
  fb.setTextPadding(0);
  fb.setTextColor(TFT_WHITE, BTN_FILL);
  fb.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2 + 1, 4);   // font 4 = 26 px
}

// index of the button under (x, y), or -1
int buttonAt(const Btn *btns, int n, int x, int y) {
  for (int i = 0; i < n; i++) {
    const Btn &b = btns[i];
    if (x >= b.x - 3 && x < b.x + b.w + 3 && y >= b.y - 3 && y < b.y + b.h + 3) return i;
  }
  return -1;
}

// Main menu: 4 rows of 40 px (+8 px gaps) centered; row 2 holds "20" and "40".
enum MenuItem { M_RESET, M_20, M_40, M_DICE, M_LIGHT, M_BACK, M_COUNT };
const Btn MENU_BTNS[M_COUNT] = {
  { 50,  28, 140, 40, "Reiniciar" },
  { 50,  76,  66, 40, "20" },
  { 124, 76,  66, 40, "40" },
  { 50, 124,  66, 40, "Dado" },
  { 124, 124, 66, 40, "Luz" },
  { 50, 172, 140, 40, "Voltar" },
};

// Dice picker: 3x2 grid of dice + "Voltar".
const int DICE_COUNT = 6;
const int DICE_SIDES[DICE_COUNT] = { 4, 6, 8, 10, 12, 20 };
const Btn DICE_BTNS[DICE_COUNT + 1] = {
  { 28,  48, 56, 48, "d4" },  { 92,  48, 56, 48, "d6" },  { 156,  48, 56, 48, "d8" },
  { 28, 104, 56, 48, "d10" }, { 92, 104, 56, 48, "d12" }, { 156, 104, 56, 48, "d20" },
  { 60, 164, 120, 40, "Voltar" },
};

void showScreen(Screen s) {
  screen = s;
  wantMarkerX = wantMarkerY = -1;
  fb.fillSprite(TFT_BLACK);
  markerX = markerY = -1;
}

void drawMenu() {
  showScreen(S_MENU);
  for (int i = 0; i < M_COUNT; i++) {
    bool current = (i == M_20 && startingLife == 20) || (i == M_40 && startingLife == 40);
    uint16_t border = (i == M_RESET) ? TFT_ORANGE : (current ? TFT_GREEN : GREY);
    drawButton(MENU_BTNS[i], border);
  }
  markDirty(0, 0, W, H);
  flush();
}

void drawDicePicker() {
  showScreen(S_DICE_PICK);
  for (int i = 0; i <= DICE_COUNT; i++) drawButton(DICE_BTNS[i], GREY);
  markDirty(0, 0, W, H);
  flush();
}

// Big result: font 7 (7-segment, 48 px) scaled x2 = 96 px digits.
const int DICE_NUM_Y = CY + 8, DICE_NUM_W = 200, DICE_NUM_H = 110;

void drawDiceNumber(int v, uint16_t color) {
  fb.fillRect(CX - DICE_NUM_W / 2, DICE_NUM_Y - DICE_NUM_H / 2, DICE_NUM_W, DICE_NUM_H, TFT_BLACK);
  fb.setTextDatum(MC_DATUM);
  fb.setTextPadding(0);
  fb.setTextSize(2);
  fb.setTextColor(color, TFT_BLACK);
  fb.drawString(String(v), CX, DICE_NUM_Y, 7);
  fb.setTextSize(1);
  markDirty(CX - DICE_NUM_W / 2, DICE_NUM_Y - DICE_NUM_H / 2, DICE_NUM_W, DICE_NUM_H);
  flush();
}

void rollDice(int sides) {
  showScreen(S_DICE_RESULT);
  fb.setTextDatum(MC_DATUM);
  fb.setTextPadding(0);
  fb.setTextColor(GREY, TFT_BLACK);
  fb.drawString("d" + String(sides), CX, 40, 4);
  fb.drawString("toque para sair", CX, 205, 2);
  markDirty(0, 0, W, H);
  flush();

  // short shuffle animation, then the real roll
  for (int i = 0; i < 8; i++) {
    drawDiceNumber(random(1, sides + 1), GREY);
    delay(50);
  }
  int v = random(1, sides + 1);
  uint16_t color = (v == sides) ? TFT_GREEN : (v == 1 ? TFT_RED : TFT_WHITE);
  drawDiceNumber(v, color);
  DBG("dice d%d -> %d", sides, v);
}

// Backlight PWM on D6 (active high).
void applyBrightness() {
  analogWrite(BACKLIGHT, brightness * 255 / 100);
}

// Brightness screen: works like the life screen. "-" / "+" tap = 1%, hold = 10%
// repeating; dragging around the rim = 1% per BRIGHT_STEP_DEG. The "apagar tela"
// button cycles the auto-off time. OK saves and goes back.
enum { B_MINUS, B_PLUS, B_SLEEP, B_OK, B_COUNT };
const Btn BRIGHT_BTNS[B_COUNT] = {
  { 34,  62, 52, 50, "-" },
  { 154, 62, 52, 50, "+" },
  { 65, 142, 110, 36, nullptr },        // label: SLEEP_LABELS[sleepIdx]
  { 80, 186, 80, 32, "OK" },
};

void drawBrightnessValue() {
  fb.fillRect(CX - 31, 72, 62, 30, TFT_BLACK);
  fb.setTextDatum(MC_DATUM);
  fb.setTextPadding(0);
  fb.setTextColor(TFT_WHITE, TFT_BLACK);
  fb.drawString(String(brightness) + "%", CX, 88, 4);
  markDirty(CX - 31, 72, 62, 30);
}

void drawSleepButton() {
  Btn b = BRIGHT_BTNS[B_SLEEP];
  b.label = SLEEP_LABELS[sleepIdx];
  drawButton(b, GREY);
  markDirty(b.x, b.y, b.w, b.h);
}

void drawBrightness() {
  showScreen(S_BRIGHT);
  drawRing();
  fb.setTextDatum(MC_DATUM);
  fb.setTextPadding(0);
  fb.setTextColor(GREY, TFT_BLACK);
  fb.drawString("Brilho", CX, 40, 4);
  fb.drawString("apagar tela", CX, 130, 2);
  drawButton(BRIGHT_BTNS[B_MINUS], GREY);
  drawButton(BRIGHT_BTNS[B_PLUS], GREY);
  drawButton(BRIGHT_BTNS[B_OK], TFT_GREEN);
  drawSleepButton();
  drawBrightnessValue();
  markDirty(0, 0, W, H);
  flush();
}

void changeBrightness(int d) {
  int b = constrain(brightness + d, BRIGHT_MIN, 100);
  if (b == brightness) return;
  brightness = b;
  applyBrightness();
  brightDirty = true;
}

void onBrightnessTap(int x, int y) {
  int i = buttonAt(BRIGHT_BTNS, B_COUNT, x, y);
  if (i == B_SLEEP) {
    sleepIdx = (sleepIdx + 1) % SLEEP_OPTIONS;
    DBG("auto-off %d s", SLEEP_SECS[sleepIdx]);
    drawSleepButton();
    flush();
  } else if (i == B_OK) {
    DBG("brightness %d, auto-off %d s saved", brightness, SLEEP_SECS[sleepIdx]);
    requestSave();
    closeMenu();
  }
}

// ---- screen auto-off ----
// GC9A01: 0x28 display off, 0x10 sleep in / 0x11 sleep out, 0x29 display on.
// The panel keeps its RAM while asleep; we still repaint everything on wake.
void screenSleep() {
  DBG("screen off");
  asleep = true;
  wantMarkerX = wantMarkerY = -1;
  analogWrite(BACKLIGHT, 0);
  tft.writecommand(0x28);
  tft.writecommand(0x10);
}

void screenWake() {
  DBG("screen on");
  asleep = false;
  tft.writecommand(0x11);
  delay(120);                           // sleep-out settle time (datasheet)
  tft.writecommand(0x29);
  markDirty(0, 0, W, H);
  flush();
  applyBrightness();
}

void openMenu() {
  DBG("menu open");
  drawMenu();
}

void closeMenu() {
  screen = S_LIFE;
  fullRedraw();
}

void resetGame(int start) {
  startingLife = start;
  life = start;
  pendingDelta = 0;
  requestSave();
}

void onMenuTap(int x, int y) {
  int item = buttonAt(MENU_BTNS, M_COUNT, x, y);
  DBG("menu tap item=%d", item);
  switch (item) {
    case M_RESET: resetGame(startingLife); break;
    case M_20:    resetGame(20); break;
    case M_40:    resetGame(40); break;
    case M_DICE:  drawDicePicker(); return;
    case M_LIGHT: drawBrightness(); return;
    case M_BACK:  break;
    default:      return;               // tapped outside the buttons: keep the menu open
  }
  closeMenu();
}

void onDicePickTap(int x, int y) {
  int i = buttonAt(DICE_BTNS, DICE_COUNT + 1, x, y);
  DBG("dice pick tap item=%d", i);
  if (i < 0) return;                    // outside the buttons: stay
  if (i == DICE_COUNT) closeMenu();     // Voltar
  else rollDice(DICE_SIDES[i]);
}

void onScreenTap(int x, int y) {
  switch (screen) {
    case S_MENU:        onMenuTap(x, y); break;
    case S_DICE_PICK:   onDicePickTap(x, y); break;
    case S_DICE_RESULT: closeMenu(); break;   // any tap leaves the result
    case S_BRIGHT:      onBrightnessTap(x, y); break;
    default: break;
  }
}

// Seed random() from the nRF52 hardware RNG (free to use: SoftDevice is off).
void seedRandom() {
  uint32_t seed = 0;
  NRF_RNG->CONFIG = RNG_CONFIG_DERCEN_Enabled << RNG_CONFIG_DERCEN_Pos;  // bias correction
  NRF_RNG->TASKS_START = 1;
  for (int i = 0; i < 4; i++) {
    NRF_RNG->EVENTS_VALRDY = 0;
    while (!NRF_RNG->EVENTS_VALRDY) {}
    seed = (seed << 8) | NRF_RNG->VALUE;
  }
  NRF_RNG->TASKS_STOP = 1;
  randomSeed(seed);
}

// ================= Logic =================
void changeLife(int d) {
  life += d;
  pendingDelta += d;
  lastChangeMs = millis();
  lifeDirty = deltaDirty = true;
  requestSave();
}

// +/- from taps, holds and the rim: changes whatever the current screen controls
void nudge(int d) {
  if (screen == S_BRIGHT) changeBrightness(d);
  else changeLife(d);
}

float angleOf(int x, int y) {
  return atan2f((float)(y - CY), (float)(x - CX)) * 180.0f / PI; // clockwise positive
}

int distOf(int x, int y) {
  int ddx = x - CX, ddy = y - CY;
  return (int)sqrtf((float)(ddx * ddx + ddy * ddy));
}

Zone zoneOf(int x, int y) {
  int d = distOf(x, y);
  if (screen == S_BRIGHT) {
    int b = buttonAt(BRIGHT_BTNS, B_COUNT, x, y);
    if (b == B_MINUS) return Z_MINUS;
    if (b == B_PLUS)  return Z_PLUS;
    if (b < 0 && d >= RIM_INNER_R) return Z_RIM;
    return Z_MENU;                      // OK (or empty space): handled on touch end
  }
  if (screen != S_LIFE) return Z_MENU;
  if (d >= RIM_INNER_R) return Z_RIM;
  if (d < CENTER_R)     return Z_CENTER;
  return (x >= CX) ? Z_PLUS : Z_MINUS;
}

void onTouchStart(int x, int y) {
  lastActivityMs = millis();
  if (asleep) {                         // this touch only wakes the screen
    screenWake();
    touching = true;
    zone = Z_NONE;
    DBG("wake touch ignored");
    return;
  }
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

  const float step = (screen == S_BRIGHT) ? BRIGHT_STEP_DEG : STEP_DEGREES;
  while (angleAccum >= step)  { nudge(+1); angleAccum -= step; }
  while (angleAccum <= -step) { nudge(-1); angleAccum += step; }

  if (screen != S_LIFE) return;         // the dot is only drawn on the life screen

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
    nudge(zone == Z_PLUS ? +10 : -10);
  }
}

void onTouchEnd() {
  unsigned long held = millis() - touchStart;
  DBG("touch end zone=%d held=%lu life=%d", zone, held, life);
  touching = false;
  wantMarkerX = wantMarkerY = -1;
  switch (zone) {
    case Z_PLUS:   if (!holdFired) nudge(+1); break;
    case Z_MINUS:  if (!holdFired) nudge(-1); break;
    case Z_CENTER: if (held < MENU_TAP_MAX_MS) openMenu(); break;
    case Z_MENU:   onScreenTap(touchX, touchY); break;
    default: break;
  }
  zone = Z_NONE;
}

// ================= Arduino =================
void setup() {
#if DEBUG
  Serial.begin(115200);
#endif
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(BACKLIGHT, OUTPUT);
  digitalWrite(BACKLIGHT, LOW);         // dark until the first real frame is on the panel
  Wire.begin();

  loadState();

  tft.init();
  tft.setRotation(0);

  fb.setColorDepth(16);
  fbPix = (uint16_t *)fb.createSprite(W, H);
  if (!fbPix) {
    digitalWrite(BACKLIGHT, HIGH);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("no RAM for framebuffer", 20, 110, 2);
    while (true) delay(1000);
  }

  seedRandom();

  unsigned long t1 = micros();
  fullRedraw();
  applyBrightness();                    // light up only once the first frame is on the panel
  unsigned long frameUs = micros() - t1;

#if DEBUG
  // On USB, give the serial monitor up to 3 s to attach (the life screen is already up).
  // On battery there is no host, so don't wait at all.
  if (usbPowered()) {
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 3000) delay(10);
  }
#endif
  lastActivityMs = millis();
  DBG("boot (life=%d start=%d bright=%d sleep=%ds), first frame in %lu us", life, startingLife, brightness, SLEEP_SECS[sleepIdx], frameUs);
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
    lastActivityMs = lastTouchReadMs;
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

  // refresh battery every 30 s, or right away when USB is plugged / unplugged
  if (millis() - lastBatteryMs > 30000 || (screen == S_LIFE && usbPowered() != shownUsb)) {
    lastBatteryMs = millis();
    drawBattery();
  }

  // auto-off after SLEEP_SECS of no touches; menus / dice / brightness get at least
  // MENU_MIN_SLEEP_S so a short setting doesn't blank them mid-choice
  int sleepS = SLEEP_SECS[sleepIdx];
  if (sleepS > 0 && screen != S_LIFE) sleepS = max(sleepS, MENU_MIN_SLEEP_S);
  if (!asleep && !touching && sleepS > 0 &&
      millis() - lastActivityMs > (unsigned long)sleepS * 1000UL) {
    screenSleep();
  }

  // save to flash once things have been quiet for a moment (and not mid-touch)
  if (saveDirty && !touching && millis() - saveDirtySince > SAVE_DELAY_MS) saveState();

  // one frame: draw what changed into the framebuffer, then push the dirty rect
  static unsigned long lastFrame = 0;
  if (!asleep && (screen == S_LIFE || screen == S_BRIGHT) && millis() - lastFrame >= FRAME_MS) {
    lastFrame = millis();
    if (screen == S_LIFE) {
      if (lifeDirty)  { lifeDirty = false;  drawLife(); }
      if (deltaDirty) { deltaDirty = false; drawDelta(); }
    } else if (brightDirty) {
      brightDirty = false;
      drawBrightnessValue();
    }
    drawMarker();
    flush();
  }

  delay(asleep ? 20 : 2);               // asleep: poll the touch less often
}
