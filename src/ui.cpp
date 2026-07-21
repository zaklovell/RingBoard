// Oura-app-inspired dark theme on a 320x240 ST7789. Same three-band sprite
// rendering as the planes board: one reusable 320x80 16-bit sprite, three
// flicker-free pushes, ~51KB peak graphics RAM.
//
//   Band A: readiness and sleep scores, side by side, Oura-colored.
//   Band B: total sleep plus a stacked deep/REM/light/awake stage bar.
//   Band C: steps, distance, calories, inactive time.
#include "ui.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

static TFT_eSPI *tft;
static TFT_eSprite *spr;

// Oura's palette: near-black navy, soft grey-blue subtext, teal for optimal.
static const uint16_t COL_BG = RGB565(10, 14, 22);
static const uint16_t COL_TEXT = 0xFFFF;
static const uint16_t COL_SUB = RGB565(136, 148, 168);
static const uint16_t COL_LINE = RGB565(38, 46, 62);
static const uint16_t COL_TEAL = RGB565(64, 211, 198);    // optimal, 85+
static const uint16_t COL_BLUE = RGB565(116, 166, 255);   // good, 70-84
static const uint16_t COL_AMBER = RGB565(255, 159, 10);   // fair, 60-69
static const uint16_t COL_RED = RGB565(255, 89, 79);      // pay attention
static const uint16_t COL_DEEP = RGB565(73, 100, 246);    // sleep stages
static const uint16_t COL_REM = RGB565(64, 211, 198);
static const uint16_t COL_LIGHT = RGB565(150, 178, 255);
static const uint16_t COL_AWAKE = RGB565(90, 100, 118);

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
static void bandPush(int y) { spr->pushSprite(0, y); }

static uint16_t scoreColor(int s) {
    if (s >= 85) return COL_TEAL;
    if (s >= 70) return COL_BLUE;
    if (s >= 60) return COL_AMBER;
    return COL_RED;
}

static const char *scoreWord(int s) {
    if (s >= 85) return "Optimal";
    if (s >= 70) return "Good";
    if (s >= 60) return "Fair";
    return "Pay attention";
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

// Oura-style score ring: progress arc from 12 o'clock (TFT_eSPI puts 0
// degrees at 6 o'clock, so the top is 180), score inside, label below.
static void drawRing(int cx, const char *label, bool have, int score) {
    const int cy = 34, r = 27, ir = 22;
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
    spr->setTextFont(2);
    spr->setTextDatum(TC_DATUM);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString(label, cx, 64);
}

static void drawBandA(const BoardView &v) {
    bandStart();
    statusDot(v.status);
    drawRing(64, "READINESS", v.d.haveReadiness, v.d.readinessScore);
    drawRing(160, "SLEEP", v.d.haveSleep, v.d.sleepScore);
    drawRing(256, "ACTIVITY", v.d.haveActivity && v.d.activityScore > 0,
             v.d.activityScore);
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

    // Legend entries take their stage color instead of grey; the swatch
    // squares become redundant, so the text stands alone.
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
    if (v.d.haveSleepDetail) {
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
        spr->drawString("No sleep data yet", 18, 40);
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

// White value with a grey unit after it, left-anchored at an ML baseline.
// Fonts differ between the pieces, so widths are measured per piece; vf null
// means the value uses the small font too.
static void valueUnitL(int x, int y, const GFXfont *vf, const char *val,
                       const char *unit) {
    spr->setTextDatum(ML_DATUM);
    if (vf) spr->setFreeFont(vf);
    else spr->setTextFont(2);
    spr->setTextColor(COL_TEXT, COL_BG);
    spr->drawString(val, x, y);
    int w = spr->textWidth(val);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString(unit, x + w + 6, y + (vf ? 3 : 0));
}

// Same, right-anchored: grey unit hugs xr, white value sits left of it.
static void valueUnitR(int xr, int y, const GFXfont *vf, const char *val,
                       const char *unit) {
    spr->setTextDatum(MR_DATUM);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString(unit, xr, y + (vf ? 3 : 0));
    int uw = spr->textWidth(unit);
    if (vf) spr->setFreeFont(vf);
    else spr->setTextFont(2);
    spr->setTextColor(COL_TEXT, COL_BG);
    spr->drawString(val, xr - uw - 6, y);
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

    char steps[16], buf[40];
    fmtThousands(v.d.steps, steps, sizeof(steps));
    valueUnitL(18, 34, &FreeSansBold12pt7b, steps, "steps");
    snprintf(buf, sizeof(buf), "%.1f", v.d.distanceM / 1609.34f);
    valueUnitR(302, 34, &FreeSansBold12pt7b, buf, "mi");

    char totalCal[12];
    fmtThousands(v.d.totalCal, totalCal, sizeof(totalCal));
    snprintf(buf, sizeof(buf), "%d / %s", v.d.activeCal, totalCal);
    valueUnitL(18, 64, nullptr, buf, "cal");

    char sed[12];
    fmtHoursMin(v.d.sedentarySec, sed, sizeof(sed));
    valueUnitR(302, 64, nullptr, sed, "inactive");

    bandPush(160);
}

// ---- Page 2: sleep debt against SLEEP_NEED_SEC over the last HIST_DAYS ----

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
    spr->setFreeFont(&FreeSansBold24pt7b);
    spr->setTextColor(debtColor(v.d.sleepDebtSec), COL_BG);
    spr->drawString(val, 18, 46);

    // Context on the right: the fixed need, and the average of logged nights.
    int nights = 0;
    int64_t sum = 0;
    for (int i = 0; i < HIST_DAYS; i++) {
        if (v.d.histSec[i] > 0) {
            nights++;
            sum += v.d.histSec[i];
        }
    }
    char buf[28], hm[12];
    spr->setTextDatum(TR_DATUM);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    snprintf(buf, sizeof(buf), "vs %ldh need", (long)(SLEEP_NEED_SEC / 3600));
    spr->drawString(buf, 302, 26);
    if (nights > 0) {
        fmtHoursMin((int32_t)(sum / nights), hm, sizeof(hm));
        snprintf(buf, sizeof(buf), "avg %s", hm);
        spr->drawString(buf, 302, 44);
    }
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
                (int)((int64_t)SLEEP_NEED_SEC * (chartBot - chartTop) / maxSec);
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
        uint16_t c = d.histSec[i] >= SLEEP_NEED_SEC          ? COL_TEAL
                     : d.histSec[i] >= SLEEP_NEED_SEC * 3 / 4 ? COL_AMBER
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
        spr->drawString("14 nights ago", 24, 62);
        spr->setTextDatum(TR_DATUM);
        spr->drawString("last night", 296, 62);
    }
    bandPush(160);
}

void uiRender(const BoardView &v, uint8_t page) {
    if (page == 1) {
        drawDebtHeader(v);
        drawDebtBands(v);
    } else {
        drawBandA(v);
        drawBandB(v);
        drawBandC(v);
    }
}
