#pragma once
/* Single-threaded host harness; device synchronization uses real FreeRTOS. */
typedef int SemaphoreHandle_t;
static inline int xSemaphoreCreateMutex(void) { return 1; }
static inline void xSemaphoreTake(int lock, int timeout) { (void)lock; (void)timeout; }
static inline void xSemaphoreGive(int lock) { (void)lock; }
