#ifndef CHANNEL_EMULATOR_H
#define CHANNEL_EMULATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Opaque Channel Emulator instance.
 */
typedef struct ChannelEmulator ChannelEmulator;

/*
 * Describes the path a datagram is taking through the channel.
 *
 * OUTBOUND:
 *   Local Transport -> simulated channel -> remote endpoint
 *
 * INBOUND:
 *   Remote endpoint -> simulated channel -> local Transport / ARQ
 */
typedef enum {
    CHANNEL_DIRECTION_OUTBOUND,
    CHANNEL_DIRECTION_INBOUND
} ChannelDirection;

/*
 * Configuration for the simulated channel.
 *
 * Probabilities are in the range [0.0, 1.0].
 * Bandwidth is shared by both directions.
 */
typedef struct ChannelEmulatorConfig {
    uint32_t bandwidth_bps;

    uint32_t delay_ms;
    uint32_t jitter_ms;

    double loss_probability;
    double duplication_probability;
    double corruption_probability;
    double reordering_probability;

    uint32_t reordering_delay_ms;

    size_t queue_limit_bytes;

    /*
     * A fixed seed makes random channel decisions reproducible.
     * A seed of zero requests a seed chosen at initialization.
     */
    uint32_t random_seed;
} ChannelEmulatorConfig;

/*
 * Called asynchronously by the emulator's worker thread
 * when a datagram survives processing and is ready for Transport.
 *
 * The data pointer is borrowed and is valid only for the
 * duration of the callback. Transport must copy the data if
 * it needs to retain it.
 *
 * The callback must be thread-safe and should return promptly.
 */
typedef void (*ChannelEmulatorOutputCallback)(
    void *context,
    ChannelDirection direction,
    const uint8_t *data,
    size_t length
);

/*
 * Creates the emulator, initializes its channel state,
 * and starts its worker thread.
 *
 * Returns NULL if configuration is invalid or initialization
 * fails.
 */
ChannelEmulator *channel_emulator_create(
    const ChannelEmulatorConfig *config,
    ChannelEmulatorOutputCallback output_callback,
    void *callback_context
);

/*
 * Submits a datagram for asynchronous channel processing.
 *
 * The emulator copies the input data, so the caller may reuse
 * its buffer immediately after this function returns.
 *
 * Returns false if the input is invalid, the emulator is
 * shutting down, or the queue cannot accept the datagram.
 *
 * A true result means accepted for processing, not guaranteed
 * delivery. The emulator may subsequently drop the datagram.
 */
bool channel_emulator_submit(
    ChannelEmulator *emulator,
    ChannelDirection direction,
    const uint8_t *data,
    size_t length
);

/*
 * Stops the worker thread, discards pending datagrams,
 * and releases emulator resources.
 *
 * The caller must ensure that no other thread is submitting
 * datagrams while the emulator is being destroyed.
 */
void channel_emulator_destroy(
    ChannelEmulator *emulator
);

#endif