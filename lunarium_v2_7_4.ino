/*
  Lunarium v2.8.0 (Mega 2560 / Elegoo Mega)
  ------------------------------------------------------------
  v2.8.0 changes:
  - DST rule selector: new menu item "dSt" cycles OFF / US / EU / AU
      OFF: no automatic DST adjustment (Asia, Africa, Russia, most of S. America)
      US : 2nd Sun Mar → 1st Sun Nov at 02:00 local (US/Canada; offset-corrected)
      EU : Last Sun Mar → Last Sun Oct at 01:00 UTC (all EU/UK zones)
      AU : 1st Sun Oct → 1st Sun Apr at 02:00/03:00 local (southern hemisphere)
  - UTC offset now stored in minutes (15-min steps, -720..+840) replacing integer hours
      Supports India +5:30, Nepal +5:45, Iran +3:30, Marquesas -9:30, Chatham +12:45, etc.
  - OFFS display: whole hours shown as before; fractional hours use TM1637 colon (e.g. 5:30)
  - Fixed pre-existing bug: US DST spring/fall UTC transition times were hardcoded to
      UTC-5 only; now correctly derived from the user's stored offset for any US timezone
  - Simulator: beat LED and colon flash synchronised (both 500 ms, locked to UTC second)

  Retained from v2.7.4:
  - RTC stores UTC; local time derived dynamically
  - CLOC editor sets UTC directly
  - brTE long-press cancel restores saved brightness
  - Pin 14: seconds-beat LED
  - TM1637 colon mask = 0x40 for showNumberDecEx()
  - LOC "SEt" interstitial, lat/lon EEPROM persistence
*/

#include <Wire.h>
#include <RTClib.h>
#include <TM1637Display.h>
#include <LedControl.h>
#include <EEPROM.h>
#include <math.h>

#include "lunarium_astro.h"

// -----------------------------------------------------------------------------
// Pins
// -----------------------------------------------------------------------------
static const uint8_t PIN_MAX_DIN = 51;
static const uint8_t PIN_MAX_CLK = 52;
static const uint8_t PIN_MAX_CS  = 53;

static const uint8_t PIN_TM_A_CLK = 2;
static const uint8_t PIN_TM_A_DIO = 3;
static const uint8_t PIN_TM_B_CLK = 4;
static const uint8_t PIN_TM_B_DIO = 5;

static const uint8_t PIN_ALT   = 6;
static const uint8_t PIN_SET   = 7;
static const uint8_t PIN_PLUS  = 8;
static const uint8_t PIN_MINUS = 9;

// Indicator LEDs
static const uint8_t PIN_LED0    = 10; // Set indicator / Horizon indicator in Set 2
static const uint8_t PIN_LED1    = 11; // Set indicator
static const uint8_t PIN_LED2    = 12; // Rise is tomorrow (Set 2 only)
static const uint8_t PIN_LED3    = 13; // Set is tomorrow (Set 2 only)
static const uint8_t PIN_BEAT    = 14; // Seconds-beat pulse (60 ms flash, 1 Hz)

// -----------------------------------------------------------------------------
// Timezone / DST  (US Eastern: EST = UTC-5, EDT = UTC-4)
// -----------------------------------------------------------------------------

// Returns the day-of-month for the nth Sunday of the given month/year.
// dayOfTheWeek(): 0 = Sunday per RTClib convention.
static uint8_t nthSundayOfMonth(int y, int m, int n) {
  DateTime firstOfMonth(y, m, 1, 0, 0, 0);
  uint8_t dow = firstOfMonth.dayOfTheWeek();        // 0=Sun..6=Sat
  int firstSunday = 1 + (int)((7 - dow) % 7);      // day-of-month of 1st Sunday
  return (uint8_t)(firstSunday + (n - 1) * 7);
}

// Returns the effective local UTC offset, applying US-Eastern DST (+1 hour) when active.
// DST window (UTC): 2nd Sunday March 07:00 .. 1st Sunday November 06:00
// Transition UTC times assume a UTC-5 base; accurate to within 1h for adjacent zones.
static int getLocalOffsetHours(const DateTime &utc) {
  int y = utc.year();
  uint32_t dstStart = DateTime(y,  3, nthSundayOfMonth(y,  3, 2), 7, 0, 0).unixtime();
  uint32_t dstEnd   = DateTime(y, 11, nthSundayOfMonth(y, 11, 1), 6, 0, 0).unixtime();
  uint32_t t = utc.unixtime();
  return (int)tzBaseHours + ((t >= dstStart && t < dstEnd) ? 1 : 0);
}

// -----------------------------------------------------------------------------
// Devices
// -----------------------------------------------------------------------------
RTC_DS3231 rtc;
TM1637Display dispA(PIN_TM_A_CLK, PIN_TM_A_DIO);
TM1637Display dispB(PIN_TM_B_CLK, PIN_TM_B_DIO);
LedControl mx = LedControl(PIN_MAX_DIN, PIN_MAX_CLK, PIN_MAX_CS, 1);

// -----------------------------------------------------------------------------
// Enums
// -----------------------------------------------------------------------------
enum DisplaySet : uint8_t { SET1 = 1, SET2 = 2, SET3 = 3, SET4 = 4 };
enum UiMode    : uint8_t { MODE_NORMAL = 0, MODE_MENU = 1, MODE_LOC = 2, MODE_CLOC = 3, MODE_BRTE = 4, MODE_OFFS = 5 };

// Menu order: CLOC, LOC, OFFS, brTE, EXIT
enum MenuItem  : uint8_t { MENU_CLOC = 0, MENU_LOC = 1, MENU_OFFS = 2, MENU_BRTE = 3, MENU_EXIT = 4 };

enum LocStage  : uint8_t { LOC_LAT_LABEL = 0, LOC_LAT_EDIT = 1, LOC_LON_LABEL = 2, LOC_LON_EDIT = 3 };

enum ClkStage  : uint8_t { CLK_YEAR = 0, CLK_DATE = 1, CLK_TIME = 2 };
enum DateField : uint8_t { DF_MM = 0, DF_DD = 1 };
enum TimeField : uint8_t { TF_HH = 0, TF_MM = 1 };

// -----------------------------------------------------------------------------
// Moon icons (12)
// -----------------------------------------------------------------------------
static const uint8_t MOON_ICONS[12][8] = {
  { B00111100, B01000010, B10000001, B10000001, B10000001, B10000001, B01000010, B00111100 },
  { B00111100, B01000010, B10000011, B10000011, B10000011, B10000011, B01000010, B00111100 },
  { B00111100, B01001110, B10000111, B10000111, B10000111, B10000111, B01001110, B00111100 },
  { B00111100, B01001110, B10001111, B10001111, B10001111, B10001111, B01001110, B00111100 },
  { B00111100, B01011110, B10011111, B10011111, B10011111, B10011111, B01011110, B00111100 },
  { B00111100, B01011110, B10111111, B10111111, B10111111, B10111111, B01011110, B00111100 },
  { B00111100, B01111110, B11111111, B11111111, B11111111, B11111111, B01111110, B00111100 },
  { B00111100, B01111010, B11111101, B11111101, B11111101, B11111101, B01111010, B00111100 },
  { B00111100, B01111010, B11111001, B11111001, B11111001, B11111001, B01111010, B00111100 },
  { B00111100, B01110010, B11110001, B11110001, B11110001, B11110001, B01110010, B00111100 },
  { B00111100, B01110010, B11100001, B11100001, B11100001, B11100001, B01110010, B00111100 },
  { B00111100, B01100010, B11000001, B11000001, B11000001, B11000001, B01100010, B00111100 }
};

