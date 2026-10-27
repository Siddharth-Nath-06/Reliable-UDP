#include "transport.h"
#include "../channel/channel_emulator.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>

    typedef SOCKET TransportSocket;
    typedef int TransportSocketLength;

    #define TRANSPORT_INVALID_SOCKET INVALID_SOCKET
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>

    typedef int TransportSocket;
    typedef socklen_t TransportSocketLength;

    #define TRANSPORT_INVALID_SOCKET (-1)
#endif

/*
 * Maximum UDP payload size for IPv4.
 */
#define TRANSPORT_MAX_DATAGRAM_SIZE 65507U

/*
 * Maximum amount of datagram payload data that Transport
 * will buffer while waiting for the application to receive it.
 */
#define TRANSPORT_RECEIVE_QUEUE_LIMIT_BYTES \
    (4U * 1024U * 1024U)

/*
 * The receiver thread checks the shutdown state periodically.
 */
#define TRANSPORT_SOCKET_POLL_INTERVAL_MS 100U


/*
 * A datagram waiting to be returned by transport_receive().
 */
typedef struct TransportDatagram {
    uint8_t *data;
    size_t length;
    struct TransportDatagram *next;
} TransportDatagram;


/*
 * Private Transport implementation.
 */
struct Transport {
    TransportRole role;

    TransportSocket socket_handle;

    struct sockaddr_storage peer_endpoint;
    TransportSocketLength peer_endpoint_length;

    ChannelEmulator *emulator;

    pthread_t receive_thread;
    bool receive_thread_started;

    pthread_mutex_t mutex;
    pthread_cond_t queue_condition;

    bool mutex_initialized;
    bool condition_initialized;

    bool shutting_down;
    bool asynchronous_error;

    TransportDatagram *queue_head;
    TransportDatagram *queue_tail;
    size_t queue_bytes;

    bool winsock_started;
};


/* ---------------------------------------------------------
 * Platform helpers
 * --------------------------------------------------------- */

static void close_socket(TransportSocket socket_handle)
{
#ifdef _WIN32
    closesocket(socket_handle);
#else
    close(socket_handle);
#endif
}


static bool socket_error_interrupted(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}


static bool socket_error_would_block(void)
{
#ifdef _WIN32
    int error = WSAGetLastError();

    return error == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}


/*
 * Resolve an IPv4 address and port into a socket endpoint.
 */
static bool resolve_endpoint(
    const char *address,
    uint16_t port,
    struct sockaddr_storage *endpoint,
    TransportSocketLength *endpoint_length)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char service[6];

    if (address == NULL || endpoint == NULL ||
        endpoint_length == NULL || port == 0) {
        return false;
    }

    snprintf(service, sizeof(service), "%u", (unsigned)port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    if (getaddrinfo(address, service, &hints, &result) != 0) {
        return false;
    }

    if (result == NULL ||
        result->ai_addrlen > sizeof(*endpoint)) {
        freeaddrinfo(result);
        return false;
    }

    memset(endpoint, 0, sizeof(*endpoint));

    memcpy(
        endpoint,
        result->ai_addr,
        result->ai_addrlen
    );

    *endpoint_length =
        (TransportSocketLength)result->ai_addrlen;

    freeaddrinfo(result);
    return true;
}


/*
 * Compare two IPv4 endpoints, including their ports.
 */
static bool same_endpoint(
    const struct sockaddr_storage *a,
    const struct sockaddr_storage *b)
{
    const struct sockaddr_in *addr_a;
    const struct sockaddr_in *addr_b;

    if (a == NULL || b == NULL ||
        a->ss_family != AF_INET ||
        b->ss_family != AF_INET) {
        return false;
    }

    addr_a = (const struct sockaddr_in *)a;
    addr_b = (const struct sockaddr_in *)b;

    return addr_a->sin_addr.s_addr ==
               addr_b->sin_addr.s_addr &&
           addr_a->sin_port == addr_b->sin_port;
}


/*
 * Send one complete UDP datagram.
 */
