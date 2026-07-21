// Tiny HTTP API for Home Assistant at http://ringboard.local
//   GET  /api/status      -> JSON state
//   GET  /api/screen/on   -> force screen on   (overrides the night schedule)
//   GET  /api/screen/off  -> force screen off
//   GET  /api/screen/auto -> back to the schedule
//   POST /api/token       -> body = new Oura refresh token, X-Auth: DEVICE_SECRET
#include "webapi.h"
#include "config.h"
#include "oura.h"
#include "secrets.h"
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

static WebServer server(80);
static ScreenMode mode = SCREEN_AUTO;
static WebStatus status;

static const char *modeName() {
    switch (mode) {
        case SCREEN_FORCED_ON: return "on";
        case SCREEN_FORCED_OFF: return "off";
        default: return "auto";
    }
}

static void handleStatus() {
    long age = status.updatedAt > 0 ? (long)time(nullptr) - status.updatedAt : -1;
    char body[320];
    snprintf(body, sizeof(body),
             "{\"screen\":\"%s\",\"screen_on\":%s,\"have_data\":%s,"
             "\"readiness\":%d,\"sleep\":%d,\"steps\":%ld,"
             "\"sleep_debt_min\":%ld,"
             "\"updated_at\":%ld,\"age_sec\":%ld,"
             "\"auth\":\"%s\",\"oura_calls_today\":%d,\"free_heap\":%u}",
             modeName(), status.screenOn ? "true" : "false",
             status.haveData ? "true" : "false", status.readiness,
             status.sleep, (long)status.steps,
             (long)(status.sleepDebtSec / 60), status.updatedAt, age,
             ouraAuthState(), ouraCallsToday(), (unsigned)ESP.getFreeHeap());
    server.send(200, "application/json", body);
}

static void handleScreen(ScreenMode m) {
    mode = m;
    char body[48];
    snprintf(body, sizeof(body), "{\"screen\":\"%s\"}", modeName());
    server.send(200, "application/json", body);
}

void webApiInit() {
    if (MDNS.begin(MDNS_NAME)) {
        MDNS.addService("http", "tcp", 80);
    } else {
        Serial.println("[web] mDNS start failed");
    }
    static const char *collected[] = {"X-Auth"};
    server.collectHeaders(collected, 1);
    server.on("/api/status", []() { handleStatus(); });
    server.on("/api/token", HTTP_POST, []() {
        if (server.header("X-Auth") != String(DEVICE_SECRET)) {
            server.send(401, "application/json", "{\"error\":\"bad auth\"}");
            return;
        }
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
    server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
    server.begin();
    Serial.printf("[web] http://%s.local (%s)\n", MDNS_NAME,
                  WiFi.localIP().toString().c_str());
}

void webApiLoop() { server.handleClient(); }

ScreenMode screenMode() { return mode; }

void webApiSetStatus(const WebStatus &s) { status = s; }
