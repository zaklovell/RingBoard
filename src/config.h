// Device knobs for RingBoard. API credentials live in secrets.h.
#pragma once
#include <stdint.h>

// Sandbox mode hits /v2/sandbox/* (fake data, no auth) so the display can be
// developed and tested without touching real health data. Set to 0 to fetch
// the real account's data once everything looks right.
#define OURA_USE_SANDBOX 0

#define OURA_API_BASE "https://api.ouraring.com"
#define OURA_TOKEN_URL "https://api.ouraring.com/oauth/token"
#define USER_AGENT "ringboard-esp32/0.1"

// Oura data barely changes minute to minute; 10 minutes is plenty while the
// screen is on. Screen off, drop to 30 so Home Assistant stays fresh
// overnight without torching the call budget (4-5 calls per refresh).
constexpr uint32_t REFRESH_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t REFRESH_OFF_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t FETCH_RETRY_MS = 20UL * 1000UL;  // after a failed fetch
constexpr int OURA_DAILY_BUDGET = 600;  // max API calls per day
constexpr uint32_t STALE_AFTER_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t ERROR_AFTER_MS = 90UL * 60UL * 1000UL;

// Slow endpoints ride their own cadence inside oura.cpp: daily_stress at
// most hourly, sleep_time (bedtime guidance) at most every 12h.
constexpr uint32_t STRESS_EVERY_MS = 60UL * 60UL * 1000UL;
constexpr uint32_t BEDTIME_EVERY_MS = 12UL * 3600UL * 1000UL;

// Refresh the access token this long before it actually expires.
constexpr uint32_t TOKEN_SLACK_SEC = 600;

// Sleep-debt page: nightly need to compare against (the API doesn't expose
// Oura's personalized Sleep Need, so this is RingBoard's own fixed target;
// overridable at runtime via POST /api/config, persisted in NVS).
constexpr int32_t SLEEP_NEED_SEC = 8L * 3600L;
// Default wake time for the "in bed by" plan line, minutes past midnight.
constexpr int DEFAULT_WAKE_MIN = 7 * 60;

// After this hour (local), sleep/readiness data must be filed under TODAY
// to count as fresh; anything older is yesterday's night still on screen
// (ring not synced yet) and greys out as "waiting" instead.
constexpr int FRESH_AFTER_HOUR = 3;

// A tap (XPT2046 T_IRQ read as plain GPIO) cycles pages; a long press
// dismisses a cue banner (or drops a forced screen back to auto). With the
// screen off, the first tap just wakes it for WAKE_TAP_MS.
constexpr int TOUCH_IRQ_PIN = 36;
constexpr uint32_t LONGPRESS_MS = 900;
constexpr uint32_t PAGE2_RETURN_MS = 30UL * 1000UL;
constexpr uint32_t WAKE_TAP_MS = 30UL * 1000UL;

// HA cue banners auto-clear after this long if never dismissed.
constexpr uint32_t CUE_AUTOCLEAR_MS = 20UL * 60UL * 1000UL;

// Screen schedule (local time): off 1am-8am, matching Zak's actual sleep
// (in bed ~12:30-1, up 8:30-9 — screen wakes just before he does). Home
// Assistant overrides win over this.
constexpr int SLEEP_START_HOUR = 1;
constexpr int SLEEP_END_HOUR = 8;
#define TZ_SPEC "PST8PDT,M3.2.0,M11.1.0"
#define MDNS_NAME "ringboard"

// CYD onboard RGB LED (active LOW, very bright — PWM keeps it polite).
// Glance vocabulary: blue pulse = waiting for morning sync, amber = a cue
// is waiting, green double-blink = activity goal met, red = errors only.
// Dark whenever the screen is off (same quiet hours).
constexpr int LED_R_PIN = 4;
constexpr int LED_G_PIN = 16;
constexpr int LED_B_PIN = 17;
constexpr int LED_DUTY = 25;  // of 255: low-duty so it reads, not glares
