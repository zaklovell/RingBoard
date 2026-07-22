// Tiny HTTP API for Home Assistant at http://ringboard.local
//   GET  /api/status         -> JSON state (automation-grade: per-metric
//                               freshness, targets, focus, stress, timing)
//   GET  /api/screen/on|off|auto -> screen override
//   POST /api/token          -> body = new Oura refresh token (X-Auth)
//   POST /api/cue?c=...      -> show/clear a banner cue (X-Auth)
//   GET/POST /api/config     -> wake_min + need_min for the bedtime plan
//   POST /api/demo           -> sample-data mode for safe screenshots (X-Auth)
//   GET  /api/screenshot?page=N -> raw RGB565 framebuffer dump (X-Auth)
#include "webapi.h"
#include "config.h"
#include "oura.h"
#include "secrets.h"
#include "ui.h"
#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

static WebServer server(80);
static ScreenMode mode = SCREEN_AUTO;
static WebMeta meta;
static const BoardView *boardView = nullptr;
static Preferences cfg;

static uint8_t cue = CUE_NONE;
static uint32_t cueSetMs = 0;

static int wakeMin = DEFAULT_WAKE_MIN;
static int32_t needSec = SLEEP_NEED_SEC;

static bool demoPending = false;
static int demoPage = -1;
static int demoMinutes = 10;

static std::function<void(WiFiClient &, int)> shotCb;

static const char *modeName() {
    switch (mode) {
        case SCREEN_FORCED_ON: return "on";
        case SCREEN_FORCED_OFF: return "off";
        default: return "auto";
    }
}

static const char *cueName(uint8_t c) {
    switch (c) {
        case CUE_SYNC_RING: return "sync_ring";
        case CUE_WIND_DOWN: return "wind_down";
        case CUE_MOVE_BREAK: return "move_break";
        default: return "none";
    }
}

static const char *focusName(uint8_t f) {
    switch (f) {
        case FOCUS_BEDTIME: return "bedtime";
        case FOCUS_EASY: return "easy";
        case FOCUS_MOVE: return "move";
        case FOCUS_WINDDOWN: return "winddown";
        default: return "none";
    }
}

static bool authed() {
    if (server.header("X-Auth") == String(DEVICE_SECRET)) return true;
    server.send(401, "application/json", "{\"error\":\"bad auth\"}");
    return false;
}

// The freshness rule HA automations key on for the morning nudge (#5):
// before FRESH_AFTER_HOUR there is nothing to wait for; after it, sleep
// must be filed under today.
static bool sleepFresh() {
    if (!boardView || !boardView->d.haveSleepDetail) return false;
    struct tm t;
    if (!getLocalTime(&t, 10)) return true;
    if (t.tm_hour < FRESH_AFTER_HOUR) return true;
    return boardView->d.sleepAgoDays == 0;
}

static void handleStatus() {
    const OuraData &d = boardView->d;
    long updatedAt = (long)boardView->updatedAt;
    long age = updatedAt > 0 ? (long)time(nullptr) - updatedAt : -1;
    int calToGoal = -1;
    if (d.targetCal > 0) {
        calToGoal = d.targetCal - d.activeCal;
        if (calToGoal < 0) calToGoal = 0;
    }
    char body[1024];
    snprintf(
        body, sizeof(body),
        "{\"screen\":\"%s\",\"screen_on\":%s,\"have_data\":%s,\"demo\":%s,"
        "\"readiness\":%d,\"readiness_ago\":%d,"
        "\"sleep\":%d,\"sleep_ago\":%d,\"sleep_fresh\":%s,"
        "\"total_sleep_min\":%ld,\"rhr\":%d,\"hrv\":%d,"
        "\"activity\":%d,\"steps\":%ld,\"active_cal\":%d,\"target_cal\":%d,"
        "\"cal_to_goal\":%d,"
        "\"sleep_debt_min\":%ld,\"hist_nights\":%d,"
        "\"focus\":\"%s\",\"cue\":\"%s\","
        "\"stress_high_min\":%ld,\"recovery_high_min\":%ld,"
        "\"wake_min\":%d,\"need_min\":%ld,"
        "\"updated_at\":%ld,\"age_sec\":%ld,"
        "\"last_poll\":%ld,\"last_success\":%ld,"
        "\"auth\":\"%s\",\"oura_calls_today\":%d,\"free_heap\":%u}",
        modeName(), meta.screenOn ? "true" : "false",
        meta.haveData ? "true" : "false", meta.demo ? "true" : "false",
        d.haveReadiness ? d.readinessScore : 0, d.readinessAgoDays,
        d.haveSleep ? d.sleepScore : 0, d.sleepAgoDays,
        sleepFresh() ? "true" : "false",
        (long)(d.haveSleepDetail ? d.totalSleepSec / 60 : 0), d.lowestHr,
        d.avgHrv, d.haveActivity ? d.activityScore : 0, (long)d.steps,
        d.activeCal, d.targetCal, calToGoal,
        (long)(d.haveHist ? d.sleepDebtSec / 60 : 0), d.histNights,
        focusName(d.focusCue), cueName(webApiCue()),
        (long)(d.stressHighSec >= 0 ? d.stressHighSec / 60 : -1),
        (long)(d.recoveryHighSec >= 0 ? d.recoveryHighSec / 60 : -1), wakeMin,
        (long)(needSec / 60), updatedAt, age, meta.lastPollAt,
        meta.lastSuccessAt, ouraAuthState(), ouraCallsToday(),
        (unsigned)ESP.getFreeHeap());
    server.send(200, "application/json", body);
}

