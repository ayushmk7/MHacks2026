// src/native_apps/registry.cpp — the single list of compiled-in apps
#include "../app_host/badge_api.h"
extern "C" const badge_app_desc_t BADGE_APP_DESC_TipJar;
extern "C" const badge_app_desc_t *const BADGE_NATIVE_APPS[] = {
    &BADGE_APP_DESC_TipJar,
    nullptr,   // terminator
};
