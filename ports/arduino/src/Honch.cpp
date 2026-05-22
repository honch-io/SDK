#include "Honch.h"

HonchClass Honch;

bool HonchClass::begin(const HonchConfig &) { return false; }
bool HonchClass::track(const char *, const char *) { return false; }
bool HonchClass::identify(const char *, const char *) { return false; }
bool HonchClass::setProperty(const char *, const char *) { return false; }
bool HonchClass::sessionStart(const char *) { return false; }
bool HonchClass::sessionEnd() { return false; }
bool HonchClass::flush() { return false; }
bool HonchClass::shutdown() { return false; }
bool HonchClass::reset() { return false; }
const char *HonchClass::deviceId() { return ""; }
const char *HonchClass::lastError() { return "not initialized"; }
