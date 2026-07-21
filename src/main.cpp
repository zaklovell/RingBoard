// RingBoard: Oura ring stats on the CYD. Readiness and sleep scores up top,
// sleep stages in the middle, activity along the bottom. Refreshes every 10
// minutes and exposes a tiny HTTP API for Home Assistant.
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "models.h"
#include "oura.h"
#include "secrets.h"
#include "ui.h"
#include "webapi.h"

static TFT_eSPI tft;
static BoardView view;

static uint32_t lastFetch = 0;
static uint32_t lastGoodFetch = 0;
static bool haveData = false;
static bool screenOn = true;
static uint8_t page = 0;          // 0 = main, 1 = sleep debt
static uint32_t pageFlipAt = 0;
static uint32_t lastTapMs = 0;

static void ledInit() {
    pinMode(LED_R_PIN, OUTPUT);
    pinMode(LED_G_PIN, OUTPUT);
    pinMode(LED_B_PIN, OUTPUT);
    digitalWrite(LED_R_PIN, HIGH);  // active low: all off
    digitalWrite(LED_G_PIN, HIGH);
    digitalWrite(LED_B_PIN, HIGH);
}

static void ledError(bool on) { digitalWrite(LED_R_PIN, on ? LOW : HIGH); }

static void connectWiFi() {
    char msg[64];
    snprintf(msg, sizeof(msg), "connecting to %s", WIFI_SSID);
    uiBoot("RingBoard", msg);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
        if (millis() - started > 30000) {
            uiBoot("RingBoard", "WiFi failed, retrying");
            WiFi.disconnect();
            delay(500);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            started = millis();
        }
    }
    Serial.printf("[wifi] connected, ip %s\n", WiFi.localIP().toString().c_str());
}

static uint8_t currentStatus() {
    if (WiFi.status() != WL_CONNECTED) return ST_ERROR;
    uint32_t age = millis() - lastGoodFetch;
    if (!haveData || age > ERROR_AFTER_MS) return ST_ERROR;
    if (age > STALE_AFTER_MS) return ST_STALE;
    return ST_OK;
}

static void publishWebStatus() {
    WebStatus s;
    s.screenOn = screenOn;
    s.haveData = haveData;
    s.readiness = view.d.haveReadiness ? view.d.readinessScore : 0;
    s.sleep = view.d.haveSleep ? view.d.sleepScore : 0;
    s.steps = view.d.haveActivity ? view.d.steps : 0;
    s.sleepDebtSec = view.d.haveHist ? view.d.sleepDebtSec : 0;
    s.updatedAt = (long)view.updatedAt;
    webApiSetStatus(s);
}

// The "updated" stamp tracks when the DATA last changed, not when the last
// fetch ran — a 10-minute poll that returns the same numbers doesn't count.
static bool sameData(const OuraData &a, const OuraData &b) {
    return a.readinessScore == b.readinessScore &&
           a.sleepScore == b.sleepScore &&
           a.activityScore == b.activityScore &&
           a.totalSleepSec == b.totalSleepSec && a.deepSec == b.deepSec &&
           a.remSec == b.remSec && a.lightSec == b.lightSec &&
           a.awakeSec == b.awakeSec && a.steps == b.steps &&
           a.activeCal == b.activeCal && a.totalCal == b.totalCal &&
           a.sedentarySec == b.sedentarySec;
}

static void refreshBoard() {
    OuraData d;
    if (!ouraFetchAll(&d)) {
        Serial.println("[board] fetch failed");
        if (!haveData) {
            const char *why = strcmp(ouraAuthState(), "auth") == 0
                                  ? "auth needed: run tools/oura_auth.py"
                                  : "can't reach Oura, retrying";
            uiBoot("RingBoard", why);
        }
        return;
    }
    bool changed = !haveData || !sameData(d, view.d);
    view.d = d;
    if (changed) view.updatedAt = time(nullptr);
    lastGoodFetch = millis();
    haveData = true;
    Serial.printf(
        "[board] readiness=%d sleep=%d steps=%ld heap=%u calls=%d\n",
        d.readinessScore, d.sleepScore, (long)d.steps,
        (unsigned)ESP.getFreeHeap(), ouraCallsToday());
}