static bool socket_send_datagram(
    TransportSocket socket_handle,
    const struct sockaddr_storage *destination,
    TransportSocketLength destination_length,
    const uint8_t *data,
    size_t length)
{
    if (socket_handle == TRANSPORT_INVALID_SOCKET ||
        destination == NULL ||
        data == NULL ||
        length == 0 ||
        length > TRANSPORT_MAX_DATAGRAM_SIZE) {
        return false;
    }

#ifdef _WIN32
    {
        int sent = sendto(
            socket_handle,
            (const char *)data,
            (int)length,
            0,
            (const struct sockaddr *)destination,
            destination_length
        );

        return sent == (int)length;
    }
#else
    {
        ssize_t sent = sendto(
            socket_handle,
            data,
            length,
            0,
            (const struct sockaddr *)destination,
            destination_length
        );

        return sent == (ssize_t)length;
    }
#endif
}


/* ---------------------------------------------------------
 * Shared state and receive queue
 * --------------------------------------------------------- */

static bool transport_is_shutting_down(Transport *transport)
{
    bool result;

    pthread_mutex_lock(&transport->mutex);
    result = transport->shutting_down;
    pthread_mutex_unlock(&transport->mutex);

    return result;
}


static void transport_set_async_error(Transport *transport)
{
    pthread_mutex_lock(&transport->mutex);

    if (!transport->shutting_down) {
        transport->asynchronous_error = true;
        pthread_cond_broadcast(&transport->queue_condition);
    }

    pthread_mutex_unlock(&transport->mutex);
}


/*
 * Add a copy of a datagram to the receive queue.
 *
 * Returns false if allocation fails, shutdown has begun,
 * or the queue has insufficient capacity.
 *
 * Queue overflow drops the datagram rather than blocking
 * the socket receiver or the emulator worker.
 */
static bool transport_queue_datagram(
    Transport *transport,
    const uint8_t *data,
    size_t length)
{
    TransportDatagram *datagram;

    if (transport == NULL ||
        data == NULL ||
        length == 0 ||
        length > TRANSPORT_MAX_DATAGRAM_SIZE ||
        length > TRANSPORT_RECEIVE_QUEUE_LIMIT_BYTES) {
        return false;
    }

    datagram = malloc(sizeof(*datagram));
    if (datagram == NULL) {
        return false;
    }

    datagram->data = malloc(length);
    if (datagram->data == NULL) {
        free(datagram);
        return false;
    }

    memcpy(datagram->data, data, length);

    datagram->length = length;
    datagram->next = NULL;

    pthread_mutex_lock(&transport->mutex);

    if (transport->shutting_down ||
        length > TRANSPORT_RECEIVE_QUEUE_LIMIT_BYTES -
                     transport->queue_bytes) {
        pthread_mutex_unlock(&transport->mutex);

        free(datagram->data);
        free(datagram);
        return false;
    }

    if (transport->queue_tail == NULL) {
        transport->queue_head = datagram;
        transport->queue_tail = datagram;
    } else {
        transport->queue_tail->next = datagram;
        transport->queue_tail = datagram;
    }

    transport->queue_bytes += length;

    pthread_cond_signal(&transport->queue_condition);
    pthread_mutex_unlock(&transport->mutex);

    return true;
}


static void transport_free_queue(Transport *transport)
{
    TransportDatagram *current = transport->queue_head;

    while (current != NULL) {
        TransportDatagram *next = current->next;

        free(current->data);
        free(current);

        current = next;
    }

    transport->queue_head = NULL;
    transport->queue_tail = NULL;
    transport->queue_bytes = 0;
}


/* ---------------------------------------------------------
 * Channel Emulator callback
 * --------------------------------------------------------- */

/*
 * Called by the Channel Emulator worker thread.
 *
 * OUTBOUND:
 *   Send the processed datagram to the remote receiver.
 *
 * INBOUND:
 *   Queue the processed datagram for ARQ to receive.
 */
static void transport_on_emulator_output(
    void *context,
    ChannelDirection direction,
    const uint8_t *data,
    size_t length)
{
    Transport *transport = context;

    if (transport == NULL ||
        data == NULL ||
        length == 0 ||
        length > TRANSPORT_MAX_DATAGRAM_SIZE) {
        return;
    }

    if (transport_is_shutting_down(transport)) {
        return;
    }

    switch (direction) {
    case CHANNEL_DIRECTION_OUTBOUND:
        if (!socket_send_datagram(
                transport->socket_handle,
                &transport->peer_endpoint,
                transport->peer_endpoint_length,
                data,
                length)) {
            transport_set_async_error(transport);
        }
        break;

    case CHANNEL_DIRECTION_INBOUND:
        /*
         * The queue makes a copy because the emulator owns
         * the callback buffer and may release it on return.
         *
         * If the receive queue is full, the datagram is
         * dropped. ARQ can recover through retransmission.
         */
        (void)transport_queue_datagram(
            transport,
            data,
            length
        );
        break;

    default:
        transport_set_async_error(transport);
        break;
    }
}