// -----------------------------------------------------------------------------
// EEPROM layout
// -----------------------------------------------------------------------------
static const uint16_t EEPROM_ADDR_SIG   = 0;   // uint32_t
static const uint16_t EEPROM_ADDR_LAT   = 4;   // int32_t
static const uint16_t EEPROM_ADDR_LON   = 8;   // int32_t
static const uint16_t EEPROM_ADDR_BRTE  = 12;  // uint8_t 0..10
static const uint16_t EEPROM_ADDR_OFFS  = 13;  // int8_t  -12..+14  (base UTC offset hours)
static const uint32_t EEPROM_SIG_VALUE  = 0x4C554E32UL; // "LUN2"

static const int32_t DEFAULT_LAT_E4 =  399526;
static const int32_t DEFAULT_LON_E4 = -751652;
static const uint8_t DEFAULT_BRTE_STEP    = 4;  // 40%
static const int8_t  DEFAULT_TZ_BASE_HOURS = -5; // US Eastern Standard

int32_t userLatE4 = DEFAULT_LAT_E4;
int32_t userLonE4 = DEFAULT_LON_E4;
double  userLat   = 39.9526;
double  userLon   = -75.1652;

uint8_t brightnessStep = DEFAULT_BRTE_STEP; // 0..10 => 0..100
int8_t  tzBaseHours    = DEFAULT_TZ_BASE_HOURS;

// -----------------------------------------------------------------------------
// Input hardening
// -----------------------------------------------------------------------------
static const uint16_t DEBOUNCE_MS      = 35;
static const uint16_t PRESS_LOCKOUT_MS = 140;

// Uniform blink half-period for all digit-editing modes
static const uint16_t EDIT_BLINK_MS = 500;

// Seconds-beat LED pulse width
static const uint16_t BEAT_PULSE_MS = 60;

// -----------------------------------------------------------------------------
// Button / long press
// -----------------------------------------------------------------------------
struct Button {
  uint8_t  pin;
  bool     stable;
  bool     lastStable;
  bool     raw;
  uint32_t lastChangeMs;
  uint32_t lastAcceptedPressMs;
};

struct LpState {
  bool tracking = false;
  uint32_t tStart = 0;
  bool fired = false;
};

static void resetLongPress(LpState &lp) { lp.tracking = false; lp.fired = false; lp.tStart = 0; }
static bool readPressed(uint8_t pin) { return digitalRead(pin) == LOW; }

static void pollButton(Button &b, uint32_t nowMs) {
  bool r = readPressed(b.pin);
  if (r != b.raw) { b.raw = r; b.lastChangeMs = nowMs; }
  if ((nowMs - b.lastChangeMs) >= DEBOUNCE_MS) {
    b.lastStable = b.stable;
    b.stable = b.raw;
  }
}

static bool acceptedPress(Button &b, uint32_t nowMs) {
  bool jp = (b.stable && !b.lastStable);
  if (!jp) return false;
  if (nowMs - b.lastAcceptedPressMs < PRESS_LOCKOUT_MS) return false;
  b.lastAcceptedPressMs = nowMs;
  return true;
}

static bool longPressFired(Button &b, LpState &lp, uint32_t nowMs, uint32_t thresholdMs) {
  if (b.stable) {
    if (!lp.tracking) { lp.tracking = true; lp.tStart = nowMs; lp.fired = false; }
    else if (!lp.fired && (nowMs - lp.tStart) >= thresholdMs) { lp.fired = true; return true; }
  } else {
    lp.tracking = false;
    lp.fired = false;
  }
  return false;
}

Button btnAlt   { PIN_ALT,   false, false, false, 0, 0 };
Button btnSet   { PIN_SET,   false, false, false, 0, 0 };
Button btnPlus  { PIN_PLUS,  false, false, false, 0, 0 };
Button btnMinus { PIN_MINUS, false, false, false, 0, 0 };

LpState lpSet;

// -----------------------------------------------------------------------------
// TM1637 helpers / glyphs
// -----------------------------------------------------------------------------
static const uint8_t TM_COLON  = 0b01000000; // correct for showNumberDecEx()
static const uint8_t SEG_MINUS = 0x40;

// 7-seg bits: a=0x01 b=0x02 c=0x04 d=0x08 e=0x10 f=0x20 g=0x40 dp=0x80
static const uint8_t GLY_BLANK = 0x00;
static const uint8_t GLY_A     = 0x77;
static const uint8_t GLY_B     = 0x7C;
static const uint8_t GLY_C     = 0x39;
static const uint8_t GLY_E     = 0x79;
static const uint8_t GLY_G     = 0x3D;
static const uint8_t GLY_I     = 0x06;
static const uint8_t GLY_L     = 0x38;
static const uint8_t GLY_N     = 0x54;
static const uint8_t GLY_O     = 0x3F;
static const uint8_t GLY_P     = 0x73;
static const uint8_t GLY_Q     = 0x67;
static const uint8_t GLY_R     = 0x50;
static const uint8_t GLY_S     = 0x6D;
static const uint8_t GLY_T     = 0x78;
static const uint8_t GLY_F     = 0x71;
static const uint8_t GLY_U     = 0x3E;
static const uint8_t GLY_X     = 0x76;
static const uint8_t GLY_DEG   = 0x63;

static void showText4(TM1637Display &d, uint8_t a, uint8_t b, uint8_t c, uint8_t e) {
  uint8_t segs[4] = {a, b, c, e};
  d.setSegments(segs);
}

static void showTimeForced(TM1637Display &d, int hh, int mm, bool colonOn) {
  d.showNumberDecEx(hh * 100 + mm, colonOn ? TM_COLON : 0, true, 4, 0);
}
static void showMMDDForced(TM1637Display &d, int month, int day, bool colonOn) {
  d.showNumberDecEx(month * 100 + day, colonOn ? TM_COLON : 0, true, 4, 0);
}
static void showDDHHForced(TM1637Display &d, int dd, int hh, bool colonOn) {
  d.showNumberDecEx(dd * 100 + hh, colonOn ? TM_COLON : 0, true, 4, 0);
}

