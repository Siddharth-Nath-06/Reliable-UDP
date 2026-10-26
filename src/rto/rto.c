#include "rto.h"

#include <stdlib.h>

struct RtoEstimator {
    uint64_t srtt_ms;
    uint64_t rttvar_ms;
    uint64_t rto_ms;
    uint64_t min_rto_ms;
    uint64_t max_rto_ms;
    bool initialized;
};

static uint64_t clamp_rto(
    const RtoEstimator *rto,
    uint64_t value)
{
    if (value < rto->min_rto_ms) {
        return rto->min_rto_ms;
    }

    if (value > rto->max_rto_ms) {
        return rto->max_rto_ms;
    }

    return value;
}

RtoEstimator *rto_create(
    uint64_t initial_rto_ms,
    uint64_t min_rto_ms,
    uint64_t max_rto_ms)
{
    if (min_rto_ms == 0 ||
        max_rto_ms < min_rto_ms ||
        initial_rto_ms < min_rto_ms ||
        initial_rto_ms > max_rto_ms) {
        return NULL;
    }

    RtoEstimator *rto = calloc(1, sizeof(*rto));

    if (rto == NULL) {
        return NULL;
    }

    rto->rto_ms = initial_rto_ms;
    rto->min_rto_ms = min_rto_ms;
    rto->max_rto_ms = max_rto_ms;
    rto->initialized = false;

    return rto;
}

void rto_destroy(RtoEstimator *rto)
{
    free(rto);
}

void rto_update(
    RtoEstimator *rto,
    uint64_t rtt_ms,
    bool retransmitted)
{
    if (rto == NULL || rtt_ms == 0 || retransmitted) {
        return;
    }

    if (!rto->initialized) {
        rto->srtt_ms = rtt_ms;
        rto->rttvar_ms = rtt_ms / 2;

        if (rto->rttvar_ms == 0) {
            rto->rttvar_ms = 1;
        }

        rto->rto_ms =
            rto->srtt_ms + 4 * rto->rttvar_ms;

        rto->rto_ms =
            clamp_rto(rto, rto->rto_ms);

        rto->initialized = true;

        return;
    }

    uint64_t difference;

    if (rto->srtt_ms > rtt_ms) {
        difference = rto->srtt_ms - rtt_ms;
    } else {
        difference = rtt_ms - rto->srtt_ms;
    }

    rto->rttvar_ms =
        (3 * rto->rttvar_ms + difference) / 4;

    rto->srtt_ms =
        (7 * rto->srtt_ms + rtt_ms) / 8;

    if (rto->rttvar_ms == 0) {
        rto->rttvar_ms = 1;
    }

    rto->rto_ms =
        rto->srtt_ms + 4 * rto->rttvar_ms;

    rto->rto_ms =
        clamp_rto(rto, rto->rto_ms);
}

uint64_t rto_get(const RtoEstimator *rto)
{
    if (rto == NULL) {
        return 0;
    }

    return rto->rto_ms;
}

uint64_t rto_get_srtt(const RtoEstimator *rto)
{
    if (rto == NULL) {
        return 0;
    }

    return rto->srtt_ms;
}

uint64_t rto_get_rttvar(const RtoEstimator *rto)
{
    if (rto == NULL) {
        return 0;
    }

    return rto->rttvar_ms;
}