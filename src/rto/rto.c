#include "rto.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>


struct RtoEstimator {
    uint64_t srtt_ms;
    uint64_t rttvar_ms;
    uint64_t rto_ms;

    uint64_t min_rto_ms;
    uint64_t max_rto_ms;

    bool initialized;
};


/*
 * Clamp a value to the configured RTO interval.
 */
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


/*
 * Calculate floor((7 * old_value + sample) / 8) without
 * overflowing uint64_t.
 *
 * This preserves the integer result of the original formula.
 */
static uint64_t weighted_average_7_1(
    uint64_t old_value,
    uint64_t sample)
{
    uint64_t result =
        7U * (old_value / 8U) +
        (sample / 8U);

    result +=
        (7U * (old_value % 8U) + (sample % 8U)) / 8U;

    return result;
}


/*
 * Calculate floor((3 * old_value + sample) / 4) without
 * overflowing uint64_t.
 *
 * This preserves the integer result of the original formula.
 */
static uint64_t weighted_average_3_1(
    uint64_t old_value,
    uint64_t sample)
{
    uint64_t result =
        3U * (old_value / 4U) +
        (sample / 4U);

    result +=
        (3U * (old_value % 4U) + (sample % 4U)) / 4U;

    return result;
}


/*
 * Recalculate RTO using SRTT + 4 * RTTVAR while avoiding overflow.
 *
 * Since the returned RTO is capped by max_rto_ms, intermediate
 * values that would exceed that bound can be saturated directly.
 */
static uint64_t calculate_rto(const RtoEstimator *rto)
{
    uint64_t remaining;
    uint64_t value;

    if (rto->srtt_ms >= rto->max_rto_ms) {
        return rto->max_rto_ms;
    }

    remaining = rto->max_rto_ms - rto->srtt_ms;

    if (rto->rttvar_ms > remaining / 4U) {
        return rto->max_rto_ms;
    }

    value = rto->srtt_ms + 4U * rto->rttvar_ms;

    return clamp_rto(rto, value);
}


RtoEstimator *rto_create(
    uint64_t initial_rto_ms,
    uint64_t min_rto_ms,
    uint64_t max_rto_ms)
{
    RtoEstimator *rto;

    if (min_rto_ms == 0 ||
        max_rto_ms < min_rto_ms ||
        initial_rto_ms < min_rto_ms ||
        initial_rto_ms > max_rto_ms) {
        return NULL;
    }

    rto = calloc(1, sizeof(*rto));
    if (rto == NULL) {
        return NULL;
    }

    rto->srtt_ms = 0;
    rto->rttvar_ms = 0;
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
    uint64_t difference;

    if (rto == NULL || rtt_ms == 0 || retransmitted) {
        return;
    }

    /*
     * First valid sample:
     *
     * SRTT   = RTT
     * RTTVAR = RTT / 2
     * RTO    = SRTT + 4 * RTTVAR
     */
    if (!rto->initialized) {
        rto->srtt_ms = rtt_ms;
        rto->rttvar_ms = rtt_ms / 2U;

        if (rto->rttvar_ms == 0) {
            rto->rttvar_ms = 1;
        }

        rto->rto_ms = calculate_rto(rto);
        rto->initialized = true;
        return;
    }

    /*
     * RTTVAR uses the difference from the previous SRTT.
     * Compute its absolute value without signed arithmetic.
     */
    if (rto->srtt_ms > rtt_ms) {
        difference = rto->srtt_ms - rtt_ms;
    } else {
        difference = rtt_ms - rto->srtt_ms;
    }

    /*
     * Jacobson/Karels coefficients:
     *
     * RTTVAR = (3 * RTTVAR + |SRTT - RTT|) / 4
     * SRTT   = (7 * SRTT + RTT) / 8
     *
     * The helpers preserve integer-floor semantics without
     * overflow in the intermediate weighted sums.
     */
    rto->rttvar_ms =
        weighted_average_3_1(rto->rttvar_ms, difference);

    rto->srtt_ms =
        weighted_average_7_1(rto->srtt_ms, rtt_ms);

    if (rto->rttvar_ms == 0) {
        rto->rttvar_ms = 1;
    }

    rto->rto_ms = calculate_rto(rto);
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
    if (rto == NULL || !rto->initialized) {
        return 0;
    }

    return rto->srtt_ms;
}


uint64_t rto_get_rttvar(const RtoEstimator *rto)
{
    if (rto == NULL || !rto->initialized) {
        return 0;
    }

    return rto->rttvar_ms;
}