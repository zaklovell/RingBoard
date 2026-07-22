#pragma once
#include <WiFiClient.h>
#include <functional>
#include <stdint.h>
#include "models.h"

enum ScreenMode { SCREEN_AUTO, SCREEN_FORCED_ON, SCREEN_FORCED_OFF };

// Fetch-cycle metadata for /api/status; the data itself is read straight
// from the BoardView registered at init (single-threaded loop, no locking).
struct WebMeta {
    bool screenOn = true;
    bool haveData = false;
    bool demo = false;
    long lastPollAt = 0;     // epoch of the last fetch attempt
    long lastSuccessAt = 0;  // epoch of the last successful fetch
};

void webApiInit(const BoardView *view);
void webApiLoop();
ScreenMode screenMode();
void webApiScreenAuto();  // long-press: forced screen mode back to schedule
void webApiSetMeta(const WebMeta &m);

// HA cue banner (#11): POST /api/cue?c=sync_ring|wind_down|move_break|clear.
uint8_t webApiCue();    // current cue; auto-expires after CUE_AUTOCLEAR_MS
void webApiClearCue();  // long-press dismiss

// Bedtime plan config (#4): NVS-backed, POST /api/config?wake_min=&need_min=.
int webApiWakeMin();
int32_t webApiNeedSec();

// Demo mode: POST /api/demo?page=&minutes= loads sample data so screenshots
// never contain real health numbers. main consumes the request flag.
bool webApiDemoPending(int *page, int *minutes);

// Screenshot: main registers a callback that streams the requested page.
void webApiSetScreenshotCb(std::function<void(WiFiClient &, int)> cb);
