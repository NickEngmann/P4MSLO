/**
 * @brief FreeRTOS mock for host-based testing
 */
#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef unsigned int UBaseType_t;
typedef int BaseType_t;

#define portMAX_DELAY   0xFFFFFFFF
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE  1
#define pdFALSE 0

#define configMINIMAL_STACK_SIZE 128
#define portTICK_PERIOD_MS      1

static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
    uint32_t stack, void *params, UBaseType_t priority, TaskHandle_t *handle) {
    (void)fn; (void)name; (void)stack; (void)params; (void)priority;
    if (handle) *handle = (void*)1;
    return pdTRUE;
}

static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
    uint32_t stack, void *params, UBaseType_t priority, TaskHandle_t *handle, int core) {
    (void)core;
    return xTaskCreate(fn, name, stack, params, priority, handle);
}

static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }
static inline void vTaskDelete(TaskHandle_t handle) { (void)handle; }
