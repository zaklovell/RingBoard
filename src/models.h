#pragma once
#include <stdint.h>
#include <time.h>

enum { ST_OK = 0, ST_STALE = 1, ST_ERROR = 2 };

// Nights of sleep history kept for the sleep-debt page. Lives here (not
// config.h) because it shapes OuraData.
constexpr int HIST_DAYS = 14;

// Today's Focus: one conservative cue derived from the lowest score
// contributor. The reason index points into kFocusReasons (ui.cpp).
enum FocusCue : uint8_t {
    FOCUS_NONE = 0,
    FOCUS_BEDTIME,   // sleep balance / short night
    FOCUS_EASY,      // recovery signals down (HRV, RHR, temp)
    FOCUS_MOVE,      // activity balance lagging
    FOCUS_WINDDOWN,  // sleep quality (efficiency/latency/timing)
};

// Cues Home Assistant can push onto the screen (POST /api/cue).
enum HaCue : uint8_t {
    CUE_NONE = 0,
    CUE_SYNC_RING,
    CUE_WIND_DOWN,
    CUE_MOVE_BREAK,
};

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

    // Days-ago of each metric's newest document (0 = filed under today,
    // -1 = unknown). Oura files a night under its wake day, so a morning
    // where the sleep ones aren't 0 means the ring hasn't synced yet.
    int readinessAgoDays = -1;
    int sleepScoreAgoDays = -1;
    int sleepAgoDays = -1;  // long-sleep detail document
    int activityAgoDays = -1;

    int32_t totalSleepSec = 0;
    int32_t deepSec = 0;
    int32_t remSec = 0;
    int32_t lightSec = 0;
    int32_t awakeSec = 0;

    // Recovery numbers from the long-sleep doc: average HRV (ms) and the
    // night's lowest heart rate (bpm). 0 = not present.
    int avgHrv = 0;
    int lowestHr = 0;

    int32_t steps = 0;
    float distanceM = 0;
    int activeCal = 0;
    int totalCal = 0;
    int targetCal = 0;  // Oura's daily active-calorie target; 0 = unknown
    int32_t sedentarySec = 0;

    // Per-day score history, oldest first; [HIST_DAYS-1] = today's slot.
    // 0 = no score that day. Feeds the 7-day-average trend deltas.
    int8_t readinessHist[HIST_DAYS] = {};
    int8_t sleepScoreHist[HIST_DAYS] = {};
    int8_t activityHist[HIST_DAYS] = {};

    // Today's Focus (see FocusCue): derived from the lowest contributor
    // across the newest readiness + sleep-score documents.
    uint8_t focusCue = FOCUS_NONE;
    uint8_t focusReason = 0;  // index into kFocusReasons
    int focusValue = 0;       // the contributor score that drove it

    // Daily stress (fetched hourly, not every refresh). Seconds of high
    // stress / high recovery today; -1 = not present in the response.
    bool haveStress = false;
    int stressAgoDays = -1;
    int32_t stressHighSec = -1;
    int32_t recoveryHighSec = -1;

    // Oura bedtime guidance (sleep_time endpoint, fetched twice a day).
    // Offsets are seconds relative to midnight of the guidance day; the
    // endpoint may return nothing for stretches — always have a fallback.
    bool haveBedtime = false;
    int32_t bedtimeStartSec = 0;
    int32_t bedtimeEndSec = 0;

    // Per-night total sleep, oldest first; [HIST_DAYS-1] = last night
    // (Oura files a sleep period under the day you woke up). <=0 = no data.
    bool haveHist = false;
    int32_t histSec[HIST_DAYS] = {};
    int32_t sleepDebtSec = 0;  // running shortfall vs need, floor 0
    int histNights = 0;        // nights with data, for the coverage note
};

struct BoardView {
    uint8_t status = ST_ERROR;
    time_t updatedAt = 0;  // wall clock of when fetched data last CHANGED
    uint8_t cue = CUE_NONE;   // HA-pushed banner, CUE_NONE = hidden
    OuraData d;
};