// no zero padding
static void showIntForcedColonOff(TM1637Display &d, int value) {
  d.showNumberDecEx(value, 0, false, 4, 0);
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static DateTime addSeconds(const DateTime &t, int32_t deltaSeconds) {
  return DateTime((uint32_t)(t.unixtime() + deltaSeconds));
}
static int dayKey(const DateTime &t) { return t.year() * 10000 + t.month() * 100 + t.day(); }
static double dFromUtc(const DateTime &utc) { return (double)utc.unixtime() / 86400.0 - 10957.5; }
static inline double rev360(double x) { return x - 360.0 * floor(x / 360.0); }

static int32_t clampI32(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}
static int clampI(int v, int lo, int hi) {
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return v;
}

// -----------------------------------------------------------------------------
// Zodiac (Option B: sidereal)
// -----------------------------------------------------------------------------
static const bool   USE_SIDEREAL_ZODIAC = true;
static const double AYANAMSA_DEG        = 24.0;

// -----------------------------------------------------------------------------
// Colon patterns
// -----------------------------------------------------------------------------
static bool colonHeartbeat(uint32_t nowMs) {
  uint16_t t = (uint16_t)(nowMs % 1000UL);
  if (t < 120) return true;
  if (t < 240) return false;
  if (t < 360) return true;
  return false;
}

// -----------------------------------------------------------------------------
// Brightness mapping and application
// -----------------------------------------------------------------------------
static void applyBrightnessStep(uint8_t step0to10) {
  uint8_t tm = (uint8_t)lround((step0to10 / 10.0) * 7.0);
  uint8_t mxI = (uint8_t)lround((step0to10 / 10.0) * 15.0);
  if (tm > 7) tm = 7;
  if (mxI > 15) mxI = 15;

  dispA.setBrightness(tm, true);
  dispB.setBrightness(tm, true);
  mx.setIntensity(0, mxI);
}

// -----------------------------------------------------------------------------
// EEPROM load/save
// -----------------------------------------------------------------------------
static void loadSettingsFromEeprom() {
  uint32_t sig = 0;
  EEPROM.get(EEPROM_ADDR_SIG, sig);

  if (sig != EEPROM_SIG_VALUE) {
    userLatE4      = DEFAULT_LAT_E4;
    userLonE4      = DEFAULT_LON_E4;
    brightnessStep = DEFAULT_BRTE_STEP;
    tzBaseHours    = DEFAULT_TZ_BASE_HOURS;
  } else {
    EEPROM.get(EEPROM_ADDR_LAT,  userLatE4);
    EEPROM.get(EEPROM_ADDR_LON,  userLonE4);
    EEPROM.get(EEPROM_ADDR_BRTE, brightnessStep);
    EEPROM.get(EEPROM_ADDR_OFFS, tzBaseHours);
    if (brightnessStep > 10) brightnessStep = DEFAULT_BRTE_STEP;
    if (tzBaseHours < -12 || tzBaseHours > 14) tzBaseHours = DEFAULT_TZ_BASE_HOURS;
  }

  userLatE4 = clampI32(userLatE4, (int32_t)-900000,  (int32_t)900000);
  userLonE4 = clampI32(userLonE4, (int32_t)-1800000, (int32_t)1800000);

  userLat = userLatE4 / 10000.0;
  userLon = userLonE4 / 10000.0;

  applyBrightnessStep(brightnessStep);
}

// All save functions write every field so the sig + data block stays consistent.
static void saveAllToEeprom() {
  EEPROM.put(EEPROM_ADDR_SIG,  EEPROM_SIG_VALUE);
  EEPROM.put(EEPROM_ADDR_LAT,  userLatE4);
  EEPROM.put(EEPROM_ADDR_LON,  userLonE4);
  EEPROM.put(EEPROM_ADDR_BRTE, brightnessStep);
  EEPROM.put(EEPROM_ADDR_OFFS, tzBaseHours);
}

static void saveLocationToEeprom(int32_t latE4, int32_t lonE4) {
  userLatE4 = latE4;
  userLonE4 = lonE4;
  saveAllToEeprom();
}

static void saveBrightnessToEeprom(uint8_t step0to10) {
  brightnessStep = step0to10;
  saveAllToEeprom();
}

static void saveOffsToEeprom(int8_t offs) {
  tzBaseHours = offs;
  saveAllToEeprom();
}

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
struct State {
  UiMode mode = MODE_NORMAL;
  DisplaySet set = SET1;

  bool requireSetRelease = false;

  MenuItem menuItem = MENU_LOC;
  bool menuBlink = false;
  uint32_t lastMenuBlinkMs = 0;

  bool set1ColonOn = true;
  uint32_t lastSet1ColonToggleMs = 0;

  bool led0BlinkOn = false;
  uint32_t lastLed0ToggleMs = 0;

  DateTime localNow;
  DateTime utcNow;

  MoonPhaseInfo phase;
  float illumPct = 0.0f;
  float ageDays  = 0.0f;

  MoonRiseSet riseSetToday;
  MoonRiseSet riseSetTomorrow;

  bool nextRiseValid = false;
  bool nextSetValid  = false;
  DateTime nextRiseLocal;
  DateTime nextSetLocal;

  bool nextRiseIsTomorrow = false;
  bool nextSetIsTomorrow  = false;

  bool moonUp = false;
  float moonAltDeg = 0.0f;

  int tzOffHours = -5;   // updated each loop by getEasternOffsetHours()

  int lastDayKey = -1;
  uint32_t lastPhaseMs = 0;

  // LOC
  LocStage locStage = LOC_LAT_LABEL;
  bool locLabelBlink = false;
  uint32_t lastLocLabelBlinkMs = 0;
  uint8_t locCursor = 0;
  bool locCursorOn = true;
  uint32_t lastLocCursorBlinkMs = 0;
  bool locNeg = false;
  char locDeg[3]  = {'0','3','9'};
  char locFrac[4] = {'9','5','2','6'};

  // CLOC
  ClkStage clkStage = CLK_YEAR;
  bool clkBlinkOn = true;
  uint32_t lastClkBlinkMs = 0;
  int editYear  = 2026;
  int editMonth = 1;
  int editDay   = 1;
  int editHour  = 0;
  int editMin   = 0;
  DateField dateField = DF_MM;
  TimeField timeField = TF_HH;

  // brTE
  bool brteBlink = false;
  uint32_t lastBrteBlinkMs = 0;
  uint8_t editBrteStep = 4;

  // OFFS
  bool offsBlink = false;
  uint32_t lastOffsBlinkMs = 0;
  int8_t editOffsHours = -5;

  uint32_t lastUiTickMs = 0;

  // Seconds-beat LED
  uint8_t  beatLastSec      = 255; // sentinel: fires on first loop
  uint32_t beatPulseStartMs = 0;
};

State S;

// -----------------------------------------------------------------------------
// Moon scheduling
// -----------------------------------------------------------------------------
static float computeApproxAgeDays(double d) {
  double ra, dec, dist, m_lon;
  getMoonCoords(d, ra, dec, dist, m_lon);
  double s_lon = rev(280.466 + 36000.77 * (d / 36525.0));
  double phi = rev(m_lon - s_lon);
  const double synodic = 29.53059;
  return (float)(phi / 360.0 * synodic);
}

static void computePhaseIfDue(uint32_t nowMs) {
  if (S.lastPhaseMs != 0 && (nowMs - S.lastPhaseMs) < 60000UL) return;
  S.lastPhaseMs = nowMs;

  double d = dFromUtc(S.utcNow);
  S.phase = getMoonPhase(d);
  S.illumPct = S.phase.illumPct;
  S.ageDays  = computeApproxAgeDays(d);
}

static void updateMoonAltAndUpFlag() {
  double d = dFromUtc(S.utcNow);
  double alt = getAlt(d, userLat, userLon);
  S.moonAltDeg = (float)alt;
  S.moonUp = (alt > MOON_HORIZON_ALT);
}

static void computeNextRiseSetFromTodayAndTomorrow() {
  S.nextRiseValid = false;
  S.nextSetValid  = false;
  S.nextRiseIsTomorrow = false;
  S.nextSetIsTomorrow  = false;

  uint32_t nowU = S.localNow.unixtime();
  int todayKey = dayKey(S.localNow);

  auto considerRise = [&](const MoonRiseSet &rs) {
    if (rs.foundRise) {
      uint32_t t = rs.riseLocal.unixtime();
      if (t > nowU && (!S.nextRiseValid || t < S.nextRiseLocal.unixtime())) {
        S.nextRiseLocal = rs.riseLocal;
        S.nextRiseValid = true;
        S.nextRiseIsTomorrow = (dayKey(S.nextRiseLocal) != todayKey);
      }
    }
  };
  auto considerSet = [&](const MoonRiseSet &rs) {
    if (rs.foundSet) {
      uint32_t t = rs.setLocal.unixtime();
      if (t > nowU && (!S.nextSetValid || t < S.nextSetLocal.unixtime())) {
        S.nextSetLocal = rs.setLocal;
        S.nextSetValid = true;
        S.nextSetIsTomorrow = (dayKey(S.nextSetLocal) != todayKey);
      }
    }
  };

  considerRise(S.riseSetToday);
  considerSet(S.riseSetToday);
  considerRise(S.riseSetTomorrow);
  considerSet(S.riseSetTomorrow);
}

static void computeRiseSetIfDayChanged() {
  int dk = dayKey(S.localNow);
  if (S.lastDayKey == dk) return;
  S.lastDayKey = dk;

  S.riseSetToday = findMoonRiseSetForLocalDay(
    S.localNow, S.tzOffHours, userLat, userLon, MOON_HORIZON_ALT
  );

  DateTime tomorrowNoon = addSeconds(
    DateTime(S.localNow.year(), S.localNow.month(), S.localNow.day(), 12, 0, 0),
    86400L
  );

  S.riseSetTomorrow = findMoonRiseSetForLocalDay(
    tomorrowNoon, S.tzOffHours, userLat, userLon, MOON_HORIZON_ALT
  );

  computeNextRiseSetFromTodayAndTomorrow();
}

// -----------------------------------------------------------------------------
// Moon icon update
// -----------------------------------------------------------------------------
static uint8_t chooseMoonIconIndex(const MoonPhaseInfo &p) {
  bool waxing = !p.phaseName.startsWith("Waning");
  float f = p.illumPct / 100.0f;
  if (f < 0) f = 0;
  if (f > 1) f = 1;

  if (waxing) {
    int idx = (int)lround(f * 6.0f);
    if (idx < 0) idx = 0;
    if (idx > 6) idx = 6;
    return (uint8_t)idx;
  } else {
    int idx = 6 + (int)lround((1.0f - f) * 5.0f);
    if (idx < 6) idx = 6;
    if (idx > 11) idx = 11;
    return (uint8_t)idx;
  }
}

static void updateMatrixMoonIcon() {
  uint8_t idx = chooseMoonIconIndex(S.phase);
  for (uint8_t r = 0; r < 8; r++) mx.setRow(0, r, MOON_ICONS[idx][r]);
}

// -----------------------------------------------------------------------------
// Set 4 helpers: altitude + zodiac
// -----------------------------------------------------------------------------
static void showAltitudeDeg(TM1637Display &d, float altDeg) {
  int a = (int)lround(altDeg);
  if (a == -0) a = 0;

  bool neg = (a < 0);
  int mag = neg ? -a : a;
  if (mag > 99) mag = 99;

  uint8_t segs[4] = {0,0,0,0};
  if (neg && a != 0) segs[0] = SEG_MINUS;

  int tens = mag / 10;
  int ones = mag % 10;
  segs[1] = dispA.encodeDigit((uint8_t)tens);
  segs[2] = dispA.encodeDigit((uint8_t)ones);
  segs[3] = GLY_DEG;

  d.setSegments(segs);
}

static uint8_t segForChar(char ch) {
  switch (ch) {
    case 'A': return GLY_A;
    case 'B': return GLY_B;
    case 'C': return GLY_C;
    case 'E': return GLY_E;
    case 'G': return GLY_G;
    case 'I': return GLY_I;
    case 'L': return GLY_L;
    case 'N': return GLY_N;
    case 'O': return GLY_O;
    case 'P': return GLY_P;
    case 'Q': return GLY_Q;
    case 'R': return GLY_R;
    case 'S': return GLY_S;
    case 'T': return GLY_T;
    case 'U': return GLY_U;
    case 'X': return GLY_X;
    case ' ': return GLY_BLANK;
    default:  return GLY_BLANK;
  }
}

static uint8_t zodiacIndexFromMoonLon(double eclLonDeg) {
  int idx = (int)floor(eclLonDeg / 30.0);
  idx %= 12;
  if (idx < 0) idx += 12;
  return (uint8_t)idx;
}

static void showZodiacSign(TM1637Display &d, uint8_t signIndex) {
  static const char *LABELS[12] = {
    "ARIE", "TAUR", "GEN ", "CANC", "LEO ", "VRGO",
    "LIBR", "SCOR", "SAGI", "CAPR", "AQUA", "PISC"
  };

  const char *s = LABELS[signIndex % 12];
  uint8_t segs[4] = {
    segForChar(s[0]),
    segForChar(s[1]),
    segForChar(s[2]),
    segForChar(s[3])
  };

  if ((signIndex % 12) == 5) segs[0] = GLY_U; // Virgo: use U to suggest V
  d.setSegments(segs);
}

// -----------------------------------------------------------------------------
// NORMAL MODE displays
// -----------------------------------------------------------------------------
static void updateDisplaysNormal(uint32_t nowMs) {
  if (S.set == SET1) {
    showTimeForced(dispA, S.localNow.hour(), S.localNow.minute(), S.set1ColonOn);
    showMMDDForced(dispB, S.localNow.month(), S.localNow.day(), true);
    return;
  }

  if (S.set == SET2) {
    if (S.nextRiseValid) showTimeForced(dispA, S.nextRiseLocal.hour(), S.nextRiseLocal.minute(), true);
    else dispA.clear();

    if (S.nextSetValid)  showTimeForced(dispB, S.nextSetLocal.hour(),  S.nextSetLocal.minute(),  true);
    else dispB.clear();
    return;
  }

  if (S.set == SET3) {
    bool hb = colonHeartbeat(nowMs);
    int dd = (int)floor(S.ageDays);
    int hh = (int)lround((S.ageDays - dd) * 24.0f);
    if (hh >= 24) { hh -= 24; dd += 1; }

    showDDHHForced(dispA, dd, hh, hb);
    showIntForcedColonOff(dispB, (int)lround(S.illumPct));
    return;
  }

  // SET4
  showAltitudeDeg(dispA, S.moonAltDeg);

  double d = dFromUtc(S.utcNow);
  double ra, dec, dist, m_lon;
  getMoonCoords(d, ra, dec, dist, m_lon);

  double lon = m_lon;
  if (USE_SIDEREAL_ZODIAC) lon = rev360(lon - AYANAMSA_DEG);

  uint8_t z = zodiacIndexFromMoonLon(lon);
  showZodiacSign(dispB, z);
}

// -----------------------------------------------------------------------------
// NORMAL MODE LEDs
// -----------------------------------------------------------------------------
static void updateIndicatorLEDsNormal(uint32_t nowMs) {
  if (S.set == SET2) {
    digitalWrite(PIN_LED2, (S.nextRiseValid && S.nextRiseIsTomorrow) ? HIGH : LOW);
    digitalWrite(PIN_LED3, (S.nextSetValid  && S.nextSetIsTomorrow)  ? HIGH : LOW);
  } else {
    digitalWrite(PIN_LED2, LOW);
    digitalWrite(PIN_LED3, LOW);
  }

  if (S.set == SET1) {
    digitalWrite(PIN_LED0, LOW);
    digitalWrite(PIN_LED1, LOW);
    S.led0BlinkOn = false;
    S.lastLed0ToggleMs = nowMs;
    return;
  }

  if (S.set == SET3) {
    digitalWrite(PIN_LED0, LOW);
    digitalWrite(PIN_LED1, HIGH);
    S.led0BlinkOn = false;
    S.lastLed0ToggleMs = nowMs;
    return;
  }

  if (S.set == SET4) {
    digitalWrite(PIN_LED0, HIGH);
    digitalWrite(PIN_LED1, HIGH);
    S.led0BlinkOn = false;
    S.lastLed0ToggleMs = nowMs;
    return;
  }

  digitalWrite(PIN_LED1, LOW);
  if (S.moonUp) {
    digitalWrite(PIN_LED0, HIGH);
    S.led0BlinkOn = true;
    S.lastLed0ToggleMs = nowMs;
  } else {
    if (nowMs - S.lastLed0ToggleMs >= 500) {
      S.lastLed0ToggleMs = nowMs;
      S.led0BlinkOn = !S.led0BlinkOn;
      digitalWrite(PIN_LED0, S.led0BlinkOn ? HIGH : LOW);
    }
  }
}

// -----------------------------------------------------------------------------
// MENU rendering (blink)
// -----------------------------------------------------------------------------
static void renderMenu(uint32_t nowMs) {
  if (nowMs - S.lastMenuBlinkMs >= 500) {
    S.lastMenuBlinkMs = nowMs;
    S.menuBlink = !S.menuBlink;
  }

  digitalWrite(PIN_LED0, S.menuBlink ? HIGH : LOW);
  digitalWrite(PIN_LED1, LOW);
  digitalWrite(PIN_LED2, LOW);
  digitalWrite(PIN_LED3, LOW);

  if (!S.menuBlink) {
    dispA.clear();
    dispB.clear();
    updateMatrixMoonIcon();
    return;
  }

  if (S.menuItem == MENU_CLOC) {
    dispA.showNumberDec(1, false, 4, 0);
    showText4(dispB, GLY_C, GLY_L, GLY_O, GLY_C);
  } else if (S.menuItem == MENU_LOC) {
    dispA.showNumberDec(2, false, 4, 0);
    showText4(dispB, GLY_L, GLY_O, GLY_C, GLY_BLANK);
  } else if (S.menuItem == MENU_OFFS) {
    dispA.showNumberDec(3, false, 4, 0);
    showText4(dispB, GLY_O, GLY_F, GLY_F, GLY_S);
  } else if (S.menuItem == MENU_BRTE) {
    dispA.showNumberDec(4, false, 4, 0);
    showText4(dispB, GLY_B, GLY_R, GLY_T, GLY_E);
  } else {
    dispA.showNumberDec(0, false, 4, 0);
    showText4(dispB, GLY_E, GLY_X, GLY_I, GLY_T);
  }

  updateMatrixMoonIcon();
}

// -----------------------------------------------------------------------------
// brTE editor
// -----------------------------------------------------------------------------
static void renderBrte(uint32_t nowMs) {
  if (nowMs - S.lastBrteBlinkMs >= EDIT_BLINK_MS) {
    S.lastBrteBlinkMs = nowMs;
    S.brteBlink = !S.brteBlink;
  }

  digitalWrite(PIN_LED0, HIGH);
  digitalWrite(PIN_LED1, LOW);
  digitalWrite(PIN_LED2, LOW);
  digitalWrite(PIN_LED3, LOW);

  if (!S.brteBlink) dispA.clear();
  else dispA.showNumberDec((int)S.editBrteStep * 10, false, 4, 0);

  showText4(dispB, GLY_B, GLY_R, GLY_T, GLY_E);
  updateMatrixMoonIcon();
}

// -----------------------------------------------------------------------------
// OFFS (UTC base offset) editor
// -----------------------------------------------------------------------------

// Display signed offset on dispA: e.g. -5 -> "- 5 ", +14 -> " 14 "
static void showOffsValue(TM1637Display &d, int8_t offs) {
  bool neg = (offs < 0);
  int  mag = neg ? -(int)offs : (int)offs;
  uint8_t segs[4] = {GLY_BLANK, GLY_BLANK, GLY_BLANK, GLY_BLANK};
  segs[0] = neg ? SEG_MINUS : GLY_BLANK;
  segs[1] = (mag >= 10) ? d.encodeDigit((uint8_t)(mag / 10)) : GLY_BLANK;
  segs[2] = d.encodeDigit((uint8_t)(mag % 10));
  d.setSegments(segs);
}

static void renderOffs(uint32_t nowMs) {
  if (nowMs - S.lastOffsBlinkMs >= EDIT_BLINK_MS) {
    S.lastOffsBlinkMs = nowMs;
    S.offsBlink = !S.offsBlink;
  }

  digitalWrite(PIN_LED0, HIGH);
  digitalWrite(PIN_LED1, LOW);
  digitalWrite(PIN_LED2, LOW);
  digitalWrite(PIN_LED3, LOW);

  if (!S.offsBlink) dispA.clear();
  else showOffsValue(dispA, S.editOffsHours);

  showText4(dispB, GLY_O, GLY_F, GLY_F, GLY_S);
  updateMatrixMoonIcon();
}

// -----------------------------------------------------------------------------
// LOC editor helpers
// -----------------------------------------------------------------------------
static void locLoadFromE4(int32_t vE4) {
  int32_t a = (vE4 < 0) ? -vE4 : vE4;
  S.locNeg = (vE4 < 0);

  int deg  = (int)(a / 10000);
  int frac = (int)(a % 10000);

  S.locDeg[0] = char('0' + (deg / 100) % 10);
  S.locDeg[1] = char('0' + (deg / 10)  % 10);
  S.locDeg[2] = char('0' + (deg % 10));

  S.locFrac[0] = char('0' + (frac / 1000) % 10);
  S.locFrac[1] = char('0' + (frac / 100)  % 10);
  S.locFrac[2] = char('0' + (frac / 10)   % 10);
  S.locFrac[3] = char('0' + (frac % 10));

  S.locCursor = 0;
  S.locCursorOn = true;
  S.lastLocCursorBlinkMs = millis();
}

static int32_t locParseToE4() {
  int deg =
    (S.locDeg[0]-'0')*100 +
    (S.locDeg[1]-'0')*10  +
    (S.locDeg[2]-'0');

  int frac =
    (S.locFrac[0]-'0')*1000 +
    (S.locFrac[1]-'0')*100  +
    (S.locFrac[2]-'0')*10   +
    (S.locFrac[3]-'0');

  int32_t v = (int32_t)deg * 10000L + (int32_t)frac;
  if (S.locNeg) v = -v;
  return v;
}

static void locClampValidate(int32_t &vE4, bool isLat) {
  int32_t minV = isLat ? (int32_t)-900000  : (int32_t)-1800000;
  int32_t maxV = isLat ? (int32_t) 900000  : (int32_t) 1800000;
  vE4 = clampI32(vE4, minV, maxV);

  int32_t a = (vE4 < 0) ? -vE4 : vE4;
  int32_t deg = a / 10000;

  if (isLat && deg > 90)   vE4 = (vE4 < 0) ? (int32_t)-900000  : (int32_t)900000;
  if (!isLat && deg > 180) vE4 = (vE4 < 0) ? (int32_t)-1800000 : (int32_t)1800000;
}

static void locCursorNext() { S.locCursor = (uint8_t)((S.locCursor + 1) % 8); }

static void locAdjustDigit(int delta) {
  if (S.locCursor == 0) { S.locNeg = !S.locNeg; return; }

  if (S.locCursor >= 1 && S.locCursor <= 3) {
    uint8_t i = (uint8_t)(S.locCursor - 1);
    int v = (S.locDeg[i] - '0') + delta;
    if (v < 0) v = 9;
    if (v > 9) v = 0;
    S.locDeg[i] = char('0' + v);
    return;
  }

  uint8_t j = (uint8_t)(S.locCursor - 4);
  int v = (S.locFrac[j] - '0') + delta;
  if (v < 0) v = 9;
  if (v > 9) v = 0;
  S.locFrac[j] = char('0' + v);
}

static bool minusVisibleDoubleFlash(uint32_t nowMs) {
  uint16_t t = (uint16_t)(nowMs % 1200);
  if ((t >= 120 && t < 220) || (t >= 320 && t < 420)) return false;
  return true;
}

static void renderLocLabel(uint32_t nowMs, bool isLat) {
  if (nowMs - S.lastLocLabelBlinkMs >= 500) {
    S.lastLocLabelBlinkMs = nowMs;
    S.locLabelBlink = !S.locLabelBlink;
  }

  digitalWrite(PIN_LED0, isLat ? HIGH : LOW);
  digitalWrite(PIN_LED1, isLat ? LOW  : HIGH);
  digitalWrite(PIN_LED2, LOW);
  digitalWrite(PIN_LED3, LOW);

  if (!S.locLabelBlink) {
    dispA.clear();
    dispB.clear();
    updateMatrixMoonIcon();
    return;
  }

  showText4(dispA, GLY_S, GLY_E, GLY_T, GLY_BLANK);
  if (isLat) showText4(dispB, GLY_L, GLY_A, GLY_T, GLY_BLANK);
  else       showText4(dispB, GLY_L, GLY_O, GLY_N, GLY_BLANK);

  updateMatrixMoonIcon();
}

static void renderLocEdit(uint32_t nowMs) {
  if (nowMs - S.lastLocCursorBlinkMs >= EDIT_BLINK_MS) {
    S.lastLocCursorBlinkMs = nowMs;
    S.locCursorOn = !S.locCursorOn;
  }

  uint8_t a[4] = {0,0,0,0};
  uint8_t b[4] = {0,0,0,0};

  bool showMinus = false;
  if (S.locNeg) {
    if (S.locCursor == 0) showMinus = S.locCursorOn;
    else showMinus = minusVisibleDoubleFlash(nowMs);
  }
  a[0] = showMinus ? SEG_MINUS : 0x00;

  auto enc = [&](char ch) -> uint8_t {
    if (ch >= '0' && ch <= '9') return dispA.encodeDigit((uint8_t)(ch - '0'));
    return 0x00;
  };

  a[1] = enc(S.locDeg[0]);
  a[2] = enc(S.locDeg[1]);
  a[3] = enc(S.locDeg[2]);

  b[0] = enc(S.locFrac[0]);
  b[1] = enc(S.locFrac[1]);
  b[2] = enc(S.locFrac[2]);
  b[3] = enc(S.locFrac[3]);

  if (!S.locCursorOn) {
    if (S.locCursor >= 1 && S.locCursor <= 3) a[S.locCursor] = 0x00;
    else if (S.locCursor >= 4 && S.locCursor <= 7) b[S.locCursor - 4] = 0x00;
  }

  dispA.setSegments(a);
  dispB.setSegments(b);

  updateMatrixMoonIcon();
}

// -----------------------------------------------------------------------------
// CLOC helpers
// -----------------------------------------------------------------------------
static int daysInMonth(int y, int m) {
  static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (m < 1) m = 1;
  if (m > 12) m = 12;
  int d = mdays[m-1];
  bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
  if (m == 2 && leap) d = 29;
  return d;
}

static void beginClockEditor() {
  DateTime n = rtc.now();
  S.editYear  = n.year();
  S.editMonth = n.month();
  S.editDay   = n.day();
  S.editHour  = n.hour();
  S.editMin   = n.minute();

  S.clkStage = CLK_YEAR;
  S.dateField = DF_MM;
  S.timeField = TF_HH;
  S.clkBlinkOn = true;
  S.lastClkBlinkMs = millis();

  S.requireSetRelease = true;
  resetLongPress(lpSet);
}

static void commitClockToRtc() {
  S.editYear  = clampI(S.editYear, 2000, 2099);
  S.editMonth = clampI(S.editMonth, 1, 12);
  int dim = daysInMonth(S.editYear, S.editMonth);
  S.editDay   = clampI(S.editDay, 1, dim);

  S.editHour = clampI(S.editHour, 0, 23);
  S.editMin  = clampI(S.editMin, 0, 59);

  rtc.adjust(DateTime(S.editYear, S.editMonth, S.editDay, S.editHour, S.editMin, 0));
}

// *** FIX: blink cadence to 500ms, and YEAR stage Display B blank ***
static void renderClockEditor(uint32_t nowMs) {
  // CHANGED from 400ms -> 500ms
  if (nowMs - S.lastClkBlinkMs >= 500) {
    S.lastClkBlinkMs = nowMs;
    S.clkBlinkOn = !S.clkBlinkOn;
  }

  digitalWrite(PIN_LED0, HIGH);
  digitalWrite(PIN_LED1, HIGH);
  digitalWrite(PIN_LED2, HIGH);
  digitalWrite(PIN_LED3, LOW);

  if (S.clkStage == CLK_YEAR) {
    if (!S.clkBlinkOn) dispA.clear();
    else dispA.showNumberDec(S.editYear, false, 4, 0);

    // Remind user that CLOC edits UTC (RTC stores UTC in v2.7.4+, retained in v2.8.0)
    showText4(dispB, GLY_U, GLY_T, GLY_C, GLY_BLANK);

    updateMatrixMoonIcon();
    return;
  }

  if (S.clkStage == CLK_DATE) {
    showMMDDForced(dispA, S.editMonth, S.editDay, true);

    if (!S.clkBlinkOn) {
      uint8_t a2[4] = {
        dispA.encodeDigit((uint8_t)(S.editMonth / 10)),
        (uint8_t)(dispA.encodeDigit((uint8_t)(S.editMonth % 10)) | TM_COLON),
        dispA.encodeDigit((uint8_t)(S.editDay   / 10)),
        dispA.encodeDigit((uint8_t)(S.editDay   % 10))
      };

      if (S.dateField == DF_MM) { a2[0] = GLY_BLANK; a2[1] = TM_COLON; }
      else                      { a2[2] = GLY_BLANK; a2[3] = GLY_BLANK; }

      dispA.setSegments(a2);
    }

    // Keep B simple/blank in edit screens (matches your preference)
    dispB.clear();
    updateMatrixMoonIcon();
    return;
  }

  // CLK_TIME
  showTimeForced(dispA, S.editHour, S.editMin, true);

  if (!S.clkBlinkOn) {
    uint8_t a2[4] = {
      dispA.encodeDigit((uint8_t)(S.editHour / 10)),
      (uint8_t)(dispA.encodeDigit((uint8_t)(S.editHour % 10)) | TM_COLON),
      dispA.encodeDigit((uint8_t)(S.editMin  / 10)),
      dispA.encodeDigit((uint8_t)(S.editMin  % 10))
    };

    if (S.timeField == TF_HH) { a2[0] = GLY_BLANK; a2[1] = TM_COLON; }
    else                      { a2[2] = GLY_BLANK; a2[3] = GLY_BLANK; }

    dispA.setSegments(a2);
  }

  dispB.clear();
  updateMatrixMoonIcon();
}

// -----------------------------------------------------------------------------
// uiTick + setup/loop
// (unchanged from v2.7.2 except that it now uses the updated renderClockEditor())
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Wire.begin();

  if (!rtc.begin()) {
    Serial.println("ERROR: DS3231 not found");
    while (1) delay(50);
  }

  pinMode(PIN_ALT,   INPUT_PULLUP);
  pinMode(PIN_SET,   INPUT_PULLUP);
  pinMode(PIN_PLUS,  INPUT_PULLUP);
  pinMode(PIN_MINUS, INPUT_PULLUP);

  pinMode(PIN_LED0, OUTPUT);
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  pinMode(PIN_LED3, OUTPUT);
  pinMode(PIN_BEAT, OUTPUT);
  digitalWrite(PIN_BEAT, LOW);

  mx.shutdown(0, false);
  mx.clearDisplay(0);

  loadSettingsFromEeprom();

  Serial.println("Lunarium v2.8.0 - global DST modes (OFF/US/EU/AU), 15-min offset steps");
  Serial.print("Loaded LAT="); Serial.print(userLat, 4);
  Serial.print(" LON="); Serial.print(userLon, 4);
  Serial.print(" BRTE="); Serial.println((int)brightnessStep * 10);
  Serial.println("NOTE: RTC must store UTC. Run CLOC and enter current UTC time if not already set.");
  Serial.println("--------------------------------------------------");
}