static void handleScreen(ScreenMode m) {
    mode = m;
    char body[48];
    snprintf(body, sizeof(body), "{\"screen\":\"%s\"}", modeName());
    server.send(200, "application/json", body);
}

static void handleCue() {
    if (!authed()) return;
    String c = server.arg("c");
    if (c == "sync_ring") cue = CUE_SYNC_RING;
    else if (c == "wind_down") cue = CUE_WIND_DOWN;
    else if (c == "move_break") cue = CUE_MOVE_BREAK;
    else if (c == "clear") cue = CUE_NONE;
    else {
        server.send(400, "application/json", "{\"error\":\"bad cue\"}");
        return;
    }
    cueSetMs = millis();
    char body[48];
    snprintf(body, sizeof(body), "{\"cue\":\"%s\"}", cueName(cue));
    server.send(200, "application/json", body);
}

static void handleConfig() {
    if (server.method() == HTTP_POST) {
        if (!authed()) return;
        if (server.hasArg("wake_min")) {
            int w = server.arg("wake_min").toInt();
            if (w >= 0 && w < 24 * 60) {
                wakeMin = w;
                cfg.putInt("wake_min", w);
            }
        }
        if (server.hasArg("need_min")) {
            long n = server.arg("need_min").toInt();
            if (n >= 4 * 60 && n <= 12 * 60) {
                needSec = (int32_t)n * 60;
                cfg.putLong("need_sec", needSec);
            }
        }
        uiSetPlan(wakeMin, needSec);
    }
    char body[64];
    snprintf(body, sizeof(body), "{\"wake_min\":%d,\"need_min\":%ld}", wakeMin,
             (long)(needSec / 60));
    server.send(200, "application/json", body);
}

static void handleDemo() {
    if (!authed()) return;
    demoPage = server.hasArg("page") ? server.arg("page").toInt() : -1;
    demoMinutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 10;
    if (demoMinutes < 1) demoMinutes = 1;
    if (demoMinutes > 60) demoMinutes = 60;
    demoPending = true;
    server.send(200, "application/json", "{\"demo\":\"armed\"}");
}

static void handleScreenshot() {
    if (!authed()) return;
    if (!shotCb) {
        server.send(503, "application/json", "{\"error\":\"not ready\"}");
        return;
    }
    int page = server.hasArg("page") ? server.arg("page").toInt() : -1;
    server.setContentLength(320UL * 240UL * 2UL);
    server.send(200, "application/octet-stream", "");
    WiFiClient client = server.client();
    shotCb(client, page);
}

void webApiInit(const BoardView *view) {
    boardView = view;
    cfg.begin("rbcfg", false);
    wakeMin = cfg.getInt("wake_min", DEFAULT_WAKE_MIN);
    needSec = cfg.getLong("need_sec", SLEEP_NEED_SEC);
    uiSetPlan(wakeMin, needSec);

    if (MDNS.begin(MDNS_NAME)) {
        MDNS.addService("http", "tcp", 80);
    } else {
        Serial.println("[web] mDNS start failed");
    }
    static const char *collected[] = {"X-Auth"};
    server.collectHeaders(collected, 1);
    server.on("/api/status", []() { handleStatus(); });
    server.on("/api/token", HTTP_POST, []() {
        if (!authed()) return;
        String token = server.arg("plain");
        token.trim();
        if (token.length() < 20 || token.length() >= 192) {
            server.send(400, "application/json", "{\"error\":\"bad token\"}");
            return;
        }
        ouraSetRefreshToken(token.c_str());
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/api/screen/on", []() { handleScreen(SCREEN_FORCED_ON); });
    server.on("/api/screen/off", []() { handleScreen(SCREEN_FORCED_OFF); });
    server.on("/api/screen/auto", []() { handleScreen(SCREEN_AUTO); });
    server.on("/api/cue", HTTP_POST, []() { handleCue(); });
    server.on("/api/config", []() { handleConfig(); });
    server.on("/api/demo", HTTP_POST, []() { handleDemo(); });
    server.on("/api/screenshot", []() { handleScreenshot(); });
    server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
    server.begin();
    Serial.printf("[web] http://%s.local (%s)\n", MDNS_NAME,
                  WiFi.localIP().toString().c_str());
}

void webApiLoop() { server.handleClient(); }

ScreenMode screenMode() { return mode; }
void webApiScreenAuto() { mode = SCREEN_AUTO; }
void webApiSetMeta(const WebMeta &m) { meta = m; }

uint8_t webApiCue() {
    if (cue != CUE_NONE && millis() - cueSetMs > CUE_AUTOCLEAR_MS) {
        cue = CUE_NONE;
    }
    return cue;
}
void webApiClearCue() { cue = CUE_NONE; }

int webApiWakeMin() { return wakeMin; }
int32_t webApiNeedSec() { return needSec; }

bool webApiDemoPending(int *page, int *minutes) {
    if (!demoPending) return false;
    demoPending = false;
    *page = demoPage;
    *minutes = demoMinutes;
    return true;
}

void webApiSetScreenshotCb(std::function<void(WiFiClient &, int)> cb) {
    shotCb = cb;
}
