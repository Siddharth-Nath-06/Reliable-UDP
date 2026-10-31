#include "transport/transport.h"
#include "channel/channel_emulator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET TestSocket;
    typedef int TestSocketLength;
    #define TEST_INVALID_SOCKET INVALID_SOCKET
    #define test_close_socket closesocket
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
    typedef int TestSocket;
    typedef socklen_t TestSocketLength;
    #define TEST_INVALID_SOCKET (-1)
    #define test_close_socket close
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

#define TEST_ADDRESS "127.0.0.1"
#define RECEIVE_TIMEOUT_MS 2000U

typedef struct {
    uint16_t sender_port;
    uint16_t receiver_port;
} TestPorts;

static bool get_ephemeral_port(TestSocket socket_handle, uint16_t *port)
{
    struct sockaddr_in address;
    TestSocketLength address_length = (TestSocketLength)sizeof(address);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);

    if (bind(socket_handle, (const struct sockaddr *)&address,
             (TestSocketLength)sizeof(address)) != 0) {
        return false;
    }

    if (getsockname(socket_handle, (struct sockaddr *)&address,
                     &address_length) != 0) {
        return false;
    }

    *port = ntohs(address.sin_port);
    return *port != 0;
}

static bool allocate_test_ports(TestPorts *ports)
{
    TestSocket first = TEST_INVALID_SOCKET;
    TestSocket second = TEST_INVALID_SOCKET;
    bool success = false;

    if (ports == NULL) {
        return false;
    }

    first = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (first == TEST_INVALID_SOCKET) {
        goto cleanup;
    }

    second = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (second == TEST_INVALID_SOCKET) {
        goto cleanup;
    }

    if (!get_ephemeral_port(first, &ports->sender_port) ||
        !get_ephemeral_port(second, &ports->receiver_port) ||
        ports->sender_port == ports->receiver_port) {
        goto cleanup;
    }

    success = true;

cleanup:
    if (first != TEST_INVALID_SOCKET) {
        test_close_socket(first);
    }
    if (second != TEST_INVALID_SOCKET) {
        test_close_socket(second);
    }

    return success;
}

static ChannelEmulatorConfig make_channel_config(void)
{
    ChannelEmulatorConfig config;

    memset(&config, 0, sizeof(config));
    config.bandwidth_bps = 1000000U;
    config.delay_ms = 0;
    config.jitter_ms = 0;
    config.loss_probability = 0.0;
    config.duplication_probability = 0.0;
    config.corruption_probability = 0.0;
    config.reordering_probability = 0.0;
    config.reordering_delay_ms = 0;
    config.queue_limit_bytes = 1024U * 1024U;
    config.random_seed = 12345U;

    return config;
}

static bool make_transport_pair(
    Transport **sender_out,
    Transport **receiver_out,
    TestPorts *ports_out,
    ChannelEmulatorConfig *channel_config_out)
{
    TransportConfig sender_config;
    TransportConfig receiver_config;

    CHECK(sender_out != NULL);
    CHECK(receiver_out != NULL);
    CHECK(ports_out != NULL);
    CHECK(channel_config_out != NULL);

    *sender_out = NULL;
    *receiver_out = NULL;

    CHECK(allocate_test_ports(ports_out));
    *channel_config_out = make_channel_config();

    memset(&sender_config, 0, sizeof(sender_config));
    sender_config.role = TRANSPORT_SENDER;
    sender_config.sender_address = TEST_ADDRESS;
    sender_config.sender_port = ports_out->sender_port;
    sender_config.receiver_address = TEST_ADDRESS;
    sender_config.receiver_port = ports_out->receiver_port;
    sender_config.channel_config = channel_config_out;

    memset(&receiver_config, 0, sizeof(receiver_config));
    receiver_config.role = TRANSPORT_RECEIVER;
    receiver_config.sender_address = TEST_ADDRESS;
    receiver_config.sender_port = ports_out->sender_port;
    receiver_config.receiver_address = TEST_ADDRESS;
    receiver_config.receiver_port = ports_out->receiver_port;
    receiver_config.channel_config = NULL;

    *sender_out = transport_init(&sender_config);
    CHECK(*sender_out != NULL);

    *receiver_out = transport_init(&receiver_config);
    if (*receiver_out == NULL) {
        transport_close(*sender_out);
        *sender_out = NULL;
        return false;
    }

    return true;
}

