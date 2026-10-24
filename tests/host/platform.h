// SPDX-License-Identifier: MIT
#pragma once
/* 确定性主机调度替身：只用于验证真实合成代码，不模拟 RTOS 并发或设备时序。 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef unsigned TickType_t;
typedef int BaseType_t;
typedef int portMUX_TYPE;
typedef void *TaskHandle_t;
typedef struct host_queue *QueueHandle_t;
typedef struct host_sem *SemaphoreHandle_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define portNUM_PROCESSORS 1
#define configMAX_PRIORITIES 25
#define tskNO_AFFINITY (-1)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#ifdef _WIN32
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif

static int host_resources;
static bool host_fail_output_alloc;
static bool host_fail_task;
static bool host_fail_queue;
static int host_fail_sem;
static bool host_in_task;
static void (*host_task_fn)(void *);
static int64_t host_now;
static void host_run_task(void)
{
    assert(host_task_fn && !host_in_task);
    void (*fn)(void *) = host_task_fn;
    host_task_fn = NULL;
    host_in_task = true;
    fn(NULL);
    host_in_task = false;
}
struct host_queue { unsigned head, count, capacity; size_t size; char *data; };
struct host_sem { bool ready; };

static QueueHandle_t xQueueCreate(unsigned count, size_t size)
{
    if (host_fail_queue) return NULL;
    QueueHandle_t q = calloc(1, sizeof(*q));
    assert(q);
    q->capacity = count; q->size = size; q->data = calloc(count, size); assert(q->data);
    ++host_resources;
    return q;
}
static int xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    if (q->count == q->capacity) return 0;
    memcpy(q->data + ((q->head + q->count) % q->capacity) * q->size, item, q->size);
    ++q->count; return pdTRUE;
}
static int xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    if (!q->count) return 0;
    memcpy(item, q->data + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->capacity; --q->count; return pdTRUE;
}
static unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->count; }
static void vQueueDelete(QueueHandle_t q) { free(q->data); free(q); --host_resources; }
static SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    if (host_fail_sem > 0 && --host_fail_sem == 0) return NULL;
    ++host_resources;
    return calloc(1, sizeof(struct host_sem));
}
static int xSemaphoreGive(SemaphoreHandle_t sem) { sem->ready = true; return pdTRUE; }
static int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t wait)
{
    if (!sem->ready && wait && host_task_fn && !host_in_task) host_run_task();
    if (!sem->ready) return 0;
    sem->ready = false; return pdTRUE;
}
static void vSemaphoreDelete(SemaphoreHandle_t sem) { free(sem); --host_resources; }
static int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack,
                                 void *arg, unsigned priority, TaskHandle_t *task, int core)
{
    (void)name; (void)stack; (void)arg; (void)priority; (void)core;
    if (host_fail_task) return 0;
    host_task_fn = fn; *task = (void *)(uintptr_t)1; return pdPASS;
}
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return host_in_task ? (void *)(uintptr_t)1 : NULL; }
static void vTaskDelete(TaskHandle_t task) { (void)task; }
static void vTaskDelay(TickType_t ticks) { host_now += (int64_t)ticks * 1000; }
static int64_t esp_timer_get_time(void) { return host_now += 50; }
static size_t heap_caps_get_free_size(unsigned caps) { return caps == MALLOC_CAP_SPIRAM ? 0 : 16 * 1024 * 1024; }
static void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{
    (void)caps;
    if (host_fail_output_alloc) return NULL;
    void *ptr = calloc(n, size);
    if (ptr) ++host_resources;
    return ptr;
}
static void heap_caps_free(void *ptr) { if (ptr) --host_resources; free(ptr); }
static void host_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGI(...) host_log(__VA_ARGS__)
