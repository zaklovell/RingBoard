// Oura-app-inspired dark theme on a 320x240 ST7789. Same three-band sprite
// rendering as the planes board: one reusable 320x80 16-bit sprite, three
// flicker-free pushes, ~51KB peak graphics RAM.
//
// Pages (short tap cycles, empty pages are skipped):
//   MAIN:   readiness/sleep/activity rings with 7-day trend deltas; total
//           sleep + stage bar + RHR/HRV; activity as distance-to-goal.
//   FOCUS:  one conservative cue from the lowest score contributor.
//   DEBT:   sleep debt vs need, bedtime plan line, 14-night bar chart.
//   STRESS: daily stress vs recovery time (hourly endpoint).
// An HA-pushed cue (SYNC RING / WIND DOWN / MOVE BREAK) renders as a banner
// across the top band; long-press dismisses it.
#include "ui.h"
#include "config.h"
#include "oura.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static TFT_eSPI *tft;
static TFT_eSprite *spr;

// Screenshot capture: when set, bandPush streams pixels here instead of
// pushing to the panel.
static WiFiClient *cap = nullptr;

// Bedtime plan config (NVS-backed, set from webapi via uiSetPlan).
static int planWakeMin = DEFAULT_WAKE_MIN;
static int32_t planNeedSec = SLEEP_NEED_SEC;
void uiSetPlan(int wakeMin, int32_t needSec) {
    if (wakeMin >= 0 && wakeMin < 24 * 60) planWakeMin = wakeMin;
    if (needSec >= 4L * 3600L && needSec <= 12L * 3600L) planNeedSec = needSec;
}

// Oura's palette: near-black navy, soft grey-blue subtext, teal for optimal.
static const uint16_t COL_BG = RGB565(10, 14, 22);
static const uint16_t COL_TEXT = 0xFFFF;
static const uint16_t COL_SUB = RGB565(136, 148, 168);
static const uint16_t COL_DIM = RGB565(112, 122, 140);
static const uint16_t COL_LINE = RGB565(38, 46, 62);
static const uint16_t COL_TEAL = RGB565(64, 211, 198);    // optimal, 85+
static const uint16_t COL_BLUE = RGB565(116, 166, 255);   // good, 70-84
static const uint16_t COL_AMBER = RGB565(255, 159, 10);   // fair, 60-69
static const uint16_t COL_RED = RGB565(255, 89, 79);      // pay attention
static const uint16_t COL_DEEP = RGB565(73, 100, 246);    // sleep stages
static const uint16_t COL_REM = RGB565(64, 211, 198);
static const uint16_t COL_LIGHT = RGB565(150, 178, 255);
static const uint16_t COL_AWAKE = RGB565(90, 100, 118);
static const uint16_t COL_GREEN = RGB565(74, 222, 128);

static const int W = 320;
static const int BAND_H = 80;

void uiInit(TFT_eSPI *t) {
    tft = t;
    tft->init();
    tft->setRotation(3);  // 180° from stock: board sits upside down on the desk
    tft->fillScreen(COL_BG);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    spr = new TFT_eSprite(tft);
    spr->setColorDepth(16);
    if (!spr->createSprite(W, BAND_H)) {
        Serial.println("[ui] sprite alloc failed");
    }
    spr->setTextWrap(false);
}

void uiBacklight(bool on) { digitalWrite(TFT_BL, on ? HIGH : LOW); }

void uiBoot(const char *line1, const char *line2) {
    tft->fillScreen(COL_BG);
    tft->setTextDatum(MC_DATUM);
    tft->setFreeFont(&FreeSansBold18pt7b);
    tft->setTextColor(COL_TEXT, COL_BG);
    tft->drawString(line1, W / 2, 100);
    tft->setFreeFont(&FreeSans9pt7b);
    tft->setTextColor(COL_SUB, COL_BG);
    tft->drawString(line2, W / 2, 150);
}

static void bandStart() { spr->fillSprite(COL_BG); }

static uint32_t capDeadline = 0;