static bool test_invalid_arguments(void)
{
    TransportConfig config;
    Transport *transport;
    size_t received_length = 123U;
    uint8_t byte = 0;

    memset(&config, 0, sizeof(config));
    config.role = TRANSPORT_SENDER;
    config.sender_address = TEST_ADDRESS;
    config.sender_port = 41001U;
    config.receiver_address = TEST_ADDRESS;
    config.receiver_port = 41002U;
    config.channel_config = NULL;

    transport = transport_init(&config);
    CHECK(transport == NULL);

    CHECK(!transport_send(NULL, &byte, sizeof(byte)));
    CHECK(!transport_send(NULL, NULL, 1U));

    CHECK(transport_receive(
        NULL, &byte, sizeof(byte), &received_length, 0U
    ) == TRANSPORT_RECEIVE_ERROR);
    CHECK(received_length == 0U);

    puts("PASS: invalid arguments and missing sender channel config");
    return true;
}

static bool test_receive_timeout(
    Transport *receiver)
{
    uint8_t buffer[32];
    size_t received_length = 999U;

    CHECK(transport_receive(
        receiver,
        buffer,
        sizeof(buffer),
        &received_length,
        50U
    ) == TRANSPORT_RECEIVE_TIMEOUT);

    CHECK(received_length == 0U);
    puts("PASS: receive timeout");
    return true;
}

static bool test_sender_to_receiver(
    Transport *sender,
    Transport *receiver)
{
    static const uint8_t payload[] = {
        0x00, 0x01, 0x02, 0x7F, 0x80, 0xFE, 0xFF
    };
    uint8_t received[64];
    size_t received_length = 0;

    CHECK(!transport_send(sender, payload, 0U));
    CHECK(!transport_send(sender, NULL, sizeof(payload)));
    CHECK(transport_send(sender, payload, sizeof(payload)));

    CHECK(transport_receive(
        receiver,
        received,
        sizeof(received),
        &received_length,
        RECEIVE_TIMEOUT_MS
    ) == TRANSPORT_RECEIVE_OK);

    CHECK(received_length == sizeof(payload));
    CHECK(memcmp(received, payload, sizeof(payload)) == 0);

    puts("PASS: outbound datagram reaches receiver unchanged");
    return true;
}

static bool test_receiver_to_sender(
    Transport *sender,
    Transport *receiver)
{
    static const uint8_t ack[] = { 'A', 'C', 'K', 0x00, 0x42 };
    uint8_t received[64];
    size_t received_length = 0;

    CHECK(transport_send(receiver, ack, sizeof(ack)));

    CHECK(transport_receive(
        sender,
        received,
        sizeof(received),
        &received_length,
        RECEIVE_TIMEOUT_MS
    ) == TRANSPORT_RECEIVE_OK);

    CHECK(received_length == sizeof(ack));
    CHECK(memcmp(received, ack, sizeof(ack)) == 0);

    puts("PASS: inbound response passes through emulator to sender");
    return true;
}

static bool test_receive_buffer_too_small(
    Transport *sender,
    Transport *receiver)
{
    static const uint8_t payload[] = { 10, 20, 30, 40, 50, 60 };
    uint8_t too_small[2];
    uint8_t next_buffer[32];
    size_t received_length = 777U;

    CHECK(transport_send(sender, payload, sizeof(payload)));

    CHECK(transport_receive(
        receiver,
        too_small,
        sizeof(too_small),
        &received_length,
        RECEIVE_TIMEOUT_MS
    ) == TRANSPORT_RECEIVE_ERROR);
    CHECK(received_length == 0U);

    /* The current public contract discards a datagram that will not fit. */
    CHECK(transport_receive(
        receiver,
        next_buffer,
        sizeof(next_buffer),
        &received_length,
        100U
    ) == TRANSPORT_RECEIVE_TIMEOUT);

    puts("PASS: undersized receive buffer reports error and discards datagram");
    return true;
}

int main(void)
{
#ifdef _WIN32
    WSADATA winsock_data;
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        fputs("FAIL: WSAStartup\n", stderr);
        return EXIT_FAILURE;
    }
#endif

    {
        Transport *sender = NULL;
        Transport *receiver = NULL;
        TestPorts ports;
        ChannelEmulatorConfig channel_config;
        bool passed = true;

        passed = passed && test_invalid_arguments();

        if (passed && make_transport_pair(
                &sender,
                &receiver,
                &ports,
                &channel_config)) {
            passed = passed && test_receive_timeout(receiver);
            passed = passed && test_sender_to_receiver(sender, receiver);
            passed = passed && test_receiver_to_sender(sender, receiver);
            passed = passed && test_receive_buffer_too_small(sender, receiver);
        } else {
            fputs("FAIL: could not initialize sender/receiver pair\n", stderr);
            passed = false;
        }

        if (receiver != NULL) {
            transport_close(receiver);
        }
        if (sender != NULL) {
            transport_close(sender);
        }

#ifdef _WIN32
        WSACleanup();
#endif

        if (!passed) {
            return EXIT_FAILURE;
        }
    }

    puts("All Transport black-box tests passed.");
    return EXIT_SUCCESS;
}
