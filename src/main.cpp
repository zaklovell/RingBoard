// RingBoard: Oura ring stats on the CYD. Four tap-cycled pages (main,
// focus, sleep debt, stress), an HTTP API for Home Assistant, HA cue
// banners, and a glanceable RGB-LED vocabulary. Refreshes every 10 minutes
// while the screen is on, every 30 while it's off.
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
static long lastPollAt = 0;
static long lastSuccessAt = 0;
static bool haveData = false;
static bool screenOn = true;
static uint8_t page = PAGE_MAIN;
static uint32_t pageFlipAt = 0;
static uint32_t wakeUntilMs = 0;   // tap-to-wake window while schedule says off
static uint32_t demoUntilMs = 0;   // sample-data mode for safe screenshots

// Touch state machine: short tap vs long press on the bare T_IRQ pin.
static bool pressed = false;
static bool longFired = false;
static uint32_t pressStartMs = 0;
static uint32_t lastTapMs = 0;

// LED: green double-blink fires on the goal-met transition.
static uint32_t greenBlinkUntil = 0;
static int prevCalToGoal = -1;

static bool demoActive() { return demoUntilMs != 0 && millis() < demoUntilMs; }

// ---- LED (#7): active-LOW RGB via PWM so it reads, not glares -------------
enum { LEDC_R = 0, LEDC_G = 1, LEDC_B = 2 };

static void ledInit() {
    ledcSetup(LEDC_R, 5000, 8);
    ledcSetup(LEDC_G, 5000, 8);
    ledcSetup(LEDC_B, 5000, 8);
    ledcAttachPin(LED_R_PIN, LEDC_R);
    ledcAttachPin(LED_G_PIN, LEDC_G);
    ledcAttachPin(LED_B_PIN, LEDC_B);
}

// duty 0..255 of intended brightness; pin is active low.
static void ledWrite(int r, int g, int b) {
    ledcWrite(LEDC_R, 255 - r);
    ledcWrite(LEDC_G, 255 - g);
    ledcWrite(LEDC_B, 255 - b);
}

static bool sleepWaiting() {
    if (!haveData || view.d.sleepAgoDays < 0) return false;
    struct tm t;
    if (!getLocalTime(&t, 10)) return false;
    return t.tm_hour >= FRESH_AFTER_HOUR && view.d.sleepAgoDays != 0;
}

static uint8_t currentStatus() {
    if (WiFi.status() != WL_CONNECTED) return ST_ERROR;
    if (demoActive()) return ST_OK;
    uint32_t age = millis() - lastGoodFetch;
    if (!haveData || age > ERROR_AFTER_MS) return ST_ERROR;
    if (age > STALE_AFTER_MS) return ST_STALE;
    return ST_OK;
}

// Vocabulary: red = errors only, amber = a cue waits, green blink = goal
// just met, blue pulse = waiting for the morning sync. Dark when the
// screen is dark (same quiet hours).
static void ledTask() {
    if (!screenOn) {
        ledWrite(0, 0, 0);
        return;
    }
    uint32_t now = millis();
    if (currentStatus() == ST_ERROR && haveData) {
        ledWrite(LED_DUTY, 0, 0);
        return;
    }
    if (now < greenBlinkUntil) {
        bool on = ((greenBlinkUntil - now) / 250) % 2 == 0;
        ledWrite(0, on ? LED_DUTY : 0, 0);
        return;
    }
    if (webApiCue() != CUE_NONE) {
        ledWrite(LED_DUTY, LED_DUTY / 3, 0);  // amber-ish
        return;
    }
    if (sleepWaiting()) {
        // Soft triangle-wave pulse, 4s period.
        uint32_t ph = now % 4000;
        int duty = ph < 2000 ? (int)(ph * LED_DUTY / 2000)
                             : (int)((4000 - ph) * LED_DUTY / 2000);
        ledWrite(0, 0, duty);
        return;
    }
    ledWrite(0, 0, 0);
}

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