static void bandPush(int y) {
    if (cap) {
        // Stream this band's raw RGB565 out instead of touching the panel.
        const uint8_t *buf = (const uint8_t *)spr->getPointer();
        size_t left = (size_t)W * BAND_H * 2;
        while (left > 0 && cap->connected()) {
            if ((int32_t)(capDeadline - millis()) <= 0) {
                cap->stop();  // don't leave a short 200 response dangling
                break;
            }
            size_t n = cap->write(buf, left > 1436 ? 1436 : left);
            if (n == 0) {
                cap->stop();
                break;
            }
            buf += n;
            left -= n;
        }
        return;
    }
    spr->pushSprite(0, y);
}

static uint16_t scoreColor(int s) {
    if (s >= 85) return COL_TEAL;
    if (s >= 70) return COL_BLUE;
    if (s >= 60) return COL_AMBER;
    return COL_RED;
}

static void statusDot(uint8_t st) {
    uint16_t c = st == ST_OK ? COL_TEAL : (st == ST_STALE ? COL_AMBER : COL_RED);
    spr->fillSmoothCircle(308, 10, 3, c, COL_BG);
}

static void fmtThousands(int32_t v, char *out, size_t n) {
    if (v >= 1000) {
        snprintf(out, n, "%ld,%03ld", (long)(v / 1000), (long)(v % 1000));
    } else {
        snprintf(out, n, "%ld", (long)v);
    }
}

// "7h 42m" for big numbers, "1:12" for the stage legend.
static void fmtHoursMin(int32_t sec, char *out, size_t n) {
    snprintf(out, n, "%ldh %ldm", (long)(sec / 3600), (long)((sec % 3600) / 60));
}

static void fmtClock(int32_t sec, char *out, size_t n) {
    snprintf(out, n, "%ld:%02ld", (long)(sec / 3600), (long)((sec % 3600) / 60));
}

// Minutes-past-midnight -> "10:38 PM" (12h, wraps around midnight).
static void fmtWallMin(int minutes, char *out, size_t n) {
    while (minutes < 0) minutes += 24 * 60;
    minutes %= 24 * 60;
    int h24 = minutes / 60;
    int h = h24 % 12;
    if (h == 0) h = 12;
    snprintf(out, n, "%d:%02d %s", h, minutes % 60, h24 < 12 ? "AM" : "PM");
}

// ---- Freshness (#14): each metric greys out on its own document date ------
static bool metricStale(int agoDays) {
    if (agoDays < 0) return false;  // no data at all: "--" paths handle it
    struct tm t;
    if (!getLocalTime(&t, 10)) return false;  // clock unset: show what we have
    if (t.tm_hour < FRESH_AFTER_HOUR) return false;
    return agoDays != 0;
}

// ---- Trends (#3): score vs the average of the prior 7 logged days ---------
// Returns true and sets *delta when at least 3 of the last 7 prior days had
// scores; today's slot is excluded so the comparison is honest.
static bool trendDelta(const int8_t hist[HIST_DAYS], int score, int *delta) {
    int n = 0, sum = 0;
    for (int i = HIST_DAYS - 8; i < HIST_DAYS - 1; i++) {
        if (i >= 0 && hist[i] > 0) {
            n++;
            sum += hist[i];
        }
    }
    if (n < 3) return false;
    *delta = score - (sum + n / 2) / n;
    return true;
}

