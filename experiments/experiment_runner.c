
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/driver/driver.h"
#include "../src/common/constants.h"

#define SENDER_PORT   49171
#define RECEIVER_PORT 49172
#define FILE_SOURCE   "experiments/smoke_source.bin"
#define FILE_DEST     "experiments/smoke_received.bin"

typedef struct {
    DriverConfig config;
    DriverResult result;
    DriverStatus status;
#ifndef _WIN32
    pthread_t thread;
#else
    HANDLE thread;
#endif
} Endpoint;

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = {
        .tv_sec = (time_t)(ms / 1000),
        .tv_nsec = (long)(ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
#endif
}


#ifdef _WIN32
static DWORD WINAPI run_receiver(LPVOID arg)
#else
static void *run_receiver(void *arg)
#endif
{
    Endpoint *ep = (Endpoint *)arg;
    ep->status = driver_run(&ep->config, &ep->result);

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}


static bool write_fixture(const char *path, uint64_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    /* Repeatable, nontrivial byte pattern. */
    for (uint64_t i = 0; i < size; ++i) {
        unsigned char byte = (unsigned char)((i * 31U + 17U) & 0xffU);
        if (fwrite(&byte, 1, 1, f) != 1) {
            fclose(f);
            return false;
        }
    }

    return fclose(f) == 0;
}

static bool files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return false;
    }

    bool equal = true;
    for (;;) {
        unsigned char ba[4096], bb[4096];
        size_t na = fread(ba, 1, sizeof ba, fa);
        size_t nb = fread(bb, 1, sizeof bb, fb);

        if (na != nb || memcmp(ba, bb, na) != 0) {
            equal = false;
            break;
        }
        if (na == 0) break;
    }

    if (ferror(fa) || ferror(fb)) equal = false;
    fclose(fa);
    fclose(fb);
    return equal;
}

static bool parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(s, &end, 10);
    if (errno || !s[0] || !end || *end != '\0' || s[0] == '-') {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static bool parse_double_pct(const char *s, double *out)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(s, &end);
    if (errno || !s[0] || !end || *end != '\0' ||
        value < 0.0 || value > 100.0) {
        return false;
    }
    *out = value / 100.0;
    return true;
}

static bool parse_protocol(const char *s, ArqProtocol *out)
{
    if (strcmp(s, "stop-and-wait") == 0) {
        *out = ARQ_STOP_WAIT;
    } else if (strcmp(s, "go-back-n") == 0) {
        *out = ARQ_GO_BACK_N;
    } else if (strcmp(s, "selective-repeat") == 0) {
        *out = ARQ_SELECTIVE_REPEAT;
    } else {
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    uint64_t size = 65536;
    uint64_t window_arg = 1;
    uint64_t seed = 101;
    uint64_t rto_ms = 200;
    uint64_t delay_ms = 0;
    uint64_t jitter_ms = 0;
    uint64_t reordering_delay_ms = 0;

    double loss = 0.0;
    double duplication = 0.0;
    double corruption = 0.0;
    double reordering = 0.0;

    ArqProtocol protocol = ARQ_STOP_WAIT;

    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return 2;
        }

        const char *key = argv[i++];
        const char *value = argv[i];

        if (strcmp(key, "--protocol") == 0) {
            if (!parse_protocol(value, &protocol)) {
                fprintf(stderr, "Invalid protocol: %s\n", value);
                return 2;
            }
        } else if (strcmp(key, "--window") == 0) {
            if (!parse_u64(value, &window_arg)) return 2;
        } else if (strcmp(key, "--size") == 0) {
            if (!parse_u64(value, &size)) return 2;
        } else if (strcmp(key, "--loss") == 0) {
            if (!parse_double_pct(value, &loss)) return 2;
        } else if (strcmp(key, "--duplication") == 0) {
            if (!parse_double_pct(value, &duplication)) return 2;
        } else if (strcmp(key, "--corruption") == 0) {
            if (!parse_double_pct(value, &corruption)) return 2;
        } else if (strcmp(key, "--reordering") == 0) {
            if (!parse_double_pct(value, &reordering)) return 2;
        } else if (strcmp(key, "--delay-ms") == 0) {
            if (!parse_u64(value, &delay_ms) ||
                delay_ms > UINT32_MAX) return 2;
        } else if (strcmp(key, "--jitter-ms") == 0) {
            if (!parse_u64(value, &jitter_ms) ||
                jitter_ms > UINT32_MAX) return 2;
        } else if (strcmp(key, "--reordering-delay-ms") == 0) {
            if (!parse_u64(value, &reordering_delay_ms) ||
                reordering_delay_ms > UINT32_MAX) return 2;
        } else if (strcmp(key, "--rto-ms") == 0) {
            if (!parse_u64(value, &rto_ms) ||
                rto_ms < 50 || rto_ms > 2000) return 2;
        } else if (strcmp(key, "--seed") == 0) {
            if (!parse_u64(value, &seed) || seed > UINT32_MAX) return 2;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", key);
            return 2;
        }
    }

    if (window_arg == 0 || window_arg > UINT16_MAX ||
        (protocol == ARQ_STOP_WAIT && window_arg != 1)) {
        fprintf(stderr, "Invalid window for selected protocol\n");
        return 2;
    }

    if (size > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "File size is too large for this platform\n");
        return 2;
    }

    if (!write_fixture(FILE_SOURCE, size)) {
        perror("Cannot create source fixture");
        return 3;
    }
    (void)remove(FILE_DEST);

    DriverConfig base = {0};
    base.mode = DRIVER_MODE_EXPERIMENT;
    base.enable_monitoring = true;
    base.sender_address = "127.0.0.1";
    base.sender_port = SENDER_PORT;
    base.receiver_address = "127.0.0.1";
    base.receiver_port = RECEIVER_PORT;
    base.protocol = protocol;
    base.window_size = (uint16_t)window_arg;
    base.chunk_size = 512;
    base.initial_rto_ms = rto_ms;
    base.min_rto_ms = 50;
    base.max_rto_ms = 2000;
    base.receive_timeout_ms = 20;
    base.control_timeout_ms = 500;
    base.max_control_retries = 5;
    base.max_transfer_duration_ms = 120000;
    base.channel.bandwidth_bps = 100000000U;
    base.channel.delay_ms = (uint32_t)delay_ms;
    base.channel.jitter_ms = (uint32_t)jitter_ms;
    base.channel.loss_probability = loss;
    base.channel.duplication_probability = duplication;
    base.channel.corruption_probability = corruption;
    base.channel.reordering_probability = reordering;
    base.channel.reordering_delay_ms = (uint32_t)reordering_delay_ms;
    base.channel.queue_limit_bytes = 1024U * 1024U;
    base.channel.random_seed = (uint32_t)seed;

    Endpoint receiver = {0};
    receiver.config = base;
    receiver.config.role = TRANSPORT_RECEIVER;
    receiver.config.destination_path = FILE_DEST;

    Endpoint sender = {0};
    sender.config = base;
    sender.config.role = TRANSPORT_SENDER;
    sender.config.source_path = FILE_SOURCE;

