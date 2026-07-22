// Oura API v2 client: OAuth refresh-token management plus filtered JSON
// fetches, following the planes board's TLS + stream-filter pattern so big
// responses (sleep docs carry 5-min phase strings) never sit in RAM.
//
// Token lifecycle: tools/oura_auth.py seeds OURA_REFRESH_TOKEN in secrets.h.
// Oura rotates the refresh token on every use, so the current one lives in
// NVS and survives reflashes; the compiled seed is only a first-boot
// fallback. In sandbox mode no auth is needed at all.
#include "oura.h"
#include "config.h"
#include "secrets.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <time.h>

static Preferences prefs;
static char accessToken[192] = "";
static char refreshToken[192] = "";
static uint32_t tokenExpiryMs = 0;
static bool authFailed = false;
static bool netFailed = false;

static int callsToday = 0;
static int budgetDay = -1;

int ouraCallsToday() { return callsToday; }

const char *ouraAuthState() {
    if (authFailed) return "auth";
    if (netFailed) return "net";
    return "ok";
}

static void resetBudgetIfNewDay() {
    struct tm t;
    if (getLocalTime(&t, 50) && t.tm_yday != budgetDay) {
        budgetDay = t.tm_yday;
        callsToday = 0;
    }
}

void ouraInit() {
    prefs.begin("ringboard", false);
    String stored = prefs.getString("rtok", "");
    if (stored.length() > 0) {
        snprintf(refreshToken, sizeof(refreshToken), "%s", stored.c_str());
        Serial.println("[oura] refresh token loaded from NVS");
    } else {
        snprintf(refreshToken, sizeof(refreshToken), "%s", OURA_REFRESH_TOKEN);
        Serial.println("[oura] using compiled seed refresh token");
    }
}

void ouraSetRefreshToken(const char *token) {
    snprintf(refreshToken, sizeof(refreshToken), "%s", token);
    prefs.putString("rtok", refreshToken);
    accessToken[0] = '\0';
    tokenExpiryMs = 0;
    authFailed = false;
    netFailed = false;
    Serial.println("[oura] refresh token replaced via web API");
}

