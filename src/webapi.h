#pragma once
#include <stdint.h>

enum ScreenMode { SCREEN_AUTO, SCREEN_FORCED_ON, SCREEN_FORCED_OFF };

struct WebStatus {
    bool screenOn = true;
    bool haveData = false;
    int readiness = 0;
    int sleep = 0;
    int32_t steps = 0;
};

void webApiInit();
void webApiLoop();
ScreenMode screenMode();
void webApiSetStatus(const WebStatus &s);
