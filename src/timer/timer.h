#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/types.h"

TimerId timer_create(void);

bool timer_start(TimerId id, uint64_t duration_ms);

bool timer_restart(TimerId id, uint64_t duration_ms);

bool timer_cancel(TimerId id);

bool timer_expired(TimerId id);

#endif