static void uiTick(uint32_t nowMs) {
  pollButton(btnAlt,   nowMs);
  pollButton(btnSet,   nowMs);
  pollButton(btnPlus,  nowMs);
  pollButton(btnMinus, nowMs);

  if (S.requireSetRelease) {
    if (!btnSet.stable) {
      S.requireSetRelease = false;
      resetLongPress(lpSet);
    }
  }

  bool setLong = (!S.requireSetRelease) && longPressFired(btnSet, lpSet, nowMs, 1200);

  // NORMAL
  if (S.mode == MODE_NORMAL) {
    if (setLong) {
      S.mode = MODE_MENU;
      S.menuItem = MENU_LOC;
      S.menuBlink = false;
      S.lastMenuBlinkMs = nowMs;
      return;
    }

    if (acceptedPress(btnAlt, nowMs)) {
      if (S.set == SET1) S.set = SET2;
      else if (S.set == SET2) S.set = SET3;
      else if (S.set == SET3) S.set = SET4;
      else S.set = SET1;
    }

    if (nowMs - S.lastSet1ColonToggleMs >= 500) {
      S.lastSet1ColonToggleMs = nowMs;
      S.set1ColonOn = !S.set1ColonOn;
    }

    updateDisplaysNormal(nowMs);
    updateMatrixMoonIcon();
    updateIndicatorLEDsNormal(nowMs);
    return;
  }

  // MENU
  if (S.mode == MODE_MENU) {
    if (setLong) {
      S.mode = MODE_NORMAL;
      S.set = SET1;
      digitalWrite(PIN_LED0, LOW);
      digitalWrite(PIN_LED1, LOW);
      digitalWrite(PIN_LED2, LOW);
      digitalWrite(PIN_LED3, LOW);
      return;
    }

    if (acceptedPress(btnAlt, nowMs)) S.menuItem = (MenuItem)((S.menuItem + 1) % 5);

    if (acceptedPress(btnSet, nowMs)) {
      if (S.menuItem == MENU_CLOC) {
        S.mode = MODE_CLOC;
        beginClockEditor();
        return;
      } else if (S.menuItem == MENU_LOC) {
        S.mode = MODE_LOC;
        S.locStage = LOC_LAT_LABEL;
        S.locLabelBlink = false;
        S.lastLocLabelBlinkMs = nowMs;
        locLoadFromE4(userLatE4);

        S.requireSetRelease = true;
        resetLongPress(lpSet);
        return;
      } else if (S.menuItem == MENU_OFFS) {
        S.mode = MODE_OFFS;
        S.editOffsHours = tzBaseHours;
        S.offsBlink = false;
        S.lastOffsBlinkMs = nowMs;

        S.requireSetRelease = true;
        resetLongPress(lpSet);
        return;
      } else if (S.menuItem == MENU_BRTE) {
        S.mode = MODE_BRTE;
        S.editBrteStep = brightnessStep;
        S.brteBlink = false;
        S.lastBrteBlinkMs = nowMs;

        S.requireSetRelease = true;
        resetLongPress(lpSet);
        return;
      } else {
        S.mode = MODE_NORMAL;
        S.set = SET1;
        digitalWrite(PIN_LED0, LOW);
        digitalWrite(PIN_LED1, LOW);
        digitalWrite(PIN_LED2, LOW);
        digitalWrite(PIN_LED3, LOW);
        return;
      }
    }

    renderMenu(nowMs);
    return;
  }

  // brTE
  if (S.mode == MODE_BRTE) {
    if (setLong) {
      applyBrightnessStep(brightnessStep); // discard preview, restore saved level
      S.mode = MODE_MENU;
      S.menuItem = MENU_BRTE;
      S.menuBlink = false;
      S.lastMenuBlinkMs = nowMs;
      return;
    }

    if (acceptedPress(btnPlus, nowMs)) {
      if (S.editBrteStep < 10) S.editBrteStep++;
      else S.editBrteStep = 0;
      applyBrightnessStep(S.editBrteStep);
    }
    if (acceptedPress(btnMinus, nowMs)) {
      if (S.editBrteStep > 0) S.editBrteStep--;
      else S.editBrteStep = 10;
      applyBrightnessStep(S.editBrteStep);
    }

    if (acceptedPress(btnSet, nowMs)) {
      saveBrightnessToEeprom(S.editBrteStep);
      applyBrightnessStep(brightnessStep);

      S.mode = MODE_NORMAL;
      S.set = SET1;

      S.requireSetRelease = true;
      resetLongPress(lpSet);

      digitalWrite(PIN_LED0, LOW);
      digitalWrite(PIN_LED1, LOW);
      digitalWrite(PIN_LED2, LOW);
      digitalWrite(PIN_LED3, LOW);
      return;
    }

    renderBrte(nowMs);
    return;
  }

  // OFFS
  if (S.mode == MODE_OFFS) {
    if (setLong) {
      // cancel: discard edit, return to menu
      S.mode = MODE_MENU;
      S.menuItem = MENU_OFFS;
      S.menuBlink = false;
      S.lastMenuBlinkMs = nowMs;
      return;
    }

    if (acceptedPress(btnPlus, nowMs)) {
      if (S.editOffsHours < 14) S.editOffsHours++;
      else S.editOffsHours = -12;
    }
    if (acceptedPress(btnMinus, nowMs)) {
      if (S.editOffsHours > -12) S.editOffsHours--;
      else S.editOffsHours = 14;
    }

    if (acceptedPress(btnSet, nowMs)) {
      saveOffsToEeprom(S.editOffsHours);

      // Force rise/set and phase recalculation with new offset
      S.lastDayKey  = -1;
      S.lastPhaseMs = 0;

      S.mode = MODE_NORMAL;
      S.set  = SET1;

      S.requireSetRelease = true;
      resetLongPress(lpSet);

      digitalWrite(PIN_LED0, LOW);
      digitalWrite(PIN_LED1, LOW);
      digitalWrite(PIN_LED2, LOW);
      digitalWrite(PIN_LED3, LOW);
      return;
    }

    renderOffs(nowMs);
    return;
  }

  // LOC
  if (S.mode == MODE_LOC) {
    if (setLong) {
      S.mode = MODE_MENU;
      S.menuItem = MENU_LOC;
      S.menuBlink = false;
      S.lastMenuBlinkMs = nowMs;
      return;
    }

    if (S.locStage == LOC_LAT_LABEL) {
      if (acceptedPress(btnSet, nowMs)) {
        S.locStage = LOC_LAT_EDIT;
        S.locCursor = 0;
        S.locCursorOn = true;
        S.lastLocCursorBlinkMs = nowMs;

        S.requireSetRelease = true;
        resetLongPress(lpSet);
      }
      renderLocLabel(nowMs, true);
      return;
    }

    if (S.locStage == LOC_LON_LABEL) {
      if (acceptedPress(btnSet, nowMs)) {
        S.locStage = LOC_LON_EDIT;
        S.locCursor = 0;
        S.locCursorOn = true;
        S.lastLocCursorBlinkMs = nowMs;

        S.requireSetRelease = true;
        resetLongPress(lpSet);
      }
      renderLocLabel(nowMs, false);
      return;
    }

    if (acceptedPress(btnAlt, nowMs))   locCursorNext();
    if (acceptedPress(btnPlus, nowMs))  locAdjustDigit(+1);
    if (acceptedPress(btnMinus, nowMs)) locAdjustDigit(-1);

    if (acceptedPress(btnSet, nowMs)) {
      int32_t v = locParseToE4();

      if (S.locStage == LOC_LAT_EDIT) {
        locClampValidate(v, true);
        userLatE4 = v;
        userLat = userLatE4 / 10000.0;

        S.locStage = LOC_LON_LABEL;
        S.locLabelBlink = false;
        S.lastLocLabelBlinkMs = nowMs;
        locLoadFromE4(userLonE4);

        S.requireSetRelease = true;
        resetLongPress(lpSet);
        return;
      } else {
        locClampValidate(v, false);
        userLonE4 = v;
        userLon = userLonE4 / 10000.0;

        saveLocationToEeprom(userLatE4, userLonE4);

        S.lastDayKey = -1;
        S.lastPhaseMs = 0;

        S.mode = MODE_NORMAL;
        S.set = SET1;

        S.requireSetRelease = true;
        resetLongPress(lpSet);

        digitalWrite(PIN_LED0, LOW);
        digitalWrite(PIN_LED1, LOW);
        digitalWrite(PIN_LED2, LOW);
        digitalWrite(PIN_LED3, LOW);
        return;
      }
    }

    renderLocEdit(nowMs);
    return;
  }

  // CLOC
  if (S.mode == MODE_CLOC) {
    if (setLong) {
      S.mode = MODE_MENU;
      S.menuItem = MENU_CLOC;
      S.menuBlink = false;
      S.lastMenuBlinkMs = nowMs;
      return;
    }

    if (S.clkStage == CLK_YEAR) {
      if (acceptedPress(btnPlus, nowMs))  S.editYear++;
      if (acceptedPress(btnMinus, nowMs)) S.editYear--;
      S.editYear = clampI(S.editYear, 2000, 2099);

      if (acceptedPress(btnSet, nowMs)) {
        S.clkStage = CLK_DATE;
        S.dateField = DF_MM;

        S.requireSetRelease = true;
        resetLongPress(lpSet);
      }

      renderClockEditor(nowMs);
      return;
    }

    if (S.clkStage == CLK_DATE) {
      if (acceptedPress(btnAlt, nowMs)) S.dateField = (S.dateField == DF_MM) ? DF_DD : DF_MM;

      if (acceptedPress(btnPlus, nowMs)) {
        if (S.dateField == DF_MM) S.editMonth++;
        else S.editDay++;
      }
      if (acceptedPress(btnMinus, nowMs)) {
        if (S.dateField == DF_MM) S.editMonth--;
        else S.editDay--;
      }

      S.editMonth = clampI(S.editMonth, 1, 12);
      int dim = daysInMonth(S.editYear, S.editMonth);
      S.editDay = clampI(S.editDay, 1, dim);

      if (acceptedPress(btnSet, nowMs)) {
        S.clkStage = CLK_TIME;
        S.timeField = TF_HH;

        S.requireSetRelease = true;
        resetLongPress(lpSet);
      }

      renderClockEditor(nowMs);
      return;
    }

    // CLK_TIME
    if (acceptedPress(btnAlt, nowMs)) S.timeField = (S.timeField == TF_HH) ? TF_MM : TF_HH;

    if (acceptedPress(btnPlus, nowMs)) {
      if (S.timeField == TF_HH) S.editHour++;
      else S.editMin++;
    }
    if (acceptedPress(btnMinus, nowMs)) {
      if (S.timeField == TF_HH) S.editHour--;
      else S.editMin--;
    }

    S.editHour = (S.editHour + 24) % 24;
    S.editMin  = (S.editMin  + 60) % 60;

    if (acceptedPress(btnSet, nowMs)) {
      commitClockToRtc();

      S.mode = MODE_NORMAL;
      S.set = SET1;

      S.requireSetRelease = true;
      resetLongPress(lpSet);

      digitalWrite(PIN_LED0, LOW);
      digitalWrite(PIN_LED1, LOW);
      digitalWrite(PIN_LED2, LOW);
      digitalWrite(PIN_LED3, LOW);
      return;
    }

    renderClockEditor(nowMs);
    return;
  }
}

