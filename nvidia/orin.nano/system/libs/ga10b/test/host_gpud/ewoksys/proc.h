#ifndef HOST_GPUD_PROC_H
#define HOST_GPUD_PROC_H
#include <stdint.h>
#define TASK_TYPE_PROC   0
#define TASK_TYPE_THREAD 1
typedef struct { int32_t type; int32_t father_pid; } procinfo_t;
uint32_t proc_get_uuid(int32_t pid);
int proc_info(int32_t pid, procinfo_t* info);
#endif