static bool refreshAccessToken() {
    if (refreshToken[0] == '\0') {
        Serial.println("[oura] no refresh token; run tools/oura_auth.py");
        authFailed = true;
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    http.setConnectTimeout(8000);
    http.setTimeout(15000);
    http.setUserAgent(USER_AGENT);
    if (!http.begin(client, OURA_TOKEN_URL)) return false;
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    char body[512];
    snprintf(body, sizeof(body),
             "grant_type=refresh_token&refresh_token=%s"
             "&client_id=%s&client_secret=%s",
             refreshToken, OURA_CLIENT_ID, OURA_CLIENT_SECRET);
    int code = http.POST((uint8_t *)body, strlen(body));
    if (code != 200) {
        Serial.printf("[oura] token refresh -> HTTP %d\n", code);
        http.end();
        // 4xx means the refresh token is dead (revoked or lost rotation);
        // anything else is transient network/server trouble.
        if (code >= 400 && code < 500) authFailed = true;
        else netFailed = true;
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("[oura] token json error: %s\n", err.c_str());
        netFailed = true;
        return false;
    }

    const char *at = doc["access_token"] | "";
    const char *rt = doc["refresh_token"] | "";
    uint32_t expiresIn = doc["expires_in"] | 0;
    if (!at[0]) {
        netFailed = true;
        return false;
    }
    snprintf(accessToken, sizeof(accessToken), "%s", at);
    if (expiresIn > TOKEN_SLACK_SEC) {
        tokenExpiryMs = millis() + (expiresIn - TOKEN_SLACK_SEC) * 1000UL;
    } else {
        tokenExpiryMs = millis() + 30UL * 60UL * 1000UL;
    }
    if (rt[0] && strcmp(rt, refreshToken) != 0) {
        snprintf(refreshToken, sizeof(refreshToken), "%s", rt);
        prefs.putString("rtok", refreshToken);
        Serial.println("[oura] rotated refresh token saved to NVS");
    }
    authFailed = false;
    netFailed = false;
    Serial.printf("[oura] access token refreshed, expires in %lus\n",
                  (unsigned long)expiresIn);
    return true;
}

static bool ensureToken() {
#if OURA_USE_SANDBOX
    return true;
#else
    if (accessToken[0] && (int32_t)(tokenExpiryMs - millis()) > 0) return true;
    return refreshAccessToken();
#endif
}

// Local dates for the query window: HIST_DAYS back so the sleep fetch covers
// the whole debt history (the score fetches still just pick the newest day).
// The window ends tomorrow, not today: Oura computes days in the app's
// timezone (which may differ from this board's TZ) and end_date isn't
// documented as inclusive, so pad a day. Future days just return nothing.
static bool dateRange(char *startD, size_t sn, char *endD, size_t en) {
    struct tm t;
    if (!getLocalTime(&t, 100)) return false;
    time_t fwd = time(nullptr) + 86400;
    localtime_r(&fwd, &t);
    strftime(endD, en, "%Y-%m-%d", &t);
    time_t back = time(nullptr) - (time_t)HIST_DAYS * 86400;
    localtime_r(&back, &t);
    strftime(startD, sn, "%Y-%m-%d", &t);
    return true;
}

// "YYYY-MM-DD" -> whole days before today (0 = today), or -1 if unparseable.
// Both sides anchored to noon so DST shifts can't skew the division.
static int daysAgo(const char *day) {
    int y, m, d;
    if (sscanf(day, "%d-%d-%d", &y, &m, &d) != 3) return -1;
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    time_t then = mktime(&t);
    struct tm nowTm;
    if (then == (time_t)-1 || !getLocalTime(&nowTm, 50)) return -1;
    nowTm.tm_hour = 12;
    nowTm.tm_min = 0;
    nowTm.tm_sec = 0;
    time_t noonToday = mktime(&nowTm);
    long diff = (long)(noonToday - then);
    if (diff < 0) return -1;
    return (int)((diff + 43200) / 86400);
}

static bool ouraGet(const char *endpoint, const char *startD, const char *endD,
                    const JsonDocument &filter, JsonDocument *doc) {
    resetBudgetIfNewDay();
    if (callsToday >= OURA_DAILY_BUDGET) {
        Serial.println("[oura] daily budget reached, skipping fetch");
        return false;
    }
    callsToday++;

    char url[224];
#if OURA_USE_SANDBOX
    snprintf(url, sizeof(url),
             OURA_API_BASE "/v2/sandbox/usercollection/%s"
             "?start_date=%s&end_date=%s",
             endpoint, startD, endD);
#else
    snprintf(url, sizeof(url),
             OURA_API_BASE "/v2/usercollection/%s?start_date=%s&end_date=%s",
             endpoint, startD, endD);
#endif

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    http.setConnectTimeout(8000);
    http.setTimeout(15000);
    http.setUserAgent(USER_AGENT);
    if (!http.begin(client, url)) return false;
    // The sandbox needs an Authorization header too; any string satisfies it.
    char auth[224];
#if OURA_USE_SANDBOX
    snprintf(auth, sizeof(auth), "Bearer sandbox");
#else
    snprintf(auth, sizeof(auth), "Bearer %s", accessToken);
#endif
    http.addHeader("Authorization", auth);
    http.addHeader("Accept", "application/json");

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[oura] %s -> HTTP %d\n", endpoint, code);
        http.end();
        if (code == 401) accessToken[0] = '\0';  // force re-refresh next time
        return false;
    }

    DeserializationError err = deserializeJson(
        *doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("[oura] %s json error: %s\n", endpoint, err.c_str());
        return false;
    }
    return true;
}

// ---- Today's Focus: contributor -> cue mapping ----------------------------
// Each watched contributor maps to one conservative cue; the lowest
// contributor across the newest readiness + sleep documents wins.
struct ContribMap {
    const char *key;
    uint8_t cue;
    const char *reason;
};
static const ContribMap kReadinessMap[] = {
    {"sleep_balance", FOCUS_BEDTIME, "Sleep balance is your lowest factor"},
    {"previous_night", FOCUS_BEDTIME, "Last night ran short"},
    {"hrv_balance", FOCUS_EASY, "HRV balance is below your norm"},
    {"resting_heart_rate", FOCUS_EASY, "Resting heart rate is elevated"},
    {"recovery_index", FOCUS_EASY, "Recovery index is lagging"},
    {"body_temperature", FOCUS_EASY, "Body temperature is off baseline"},
    {"activity_balance", FOCUS_MOVE, "Activity balance is trailing"},
    {"previous_day_activity", FOCUS_MOVE, "Yesterday was a light day"},
};
static const ContribMap kSleepMap[] = {
    {"total_sleep", FOCUS_BEDTIME, "Total sleep is your lowest factor"},
    {"efficiency", FOCUS_WINDDOWN, "Sleep efficiency is low"},
    {"latency", FOCUS_WINDDOWN, "Falling asleep is taking a while"},
    {"restfulness", FOCUS_WINDDOWN, "Restfulness is low"},
    {"timing", FOCUS_WINDDOWN, "Sleep timing is drifting"},
};
static const int kNReadiness = sizeof(kReadinessMap) / sizeof(kReadinessMap[0]);
static const int kNSleep = sizeof(kSleepMap) / sizeof(kSleepMap[0]);

