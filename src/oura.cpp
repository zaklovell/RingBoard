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

// Local dates for the query window: a few days back so the latest documents
// are always in range even right after midnight or before the morning sync.
// The window ends tomorrow, not today: Oura computes days in the app's
// timezone (which may differ from this board's TZ) and end_date isn't
// documented as inclusive, so pad a day. Future days just return nothing.
static bool dateRange(char *startD, size_t sn, char *endD, size_t en) {
    struct tm t;
    if (!getLocalTime(&t, 100)) return false;
    time_t fwd = time(nullptr) + 86400;
    localtime_r(&fwd, &t);
    strftime(endD, en, "%Y-%m-%d", &t);
    time_t back = time(nullptr) - 3 * 86400;
    localtime_r(&back, &t);
    strftime(startD, sn, "%Y-%m-%d", &t);
    return true;
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

// Pick the newest day explicitly; array order isn't documented.
static bool fetchScore(const char *endpoint, const char *startD,
                       const char *endD, int *score) {
    JsonDocument filter;
    filter["data"][0]["score"] = true;
    filter["data"][0]["day"] = true;
    JsonDocument doc;
    if (!ouraGet(endpoint, startD, endD, filter, &doc)) return false;
    const char *bestDay = "";
    int best = 0;
    for (JsonObject o : doc["data"].as<JsonArray>()) {
        const char *day = o["day"] | "";
        if (day[0] && strcmp(day, bestDay) >= 0) {
            bestDay = day;
            best = o["score"] | 0;
        }
    }
    *score = best;
    return best > 0;
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
    JsonDocument doc;
    if (!ouraGet("sleep", startD, endD, filter, &doc)) return false;

    // Newest day wins; on a tie prefer long_sleep over an unscored "sleep"
    // period, then the longest. Naps ("late_nap", "rest") never qualify.
    const char *bestDay = "";
    bool bestLong = false;
    int32_t bestTotal = -1;
    JsonObject best;
    for (JsonObject s : doc["data"].as<JsonArray>()) {
        const char *day = s["day"] | "";
        const char *type = s["type"] | "";
        int32_t total = s["total_sleep_duration"] | 0;
        bool isLong = strcmp(type, "long_sleep") == 0;
        if (!day[0] || (!isLong && strcmp(type, "sleep") != 0)) continue;
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
    if (bestTotal < 0) return false;
    out->totalSleepSec = bestTotal;
    out->deepSec = best["deep_sleep_duration"] | 0;
    out->remSec = best["rem_sleep_duration"] | 0;
    out->lightSec = best["light_sleep_duration"] | 0;
    out->awakeSec = best["awake_time"] | 0;
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
    f["sedentary_time"] = true;
    JsonDocument doc;
    if (!ouraGet("daily_activity", startD, endD, filter, &doc)) return false;
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
    if (!found) return false;
    out->activityScore = best["score"] | 0;
    out->steps = best["steps"] | 0;
    out->distanceM = best["equivalent_walking_distance"] | 0;
    out->activeCal = best["active_calories"] | 0;
    out->totalCal = best["total_calories"] | 0;
    out->sedentarySec = best["sedentary_time"] | 0;
    return true;
}

bool ouraFetchAll(OuraData *out) {
    char startD[12], endD[12];
    if (!dateRange(startD, sizeof(startD), endD, sizeof(endD))) {
        Serial.println("[oura] clock not set yet, skipping fetch");
        return false;
    }
    if (!ensureToken()) return false;

    out->haveReadiness =
        fetchScore("daily_readiness", startD, endD, &out->readinessScore);
    out->haveSleep = fetchScore("daily_sleep", startD, endD, &out->sleepScore);
    out->haveSleepDetail = fetchSleepDetail(startD, endD, out);
    out->haveActivity = fetchActivity(startD, endD, out);

    bool any = out->haveReadiness || out->haveSleep || out->haveSleepDetail ||
               out->haveActivity;
    if (any) netFailed = false;
    return any;
}