// Oura-style score ring: progress arc from 12 o'clock (TFT_eSPI puts 0
// degrees at 6 o'clock, so the top is 180), score inside, label + trend
// delta centered underneath.
static void drawRing(int cx, int cy, int r, const char *label, bool have,
                     int score, bool haveDelta, int delta) {
    int ir = r - 5;
    spr->drawSmoothArc(cx, cy, r, ir, 0, 360, COL_LINE, COL_BG, false);
    if (have) {
        int sweep = score * 360 / 100;
        if (sweep > 360) sweep = 360;
        if (sweep < 8) sweep = 8;
        uint16_t c = scoreColor(score);
        if (180 + sweep <= 360) {
            spr->drawSmoothArc(cx, cy, r, ir, 180, 180 + sweep, c, COL_BG, true);
        } else {
            spr->drawSmoothArc(cx, cy, r, ir, 180, 360, c, COL_BG, true);
            spr->drawSmoothArc(cx, cy, r, ir, 0, sweep - 180, c, COL_BG, true);
        }
        char num[8];
        snprintf(num, sizeof(num), "%d", score);
        spr->setTextDatum(MC_DATUM);
        spr->setFreeFont(&FreeSansBold12pt7b);
        spr->setTextColor(COL_TEXT, COL_BG);
        spr->drawString(num, cx, cy + 1);
    } else {
        spr->setTextDatum(MC_DATUM);
        spr->setFreeFont(&FreeSans9pt7b);
        spr->setTextColor(COL_SUB, COL_BG);
        spr->drawString("--", cx, cy + 1);
    }
    if (!label) return;

    // Label and optional trend arrow drawn as one centered group.
    spr->setTextFont(2);
    int labelW = spr->textWidth(label);
    char dnum[6] = "";
    int extra = 0, triW = 7, gap = 3;
    if (have && haveDelta && delta != 0) {
        snprintf(dnum, sizeof(dnum), "%d", delta < 0 ? -delta : delta);
        extra = gap + triW + 2 + spr->textWidth(dnum);
    }
    int x0 = cx - (labelW + extra) / 2;
    int ly = 64;
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString(label, x0, ly);
    if (extra > 0) {
        uint16_t dc = delta > 0 ? COL_TEAL : COL_RED;
        int tx = x0 + labelW + gap;
        int ty = ly + 5;  // triangle vertical center within the 16px font row
        if (delta > 0) {
            spr->fillTriangle(tx, ty + 6, tx + triW, ty + 6, tx + triW / 2, ty,
                              dc);
        } else {
            spr->fillTriangle(tx, ty, tx + triW, ty, tx + triW / 2, ty + 6, dc);
        }
        spr->setTextColor(dc, COL_BG);
        spr->drawString(dnum, tx + triW + 2, ly);
    }
}

// ---- HA cue banner (#11) --------------------------------------------------
static const char *cueText(uint8_t cue) {
    switch (cue) {
        case CUE_SYNC_RING: return "SYNC RING";
        case CUE_WIND_DOWN: return "WIND DOWN";
        case CUE_MOVE_BREAK: return "MOVE BREAK";
        default: return "";
    }
}

static uint16_t cueColor(uint8_t cue) {
    switch (cue) {
        case CUE_SYNC_RING: return COL_BLUE;
        case CUE_WIND_DOWN: return COL_AMBER;
        case CUE_MOVE_BREAK: return COL_GREEN;
        default: return COL_LINE;
    }
}

// Draws the banner across the top of the current sprite band; returns its
// height so callers can shift content down.
static int drawCueBanner(uint8_t cue) {
    if (cue == CUE_NONE) return 0;
    const int h = 22;
    spr->fillRect(0, 0, W, h, cueColor(cue));
    spr->setTextFont(2);
    spr->setTextDatum(ML_DATUM);
    spr->setTextColor(COL_BG, cueColor(cue));
    spr->drawString(cueText(cue), 12, h / 2 + 1);
    spr->setTextDatum(MR_DATUM);
    spr->drawString("hold to dismiss", W - 12, h / 2 + 1);
    return h;
}

// ---- Page MAIN ------------------------------------------------------------
static void drawBandA(const BoardView &v) {
    bandStart();
    int top = drawCueBanner(v.cue);
    if (top == 0) statusDot(v.status);

    bool rStale = metricStale(v.d.readinessAgoDays);
    bool sStale = metricStale(v.d.sleepScoreAgoDays);
    int rd = 0, sd = 0, ad = 0;
    bool haveRd = trendDelta(v.d.readinessHist, v.d.readinessScore, &rd);
    bool haveSd = trendDelta(v.d.sleepScoreHist, v.d.sleepScore, &sd);
    bool haveAd = trendDelta(v.d.activityHist, v.d.activityScore, &ad);

    if (top == 0) {
        drawRing(64, 34, 27, "READINESS", v.d.haveReadiness && !rStale,
                 v.d.readinessScore, haveRd, rd);
        drawRing(160, 34, 27, "SLEEP", v.d.haveSleep && !sStale,
                 v.d.sleepScore, haveSd, sd);
        drawRing(256, 34, 27, "ACTIVITY",
                 v.d.haveActivity && v.d.activityScore > 0 &&
                     !metricStale(v.d.activityAgoDays),
                 v.d.activityScore, haveAd, ad);
    } else {
        // Banner mode: smaller label-less rings tucked under the strip.
        drawRing(64, 50, 22, nullptr, v.d.haveReadiness && !rStale,
                 v.d.readinessScore, false, 0);
        drawRing(160, 50, 22, nullptr, v.d.haveSleep && !sStale,
                 v.d.sleepScore, false, 0);
        drawRing(256, 50, 22, nullptr,
                 v.d.haveActivity && v.d.activityScore > 0, v.d.activityScore,
                 false, 0);
    }
    bandPush(0);
}

