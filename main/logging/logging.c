/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "logging.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define LOG_UART_PORT_NUM           (UART_NUM_0)
/* The UART driver requires rx_buffer_size > UART_HW_FIFO_LEN even when RX is
 * unused by this module (only TX is used); 256 safely exceeds the hardware
 * FIFO length (128 bytes on ESP32-C3). */
#define LOG_UART_RX_BUF_UNUSED      (256)
#define LOG_QUEUE_DEPTH             (16)
#define LOG_MESSAGE_MAX_LEN         (128)
#define LOG_LINE_MAX_LEN            (LOG_MESSAGE_MAX_LEN + 48)
#define LOG_WRITER_TASK_STACK_WORDS (2048)
#define LOG_WRITER_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)

/** @brief A single queued log message, sized statically (no dynamic memory). */
typedef struct {
    log_level_t level;
    char module_name[LOG_MODULE_NAME_MAX_LEN + 1]; /* +1 for the null terminator. */
    char message[LOG_MESSAGE_MAX_LEN];
} log_record_t;

static const char *const s_level_name[] = {
    [LOG_LEVEL_DEBUG]   = "DEBUG",
    [LOG_LEVEL_INFO]    = "INFO",
    [LOG_LEVEL_WARNING] = "WARNING",
    [LOG_LEVEL_ERROR]   = "ERROR",
};

/* Statically allocated queue storage: no dynamic memory allocation (FR-9). */
static uint8_t s_queue_storage[LOG_QUEUE_DEPTH * sizeof(log_record_t)];
static StaticQueue_t s_queue_struct;
static QueueHandle_t s_log_queue;

/* Statically allocated log-writer task, draining the queue to UART0. */
static StaticTask_t s_writer_task_buffer;
static StackType_t s_writer_task_stack[LOG_WRITER_TASK_STACK_WORDS];

static bool s_initialized;

static void LogWriterTask(void *arg);
static void FormatRecord(const log_record_t *record, char *out, size_t out_len);

void LogInit(void)
{
    if (s_initialized) {
        return;
    }

    const uart_config_t uart_config = {
        .baud_rate  = CONFIG_LOG_UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(LOG_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_driver_install(LOG_UART_PORT_NUM,
                                         LOG_UART_RX_BUF_UNUSED,
                                         CONFIG_LOG_UART_TX_RING_BUFFER_SIZE,
                                         0, NULL, 0));

    s_log_queue = xQueueCreateStatic(LOG_QUEUE_DEPTH, sizeof(log_record_t),
                                      s_queue_storage, &s_queue_struct);
    if (s_log_queue == NULL) {
        return;
    }

    TaskHandle_t writer_task = xTaskCreateStatic(LogWriterTask, "log_writer",
                                                  LOG_WRITER_TASK_STACK_WORDS, NULL,
                                                  LOG_WRITER_TASK_PRIORITY,
                                                  s_writer_task_stack, &s_writer_task_buffer);
    if (writer_task == NULL) {
        return;
    }

    s_initialized = true;
}

void LogWrite(log_level_t level, const char *module_name, const char *fmt, ...)
{
    if (!s_initialized) {
        return;
    }

    log_record_t record = {
        .level = level,
    };
    strncpy(record.module_name, module_name, sizeof(record.module_name) - 1);
    record.module_name[sizeof(record.module_name) - 1] = '\0';

    va_list args;
    va_start(args, fmt);
    vsnprintf(record.message, sizeof(record.message), fmt, args);
    va_end(args);

    if (xQueueSend(s_log_queue, &record, 0) != pdTRUE) {
        /* Queue full: overwrite the oldest queued message with the newest one (FR-7). */
        log_record_t discarded_record;
        xQueueReceive(s_log_queue, &discarded_record, 0);
        xQueueSend(s_log_queue, &record, 0);
    }
}

static void FormatRecord(const log_record_t *record, char *out, size_t out_len)
{
#if CONFIG_LOG_TIMESTAMP_ENABLE
    int64_t uptime_ms = esp_timer_get_time() / 1000;
    snprintf(out, out_len, "[%lld][%s][%s] %s\n",
             (long long)uptime_ms, s_level_name[record->level], record->module_name,
             record->message);
#else
    snprintf(out, out_len, "[%s][%s] %s\n",
             s_level_name[record->level], record->module_name, record->message);
#endif
}

static void LogWriterTask(void *arg)
{
    (void)arg;
    log_record_t record;
    char line[LOG_LINE_MAX_LEN];

    for (;;) {
        if (xQueueReceive(s_log_queue, &record, portMAX_DELAY) == pdTRUE) {
            FormatRecord(&record, line, sizeof(line));
            uart_write_bytes(LOG_UART_PORT_NUM, line, strlen(line));
        }
    }
}
