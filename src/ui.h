#pragma once
#include <TFT_eSPI.h>
#include <WiFiClient.h>
#include "models.h"

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Pages, cycled by short taps.
enum { PAGE_MAIN = 0, PAGE_FOCUS, PAGE_DEBT, PAGE_STRESS, PAGE_COUNT };

void uiInit(TFT_eSPI *t);
void uiBoot(const char *line1, const char *line2);
void uiRender(const BoardView &v, uint8_t page);
void uiBacklight(bool on);
// True when the given page has content worth showing (tap-cycle skips
// empty ones).
bool uiPageAvailable(const BoardView &v, uint8_t page);
// Bedtime plan inputs (set from the NVS-backed web config).
void uiSetPlan(int wakeMin, int32_t needSec);
// Re-renders the given page streaming each band's raw RGB565 pixels
// (320x80x2 per band, 3 bands top to bottom) into `out` instead of the
// panel. 153,600 bytes total; the caller sends HTTP headers first.
void uiStreamScreenshot(WiFiClient &out, const BoardView &v, uint8_t page);