const char *focusReasonText(uint8_t idx) {
    if (idx < kNReadiness) return kReadinessMap[idx].reason;
    if (idx < kNReadiness + kNSleep) return kSleepMap[idx - kNReadiness].reason;
    return "";
}

// Newest-day contributor values, parallel to the map tables; -1 = absent.
struct ContribVals {
    int8_t v[8];
    void clear() { for (int8_t &x : v) x = -1; }
};

// Pick the newest day explicitly (array order isn't documented), fill the
// per-day history for trend averages, and capture the newest document's
// contributors for the Focus cue.
static bool fetchScore(const char *endpoint, const char *startD,
                       const char *endD, int *score, int *agoDays,
                       int8_t hist[HIST_DAYS], const ContribMap *cmap,
                       int nmap, ContribVals *cv) {
    JsonDocument filter;
    filter["data"][0]["score"] = true;
    filter["data"][0]["day"] = true;
    if (cmap) filter["data"][0]["contributors"] = true;
    JsonDocument doc;
    if (!ouraGet(endpoint, startD, endD, filter, &doc)) return false;
    const char *bestDay = "";
    int best = 0;
    JsonObject bestObj;
    for (JsonObject o : doc["data"].as<JsonArray>()) {
        const char *day = o["day"] | "";
        if (!day[0]) continue;
        int ago = daysAgo(day);
        int s = o["score"] | 0;
        if (hist && ago >= 0 && ago < HIST_DAYS && s > 0) {
            hist[HIST_DAYS - 1 - ago] = (int8_t)s;
        }
        if (strcmp(day, bestDay) >= 0) {
            bestDay = day;
            best = s;
            bestObj = o;
        }
    }
    if (best <= 0) return false;
    *score = best;
    if (agoDays) *agoDays = daysAgo(bestDay);
    if (cmap && cv) {
        cv->clear();
        JsonObject c = bestObj["contributors"];
        for (int i = 0; i < nmap && i < 8; i++) {
            cv->v[i] = (int8_t)(c[cmap[i].key] | -1);
        }
    }
    return true;
}

// Long-sleep document with the biggest total wins; naps lose.
static bool fetchSleepDetail(const char *startD, const char *endD,
                             OuraData *out) {
    JsonDocument filter;
    JsonObject f = filter["data"][0].to<JsonObject>();
    f["day"] = true;
    f["type"] = true;
    f["total_sleep_duration"] = true;
    f["deep_sleep_duration"] = true;
    f["rem_sleep_duration"] = true;
    f["light_sleep_duration"] = true;
    f["awake_time"] = true;
    f["average_hrv"] = true;
    f["lowest_heart_rate"] = true;
    JsonDocument doc;
    if (!ouraGet("sleep", startD, endD, filter, &doc)) return false;

    // Newest day wins; on a tie prefer long_sleep over an unscored "sleep"
    // period, then the longest. Naps ("late_nap", "rest") never qualify.
    // The same pass accumulates per-night totals (naps included, "rest"
    // excluded) into the debt history.
    const char *bestDay = "";
    bool bestLong = false;
    int32_t bestTotal = -1;
    JsonObject best;
    for (JsonObject s : doc["data"].as<JsonArray>()) {
        const char *day = s["day"] | "";
        const char *type = s["type"] | "";
        int32_t total = s["total_sleep_duration"] | 0;
        bool isLong = strcmp(type, "long_sleep") == 0;
        bool isSleep = isLong || strcmp(type, "sleep") == 0 ||
                       strcmp(type, "late_nap") == 0;
        if (!day[0] || !isSleep || total <= 0) continue;

        int ago = daysAgo(day);
        if (ago >= 0 && ago < HIST_DAYS) {
            out->histSec[HIST_DAYS - 1 - ago] += total;
            out->haveHist = true;
        }

        if (!isLong && strcmp(type, "sleep") != 0) continue;
        int cmp = strcmp(day, bestDay);
        bool better = cmp > 0;
        if (cmp == 0) {
            better = (isLong != bestLong) ? isLong : total > bestTotal;
        }
        if (better) {
            bestDay = day;
            bestLong = isLong;
            bestTotal = total;
            best = s;
        }
    }

    // Debt: run oldest to newest, surplus pays existing debt down but never
    // banks ahead, so the total floors at zero after every night with data.
    int32_t debt = 0;
    int nights = 0;
    for (int i = 0; i < HIST_DAYS; i++) {
        if (out->histSec[i] <= 0) continue;
        nights++;
        debt += SLEEP_NEED_SEC - out->histSec[i];
        if (debt < 0) debt = 0;
    }
    out->sleepDebtSec = debt;
    out->histNights = nights;

    if (bestTotal < 0) return false;
    out->sleepAgoDays = daysAgo(bestDay);
    out->totalSleepSec = bestTotal;
    out->deepSec = best["deep_sleep_duration"] | 0;
    out->remSec = best["rem_sleep_duration"] | 0;
    out->lightSec = best["light_sleep_duration"] | 0;
    out->awakeSec = best["awake_time"] | 0;
    out->avgHrv = (int)(best["average_hrv"] | 0.0f);
    out->lowestHr = (int)(best["lowest_heart_rate"] | 0.0f);
    return true;
}

