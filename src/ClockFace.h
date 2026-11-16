#pragma once

#include <cstdint>
#include <ctime>

class GfxRenderer;

// "Clock" sleep screen for boards without a battery-backed RTC (Xteink X4).
//
// The X4 normally cuts its own battery power when it "sleeps", so nothing can
// run until the power button is pressed. In Clock mode the power latch is kept
// on instead and the chip enters real deep sleep with a timer wakeup armed for
// the next minute boundary. A timer wake takes a short path through setup()
// (no SD card, no activities): redraw the time with a fast differential
// refresh and go straight back to sleep. The power button still wakes the
// device normally.
//
// Time comes from the system clock, which the ESP32-C3 keeps running through
// deep sleep. It is only shown once it has been set by SNTP since the last
// power-on (Settings > System > Clock > Sync now, or any Wi-Fi connection).
namespace clockface {

// True when this boot is a timer wakeup from Clock sleep.
bool isTimerWake();

// Save what the timer-wake path needs (timezone, format, orientation) into RTC
// memory and arm the next-minute timer. Call right before deep sleep.
void prepareSleep(uint8_t orientation, bool use12Hour, bool dark);

// Re-arm the next-minute timer from the timer-wake path.
void armNextMinute();

// Draw the full clock face for `now` into the renderer's framebuffer (does not
// refresh the panel). Applies the saved orientation.
void drawFace(GfxRenderer& renderer, time_t now, bool timeValid);

// Timer-wake path helpers.
void applySavedTimezone();
uint8_t savedOrientation();
bool use12Hour();
// The minute currently shown on the panel (epoch / 60), or -1 if unknown.
int32_t shownMinute();
void setShownMinute(int32_t minute);
// Count of fast refreshes since the last clean (half) refresh.
uint16_t fastRefreshCount();
void setFastRefreshCount(uint16_t count);

}  // namespace clockface
