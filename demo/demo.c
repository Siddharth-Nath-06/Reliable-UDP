/* Interactive two-computer demo for Reliable-UDP.
 *
 * Run this executable on both computers.
 *
 * Receiver:
 *   1. Select Receiver.
 *   2. Enter the sender's IPv4 address.
 *   3. Select the ARQ protocol that the sender will use.
 *   4. Wait for the transfer.
 *
 * Sender:
 *   1. Select Sender.
 *   2. Enter the receiver's IPv4 address.
 *   3. Select a transfer preset.
 *   4. Send a generated dummy file and display statistics.
 *
 * IMPORTANT:
 * The current protocol handshake does not negotiate the ARQ protocol.
 * Therefore, the receiver must select the same protocol as the sender.
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/driver/driver.h"

#define SENDER_PORT       49171
#define RECEIVER_PORT     49172

#define SOURCE_PATH       "experiments/demo_source.bin"
#define DEST_PATH         "experiments/demo_received.bin"

#define DEMO_FILE_SIZE    (32U * 1024U)
#define INPUT_SIZE        256

typedef struct {
    const char *name;
    const char *description;
    ArqProtocol protocol;
    uint16_t window;
    double loss;
    double duplication;
    double corruption;
    double reordering;
    uint32_t reordering_delay_ms;
} Preset;

static const Preset presets[] = {
    {
        "Basic transfer",
        "Stop-and-Wait, clean channel",
        ARQ_STOP_WAIT, 1, 0, 0, 0, 0, 0
    },
    {
        "Fast transfer",
        "Selective Repeat, clean channel",
        ARQ_SELECTIVE_REPEAT, 4, 0, 0, 0, 0, 0
    },
    {
        "Packet loss",
        "Selective Repeat, 5% packet loss",
        ARQ_SELECTIVE_REPEAT, 4, 0.05, 0, 0, 0, 0
    },
    {
        "Packet reordering",
        "Go-Back-N, 5% packet reordering",
        ARQ_GO_BACK_N, 4, 0, 0, 0, 0.05, 20
    },
    {
        "Noisy network",
        "5% loss, 2% duplication, 1% corruption, 5% reordering",
        ARQ_SELECTIVE_REPEAT, 4, 0.05, 0.02, 0.01, 0.05, 20
    }
};

static const char *protocol_name(ArqProtocol protocol)
{
    switch (protocol) {
        case ARQ_STOP_WAIT:
            return "Stop-and-Wait";

        case ARQ_GO_BACK_N:
            return "Go-Back-N";

        case ARQ_SELECTIVE_REPEAT:
            return "Selective Repeat";

        default:
            return "Unknown";
    }
}

static const char *phase_name(DriverPhase phase)
{
    switch (phase) {
        case DRIVER_PHASE_INITIALIZING:
            return "Initializing";

        case DRIVER_PHASE_WAITING_FOR_PEER:
            return "Waiting for peer";

        case DRIVER_PHASE_NEGOTIATING:
            return "Negotiating";

        case DRIVER_PHASE_TRANSFERRING:
            return "Transferring";

        case DRIVER_PHASE_VERIFYING:
            return "Verifying";

        case DRIVER_PHASE_FINISHING:
            return "Finishing";

        case DRIVER_PHASE_COMPLETED:
            return "Completed";

        case DRIVER_PHASE_FAILED:
            return "Failed";

        default:
            return "Working";
    }
}

static void progress_callback(void *context, const DriverProgress *progress)
{
    (void)context;

    if (progress->total_bytes > 0 &&
        progress->phase == DRIVER_PHASE_TRANSFERRING) {

        unsigned percent = (unsigned)(
            (progress->bytes_processed * 100U) /
            progress->total_bytes
        );

        if (percent > 100)
            percent = 100;

        unsigned filled = percent / 5U;

        printf("\r  [");

        for (unsigned i = 0; i < 20; ++i)
            putchar(i < filled ? '#' : '-');

        printf("] %3u%%", percent);
        fflush(stdout);

        if (percent == 100)
            putchar('\n');
    } else {
        printf("\r  %-20s", phase_name(progress->phase));
        fflush(stdout);
    }
}

static bool read_line(const char *prompt, char *buffer, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);

    if (!fgets(buffer, (int)size, stdin))
        return false;

    buffer[strcspn(buffer, "\r\n")] = '\0';
    return true;
}

static bool read_number(
    const char *prompt,
    long minimum,
    long maximum,
    long *value)
{
    char input[INPUT_SIZE];

    for (;;) {
        if (!read_line(prompt, input, sizeof input))
            return false;

        char *end = NULL;
        long parsed = strtol(input, &end, 10);

        while (end && (*end == ' ' || *end == '\t'))
            ++end;

        if (end != input &&
            end != NULL &&
            *end == '\0' &&
            parsed >= minimum &&
            parsed <= maximum) {
            *value = parsed;
            return true;
        }

        printf("Enter a number from %ld to %ld.\n",
               minimum, maximum);
    }
}

/*
 * Finds a non-loopback IPv4 address associated with the machine hostname.
 * If discovery fails, the user can still enter the address manually.
 */