static bool fetchActivity(const char *startD, const char *endD,
                          OuraData *out) {
    JsonDocument filter;
    JsonObject f = filter["data"][0].to<JsonObject>();
    f["day"] = true;
    f["score"] = true;
    f["steps"] = true;
    f["equivalent_walking_distance"] = true;
    f["active_calories"] = true;
    f["total_calories"] = true;
    f["target_calories"] = true;
    f["sedentary_time"] = true;
    JsonDocument doc;
    if (!ouraGet("daily_activity", startD, endD, filter, &doc)) return false;
    const char *bestDay = "";
    JsonObject best;
    bool found = false;
    for (JsonObject o : doc["data"].as<JsonArray>()) {
        const char *day = o["day"] | "";
        if (!day[0]) continue;
        int ago = daysAgo(day);
        int s = o["score"] | 0;
        if (ago >= 0 && ago < HIST_DAYS && s > 0) {
            out->activityHist[HIST_DAYS - 1 - ago] = (int8_t)s;
        }
        if (strcmp(day, bestDay) >= 0) {
            bestDay = day;
            best = o;
            found = true;
        }
    }
    if (!found) return false;
    out->activityAgoDays = daysAgo(bestDay);
    out->activityScore = best["score"] | 0;
    out->steps = best["steps"] | 0;
    out->distanceM = best["equivalent_walking_distance"] | 0;
    out->activeCal = best["active_calories"] | 0;
    out->totalCal = best["total_calories"] | 0;
    out->targetCal = best["target_calories"] | 0;
    out->sedentarySec = best["sedentary_time"] | 0;
    return true;
}

// ---- Slow endpoints: cached inside this module on their own cadence -------
// daily_stress at most hourly, sleep_time at most every 12h, so neither
// inflates the per-refresh call count.
static struct {
    bool have = false;
    int agoDays = -1;
    int32_t stressHigh = -1, recoveryHigh = -1;
    uint32_t fetchedAt = 0;
} stressCache;

static struct {
    bool have = false;
    int32_t startSec = 0, endSec = 0;
    uint32_t fetchedAt = 0;
} bedtimeCache;

static void maybeFetchStress(const char *startD, const char *endD) {
    if (stressCache.fetchedAt != 0 &&
        millis() - stressCache.fetchedAt < STRESS_EVERY_MS) {
        return;
    }
    stressCache.fetchedAt = millis();  // even on failure: no hammering
    JsonDocument filter;
    JsonObject f = filter["data"][0].to<JsonObject>();
    f["day"] = true;
    f["stress_high"] = true;
    f["recovery_high"] = true;
    JsonDocument doc;
    if (!ouraGet("daily_stress", startD, endD, filter, &doc)) return;
    const char *bestDay = "";
    JsonObject best;
    bool found = false;
    for (JsonObject o : doc["data"].as<JsonArray>()) {
        const char *day = o["day"] | "";
        if (day[0] && strcmp(day, bestDay) >= 0) {
            bestDay = day;
            best = o;
            found = true;
        }
    }
    if (!found) return;
    stressCache.have = true;
    stressCache.agoDays = daysAgo(bestDay);
    stressCache.stressHigh = best["stress_high"] | -1;
    stressCache.recoveryHigh = best["recovery_high"] | -1;
}

