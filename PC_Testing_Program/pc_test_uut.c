#include "pc_test_uut.h"
#include "test_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <arpa/inet.h>

/* Internal list for --all option mapping */
typedef struct peripherals{
    const char *name;
    uint8_t id;
} peripherals_t;

static const peripherals_t g_peripherals[] = {
    {"timer", TIMER_TEST},
    {"uart",  UART_TEST},
    {"spi",   SPI_TEST},
    {"i2c",   I2C_TEST},
    {"adc",   ADC_TEST}
};

#define PERIPH_COUNT (sizeof(g_peripherals) / sizeof(g_peripherals[0]))

#define STATUS_SUCCESS  0
#define STATUS_COMM_FAILURE  -1
/**
 * @brief Logic to send a single UDP command and wait for a result.
 * @return 0 on success, -1 on communication failure.
 */
static int run_single_test(int sockfd, test_db_t *db, uint8_t periph_id, 
                           const char *name, uint8_t iterations, const char *pattern) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET, 
        .sin_port = htons(UUT_PORT), 
        .sin_addr.s_addr = inet_addr(UUT_IP)
    };
    
    test_command_t cmd = {0};
    test_result_t res = {0};
    struct timespec start, end;
    socklen_t slen = sizeof(addr);

    cmd.test_id = (uint32_t)time(NULL) + periph_id; /* Ensure unique ID in batch */
    cmd.peripheral_id = periph_id;
    cmd.iterations = iterations;
    
    size_t plen = strlen(pattern);
    cmd.pattern_len = (plen > 255) ? 255 : (uint8_t)plen;
    strncpy((char*)cmd.pattern, pattern, cmd.pattern_len);

    printf("Starting test for %s (ID: %u)... ", name, cmd.test_id);
    fflush(stdout);

    clock_gettime(CLOCK_MONOTONIC, &start);

    if (sendto(sockfd, &cmd, sizeof(cmd), 0, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Send failed: %s\n", strerror(errno));
        return STATUS_COMM_FAILURE;
    }

    if (recvfrom(sockfd, &res, sizeof(res), 0, (struct sockaddr*)&addr, &slen) < 0) {
        fprintf(stderr, "Timeout\n");
        return STATUS_COMM_FAILURE;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double dur = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

    const char *status_str = (res.result == 1) ? "SUCCESS" : "FAILURE";
    printf("%s (%.3fs)\n", status_str, dur);

    return test_db_save(db, res.test_id, dur, name, status_str);
}

int main(int argc, char *argv[]) {
    int sockfd = STATUS_COMM_FAILURE;
    test_db_t *db = NULL;
    int status = EXIT_FAILURE;

    db = test_db_init("test_results.db");
    if (db == NULL) {
        fprintf(stderr, "Database error\n");
        return EXIT_FAILURE;
    }

    /* Print on demand */
    if (argc == 2 && strcmp(argv[1], "--report") == 0) {
        test_db_print_report(db);
        status = EXIT_SUCCESS;
        goto cleanup;
    }

    if (argc == 2 && strcmp(argv[1], "--clean") == 0) {
        if (test_db_clear(db) == 0) {
            printf("Database cleaned successfully.\n");
            status = EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Failed to clean database.\n");
            status = EXIT_FAILURE;
        }
        goto cleanup;
    }
    
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <peripheral|--all> <iterations> [pattern]\n", argv[0]);
        goto cleanup;
    }

    /* Network Setup */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "Socket error: %s\n", strerror(errno));
        goto cleanup;
    }

    struct timeval tv = { .tv_sec = TIMEOUT_SEC };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t iterations = (uint8_t)atoi(argv[2]);
    const char *pattern = (argc > 3) ? argv[3] : "Dummy Payload";

    /* Check for --all or single peripheral */
    if (strcmp(argv[1], "--all") == 0) {
        printf("Running verification for ALL peripherals...\n");
        for (size_t i = 0; i < PERIPH_COUNT; ++i) {
            run_single_test(sockfd, db, g_peripherals[i].id, 
                            g_peripherals[i].name, iterations, pattern);
            /* Short sleep to let UUT process transition if needed */
            usleep(100000); 
        }
        status = EXIT_SUCCESS;
    } else {
        uint8_t id = 0;
        for (size_t i = 0; i < PERIPH_COUNT; ++i) {
            if (strcasecmp(argv[1], g_peripherals[i].name) == 0) {
                id = g_peripherals[i].id;
                break;
            }
        }

        if (id == 0) {
            fprintf(stderr, "Unknown peripheral: %s\n", argv[1]);
            goto cleanup;
        }

        if (run_single_test(sockfd, db, id, argv[1], iterations, pattern) == 0) {
            status = EXIT_SUCCESS;
        }
    }

cleanup:
    if (sockfd != STATUS_COMM_FAILURE) { 
        close(sockfd); 
    }
    test_db_destroy(db);
    return status;
}