#pragma once
#include "models.h"

void ouraInit();
// Fetches readiness, sleep score, sleep phases, and activity. Pass in the
// PREVIOUS cycle's data: sections whose fetch fails keep their old values
// (merge-on-failure). Returns true if at least one section came back fresh.
bool ouraFetchAll(OuraData *out);
// Runtime sleep-need for the debt calculation (config #4).
void ouraSetNeedSec(int32_t sec);
void ouraRecomputeDebt(OuraData *d);
int ouraCallsToday();
// Human-readable evidence line for OuraData.focusReason.
const char *focusReasonText(uint8_t idx);
// "ok", "auth" (re-run tools/oura_auth.py), or "net" for the web API.
const char *ouraAuthState();
// Replaces the refresh token (NVS + memory) and clears the auth-failed latch,
// so a dead token can be fixed over the network instead of a reflash.
void ouraSetRefreshToken(const char *token);
