#include <assert.h>
#include <stdio.h>

#include "rto.h"

static void test_initial_rto(void)
{
    RtoEstimator *rto = rto_create(1000, 100, 5000);

    assert(rto != NULL);
    assert(rto_get(rto) == 1000);

    rto_destroy(rto);
}

static void test_first_sample(void)
{
    RtoEstimator *rto = rto_create(1000, 100, 5000);

    assert(rto != NULL);

    rto_update(rto, 100, false);

    assert(rto_get_srtt(rto) == 100);
    assert(rto_get_rttvar(rto) == 50);
    assert(rto_get(rto) == 300);

    rto_destroy(rto);
}

static void test_second_sample(void)
{
    RtoEstimator *rto = rto_create(1000, 100, 5000);

    assert(rto != NULL);

    rto_update(rto, 100, false);
    rto_update(rto, 120, false);

    assert(rto_get_srtt(rto) == 102);
    assert(rto_get_rttvar(rto) == 42);
    assert(rto_get(rto) == 270);

    rto_destroy(rto);
}

static void test_karn_algorithm(void)
{
    RtoEstimator *rto = rto_create(1000, 100, 5000);

    assert(rto != NULL);

    rto_update(rto, 100, false);

    uint64_t old_srtt = rto_get_srtt(rto);
    uint64_t old_rttvar = rto_get_rttvar(rto);
    uint64_t old_rto = rto_get(rto);

    rto_update(rto, 500, true);

    assert(rto_get_srtt(rto) == old_srtt);
    assert(rto_get_rttvar(rto) == old_rttvar);
    assert(rto_get(rto) == old_rto);

    rto_destroy(rto);
}

static void test_rto_limits(void)
{
    RtoEstimator *rto = rto_create(500, 100, 1000);

    assert(rto != NULL);

    rto_update(rto, 1, false);
    assert(rto_get(rto) >= 100);

    rto_update(rto, 10000, false);
    assert(rto_get(rto) <= 1000);

    rto_destroy(rto);
}

int main(void)
{
    test_initial_rto();
    test_first_sample();
    test_second_sample();
    test_karn_algorithm();
    test_rto_limits();

    printf("RTO and Karn tests passed!\n");

    return 0;
}