// Stacked stage bar plus a colored-dot legend underneath.
static void drawStageBar(const OuraData &d) {
    const int bx = 18, bw = W - 36, by = 46, bh = 10;
    int32_t total = d.deepSec + d.remSec + d.lightSec + d.awakeSec;
    if (total <= 0) return;

    struct Seg {
        const char *name;
        int32_t sec;
        uint16_t col;
    } segs[] = {
        {"Deep", d.deepSec, COL_DEEP},
        {"REM", d.remSec, COL_REM},
        {"Light", d.lightSec, COL_LIGHT},
        {"Awake", d.awakeSec, COL_AWAKE},
    };

    int x = bx;
    for (const Seg &s : segs) {
        int w = (int)((int64_t)s.sec * bw / total);
        if (w > 0) spr->fillRect(x, by, w, bh, s.col);
        x += w;
    }
    if (x < bx + bw) spr->fillRect(x, by, bx + bw - x, bh, COL_AWAKE);

    x = bx;
    spr->setTextFont(2);
    spr->setTextDatum(TL_DATUM);
    for (const Seg &s : segs) {
        char t[24], hm[8];
        fmtClock(s.sec, hm, sizeof(hm));
        snprintf(t, sizeof(t), "%s %s", s.name, hm);
        spr->setTextColor(s.col, COL_BG);
        spr->drawString(t, x, 62);
        x += spr->textWidth(t) + 16;
    }
}

static void drawBandB(const BoardView &v) {
    bandStart();
    spr->drawFastHLine(12, 0, W - 24, COL_LINE);
    spr->setTextDatum(TL_DATUM);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString("TOTAL SLEEP", 18, 2);

    bool stale = metricStale(v.d.sleepAgoDays);
    if (v.d.haveSleepDetail && !stale) {
        // RHR / HRV from the same night, right-aligned on the caption row.
        if (v.d.lowestHr > 0 || v.d.avgHrv > 0) {
            char rec[32];
            snprintf(rec, sizeof(rec), "RHR %d  HRV %d", v.d.lowestHr,
                     v.d.avgHrv);
            spr->setTextDatum(TR_DATUM);
            spr->drawString(rec, 302, 2);
            spr->setTextDatum(TL_DATUM);
        }
        char total[16];
        fmtHoursMin(v.d.totalSleepSec, total, sizeof(total));
        spr->setTextDatum(ML_DATUM);
        spr->setFreeFont(&FreeSansBold18pt7b);
        spr->setTextColor(COL_TEXT, COL_BG);
        spr->drawString(total, 18, 30);
        drawStageBar(v.d);
    } else {
        spr->setTextDatum(ML_DATUM);
        spr->setFreeFont(&FreeSans12pt7b);
        spr->setTextColor(COL_SUB, COL_BG);
        spr->drawString(v.d.haveSleepDetail ? "Waiting for last night's sync"
                                            : "No sleep data yet",
                        18, 34);
        if (v.d.haveSleepDetail) {
            spr->setTextFont(2);
            spr->setTextColor(COL_DIM, COL_BG);
            spr->setTextDatum(TL_DATUM);
            spr->drawString("open the Oura app on your phone to sync", 18, 56);
        }
    }
    bandPush(80);
}

