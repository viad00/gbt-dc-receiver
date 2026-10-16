/*
 * helpers_linux.c
 *
 * Linux implementation of the shared timing/LED helpers (helpers.h).
 * LICENSE: MIT
 */

#include "helpers.h"

#include <time.h>
#include <stdint.h>

uint32_t GetTS(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void DelayMs(uint32_t ms)
{
	struct timespec req;
	req.tv_sec = (time_t)(ms / 1000u);
	req.tv_nsec = (long)(ms % 1000u) * 1000000L;
	nanosleep(&req, NULL);
}

void LED(uint8_t id)
{
	(void)id; /* no LEDs on the host */
}
