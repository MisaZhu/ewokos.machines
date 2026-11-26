/* host shim: only the vdevice surface gpud.c / ga10b_dev_remote.c touch.
 * dev_cntl_by_pid loops back into the daemon handler inside the test. */
#ifndef HOST_GPUD_VDEVICE_H
#define HOST_GPUD_VDEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <ewoksys/proto.h>

#define FS_TYPE_CHAR 2

typedef struct st_vdevice {
    char desc[128];
    int (*dev_cntl)(struct st_vdevice* dev, int from_pid, int cmd,
                    proto_t* in, proto_t* ret, void* p);
    int (*loop_step)(struct st_vdevice* dev, void* p);
    char* (*cmd)(struct st_vdevice* dev, int from_pid, int argc,
                 char** argv, void* p);
} vdevice_t;

int device_run(vdevice_t* dev, const char* mnt_point, int mnt_type,
               int mode, bool multi_task);
int dev_get_pid(const char* fname);
int dev_cntl_by_pid(int pid, int cmd, proto_t* in, proto_t* out);

#endif