// "11:42 PM" from the wall clock of the last good fetch.
static void fmtUpdated(time_t ts, char *out, size_t n) {
    if (ts == 0) {
        out[0] = '\0';
        return;
    }
    struct tm t;
    localtime_r(&ts, &t);
    int h = t.tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(out, n, "%d:%02d %s", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
}

static void drawBandC(const BoardView &v) {
    bandStart();
    spr->drawFastHLine(12, 0, W - 24, COL_LINE);
    spr->setTextDatum(TL_DATUM);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString("ACTIVITY", 18, 2);

    char upd[24], stamp[32];
    fmtUpdated(v.updatedAt, upd, sizeof(upd));
    if (upd[0]) {
        snprintf(stamp, sizeof(stamp), "updated %s", upd);
        spr->setTextDatum(TR_DATUM);
        spr->drawString(stamp, 302, 2);
        spr->setTextDatum(TL_DATUM);
    }
    if (!v.d.haveActivity) {
        spr->setTextDatum(ML_DATUM);
        spr->setFreeFont(&FreeSans12pt7b);
        spr->setTextColor(COL_SUB, COL_BG);
        spr->drawString("No activity data yet", 18, 44);
        bandPush(160);
        return;
    }

    // Distance to Oura's own daily target (#2), with a progress bar.
    char buf[48];
    if (v.d.targetCal > 0) {
        int remaining = v.d.targetCal - v.d.activeCal;
        spr->setTextDatum(ML_DATUM);
        if (remaining > 0) {
            char num[12];
            fmtThousands(remaining, num, sizeof(num));
            spr->setFreeFont(&FreeSansBold12pt7b);
            spr->setTextColor(COL_TEXT, COL_BG);
            spr->drawString(num, 18, 30);
            int nw = spr->textWidth(num);
            spr->setTextFont(2);
            spr->setTextColor(COL_SUB, COL_BG);
            spr->drawString("active cal to goal", 18 + nw + 6, 33);
        } else {
            spr->setFreeFont(&FreeSansBold12pt7b);
            spr->setTextColor(COL_TEAL, COL_BG);
            spr->drawString("Goal met", 18, 30);
            int nw = spr->textWidth("Goal met");
            spr->setTextFont(2);
            spr->setTextColor(COL_SUB, COL_BG);
            snprintf(buf, sizeof(buf), "%d / %d cal", v.d.activeCal,
                     v.d.targetCal);
            spr->drawString(buf, 18 + nw + 8, 33);
        }
        int bx = 18, bw = W - 36, by = 46, bh = 7;
        spr->fillRect(bx, by, bw, bh, COL_LINE);
        int fill = (int)((int64_t)v.d.activeCal * bw /
                         (v.d.targetCal > 0 ? v.d.targetCal : 1));
        if (fill > bw) fill = bw;
        if (fill > 0) {
            spr->fillRect(bx, by, fill, bh,
                          remaining > 0 ? COL_BLUE : COL_TEAL);
        }
        char steps[16], sed[12];
        fmtThousands(v.d.steps, steps, sizeof(steps));
        fmtHoursMin(v.d.sedentarySec, sed, sizeof(sed));
        snprintf(buf, sizeof(buf), "%s steps  %.1f mi  %s inactive", steps,
                 v.d.distanceM / 1609.34f, sed);
        spr->setTextFont(2);
        spr->setTextColor(COL_DIM, COL_BG);
        spr->setTextDatum(TL_DATUM);
        spr->drawString(buf, 18, 60);
    } else {
        // No target in the response: fall back to plain numbers.
        char steps[16];
        fmtThousands(v.d.steps, steps, sizeof(steps));
        spr->setTextDatum(ML_DATUM);
        spr->setFreeFont(&FreeSansBold12pt7b);
        spr->setTextColor(COL_TEXT, COL_BG);
        spr->drawString(steps, 18, 34);
        int nw = spr->textWidth(steps);
        spr->setTextFont(2);
        spr->setTextColor(COL_SUB, COL_BG);
        spr->drawString("steps", 18 + nw + 6, 37);
        snprintf(buf, sizeof(buf), "%.1f mi  %d cal", v.d.distanceM / 1609.34f,
                 v.d.activeCal);
        spr->setTextDatum(TL_DATUM);
        spr->drawString(buf, 18, 58);
    }
    bandPush(160);
}

// ---- Page FOCUS (#1) ------------------------------------------------------
static const char *focusTitle(uint8_t cue) {
    switch (cue) {
        case FOCUS_BEDTIME: return "Protect bedtime";
        case FOCUS_EASY: return "Take it easy today";
        case FOCUS_MOVE: return "Get moving today";
        case FOCUS_WINDDOWN: return "Wind down earlier";
        default: return "All in balance";
    }
}

static void focusSuggestion(const BoardView &v, char *out, size_t n) {
    switch (v.d.focusCue) {
        case FOCUS_BEDTIME: {
            char bed[16];
            fmtWallMin(planWakeMin - (int)(planNeedSec / 60), bed, sizeof(bed));
            snprintf(out, n, "SUGGESTED  IN BED BY %s", bed);
            break;
        }
        case FOCUS_EASY:
            snprintf(out, n, "SUGGESTED  LIGHT DAY, EARLY NIGHT");
            break;
        case FOCUS_MOVE:
            snprintf(out, n, "SUGGESTED  A WALK BEFORE EVENING");
            break;
        case FOCUS_WINDDOWN:
            snprintf(out, n, "SUGGESTED  SCREENS OFF 30 MIN EARLIER");
            break;
        default:
            snprintf(out, n, "KEEP DOING WHAT YOU'RE DOING");
            break;
    }
}

static void drawFocusPage(const BoardView &v) {
    bandStart();
    int top = drawCueBanner(v.cue);
    if (top == 0) statusDot(v.status);
    spr->setTextDatum(TL_DATUM);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString("TODAY'S FOCUS", 18, top + 2);
    spr->setTextDatum(ML_DATUM);
    spr->setFreeFont(top ? &FreeSansBold12pt7b : &FreeSansBold18pt7b);
    spr->setTextColor(COL_TEXT, COL_BG);
    spr->drawString(focusTitle(v.d.focusCue), 18, top ? top + 34 : 46);
    bandPush(0);

    bandStart();
    spr->setTextFont(2);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COL_SUB, COL_BG);
    if (v.d.focusCue != FOCUS_NONE) {
        char why[64];
        snprintf(why, sizeof(why), "%s (%d)",
                 focusReasonText(v.d.focusReason), v.d.focusValue);
        spr->drawString(why, 18, 6);
    } else if (v.d.haveReadiness || v.d.haveSleep) {
        spr->drawString("No contributor is dragging today.", 18, 6);
    } else {
        spr->drawString("Waiting for scores to arrive.", 18, 6);
    }
    char sug[48];
    focusSuggestion(v, sug, sizeof(sug));
    spr->setTextColor(COL_TEAL, COL_BG);
    spr->drawString(sug, 18, 44);
    bandPush(80);

    bandStart();
    spr->drawFastHLine(12, 0, W - 24, COL_LINE);
    spr->setTextFont(2);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COL_DIM, COL_BG);
    spr->drawString("from your readiness & sleep contributors", 18, 8);
    spr->drawString(v.cue != CUE_NONE ? "tap: next page   hold: dismiss cue"
                                       : "tap: next page   hold: screen auto",
                    18, 56);
    bandPush(160);
}

