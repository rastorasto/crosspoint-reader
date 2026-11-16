#include "ClockFace.h"

#include <GfxRenderer.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "HalStorage.h"
#include "activities/reader/ReaderUtils.h"
#include "fontIds.h"

namespace clockface {

namespace {

constexpr uint32_t STATE_MAGIC = 0xC10C4A11;

// RTC slow memory survives deep sleep while powered and is cleared on power-on.
struct ClockState {
  uint32_t magic;
  int32_t shownMinute;
  uint16_t fastRefreshes;
  uint8_t orientation;
  uint8_t use12Hour;
  uint8_t dark;
  char tz[64];
};
RTC_DATA_ATTR ClockState state;

// Battery samples taken during Clock sleep. 144 samples = 24 h at 10 min.
constexpr int BATTERY_SAMPLES = 144;
constexpr int BATTERY_INTERVAL_MIN = 10;
struct BatterySample {
  int32_t minute;  // epoch / 60
  uint8_t percent;
  uint8_t usb;
};
RTC_DATA_ATTR BatterySample batterySamples[BATTERY_SAMPLES];
RTC_DATA_ATTR uint16_t batteryHead;   // next write slot
RTC_DATA_ATTR uint16_t batteryCount;  // valid samples
RTC_DATA_ATTR uint32_t batteryMagic;
constexpr uint32_t BATTERY_MAGIC = 0xBA77C10C;
uint8_t facePercent = 0;
bool faceUsb = false;

// Seven-segment layout: a=top, b=top-right, c=bottom-right, d=bottom,
// e=bottom-left, f=top-left, g=middle.
constexpr uint8_t SEGMENTS[10] = {
    0b0111111,  // 0
    0b0000110,  // 1
    0b1011011,  // 2
    0b1001111,  // 3
    0b1100110,  // 4
    0b1101101,  // 5
    0b1111101,  // 6
    0b0000111,  // 7
    0b1111111,  // 8
    0b1101111,  // 9
};

void drawDigit(const GfxRenderer& r, const int x, const int y, const int w, const int h, const int t, const int d) {
  const uint8_t s = SEGMENTS[d % 10];
  const int half = h / 2;
  if (s & 0b0000001) r.fillRect(x + t, y, w - 2 * t, t);                     // a
  if (s & 0b0000010) r.fillRect(x + w - t, y + t, t, half - t - t / 2);      // b
  if (s & 0b0000100) r.fillRect(x + w - t, y + half + t / 2, t, half - t - t / 2);  // c
  if (s & 0b0001000) r.fillRect(x + t, y + h - t, w - 2 * t, t);             // d
  if (s & 0b0010000) r.fillRect(x, y + half + t / 2, t, half - t - t / 2);   // e
  if (s & 0b0100000) r.fillRect(x, y + t, t, half - t - t / 2);              // f
  if (s & 0b1000000) r.fillRect(x + t, y + half - t / 2, w - 2 * t, t);      // g
}

}  // namespace

bool isTimerWake() {
  return state.magic == STATE_MAGIC && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

void armNextMinute() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const int64_t usIntoMinute = static_cast<int64_t>(tv.tv_sec % 60) * 1000000LL + tv.tv_usec;
  // Wake slightly after the boundary so the new minute is already current.
  int64_t us = 60LL * 1000000LL - usIntoMinute + 200000LL;
  if (us < 1000000LL) us += 60LL * 1000000LL;
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(us));
}

void prepareSleep(const uint8_t orientation, const bool use12Hour, const bool dark) {
  state.magic = STATE_MAGIC;
  state.dark = dark ? 1 : 0;
  state.orientation = orientation;
  state.use12Hour = use12Hour ? 1 : 0;
  state.fastRefreshes = 0;
  const char* tz = getenv("TZ");
  snprintf(state.tz, sizeof(state.tz), "%s", tz ? tz : "UTC0");
  armNextMinute();
}

void applySavedTimezone() {
  setenv("TZ", state.tz[0] ? state.tz : "UTC0", 1);
  tzset();
}

uint8_t savedOrientation() { return state.orientation; }
bool use12Hour() { return state.use12Hour != 0; }
int32_t shownMinute() { return state.magic == STATE_MAGIC ? state.shownMinute : -1; }
void setShownMinute(const int32_t minute) { state.shownMinute = minute; }
uint16_t fastRefreshCount() { return state.fastRefreshes; }

void setBattery(const uint8_t percent, const bool usb) {
  facePercent = percent;
  faceUsb = usb;
}

