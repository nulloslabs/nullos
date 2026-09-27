#pragma once

#include <stdbool.h>

#define KERNEL_WORK_CAPACITY 64

typedef void (*kernel_work_handler_t)(void *context);

typedef struct {
    kernel_work_handler_t handler;
    void *context;
} kernel_work_t;

bool queue_kernel_work(kernel_work_handler_t handler, void *context);
void start_kernel_workqueue(void);
