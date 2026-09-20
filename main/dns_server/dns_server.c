/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file dns_server.c
 * @brief Wildcard DNS responder task.
 */
#include "dns_server.h"
#include "logging.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DNS_HEADER_SIZE (12)
#define DNS_PACKET_MAX (512)
#define DNS_ANSWER_SIZE (16)
#define DNS_TYPE_A (1)
#define DNS_CLASS_IN (1)
#define DNS_RECEIVE_TIMEOUT_MS (200)
#define DNS_STOP_TIMEOUT_MS (2000)
#define DNS_IDLE_CLEANUP_MS (20)
#define DNS_TASK_STACK_BYTES (3072)
#define DNS_TASK_PRIORITY (4)

LOG_MODULE_REGISTER("dns", LOG_LEVEL_DEBUG);

static StaticTask_t s_task_struct;
static StackType_t s_task_stack[DNS_TASK_STACK_BYTES];
static uint8_t s_packet[DNS_PACKET_MAX];
static TaskHandle_t s_task;
static volatile bool s_is_stop_requested;
static uint32_t s_ip_addr;

size_t BuildDnsResponse(const uint8_t *query, size_t query_length, uint32_t ip_addr,
                        uint8_t *response, size_t response_size)
{
    /* Header: QR must be 0 (query), exactly one question. */
    if (query_length < DNS_HEADER_SIZE + 5 || (query[2] & 0x80) != 0 || query[4] != 0 || query[5] != 1) {
        return 0;
    }

    size_t name_end = DNS_HEADER_SIZE;
    while (name_end < query_length && query[name_end] != 0) {
        uint8_t label_length = query[name_end];
        if (label_length > 63 || name_end + label_length + 1 >= query_length) {
            return 0;
        }
        name_end += (size_t)label_length + 1;
    }
    size_t question_end = name_end + 1 + 4;  /* terminator + QTYPE + QCLASS */
    if (name_end >= query_length || question_end > query_length ||
        question_end + DNS_ANSWER_SIZE > response_size) {
        return 0;
    }

    uint16_t type = (uint16_t)((query[name_end + 1] << 8) | query[name_end + 2]);
    uint16_t class_in = (uint16_t)((query[name_end + 3] << 8) | query[name_end + 4]);
    bool is_a_query = (type == DNS_TYPE_A && class_in == DNS_CLASS_IN);

    if (response != query) {
        memcpy(response, query, question_end);
    }
    response[2] = (uint8_t)(0x80 | (query[2] & 0x01));  /* QR, opcode 0, keep RD */
    response[3] = 0x80;                                 /* RA, RCODE 0 */
    response[6] = 0;
    response[7] = is_a_query ? 1 : 0;
    memset(&response[8], 0, 4);
    if (!is_a_query) {
        return question_end;
    }

    uint8_t *answer = &response[question_end];
    const uint8_t header[] = {0xC0, 0x0C, 0, DNS_TYPE_A, 0, DNS_CLASS_IN,
                              0, 0, 0, DNS_SERVER_TTL_S, 0, 4};
    memcpy(answer, header, sizeof(header));
    memcpy(answer + sizeof(header), &ip_addr, sizeof(ip_addr));
    return question_end + DNS_ANSWER_SIZE;
}

static void RunDnsTask(void *arg)
{
    (void)arg;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct timeval timeout = {.tv_sec = 0, .tv_usec = DNS_RECEIVE_TIMEOUT_MS * 1000};

    if (socket_fd < 0 || bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        LOG_WARNING("DNS server failed to bind port %d", DNS_SERVER_PORT);
        if (socket_fd >= 0) {
            close(socket_fd);
        }
        vTaskDelete(NULL);   /* s_task stays set so StopDnsServer() still waits for the cleanup */
        return;
    }
    LOG_INFO("DNS server listening on port %d", DNS_SERVER_PORT);

    while (!s_is_stop_requested) {
        struct sockaddr_in client;
        socklen_t client_length = sizeof(client);
        int received = recvfrom(socket_fd, s_packet, sizeof(s_packet), 0,
                                (struct sockaddr *)&client, &client_length);
        if (received <= 0) {
            continue;  /* receive timeout: re-check the stop flag */
        }
        size_t response_length = BuildDnsResponse(s_packet, (size_t)received, s_ip_addr, s_packet, sizeof(s_packet));
        if (response_length > 0) {
            LOG_DEBUG("DNS reply %u bytes", (unsigned)response_length);
            (void)sendto(socket_fd, s_packet, response_length, 0, (struct sockaddr *)&client, client_length);
        }
    }

    close(socket_fd);
    vTaskDelete(NULL);
}

bool StartDnsServer(uint32_t ip_addr)
{
    if (s_task != NULL) {
        StopDnsServer();
    }
    s_ip_addr = ip_addr;
    s_is_stop_requested = false;
    s_task = xTaskCreateStatic(RunDnsTask, "dns", DNS_TASK_STACK_BYTES, NULL,
                               DNS_TASK_PRIORITY, s_task_stack, &s_task_struct);
    return s_task != NULL;
}

void StopDnsServer(void)
{
    if (s_task == NULL) {
        return;
    }
    s_is_stop_requested = true;
    /* The task deletes itself; wait until FreeRTOS has released it before the static TCB is reused. */
    TickType_t deadline_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(DNS_STOP_TIMEOUT_MS);
    while (s_task != NULL && eTaskGetState(s_task) != eDeleted && xTaskGetTickCount() < deadline_ticks) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_task != NULL && eTaskGetState(s_task) == eDeleted) {
        /* eDeleted only means "queued for cleanup": let the idle task release the static TCB before it can be reused. */
        vTaskDelay(pdMS_TO_TICKS(DNS_IDLE_CLEANUP_MS));
    }
    s_task = NULL;
    LOG_INFO("DNS server stopped");
}
