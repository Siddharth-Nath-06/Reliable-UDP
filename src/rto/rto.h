#ifndef RTO_H
#define RTO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct RtoEstimator RtoEstimator;

RtoEstimator *rto_create(
    uint64_t initial_rto_ms,
    uint64_t min_rto_ms,
    uint64_t max_rto_ms
);

void rto_destroy(RtoEstimator *rto);

void rto_update(
    RtoEstimator *rto,
    uint64_t rtt_ms,
    bool retransmitted
);

uint64_t rto_get(const RtoEstimator *rto);
uint64_t rto_get_srtt(const RtoEstimator *rto);
uint64_t rto_get_rttvar(const RtoEstimator *rto);

#endif