// ---- Page DEBT (#4) -------------------------------------------------------
static uint16_t debtColor(int32_t debtSec) {
    if (debtSec <= 3600) return COL_TEAL;
    if (debtSec <= 3 * 3600) return COL_BLUE;
    if (debtSec <= 6 * 3600) return COL_AMBER;
    return COL_RED;
}

static void drawDebtHeader(const BoardView &v) {
    bandStart();
    statusDot(v.status);
    spr->setTextDatum(TL_DATUM);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString("SLEEP DEBT", 18, 2);

    if (!v.d.haveHist) {
        spr->setTextDatum(ML_DATUM);
        spr->setFreeFont(&FreeSans12pt7b);
        spr->drawString("No sleep history yet", 18, 46);
        bandPush(0);
        return;
    }

    char val[16];
    fmtHoursMin(v.d.sleepDebtSec, val, sizeof(val));
    spr->setTextDatum(ML_DATUM);
    spr->setFreeFont(&FreeSansBold18pt7b);
    spr->setTextColor(debtColor(v.d.sleepDebtSec), COL_BG);
    spr->drawString(val, 18, 34);
    int valW = spr->textWidth(val);

    // Context on the right: the need, average, and coverage. Kept clear of
    // the big value (three short right-aligned lines).
    int nights = v.d.histNights;
    int64_t sum = 0;
    for (int i = 0; i < HIST_DAYS; i++) {
        if (v.d.histSec[i] > 0) sum += v.d.histSec[i];
    }
    char buf[36], hm[12];
    spr->setTextDatum(TR_DATUM);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    snprintf(buf, sizeof(buf), "vs %ldh need", (long)(planNeedSec / 3600));
    spr->drawString(buf, 302, 6);
    if (nights > 0) {
        fmtHoursMin((int32_t)(sum / nights), hm, sizeof(hm));
        snprintf(buf, sizeof(buf), "avg %s", hm);
        spr->drawString(buf, 302, 24);
        snprintf(buf, sizeof(buf), "%d/%d nights", nights, HIST_DAYS);
        spr->drawString(buf, 302, 42);
    }
    (void)valW;

    // Tonight's plan (#4): Oura's guidance window when present (#13),
    // otherwise our arithmetic from the configured wake time.
    char plan[64];
    if (v.d.haveBedtime) {
        char b0[16], b1[16];
        fmtWallMin((int)(v.d.bedtimeStartSec / 60), b0, sizeof(b0));
        fmtWallMin((int)(v.d.bedtimeEndSec / 60), b1, sizeof(b1));
        snprintf(plan, sizeof(plan), "OURA WINDOW  %s - %s", b0, b1);
    } else {
        char bed[16], wake[16], need[12];
        fmtWallMin(planWakeMin - (int)(planNeedSec / 60), bed, sizeof(bed));
        fmtWallMin(planWakeMin, wake, sizeof(wake));
        if (planNeedSec % 3600 == 0) {
            snprintf(need, sizeof(need), "%ldH", (long)(planNeedSec / 3600));
        } else {
            snprintf(need, sizeof(need), "%ldH%02ldM",
                     (long)(planNeedSec / 3600),
                     (long)((planNeedSec % 3600) / 60));
        }
        snprintf(plan, sizeof(plan), "IN BED BY %s  FOR %s BY %s", bed, need,
                 wake);
    }
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COL_TEAL, COL_BG);
    spr->drawString(plan, 18, 60);
    bandPush(0);
}

