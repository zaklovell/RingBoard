// Device knobs for RingBoard. API credentials live in secrets.h.
#pragma once
#include <stdint.h>

// Sandbox mode hits /v2/sandbox/* (fake data, no auth) so the display can be
// developed and tested without touching real health data. Set to 0 to fetch
// the real account's data once everything looks right.
#define OURA_USE_SANDBOX 1

#define OURA_API_BASE "https://api.ouraring.com"
#define OURA_TOKEN_URL "https://api.ouraring.com/oauth/token"
#define USER_AGENT "ringboard-esp32/0.1"

// Oura data barely changes minute to minute; 10 minutes is plenty.
constexpr uint32_t REFRESH_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t FETCH_RETRY_MS = 20UL * 1000UL;  // after a failed fetch
constexpr int OURA_DAILY_BUDGET = 600;  // max API calls per day (4 per refresh)
constexpr uint32_t STALE_AFTER_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t ERROR_AFTER_MS = 90UL * 60UL * 1000UL;

// Refresh the access token this long before it actually expires.
constexpr uint32_t TOKEN_SLACK_SEC = 600;

// Screen schedule (local time). Home Assistant overrides win over this.
constexpr int SLEEP_START_HOUR = 23;
constexpr int SLEEP_END_HOUR = 6;
#define TZ_SPEC "PST8PDT,M3.2.0,M11.1.0"
#define MDNS_NAME "ringboard"

// CYD onboard RGB LED (active LOW). Red doubles as the error indicator.
constexpr int LED_R_PIN = 4;
constexpr int LED_G_PIN = 16;
constexpr int LED_B_PIN = 17;
