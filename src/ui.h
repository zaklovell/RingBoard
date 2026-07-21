#pragma once
#include <TFT_eSPI.h>
#include "models.h"

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void uiInit(TFT_eSPI *t);
void uiBoot(const char *line1, const char *line2);
// page 0 = scores/sleep/activity, page 1 = sleep debt + nightly graph.
void uiRender(const BoardView &v, uint8_t page);
void uiBacklight(bool on);