// One bar per night in absolute screen coords; the sprite clips whatever
// falls outside the current band, so the same call renders both bands.
static void drawDebtChart(const OuraData &d, int yoff) {
    const int chartTop = 92, chartBot = 214;
    const int bx = 24, bw = W - 48;
    int32_t maxSec = 10L * 3600L;
    for (int i = 0; i < HIST_DAYS; i++) {
        if (d.histSec[i] > maxSec) maxSec = d.histSec[i];
    }
    int slotW = bw / HIST_DAYS;
    int barW = slotW - 6;

    int needY = chartBot -
                (int)((int64_t)planNeedSec * (chartBot - chartTop) / maxSec);
    for (int x = bx; x < bx + bw; x += 8) {
        spr->drawFastHLine(x, needY - yoff, 4, COL_SUB);
    }

    for (int i = 0; i < HIST_DAYS; i++) {
        int x = bx + i * slotW + 3;
        if (d.histSec[i] <= 0) {
            // ring not worn / not synced: a faint floor tick, not a zero bar
            spr->drawFastHLine(x, chartBot - 1 - yoff, barW, COL_LINE);
            continue;
        }
        int h = (int)((int64_t)d.histSec[i] * (chartBot - chartTop) / maxSec);
        if (h < 2) h = 2;
        uint16_t c = d.histSec[i] >= planNeedSec          ? COL_TEAL
                     : d.histSec[i] >= planNeedSec * 3 / 4 ? COL_AMBER
                                                           : COL_RED;
        spr->fillRect(x, chartBot - h - yoff, barW, h, c);
    }
}

static void drawDebtBands(const BoardView &v) {
    bandStart();
    spr->drawFastHLine(12, 0, W - 24, COL_LINE);
    if (v.d.haveHist) drawDebtChart(v.d, 80);
    bandPush(80);

    bandStart();
    if (v.d.haveHist) {
        drawDebtChart(v.d, 160);
        spr->setTextFont(2);
        spr->setTextColor(COL_SUB, COL_BG);
        spr->setTextDatum(TL_DATUM);
        spr->drawString("13 nights ago", 24, 62);
        spr->setTextDatum(TR_DATUM);
        spr->drawString("last night", 296, 62);
    }
    bandPush(160);
}