static void publishMeta() {
    WebMeta m;
    m.screenOn = screenOn;
    m.haveData = haveData;
    m.demo = demoActive();
    m.lastPollAt = lastPollAt;
    m.lastSuccessAt = lastSuccessAt;
    webApiSetMeta(m);
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
           a.targetCal == b.targetCal && a.sedentarySec == b.sedentarySec &&
           a.avgHrv == b.avgHrv && a.lowestHr == b.lowestHr &&
           a.focusCue == b.focusCue &&
           a.stressHighSec == b.stressHighSec &&
           a.recoveryHighSec == b.recoveryHighSec;
}

static void render() {
    if (!screenOn) return;
    view.cue = webApiCue();
    view.status = currentStatus();
    if (!uiPageAvailable(view, page)) page = PAGE_MAIN;
    uiRender(view, page);
}

static void refreshBoard() {
    OuraData d;
    lastPollAt = (long)time(nullptr);
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
    lastSuccessAt = (long)time(nullptr);
    haveData = true;

    // Goal-met green blink (#7): fire once when cal_to_goal crosses zero.
    int calToGoal =
        d.targetCal > 0 ? (d.targetCal - d.activeCal > 0 ? d.targetCal - d.activeCal : 0)
                        : -1;
    if (prevCalToGoal > 0 && calToGoal == 0) {
        greenBlinkUntil = millis() + 2000;
    }
    prevCalToGoal = calToGoal;

    Serial.printf("[board] fetch ok heap=%u calls=%d\n",
                  (unsigned)ESP.getFreeHeap(), ouraCallsToday());
}

// ---- Demo mode: the mockup dataset, so screenshots hold no real data ------
static void loadDemoData() {
    OuraData d;
    d.haveReadiness = d.haveSleep = d.haveSleepDetail = d.haveActivity = true;
    d.readinessScore = 78;
    d.sleepScore = 86;
    d.activityScore = 63;
    d.readinessAgoDays = d.sleepScoreAgoDays = d.sleepAgoDays =
        d.activityAgoDays = 0;
    d.totalSleepSec = 7 * 3600 + 24 * 60;
    d.deepSec = 5100;
    d.remSec = 6420;
    d.lightSec = 12480;
    d.awakeSec = 2640;
    d.avgHrv = 61;
    d.lowestHr = 52;
    d.steps = 6214;
    d.distanceM = 4506;
    d.activeCal = 270;
    d.totalCal = 2210;
    d.targetCal = 450;
    d.sedentarySec = 42 * 60;
    static const int8_t rh[HIST_DAYS] = {0, 72, 68, 80, 74, 77, 71, 79,
                                         73, 76, 70, 78, 74, 78};
    static const int8_t sh[HIST_DAYS] = {78, 74, 0, 82, 79, 81, 76, 83,
                                         77, 80, 82, 84, 79, 86};
    static const int8_t ah[HIST_DAYS] = {70, 75, 68, 0, 74, 77, 72, 69,
                                         73, 71, 70, 74, 72, 63};
    memcpy(d.readinessHist, rh, sizeof(rh));
    memcpy(d.sleepScoreHist, sh, sizeof(sh));
    memcpy(d.activityHist, ah, sizeof(ah));
    d.focusCue = FOCUS_BEDTIME;
    d.focusReason = 0;  // "Sleep balance is your lowest factor"
    d.focusValue = 61;
    d.haveStress = true;
    d.stressAgoDays = 0;
    d.stressHighSec = 2 * 3600 + 40 * 60;
    d.recoveryHighSec = 65 * 60;
    d.haveBedtime = false;
    d.haveHist = true;
    static const int32_t hist[HIST_DAYS] = {
        27000, 23400, 30600, 18000, 25200, 32400, 0,     24300,
        29100, 21600, 26100, 31500, 23700, 26640};
    memcpy(d.histSec, hist, sizeof(hist));
    d.histNights = 13;
    d.sleepDebtSec = 3 * 3600 + 10 * 60;
    view.d = d;
    view.updatedAt = time(nullptr);
    haveData = true;
    lastGoodFetch = millis();
}

