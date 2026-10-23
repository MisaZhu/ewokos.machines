#ifndef QNN_HTP_EXT_H
#define QNN_HTP_EXT_H

#include "qnn_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief QNN HTP backend private extension API (SC8280XP / Hexagon V68, Q8B)
 * Supported only by libQnnHtp.so; not part of the standard QNN Core API.
 * Used solely for HTP NPU tuning.
 */

// ========= Enum definitions =========
typedef enum {
    QNN_HTP_PERF_MODE_POWER_SAVER = 0,
    QNN_HTP_PERF_MODE_BURST,
    QNN_HTP_PERF_MODE_HIGH_PERF,
    QNN_HTP_PERF_MODE_TURBO
} QnnHtp_PerfMode_t;

typedef enum {
    QNN_HTP_PRECISION_DEFAULT = 0,
    QNN_HTP_PRECISION_RELAXED_FP16,   // allow relaxed fp16 precision for higher speed
    QNN_HTP_PRECISION_STRICT
} QnnHtp_PrecisionHint_t;

// ========= Handle aliases, consistent with core =========
typedef Qnn_ContextHandle_t  QnnHtp_ContextHandle_t;
typedef Qnn_GraphHandle_t    QnnHtp_GraphHandle_t;
typedef Qnn_DeviceHandle_t   QnnHtp_DeviceHandle_t;

// ========= Context extension API =========

/**
 * @brief Set the HTP performance mode
 * @param ctx QNN Context handle
 * @param perfMode performance tier: power_saver / burst / highperf / turbo
 */
Qnn_ErrorHandle_t QnnHtpContext_SetPerfConfig(QnnHtp_ContextHandle_t ctx, QnnHtp_PerfMode_t perfMode);

/**
 * @brief Limit the available VTCM size (in bytes)
 */
Qnn_ErrorHandle_t QnnHtpContext_SetVtcmLimit(QnnHtp_ContextHandle_t ctx, uint64_t vtcmLimitBytes);

/**
 * @brief Register an external shared DMA/ION buffer with the HTP for zero-copy tensors
 * @param ctx context
 * @param fd ION dmabuf fd
 * @param offset offset within the buffer
 * @param size total buffer size in bytes
 * @param outHostPtr returns the mapped host virtual address
 */
Qnn_ErrorHandle_t QnnHtpContext_RegisterSharedBuffer(QnnHtp_ContextHandle_t ctx,
                                                     int fd,
                                                     uint64_t offset,
                                                     uint64_t size,
                                                     void** outHostPtr);

/**
 * @brief Set the concurrency group id; binding multiple graphs to the same
 *        concurrency group enables hardware-pipelined concurrency
 * @param ctx context
 * @param groupId concurrency group number
 */
Qnn_ErrorHandle_t QnnHtpContext_SetConcurrentGroup(QnnHtp_ContextHandle_t ctx, uint32_t groupId);

// ========= Graph extension API =========

/**
 * @brief Set the precision hint for this graph
 */
Qnn_ErrorHandle_t QnnHtpGraph_SetPrecisionHint(QnnHtp_GraphHandle_t graph, QnnHtp_PrecisionHint_t hint);

/**
 * @brief Skip the pre-execution CPU cache flush (enable only when you guarantee
 *        cache coherency yourself; a zero-copy optimization)
 * @param graph graph handle
 * @param skip true = skip the flush
 */
Qnn_ErrorHandle_t QnnHtpGraph_SetSkipDataFlush(QnnHtp_GraphHandle_t graph, bool skip);

// ========= Device extension API =========

/**
 * @brief Specify the usable NSP core mask (bitmask); V68 typically has multiple NPU cores
 * example: mask=0b01 use core0 only; mask=0b11 core0+core1
 */
Qnn_ErrorHandle_t QnnHtpDevice_SetDspCoreMask(QnnHtp_DeviceHandle_t dev, uint32_t coreMask);

// ========= Optional: HTP profiling private extension (present in some SDK versions) =========
/**
 * @brief Enable the HTP internal hardware counters (VTCM hits, stalls, HMX utilization, etc.)
 */
Qnn_ErrorHandle_t QnnHtpProfile_EnableHwCounters(Qnn_ProfileHandle_t prof, bool enable);

#ifdef __cplusplus
}
#endif

#endif