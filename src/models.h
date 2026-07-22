#pragma once
#include <stdint.h>
#include <time.h>

enum { ST_OK = 0, ST_STALE = 1, ST_ERROR = 2 };

// Nights of sleep history kept for the sleep-debt page. Lives here (not
// config.h) because it shapes OuraData.
constexpr int HIST_DAYS = 14;

// Everything the screen shows, filled by ouraFetchAll. Durations in seconds,
// distance in meters, straight from the API.
struct OuraData {
    bool haveReadiness = false;
    bool haveSleep = false;       // daily sleep score
    bool haveSleepDetail = false; // long-sleep document with phase durations
    bool haveActivity = false;

    int readinessScore = 0;
    int sleepScore = 0;
    int activityScore = 0;

    int32_t totalSleepSec = 0;
    int32_t deepSec = 0;
    int32_t remSec = 0;
    int32_t lightSec = 0;
    int32_t awakeSec = 0;

    int32_t steps = 0;
    float distanceM = 0;
    int activeCal = 0;
    int totalCal = 0;
    int32_t sedentarySec = 0;

    // Days-ago of the newest long-sleep document (0 = filed under today,
    // 1 = yesterday, -1 = unknown). Oura files a night under its wake day,
    // so a morning where this isn't 0 means the ring hasn't synced yet.
    int sleepAgoDays = -1;

    // Per-night total sleep, oldest first; [HIST_DAYS-1] = last night
    // (Oura files a sleep period under the day you woke up). <=0 = no data.
    bool haveHist = false;
    int32_t histSec[HIST_DAYS] = {};
    int32_t sleepDebtSec = 0;  // running shortfall vs SLEEP_NEED_SEC, floor 0
};

struct BoardView {
    uint8_t status = ST_ERROR;
    time_t updatedAt = 0;  // wall clock of when fetched data last CHANGED
    OuraData d;
};