#ifndef _WIN32
    if (pthread_create(&receiver.thread, NULL, run_receiver, &receiver) != 0) {
        fprintf(stderr, "Could not start receiver thread\n");
        return 4;
    }
#else
    receiver.thread = CreateThread(NULL, 0,
    run_receiver, &receiver, 0, NULL);
    if (!receiver.thread) {
        fprintf(stderr, "Could not start receiver thread\n");
        return 4;
    }
#endif

    sleep_ms(150);
    sender.status = driver_run(&sender.config, &sender.result);

#ifndef _WIN32
    pthread_join(receiver.thread, NULL);
#else
    WaitForSingleObject(receiver.thread, INFINITE);
    CloseHandle(receiver.thread);
#endif

    bool equal = files_equal(FILE_SOURCE, FILE_DEST);
    bool integrity_ok =
        sender.status == DRIVER_STATUS_OK &&
        receiver.status == DRIVER_STATUS_OK &&
        sender.result.integrity_verified &&
        receiver.result.integrity_verified &&
        sender.result.bytes_read_from_source == size &&
        receiver.result.bytes_written_to_destination == size &&
        equal;

    MonitorStats *sender_m = &sender.result.metrics;
    MonitorStats *receiver_m = &receiver.result.metrics;

    bool stats_ok =
        sender.result.monitor_stats_available &&
        receiver.result.monitor_stats_available;

    fprintf(stderr, "sender: %s; receiver: %s; files_equal: %s\n",
        driver_status_string(sender.status),
        driver_status_string(receiver.status),
        equal ? "true" : "false");

    printf("{\"packets_sent\":%" PRIu64
       ",\"retransmissions\":%" PRIu64
       ",\"timeouts\":%" PRIu64
       ",\"integrity_ok\":%s"
       ",\"goodput_bytes_per_second\":%.3f"
       ",\"exit_code\":%d}\n",
       stats_ok ? sender_m->packets_sent : 0,
       stats_ok ? sender_m->retransmissions : 0,
       stats_ok ? sender_m->timeouts : 0,
       integrity_ok ? "true" : "false",
       stats_ok ? receiver_m->goodput_bytes_per_second : 0.0,
       integrity_ok && stats_ok ? 0 : 1);

    (void)remove(FILE_SOURCE);
    (void)remove(FILE_DEST);
    return integrity_ok && stats_ok ? 0 : 1;
}