static void maybeFetchBedtime(const char *startD, const char *endD) {
    if (bedtimeCache.fetchedAt != 0 &&
        millis() - bedtimeCache.fetchedAt < BEDTIME_EVERY_MS) {
        return;
    }
    bedtimeCache.fetchedAt = millis();
    JsonDocument filter;
    JsonObject f = filter["data"][0].to<JsonObject>();
    f["day"] = true;
    f["optimal_bedtime"]["start_offset"] = true;
    f["optimal_bedtime"]["end_offset"] = true;
    JsonDocument doc;
    if (!ouraGet("sleep_time", startD, endD, filter, &doc)) return;
    const char *bestDay = "";
    JsonObject best;
    bool found = false;
    for (JsonObject o : doc["data"].as<JsonArray>()) {
        const char *day = o["day"] | "";
        if (day[0] && strcmp(day, bestDay) >= 0 &&
            !o["optimal_bedtime"].isNull()) {
            bestDay = day;
            best = o;
            found = true;
        }
    }
    // Guidance is often simply absent (needs history/eligibility) — that's
    // fine, the debt page falls back to the configured wake-time arithmetic.
    if (!found) return;
    bedtimeCache.have = true;
    bedtimeCache.startSec = best["optimal_bedtime"]["start_offset"] | 0;
    bedtimeCache.endSec = best["optimal_bedtime"]["end_offset"] | 0;
}

// Lowest contributor across both newest documents drives the Focus cue.
// Anything >= 80 is healthy — then there's nothing to fix and the Focus
// page gets to say so.
static void deriveFocus(OuraData *out, const ContribVals &rv, bool haveR,
                        const ContribVals &sv, bool haveS) {
    int minVal = 127;
    int cue = FOCUS_NONE, reason = 0;
    if (haveR) {
        for (int i = 0; i < kNReadiness && i < 8; i++) {
            if (rv.v[i] >= 0 && rv.v[i] < minVal) {
                minVal = rv.v[i];
                cue = kReadinessMap[i].cue;
                reason = i;
            }
        }
    }
    if (haveS) {
        for (int i = 0; i < kNSleep && i < 8; i++) {
            if (sv.v[i] >= 0 && sv.v[i] < minVal) {
                minVal = sv.v[i];
                cue = kSleepMap[i].cue;
                reason = kNReadiness + i;
            }
        }
    }
    if (minVal >= 80 || minVal == 127) {
        out->focusCue = FOCUS_NONE;
        out->focusReason = 0;
        out->focusValue = minVal == 127 ? 0 : minVal;
        return;
    }
    out->focusCue = (uint8_t)cue;
    out->focusReason = (uint8_t)reason;
    out->focusValue = minVal;
}

bool ouraFetchAll(OuraData *out) {
    char startD[12], endD[12];
    if (!dateRange(startD, sizeof(startD), endD, sizeof(endD))) {
        Serial.println("[oura] clock not set yet, skipping fetch");
        return false;
    }
    if (!ensureToken()) return false;

    ContribVals rv, sv;
    rv.clear();
    sv.clear();
    out->haveReadiness = fetchScore(
        "daily_readiness", startD, endD, &out->readinessScore,
        &out->readinessAgoDays, out->readinessHist, kReadinessMap,
        kNReadiness, &rv);
    out->haveSleep = fetchScore("daily_sleep", startD, endD, &out->sleepScore,
                                &out->sleepScoreAgoDays, out->sleepScoreHist,
                                kSleepMap, kNSleep, &sv);
    out->haveSleepDetail = fetchSleepDetail(startD, endD, out);
    out->haveActivity = fetchActivity(startD, endD, out);
    deriveFocus(out, rv, out->haveReadiness, sv, out->haveSleep);

    maybeFetchStress(startD, endD);
    out->haveStress = stressCache.have;
    out->stressAgoDays = stressCache.agoDays;
    out->stressHighSec = stressCache.stressHigh;
    out->recoveryHighSec = stressCache.recoveryHigh;

    maybeFetchBedtime(startD, endD);
    out->haveBedtime = bedtimeCache.have;
    out->bedtimeStartSec = bedtimeCache.startSec;
    out->bedtimeEndSec = bedtimeCache.endSec;

    bool any = out->haveReadiness || out->haveSleep || out->haveSleepDetail ||
               out->haveActivity;
    if (any) netFailed = false;
    return any;
}
