#pragma once
#include <stdint.h>

enum { ST_OK = 0, ST_STALE = 1, ST_ERROR = 2 };

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
};

struct BoardView {
    uint8_t status = ST_ERROR;
    OuraData d;
};
