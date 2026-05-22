#pragma once

#include "Honch.h"
#include "honch/core/config.h"

honch_core_config_t honch_arduino_make_core_config(const HonchConfig &config);
void honch_arduino_release_core_config(honch_core_config_t *config);
