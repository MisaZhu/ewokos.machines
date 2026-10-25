#ifndef NPU_LISTENER_H
#define NPU_LISTENER_H
#include <stdint.h>

/* Qualcomm adsp_listener.next2 + apps_remotectl/apps_std, read-only loader
 * service.  Wire layouts follow quic/fastrpc IDL and generated stubs. */
int npu_listener_start(uint32_t session);
int npu_listener_shutdown(void);
/* Snapshot retained reverse-RPC history on failure; performs no DSP calls. */
void npu_listener_dump_diagnostics(void);

/* Dispatch one packed listener request.
 * A nonzero result is an AEE/HLOS error, never a successful empty response. */
int npu_listener_dispatch(uint32_t handle, uint32_t sc,
        const uint8_t *input, uint32_t input_size,
        uint8_t **output, uint32_t *output_size);
void npu_listener_files_close(void);
#endif