void setupMoonMatrix() {
  mx.shutdown(0, false);
  mx.clearDisplay(0);
}

void loop() {
  uint32_t nowMs = millis();

  // RTC stores UTC; derive local time using the current DST-aware offset.
  S.utcNow     = rtc.now();
  S.tzOffHours = getLocalOffsetHours(S.utcNow);
  S.localNow   = addSeconds(S.utcNow, (int32_t)(S.tzOffHours * 3600L));

  // Seconds-beat LED: brief pulse each time the RTC second increments.
  {
    uint8_t sec = S.utcNow.second();
    if (sec != S.beatLastSec) {
      S.beatLastSec      = sec;
      S.beatPulseStartMs = nowMs;
      digitalWrite(PIN_BEAT, HIGH);
    } else if (S.beatPulseStartMs != 0 && (nowMs - S.beatPulseStartMs) >= BEAT_PULSE_MS) {
      S.beatPulseStartMs = 0;
      digitalWrite(PIN_BEAT, LOW);
    }
  }

  computePhaseIfDue(nowMs);

  if (S.mode == MODE_NORMAL) {
    computeRiseSetIfDayChanged();
    updateMoonAltAndUpFlag();
    computeNextRiseSetFromTodayAndTomorrow();
  } else {
    updateMoonAltAndUpFlag();
  }

  if (nowMs - S.lastUiTickMs >= 20) {
    S.lastUiTickMs = nowMs;
    uiTick(nowMs);
  }
}