static bool discover_local_ipv4(char *output, size_t output_size)
{
    char hostname[256];

    if (gethostname(hostname, sizeof hostname) != 0)
        return false;

    hostname[sizeof hostname - 1] = '\0';

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *addresses = NULL;

    if (getaddrinfo(hostname, NULL, &hints, &addresses) != 0)
        return false;

    bool found = false;

    for (struct addrinfo *entry = addresses;
         entry != NULL;
         entry = entry->ai_next) {

        struct sockaddr_in *address =
            (struct sockaddr_in *)entry->ai_addr;

        char ip[INET_ADDRSTRLEN];

        if (!inet_ntop(AF_INET, &address->sin_addr,
                       ip, sizeof ip)) {
            continue;
        }

        if (strncmp(ip, "127.", 4) == 0)
            continue;

        if (strlen(ip) + 1 > output_size)
            continue;

        strcpy(output, ip);
        found = true;
        break;
    }

    freeaddrinfo(addresses);
    return found;
}

static bool valid_ipv4(const char *ip)
{
    struct in_addr address;
    return inet_pton(AF_INET, ip, &address) == 1;
}

static bool write_fixture(const char *path, size_t size)
{
    FILE *file = fopen(path, "wb");

    if (!file) {
        perror("Could not create source file");
        return false;
    }

    for (size_t i = 0; i < size; ++i) {
        unsigned char byte =
            (unsigned char)((i * 31U + 17U) & 0xffU);

        if (fwrite(&byte, 1, 1, file) != 1) {
            perror("Could not write source file");
            fclose(file);
            return false;
        }
    }

    if (fclose(file) != 0) {
        perror("Could not close source file");
        return false;
    }

    return true;
}

static void print_statistics(
    const char *label,
    const DriverResult *result)
{
    printf("\n%s statistics\n", label);
    printf("------------------------------\n");

    if (!result->monitor_stats_available) {
        puts("Monitoring statistics unavailable.");
        return;
    }

    const MonitorStats *metrics = &result->metrics;

    printf("Packets sent:      %" PRIu64 "\n",
           metrics->packets_sent);

    printf("Retransmissions:   %" PRIu64 "\n",
           metrics->retransmissions);

    printf("Timeouts:          %" PRIu64 "\n",
           metrics->timeouts);

    printf("Duration:          %" PRIu64 " ms\n",
           metrics->duration_ms);

    printf("Goodput:           %.1f bytes/s\n",
           metrics->goodput_bytes_per_second);
}

static void print_preset_menu(void)
{
    puts("\nSelect transfer configuration:");
    puts("----------------------------------------");

    for (size_t i = 0;
         i < sizeof presets / sizeof presets[0];
         ++i) {

        printf("%zu. %-20s %s\n",
               i + 1,
               presets[i].name,
               presets[i].description);
    }

    puts("0. Cancel");
}

static bool choose_protocol(ArqProtocol *protocol)
{
    long choice;

    puts("\nSelect the protocol the sender will use:");
    puts("1. Stop-and-Wait");
    puts("2. Go-Back-N");
    puts("3. Selective Repeat");

    if (!read_number("Protocol: ", 1, 3, &choice))
        return false;

    switch (choice) {
        case 1:
            *protocol = ARQ_STOP_WAIT;
            break;

        case 2:
            *protocol = ARQ_GO_BACK_N;
            break;

        case 3:
            *protocol = ARQ_SELECTIVE_REPEAT;
            break;

        default:
            return false;
    }

    return true;
}