// Screen power: HA override wins, then a tap-to-wake window, then the
// night schedule.
static bool screenShouldBeOn() {
    if (screenMode() == SCREEN_FORCED_ON) return true;
    if (screenMode() == SCREEN_FORCED_OFF) return false;
    if (millis() < wakeUntilMs) return true;
    if (demoActive()) return true;
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
        lastFetch = 0;  // refresh soon after wake
        render();
    }
}

// Short tap: wake the screen, or cycle pages (skipping empty ones).
// Long press: dismiss the active cue, else drop a forced screen to auto.
static void pollTouch() {
    if (!haveData) return;
    uint32_t now = millis();
    int lvl = digitalRead(TOUCH_IRQ_PIN);

    if (lvl == LOW) {
        if (!pressed) {
            pressed = true;
            longFired = false;
            pressStartMs = now;
        } else if (!longFired && now - pressStartMs >= LONGPRESS_MS) {
            longFired = true;
            if (webApiCue() != CUE_NONE) {
                webApiClearCue();
            } else if (screenMode() != SCREEN_AUTO) {
                webApiScreenAuto();
                applyScreenPower();
            }
            render();
        }
        return;
    }

    if (pressed) {
        pressed = false;
        if (longFired || now - lastTapMs < 300) return;
        lastTapMs = now;
        if (!screenOn) {
            wakeUntilMs = now + WAKE_TAP_MS;
            applyScreenPower();
            return;
        }
        for (int i = 0; i < PAGE_COUNT; i++) {
            page = (page + 1) % PAGE_COUNT;
            if (uiPageAvailable(view, page)) break;
        }
        pageFlipAt = now;
        render();
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
    webApiInit(&view);
    webApiSetScreenshotCb([](WiFiClient &c, int p) {
        view.cue = webApiCue();
        view.status = currentStatus();
        uiStreamScreenshot(c, view, p >= 0 && p < PAGE_COUNT ? (uint8_t)p : page);
    });
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

    int dPage, dMin;
    if (webApiDemoPending(&dPage, &dMin)) {
        demoUntilMs = millis() + (uint32_t)dMin * 60000UL;
        loadDemoData();
        if (dPage >= 0 && dPage < PAGE_COUNT) page = (uint8_t)dPage;
        applyScreenPower();
        render();
        publishMeta();
        Serial.printf("[demo] armed for %d min, page %d\n", dMin, dPage);
    }
    if (demoUntilMs != 0 && millis() >= demoUntilMs) {
        demoUntilMs = 0;
        haveData = false;  // force a real refetch below
        lastFetch = 0;
    }

    applyScreenPower();
    pollTouch();
    ledTask();
    uint32_t now = millis();

    // Re-render when the HA cue changes (POST /api/cue or auto-expiry).
    static uint8_t prevCue = CUE_NONE;
    uint8_t curCue = webApiCue();
    if (curCue != prevCue) {
        prevCue = curCue;
        render();
    }

    // Auto-return to the main page after a while on any other page.
    if (screenOn && page != PAGE_MAIN && now - pageFlipAt > PAGE2_RETURN_MS) {
        page = PAGE_MAIN;
        render();
    }

    if (WiFi.status() != WL_CONNECTED) {
        delay(250);
        return;
    }

    // Fetch on cadence even with the screen off (#9) so HA stays fresh;
    // failed fetches back off for FETCH_RETRY_MS instead of spinning.
    if (!demoActive()) {
        uint32_t wait = haveData ? (screenOn ? REFRESH_MS : REFRESH_OFF_MS)
                                 : FETCH_RETRY_MS;
        if (lastFetch == 0 || now - lastFetch >= wait) {
            lastFetch = now;
            refreshBoard();
            publishMeta();
            if (haveData) render();
        }
    }

    delay(25);
}