/* ---------------------------------------------------------
 * UDP receiving thread
 * --------------------------------------------------------- */

/*
 * This thread continuously reads the local UDP socket.
 *
 * Sender role:
 *   Incoming datagrams from the receiver are submitted to
 *   the emulator for inbound processing.
 *
 * Receiver role:
 *   Incoming datagrams from the sender are queued directly
 *   for transport_receive().
 */
static void *transport_receive_thread(void *argument)
{
    Transport *transport = argument;
    uint8_t buffer[TRANSPORT_MAX_DATAGRAM_SIZE];

    while (!transport_is_shutting_down(transport)) {
        fd_set read_set;
        struct timeval timeout;
        int selected;

        FD_ZERO(&read_set);
        FD_SET(transport->socket_handle, &read_set);

        timeout.tv_sec =
            TRANSPORT_SOCKET_POLL_INTERVAL_MS / 1000U;

        timeout.tv_usec =
            (TRANSPORT_SOCKET_POLL_INTERVAL_MS % 1000U)
            * 1000U;

#ifdef _WIN32
        selected = select(
            0,
            &read_set,
            NULL,
            NULL,
            &timeout
        );
#else
        selected = select(
            transport->socket_handle + 1,
            &read_set,
            NULL,
            NULL,
            &timeout
        );
#endif

        if (selected == 0) {
            continue;
        }

        if (selected < 0) {
            if (socket_error_interrupted()) {
                continue;
            }

            if (transport_is_shutting_down(transport)) {
                break;
            }

            transport_set_async_error(transport);
            break;
        }

        if (transport_is_shutting_down(transport)) {
            break;
        }

        {
            struct sockaddr_storage source;
            TransportSocketLength source_length =
                (TransportSocketLength)sizeof(source);

#ifdef _WIN32
            int received = recvfrom(
                transport->socket_handle,
                (char *)buffer,
                (int)sizeof(buffer),
                0,
                (struct sockaddr *)&source,
                &source_length
            );
#else
            ssize_t received = recvfrom(
                transport->socket_handle,
                buffer,
                sizeof(buffer),
                0,
                (struct sockaddr *)&source,
                &source_length
            );
#endif

            if (received < 0) {
                if (socket_error_interrupted() ||
                    socket_error_would_block()) {
                    continue;
                }

                if (transport_is_shutting_down(transport)) {
                    break;
                }

                transport_set_async_error(transport);
                break;
            }

            /*
             * The project protocol does not use empty UDP
             * datagrams. Ignore them.
             */
            if (received == 0) {
                continue;
            }

            /*
             * Accept datagrams only from the configured peer.
             */
            if (!same_endpoint(
                    &source,
                    &transport->peer_endpoint)) {
                continue;
            }

            if (transport->role == TRANSPORT_SENDER) {
                /*
                 * Do not expose raw inbound datagrams to ARQ.
                 * They must pass through the emulator first.
                 */
                if (!channel_emulator_submit(
                        transport->emulator,
                        CHANNEL_DIRECTION_INBOUND,
                        buffer,
                        (size_t)received)) {
                    /*
                     * Submission failure means the emulator
                     * could not accept the datagram. Treat
                     * this as a dropped datagram, not a fatal
                     * socket error.
                     */
                }
            } else {
                /*
                 * Receiver role: the sender-side emulator
                 * has already applied outbound conditions.
                 */
                (void)transport_queue_datagram(
                    transport,
                    buffer,
                    (size_t)received
                );
            }
        }
    }

    return NULL;
}


/* ---------------------------------------------------------
 * Initialization and cleanup
 * --------------------------------------------------------- */

