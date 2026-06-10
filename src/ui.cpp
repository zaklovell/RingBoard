// Oura-app-inspired dark theme on a 320x240 ST7789. Same three-band sprite
// rendering as the planes board: one reusable 320x80 16-bit sprite, three
// flicker-free pushes, ~51KB peak graphics RAM.
//
//   Band A: readiness and sleep scores, side by side, Oura-colored.
//   Band B: total sleep plus a stacked deep/REM/light/awake stage bar.
//   Band C: steps, distance, calories, inactive time.
#include "ui.h"
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
    tft->setRotation(1);
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

// One half of band A: small-caps label, big colored score, qualifier word.
static void drawScore(int x, const char *label, bool have, int score) {
    spr->setTextDatum(TL_DATUM);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString(label, x, 8);
    if (have) {
        char num[8];
        snprintf(num, sizeof(num), "%d", score);
        spr->setTextDatum(ML_DATUM);
        spr->setFreeFont(&FreeSansBold24pt7b);
        spr->setTextColor(scoreColor(score), COL_BG);
        spr->drawString(num, x, 48);
        int nx = x + spr->textWidth(num) + 10;
        spr->setTextFont(2);
        spr->setTextColor(COL_SUB, COL_BG);
        spr->drawString(scoreWord(score), nx, 54);
    } else {
        spr->setTextDatum(ML_DATUM);
        spr->setFreeFont(&FreeSans12pt7b);
        spr->setTextColor(COL_SUB, COL_BG);
        spr->drawString("--", x, 48);
    }
}

static void drawBandA(const BoardView &v) {
    bandStart();
    statusDot(v.status);
    drawScore(18, "READINESS", v.d.haveReadiness, v.d.readinessScore);
    spr->drawFastVLine(160, 12, 58, COL_LINE);
    drawScore(178, "SLEEP", v.d.haveSleep, v.d.sleepScore);
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
        spr->fillRect(x, 67, 6, 6, s.col);
        spr->setTextColor(COL_SUB, COL_BG);
        spr->drawString(t, x + 10, 62);
        x += 10 + spr->textWidth(t) + 12;
    }
}

static void drawBandB(const BoardView &v) {
    bandStart();
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

static void drawBandC(const BoardView &v) {
    bandStart();
    spr->setTextDatum(TL_DATUM);
    spr->setTextFont(2);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString("ACTIVITY", 18, 2);
    if (!v.d.haveActivity) {
        spr->setTextDatum(ML_DATUM);
        spr->setFreeFont(&FreeSans12pt7b);
        spr->setTextColor(COL_SUB, COL_BG);
        spr->drawString("No activity data yet", 18, 44);
        bandPush(160);
        return;
    }

    if (v.d.activityScore > 0) {
        char num[8];
        snprintf(num, sizeof(num), "%d", v.d.activityScore);
        spr->setTextDatum(TR_DATUM);
        spr->setFreeFont(&FreeSansBold9pt7b);
        spr->setTextColor(scoreColor(v.d.activityScore), COL_BG);
        spr->drawString(num, 302, 2);
    }

    char steps[16], buf[40];
    fmtThousands(v.d.steps, steps, sizeof(steps));
    spr->setTextDatum(ML_DATUM);
    spr->setFreeFont(&FreeSansBold18pt7b);
    spr->setTextColor(COL_TEXT, COL_BG);
    spr->drawString(steps, 18, 36);
    int x = 18 + spr->textWidth(steps) + 8;
    spr->setFreeFont(&FreeSans9pt7b);
    spr->setTextColor(COL_SUB, COL_BG);
    spr->drawString("steps", x, 42);

    spr->setTextDatum(MR_DATUM);
    spr->setTextColor(COL_TEXT, COL_BG);
    snprintf(buf, sizeof(buf), "%.1f mi", v.d.distanceM / 1609.34f);
    spr->drawString(buf, 302, 36);

    spr->setTextDatum(ML_DATUM);
    spr->setTextColor(COL_SUB, COL_BG);
    char totalCal[12];
    fmtThousands(v.d.totalCal, totalCal, sizeof(totalCal));
    snprintf(buf, sizeof(buf), "%d active  ·  %s total cal", v.d.activeCal,
             totalCal);
    spr->drawString(buf, 18, 66);

    char sed[12];
    fmtHoursMin(v.d.sedentarySec, sed, sizeof(sed));
    snprintf(buf, sizeof(buf), "%s inactive", sed);
    spr->setTextDatum(MR_DATUM);
    spr->drawString(buf, 302, 66);

    bandPush(160);
}

void uiRender(const BoardView &v) {
    drawBandA(v);
    drawBandB(v);
    drawBandC(v);
}