// ---- Page STRESS (#12) ----------------------------------------------------
static void drawStressPage(const BoardView &v) {
    bandStart();
    int top = drawCueBanner(v.cue);
    if (top == 0) statusDot(v.status);
    spr->setTextDatum(TL_DATUM);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString("STRESS & RECOVERY  TODAY", 18, top + 2);
    char hm[12], buf[32];
    if (v.d.stressHighSec >= 0) {
        fmtHoursMin(v.d.stressHighSec, hm, sizeof(hm));
        snprintf(buf, sizeof(buf), "%s stress", hm);
        spr->setTextDatum(ML_DATUM);
        spr->setFreeFont(top ? &FreeSansBold12pt7b : &FreeSansBold18pt7b);
        spr->setTextColor(COL_AMBER, COL_BG);
        spr->drawString(buf, 18, top ? top + 34 : 46);
    }
    bandPush(0);

    bandStart();
    if (v.d.recoveryHighSec >= 0) {
        fmtHoursMin(v.d.recoveryHighSec, hm, sizeof(hm));
        snprintf(buf, sizeof(buf), "%s recovery", hm);
        spr->setTextDatum(ML_DATUM);
        spr->setFreeFont(&FreeSansBold18pt7b);
        spr->setTextColor(COL_TEAL, COL_BG);
        spr->drawString(buf, 18, 14);
    }
    // Ratio bar: how the measured high-stress vs high-recovery time splits.
    int64_t st = v.d.stressHighSec > 0 ? v.d.stressHighSec : 0;
    int64_t rc = v.d.recoveryHighSec > 0 ? v.d.recoveryHighSec : 0;
    if (st + rc > 0) {
        int bx = 18, bw = W - 36, by = 40, bh = 12;
        int sw = (int)(st * bw / (st + rc));
        spr->fillRect(bx, by, sw, bh, COL_AMBER);
        spr->fillRect(bx + sw, by, bw - sw, bh, COL_TEAL);
    }
    spr->setTextFont(2);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COL_SUB, COL_BG);
    const char *note = (st > rc * 2)
                           ? "Recovery is trailing stress today -"
                           : "Stress and recovery look balanced";
    spr->drawString(note, 18, 60);
    bandPush(80);

    bandStart();
    if (st > rc * 2) {
        spr->setTextFont(2);
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(COL_SUB, COL_BG);
        spr->drawString("consider a real break before the evening.", 18, 4);
    }
    spr->drawFastHLine(12, 40, W - 24, COL_LINE);
    spr->setTextFont(2);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COL_DIM, COL_BG);
    spr->drawString("physiological load  updated hourly", 18, 50);
    bandPush(160);
}

// ---- Dispatch -------------------------------------------------------------
bool uiPageAvailable(const BoardView &v, uint8_t page) {
    switch (page) {
        case PAGE_MAIN: return true;
        case PAGE_FOCUS: return v.d.haveReadiness || v.d.haveSleep;
        case PAGE_DEBT: return v.d.haveHist;
        case PAGE_STRESS:
            return v.d.haveStress && v.d.stressAgoDays == 0 &&
                   (v.d.stressHighSec >= 0 || v.d.recoveryHighSec >= 0);
        default: return false;
    }
}

void uiRender(const BoardView &v, uint8_t page) {
    switch (page) {
        case PAGE_FOCUS:
            drawFocusPage(v);
            break;
        case PAGE_DEBT:
            drawDebtHeader(v);
            drawDebtBands(v);
            break;
        case PAGE_STRESS:
            drawStressPage(v);
            break;
        default:
            drawBandA(v);
            drawBandB(v);
            drawBandC(v);
            break;
    }
}

void uiStreamScreenshot(WiFiClient &out, const BoardView &v, uint8_t page) {
    cap = &out;
    capDeadline = millis() + 10000;  // whole-screen budget
    uiRender(v, page);
    cap = nullptr;
}