static void transport_release(Transport *transport)
{
    if (transport == NULL) {
        return;
    }

    /*
     * Stop new work first. The receiving thread checks this
     * flag at least every polling interval.
     */
    if (transport->mutex_initialized) {
        pthread_mutex_lock(&transport->mutex);

        transport->shutting_down = true;

        if (transport->condition_initialized) {
            pthread_cond_broadcast(
                &transport->queue_condition
            );
        }

        pthread_mutex_unlock(&transport->mutex);
    }

    /*
     * Join the UDP receiver before destroying the emulator.
     * This ensures it cannot submit new inbound datagrams
     * while the emulator is being destroyed.
     */
    if (transport->receive_thread_started) {
        pthread_join(transport->receive_thread, NULL);
        transport->receive_thread_started = false;
    }

    /*
     * Destroy the emulator before closing the socket because
     * its worker might currently be finishing a callback.
     */
    if (transport->emulator != NULL) {
        channel_emulator_destroy(transport->emulator);
        transport->emulator = NULL;
    }

    if (transport->socket_handle != TRANSPORT_INVALID_SOCKET) {
        close_socket(transport->socket_handle);
        transport->socket_handle = TRANSPORT_INVALID_SOCKET;
    }

    if (transport->mutex_initialized) {
        transport_free_queue(transport);
    }

    if (transport->condition_initialized) {
        pthread_cond_destroy(&transport->queue_condition);
    }

    if (transport->mutex_initialized) {
        pthread_mutex_destroy(&transport->mutex);
    }

#ifdef _WIN32
    if (transport->winsock_started) {
        WSACleanup();
    }
#endif

    free(transport);
}


/* ---------------------------------------------------------
 * Public API
 * --------------------------------------------------------- */

Transport *transport_init(const TransportConfig *config)
{
    Transport *transport;
    const char *local_address;
    uint16_t local_port;

    struct sockaddr_storage local_endpoint;
    TransportSocketLength local_endpoint_length;

    if (config == NULL ||
        config->sender_address == NULL ||
        config->receiver_address == NULL ||
        config->sender_port == 0 ||
        config->receiver_port == 0) {
        return NULL;
    }

    if (config->role != TRANSPORT_SENDER &&
        config->role != TRANSPORT_RECEIVER) {
        return NULL;
    }

    if (config->role == TRANSPORT_SENDER &&
        config->channel_config == NULL) {
        return NULL;
    }

    transport = calloc(1, sizeof(*transport));
    if (transport == NULL) {
        return NULL;
    }

    transport->socket_handle = TRANSPORT_INVALID_SOCKET;
    transport->role = config->role;

#ifdef _WIN32
    {
        WSADATA winsock_data;

        if (WSAStartup(
                MAKEWORD(2, 2),
                &winsock_data) != 0) {
            transport_release(transport);
            return NULL;
        }

        transport->winsock_started = true;
    }
#endif

    if (pthread_mutex_init(&transport->mutex, NULL) != 0) {
        transport_release(transport);
        return NULL;
    }
    transport->mutex_initialized = true;

    if (pthread_cond_init(
            &transport->queue_condition,
            NULL) != 0) {
        transport_release(transport);
        return NULL;
    }
    transport->condition_initialized = true;

    /*
     * Choose this instance's local endpoint and its peer.
     */
    if (config->role == TRANSPORT_SENDER) {
        local_address = config->sender_address;
        local_port = config->sender_port;

        if (!resolve_endpoint(
                config->receiver_address,
                config->receiver_port,
                &transport->peer_endpoint,
                &transport->peer_endpoint_length)) {
            transport_release(transport);
            return NULL;
        }
    } else {
        local_address = config->receiver_address;
        local_port = config->receiver_port;

        if (!resolve_endpoint(
                config->sender_address,
                config->sender_port,
                &transport->peer_endpoint,
                &transport->peer_endpoint_length)) {
            transport_release(transport);
            return NULL;
        }
    }

    if (!resolve_endpoint(
            local_address,
            local_port,
            &local_endpoint,
            &local_endpoint_length)) {
        transport_release(transport);
        return NULL;
    }

    transport->socket_handle = socket(
        AF_INET,
        SOCK_DGRAM,
        IPPROTO_UDP
    );

    if (transport->socket_handle == TRANSPORT_INVALID_SOCKET) {
        transport_release(transport);
        return NULL;
    }

    if (bind(
            transport->socket_handle,
            (const struct sockaddr *)&local_endpoint,
            local_endpoint_length) != 0) {
        transport_release(transport);
        return NULL;
    }

    /*
     * Only the sender-side Transport owns a Channel Emulator.
     */
    if (config->role == TRANSPORT_SENDER) {
        transport->emulator = channel_emulator_create(
            config->channel_config,
            transport_on_emulator_output,
            transport
        );

        if (transport->emulator == NULL) {
            transport_release(transport);
            return NULL;
        }
    }

    /*
     * Start the UDP receive thread only after all state
     * needed by that thread has been initialized.
     */
    if (pthread_create(
            &transport->receive_thread,
            NULL,
            transport_receive_thread,
            transport) != 0) {
        transport_release(transport);
        return NULL;
    }

    transport->receive_thread_started = true;

    return transport;
}