void recordBattery(const int32_t minute) {
  if (batteryMagic != BATTERY_MAGIC) {
    batteryMagic = BATTERY_MAGIC;
    batteryHead = 0;
    batteryCount = 0;
  }
  if (batteryCount > 0) {
    const int last = (batteryHead + BATTERY_SAMPLES - 1) % BATTERY_SAMPLES;
    if (minute - batterySamples[last].minute < BATTERY_INTERVAL_MIN) return;
  }
  batterySamples[batteryHead] = {minute, facePercent, static_cast<uint8_t>(faceUsb ? 1 : 0)};
  batteryHead = (batteryHead + 1) % BATTERY_SAMPLES;
  if (batteryCount < BATTERY_SAMPLES) batteryCount++;
}

void flushBatteryLog() {
  if (batteryMagic != BATTERY_MAGIC || batteryCount == 0) return;
  constexpr const char* path = "/.crosspoint/clock_battery.csv";
  const bool isNew = !Storage.exists(path);
  HalFile file = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) return;
  if (isNew) {
    const char header[] = "local_time,battery_percent,usb\n";
    file.write(header, sizeof(header) - 1);
  }
  const int start = (batteryHead + BATTERY_SAMPLES - batteryCount) % BATTERY_SAMPLES;
  for (int i = 0; i < batteryCount; i++) {
    const BatterySample& s = batterySamples[(start + i) % BATTERY_SAMPLES];
    const time_t t = static_cast<time_t>(s.minute) * 60;
    struct tm localTm;
    localtime_r(&t, &localTm);
    char line[48];
    const int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d,%u,%u\n", localTm.tm_year + 1900,
                           localTm.tm_mon + 1, localTm.tm_mday, localTm.tm_hour, localTm.tm_min, s.percent, s.usb);
    if (n > 0) file.write(line, static_cast<size_t>(n));
  }
  file.close();
  batteryCount = 0;
}
void setFastRefreshCount(const uint16_t count) { state.fastRefreshes = count; }

static void drawFaceContent(GfxRenderer& renderer, const time_t now, const bool timeValid) {
  ReaderUtils::applyOrientation(renderer, state.orientation);
  renderer.clearScreen();

  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool landscape = sw > sh;

  // Size four digits and a colon to fill most of the width.
  const int digitW = sw * (landscape ? 17 : 16) / 100;
  const int digitH = digitW * 9 / 5;
  const int thick = digitW / 6;
  const int gap = digitW / 6;
  const int colonW = thick;
  const int totalW = 4 * digitW + 4 * gap + colonW;
  const int dateLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int blockH = digitH + dateLineH * 2;
  const int x0 = (sw - totalW) / 2;
  const int y0 = (sh - blockH) / 2;

  if (!timeValid) {
    renderer.drawCenteredText(UI_12_FONT_ID, sh / 2 - dateLineH, "Clock not set", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, sh / 2 + dateLineH / 2, "Settings > System > Clock > Sync now");
    return;
  }

  struct tm localTm;
  localtime_r(&now, &localTm);
  int hour = localTm.tm_hour;
  if (state.use12Hour) {
    hour %= 12;
    if (hour == 0) hour = 12;
  }

  int x = x0;
  if (hour >= 10 || !state.use12Hour) {
    drawDigit(renderer, x, y0, digitW, digitH, thick, hour / 10);
  }
  x += digitW + gap;
  drawDigit(renderer, x, y0, digitW, digitH, thick, hour % 10);
  x += digitW + gap;
  renderer.fillRect(x, y0 + digitH * 3 / 10, colonW, colonW);
  renderer.fillRect(x, y0 + digitH * 7 / 10 - colonW, colonW, colonW);
  x += colonW + gap;
  drawDigit(renderer, x, y0, digitW, digitH, thick, localTm.tm_min / 10);
  x += digitW + gap;
  drawDigit(renderer, x, y0, digitW, digitH, thick, localTm.tm_min % 10);

  char dateLine[48];
  strftime(dateLine, sizeof(dateLine), "%A, %d %B %Y", &localTm);
  renderer.drawCenteredText(UI_12_FONT_ID, y0 + digitH + dateLineH, dateLine, true, EpdFontFamily::BOLD);
  if (state.use12Hour) {
    renderer.drawCenteredText(UI_12_FONT_ID, y0 + digitH + dateLineH * 2 + dateLineH / 2,
                              localTm.tm_hour >= 12 ? "PM" : "AM");
  }
}

void drawFace(GfxRenderer& renderer, const time_t now, const bool timeValid) {
  drawFaceContent(renderer, now, timeValid);
  if (facePercent > 0) {
    char battery[12];
    snprintf(battery, sizeof(battery), faceUsb ? "%u%% USB" : "%u%%", facePercent);
    const int w = renderer.getTextWidth(UI_12_FONT_ID, battery);
    const int lh = renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, renderer.getScreenWidth() - w - 16, renderer.getScreenHeight() - lh - 8, battery);
  }
  if (state.dark) renderer.invertScreen();
}

}  // namespace clockface
