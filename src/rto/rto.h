#ifndef RTO_H
#define RTO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Retransmission Timeout Estimator.
 *
 * Uses Jacobson/Karels RTT estimation and Karn's algorithm.
 * This module estimates timeout values but does not perform
 * retransmission decisions.
 *
 * Callers must serialize access to each estimator instance.
 */
typedef struct RtoEstimator RtoEstimator;


RtoEstimator *rto_create(
    uint64_t initial_rto_ms,
    uint64_t min_rto_ms,
    uint64_t max_rto_ms
);

void rto_destroy(RtoEstimator *rto);

/*
 * Update the RTT estimator.
 *
 * RTT samples equal to zero are ignored.
 * Samples for retransmitted packets are ignored according
 * to Karn's algorithm.
 */
void rto_update(
    RtoEstimator *rto,
    uint64_t rtt_ms,
    bool retransmitted
);

/* Current timeout, clamped to the configured bounds. */
uint64_t rto_get(const RtoEstimator *rto);

/*
 * Return zero until the first valid RTT sample has been processed.
 */
uint64_t rto_get_srtt(const RtoEstimator *rto);
uint64_t rto_get_rttvar(const RtoEstimator *rto);

#endif