// Screen power: HA override wins, otherwise the night schedule applies.
static bool screenShouldBeOn() {
    if (screenMode() == SCREEN_FORCED_ON) return true;
    if (screenMode() == SCREEN_FORCED_OFF) return false;
    struct tm t;
    if (!getLocalTime(&t, 10)) return true;  // no clock yet: stay on
    // The off-window may wrap midnight (23..6) or not (0..7).
    bool night = SLEEP_START_HOUR > SLEEP_END_HOUR
                     ? (t.tm_hour >= SLEEP_START_HOUR || t.tm_hour < SLEEP_END_HOUR)
                     : (t.tm_hour >= SLEEP_START_HOUR && t.tm_hour < SLEEP_END_HOUR);
    return !night;
}

static void applyScreenPower() {
    bool want = screenShouldBeOn();
    if (want == screenOn) return;
    screenOn = want;
    uiBacklight(screenOn);
    Serial.printf("[screen] %s\n", screenOn ? "on" : "off");
    if (screenOn) {
        lastFetch = 0;  // refresh immediately on wake
        view.status = currentStatus();
        uiRender(view, page);
    }
}

// XPT2046 T_IRQ sits LOW while the panel is pressed; polled as a plain GPIO
// so a tap flips pages without pulling in the touch library. The debt page
// falls back to the main page on its own after PAGE2_RETURN_MS.
static void pollTouch() {
    if (!screenOn || !haveData) return;
    uint32_t now = millis();
    if (digitalRead(TOUCH_IRQ_PIN) == LOW && now - lastTapMs > 500) {
        lastTapMs = now;
        page ^= 1;
        pageFlipAt = now;
        view.status = currentStatus();
        uiRender(view, page);
    }
    if (page == 1 && now - pageFlipAt > PAGE2_RETURN_MS) {
        page = 0;
        view.status = currentStatus();
        uiRender(view, page);
    }
}

void setup() {
    Serial.begin(115200);
    ledInit();
    pinMode(TOUCH_IRQ_PIN, INPUT);
    uiInit(&tft);
    ouraInit();
    connectWiFi();
    setenv("TZ", TZ_SPEC, 1);
    tzset();
    configTzTime(TZ_SPEC, "pool.ntp.org", "time.google.com");
    webApiInit();
    // OTA reflash over WiFi (pio run -t upload --upload-port <board-ip>).
    // webApiInit already started mDNS, so ArduinoOTA must not start it again.
    ArduinoOTA.setMdnsEnabled(false);
    ArduinoOTA.setHostname(MDNS_NAME);
    ArduinoOTA.setPassword(DEVICE_SECRET);
    ArduinoOTA.begin();
#if OURA_USE_SANDBOX
    uiBoot("RingBoard", "fetching (sandbox data)");
#else
    uiBoot("RingBoard", "fetching your stats");
#endif
}

void loop() {
    ArduinoOTA.handle();
    webApiLoop();
    applyScreenPower();
    pollTouch();
    uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
        ledError(true);
        delay(250);
        return;
    }

    // Failed fetches back off for FETCH_RETRY_MS instead of spinning; a
    // sustained outage must not hammer Oura or torch the daily call budget.
    uint32_t wait = haveData ? REFRESH_MS : FETCH_RETRY_MS;
    if (screenOn && (lastFetch == 0 || now - lastFetch >= wait)) {
        lastFetch = now;
        refreshBoard();
        publishWebStatus();
        if (haveData) {
            view.status = currentStatus();
            uiRender(view, page);
        }
    }

    ledError(currentStatus() == ST_ERROR && haveData);
    delay(25);
}
