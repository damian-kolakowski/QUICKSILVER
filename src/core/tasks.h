#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "project.h"

typedef enum {
  TASK_GYRO,
  TASK_RX,
  TASK_MAIN,
  TASK_USB,
  TASK_OSD,
#ifdef ENABLE_BLACKBOX
  TASK_BLACKBOX,
#endif
  TASK_VTX,

  TASK_MAX

} task_id_t;
typedef enum {
  TASK_PRIORITY_REALTIME,
  TASK_PRIORITY_HIGH,
  TASK_PRIORITY_MEDIUM,
  TASK_PRIORITY_LOW,
} task_priority_t;

typedef enum {
  TASK_MASK_DEFAULT = (0x1 << 0),
  TASK_MASK_ON_GROUND = (0x1 << 1),
  TASK_MASK_IN_AIR = (0x1 << 2),

  TASK_MASK_ALWAYS = 0xFF,
} task_mask_t;

typedef enum {
  TASK_FLAG_SKIP_STATS = (0x1 << 0),
} task_flag_t;

typedef void (*task_function_t)();
typedef bool (*task_poll_function_t)();

typedef struct {
  const char *name;

  uint8_t mask;

  task_priority_t priority;

  task_poll_function_t poll_func;
  task_function_t func;

  uint32_t last_run_time;

  uint32_t runtime_flags;
  uint32_t runtime_current;
  uint32_t runtime_min;
  uint32_t runtime_avg;
  uint32_t runtime_worst;
  uint32_t runtime_max;

  uint32_t runtime_avg_sum;
} task_t;

#define CREATE_TASK(p_name, p_mask, p_priority, p_poll_func, p_func) \
  {                                                                  \
    .name = p_name,                                                  \
    .mask = p_mask,                                                  \
    .priority = p_priority,                                          \
    .poll_func = p_poll_func,                                        \
    .func = p_func,                                                  \
    .last_run_time = 0,                                              \
    .runtime_flags = 0,                                              \
    .runtime_current = 0,                                            \
    .runtime_min = UINT32_MAX,                                       \
    .runtime_avg = 0,                                                \
    .runtime_worst = 0,                                              \
    .runtime_max = 0,                                                \
    .runtime_avg_sum = 0,                                            \
  }

extern task_t tasks[TASK_MAX];