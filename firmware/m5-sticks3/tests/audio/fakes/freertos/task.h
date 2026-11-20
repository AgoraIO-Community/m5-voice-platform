#pragma once
#include "fake.h"

/* The host test advances time inside vTaskDelay. A bare yield would spin. */
#define taskYIELD() vTaskDelay(1)