static void configure_common(
    DriverConfig *config,
    ArqProtocol protocol,
    uint16_t window)
{
    config->mode = DRIVER_MODE_DEMO;
    config->enable_monitoring = true;

    config->protocol = protocol;
    config->window_size = window;
    config->chunk_size = 512;

    config->initial_rto_ms = 200;
    config->min_rto_ms = 50;
    config->max_rto_ms = 2000;

    config->receive_timeout_ms = 20;
    config->control_timeout_ms = 500;
    config->max_control_retries = 5;
    config->max_transfer_duration_ms = 30000;
}

static void configure_channel(
    DriverConfig *config,
    const Preset *preset,
    uint32_t seed)
{
    config->channel.bandwidth_bps = 100000000U;

    config->channel.loss_probability = preset->loss;
    config->channel.duplication_probability = preset->duplication;
    config->channel.corruption_probability = preset->corruption;
    config->channel.reordering_probability = preset->reordering;

    config->channel.reordering_delay_ms =
        preset->reordering_delay_ms;

    config->channel.queue_limit_bytes = 1024U * 1024U;
    config->channel.random_seed = seed;
}

static bool run_receiver(const char *local_ip)
{
    char sender_ip[INPUT_SIZE];

    puts("\n=== RECEIVER MODE ===");

    if (!read_line("Enter sender IPv4 address: ",
                   sender_ip, sizeof sender_ip)) {
        return false;
    }

    if (!valid_ipv4(sender_ip)) {
        puts("Invalid IPv4 address.");
        return false;
    }

    ArqProtocol protocol;

    if (!choose_protocol(&protocol))
        return false;

    uint16_t window =
        protocol == ARQ_STOP_WAIT ? 1 : 4;

    DriverConfig config = {0};
    DriverResult result = {0};

    configure_common(&config, protocol, window);

    config.role = TRANSPORT_RECEIVER;

    /*
     * The receiver listens on its own address and expects packets
     * from the sender's address.
     */
    config.sender_address = sender_ip;
    config.sender_port = SENDER_PORT;

    config.receiver_address = local_ip;
    config.receiver_port = RECEIVER_PORT;

    config.destination_path = DEST_PATH;

    printf("\nReceiver endpoint\n");
    printf("------------------------------\n");
    printf("Local IP:          %s\n", local_ip);
    printf("Local UDP port:    %d\n", RECEIVER_PORT);
    printf("Expected sender:   %s:%d\n",
           sender_ip, SENDER_PORT);
    printf("Protocol:          %s\n",
           protocol_name(protocol));
    printf("Destination file:  %s\n", DEST_PATH);

    puts("\nWaiting for sender. Start the sender on the other computer.");

    DriverStatus status = driver_run(&config, &result);

    printf("\nReceiver status: %s\n",
           driver_status_string(status));

    printf("Integrity verified: %s\n",
           result.integrity_verified ? "YES" : "NO");

    print_statistics("Receiver", &result);

    if (status == DRIVER_STATUS_OK &&
        result.integrity_verified) {
        puts("\nTransfer received successfully.");
    } else {
        puts("\nTransfer failed or integrity could not be verified.");
    }

    return status == DRIVER_STATUS_OK &&
           result.integrity_verified;
}