bool transport_send(
    Transport *transport,
    const uint8_t *data,
    size_t length)
{
    if (transport == NULL ||
        data == NULL ||
        length == 0 ||
        length > TRANSPORT_MAX_DATAGRAM_SIZE) {
        return false;
    }

    if (transport_is_shutting_down(transport)) {
        return false;
    }

    if (transport->role == TRANSPORT_SENDER) {
        /*
         * The emulator copies the datagram and processes it
         * asynchronously. A successful result means accepted
         * for processing, not guaranteed network delivery.
         */
        return channel_emulator_submit(
            transport->emulator,
            CHANNEL_DIRECTION_OUTBOUND,
            data,
            length
        );
    }

    /*
     * Receiver-side output is an ACK/NACK or other response.
     * The sender-side emulator will process it on arrival.
     */
    return socket_send_datagram(
        transport->socket_handle,
        &transport->peer_endpoint,
        transport->peer_endpoint_length,
        data,
        length
    );
}


TransportReceiveStatus transport_receive(
    Transport *transport,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *received_length,
    uint32_t timeout_ms)
{
    struct timespec deadline;
    TransportDatagram *datagram;
    int wait_result = 0;

    if (received_length != NULL) {
        *received_length = 0;
    }

    if (transport == NULL ||
        buffer == NULL ||
        buffer_capacity == 0 ||
        received_length == NULL) {
        return TRANSPORT_RECEIVE_ERROR;
    }

    /*
     * pthread_cond_timedwait uses an absolute deadline.
     * TIME_UTC matches the default condition-variable clock.
     */
    if (timespec_get(&deadline, TIME_UTC) != TIME_UTC) {
        return TRANSPORT_RECEIVE_ERROR;
    }

    deadline.tv_sec += timeout_ms / 1000U;
    deadline.tv_nsec +=
        (long)(timeout_ms % 1000U) * 1000000L;

    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&transport->mutex);

    while (transport->queue_head == NULL &&
           !transport->asynchronous_error &&
           !transport->shutting_down) {

        if (timeout_ms == 0) {
            pthread_mutex_unlock(&transport->mutex);
            return TRANSPORT_RECEIVE_TIMEOUT;
        }

        wait_result = pthread_cond_timedwait(
            &transport->queue_condition,
            &transport->mutex,
            &deadline
        );

        if (wait_result == ETIMEDOUT &&
            transport->queue_head == NULL) {
            pthread_mutex_unlock(&transport->mutex);
            return TRANSPORT_RECEIVE_TIMEOUT;
        }

        if (wait_result != 0 && wait_result != ETIMEDOUT) {
            pthread_mutex_unlock(&transport->mutex);
            return TRANSPORT_RECEIVE_ERROR;
        }
    }

    /*
     * Return already-queued datagrams before reporting an
     * asynchronous socket error.
     */
    datagram = transport->queue_head;

    if (datagram == NULL) {
        pthread_mutex_unlock(&transport->mutex);
        return TRANSPORT_RECEIVE_ERROR;
    }

    /*
     * UDP datagrams must be returned whole, never partially.
     * Remove and discard a datagram that will not fit.
     */
    if (datagram->length > buffer_capacity) {
        transport->queue_head = datagram->next;

        if (transport->queue_head == NULL) {
            transport->queue_tail = NULL;
        }

        transport->queue_bytes -= datagram->length;

        pthread_mutex_unlock(&transport->mutex);

        free(datagram->data);
        free(datagram);

        return TRANSPORT_RECEIVE_ERROR;
    }

    transport->queue_head = datagram->next;

    if (transport->queue_head == NULL) {
        transport->queue_tail = NULL;
    }

    transport->queue_bytes -= datagram->length;

    memcpy(buffer, datagram->data, datagram->length);
    *received_length = datagram->length;

    pthread_mutex_unlock(&transport->mutex);

    free(datagram->data);
    free(datagram);

    return TRANSPORT_RECEIVE_OK;
}


void transport_close(Transport *transport)
{
    transport_release(transport);
}