static bool run_sender(const char *local_ip)
{
    char receiver_ip[INPUT_SIZE];

    puts("\n=== SENDER MODE ===");

    if (!read_line("Enter receiver IPv4 address: ",
                   receiver_ip, sizeof receiver_ip)) {
        return false;
    }

    if (!valid_ipv4(receiver_ip)) {
        puts("Invalid IPv4 address.");
        return false;
    }

    print_preset_menu();

    long choice;

    if (!read_number("Configuration: ", 0, 5, &choice))
        return false;

    if (choice == 0) {
        puts("Transfer cancelled.");
        return true;
    }

    const Preset *preset = &presets[choice - 1];

    const uint32_t seed = 101U;

    if (!write_fixture(SOURCE_PATH, DEMO_FILE_SIZE))
        return false;

    DriverConfig config = {0};
    DriverResult result = {0};

    configure_common(&config, preset->protocol, preset->window);

    config.role = TRANSPORT_SENDER;

    config.sender_address = local_ip;
    config.sender_port = SENDER_PORT;

    config.receiver_address = receiver_ip;
    config.receiver_port = RECEIVER_PORT;

    config.source_path = SOURCE_PATH;
    config.progress_callback = progress_callback;

    configure_channel(&config, preset, seed);

    printf("\nSelected configuration: %s\n", preset->name);
    printf("Protocol:          %s\n",
           protocol_name(preset->protocol));
    printf("Window size:       %u\n", preset->window);
    printf("File size:         %u KiB\n",
           DEMO_FILE_SIZE / 1024U);
    printf("Random seed:       %u\n", seed);

    printf("Packet loss:       %.0f%%\n",
           preset->loss * 100.0);
    printf("Duplication:       %.0f%%\n",
           preset->duplication * 100.0);
    printf("Corruption:        %.0f%%\n",
           preset->corruption * 100.0);
    printf("Reordering:        %.0f%%\n",
           preset->reordering * 100.0);

    printf("\nSender endpoint:   %s:%d\n",
           local_ip, SENDER_PORT);
    printf("Receiver endpoint: %s:%d\n",
           receiver_ip, RECEIVER_PORT);

    puts("\nStarting transfer...");

    DriverStatus status = driver_run(&config, &result);

    printf("\n\nSender status: %s\n",
           driver_status_string(status));

    printf("Sender integrity verified: %s\n",
           result.integrity_verified ? "YES" : "NO");

    print_statistics("Sender", &result);

    if (status == DRIVER_STATUS_OK &&
        result.integrity_verified) {
        puts("\nSender reports a successful transfer.");
        puts("Check the receiver's terminal for its final result.");
    } else {
        puts("\nTransfer failed. Check the receiver and network configuration.");
    }

    (void)remove(SOURCE_PATH);

    return status == DRIVER_STATUS_OK &&
           result.integrity_verified;
}

int main(void)
{
#ifdef _WIN32
    WSADATA wsa_data;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "Could not initialize Winsock.\n");
        return EXIT_FAILURE;
    }
#endif

    char local_ip[INET_ADDRSTRLEN] = "";

    puts("========================================");
    puts("          RELIABLE-UDP DEMO");
    puts("========================================");

    if (discover_local_ipv4(local_ip, sizeof local_ip)) {
        printf("Detected local IPv4: %s\n", local_ip);
    } else {
        puts("Could not automatically detect a LAN IPv4 address.");
    }

    puts("Sender UDP port:    " 
         /* Keep the configured endpoint ports consistent. */);
    printf("%d\n", SENDER_PORT);
    printf("Receiver UDP port:  %d\n", RECEIVER_PORT);

    puts("\nMake sure both computers are on a reachable network.");
    puts("Allow the demo through the firewall for UDP traffic.");

    char input[INPUT_SIZE];

    if (!local_ip[0] ||
        !read_line(
            "\nPress Enter to use the detected IP, or enter a local IPv4 address: ",
            input, sizeof input)) {
#ifdef _WIN32
        WSACleanup();
#endif
        return EXIT_FAILURE;
    }

    if (input[0] != '\0') {
        if (!valid_ipv4(input)) {
            puts("Invalid local IPv4 address.");
#ifdef _WIN32
            WSACleanup();
#endif
            return EXIT_FAILURE;
        }

        strcpy(local_ip, input);
    }

    if (!local_ip[0]) {
        puts("A local IPv4 address is required.");
#ifdef _WIN32
        WSACleanup();
#endif
        return EXIT_FAILURE;
    }

    printf("\nThis computer will use: %s\n", local_ip);

    puts("\nSelect endpoint role:");
    puts("1. Receiver");
    puts("2. Sender");
    puts("0. Exit");

    long role;

    if (!read_number("Role: ", 0, 2, &role)) {
#ifdef _WIN32
        WSACleanup();
#endif
        return EXIT_FAILURE;
    }

    if (role == 0) {
        puts("Goodbye.");
#ifdef _WIN32
        WSACleanup();
#endif
        return EXIT_SUCCESS;
    }

    bool success;

    if (role == 1)
        success = run_receiver(local_ip);
    else
        success = run_sender(local_ip);

#ifdef _WIN32
    WSACleanup();
#endif

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}