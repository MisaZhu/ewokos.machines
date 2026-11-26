/*
 * ga10b_devd.h - daemon-side extensions to the process-wide compute device.
 * Only gpud links these: they expose the shared-VM mapping primitives the
 * RPC server needs on top of the public ga10b_dev.h surface.
 */
#ifndef GA10B_DEVD_H
#define GA10B_DEVD_H

#include <stdint.h>
#include <ga10b/ga10b_dev.h>
#include <ga10b/ga10b_rpc.h>

/* persistent cacheable map of a client-owned physical range; bump VA is
 * capped below the first client slot window.  0 on failure. */
uint64_t ga10b_devd_map(uint64_t phy, uint32_t size);
void     ga10b_devd_unmap(uint64_t va, uint32_t size);
/* fixed-VA slot (re)map: unmaps old_size bytes at va first when nonzero */
int      ga10b_devd_map_at(uint64_t va, uint64_t phy, uint32_t size,
                           uint32_t old_size);

/* Queued launch: submitted without waiting, kernels still run strictly in
 * submission order.  0 queued, 1 not queueable right now (drain and retry,
 * then fall back to ga10b_dev_launch_dyn), -1 error.  Nothing else may touch
 * the device until ga10b_dev_launch_drain returned; *retired = how many of
 * the queued launches completed. */
int ga10b_dev_launch_queue(ga10b_module_t* m, int kernel,
                           const uint32_t grid[3], const uint32_t block[3],
                           void* const* args, uint32_t dyn_smem);
int ga10b_dev_launch_drain(uint32_t* retired);
/* Optional per-slot four-word release records; slot indices restart at drain. */
void ga10b_dev_queue_timestamps(int enable);
int ga10b_dev_queue_timestamp(uint32_t slot, uint64_t* timestamp);

/* kernel param metadata for the client-side packer */
int ga10b_devd_kernel_info(ga10b_module_t* m, int kid,
                           ga10b_rpc_kinfo_rep_t* out);
/* shared-VM bookkeeping for `devcmd /dev/gpu stat`: page tables in use,
 * reusable VA ranges, bump cursor.  A leak shows as tables growing across
 * client sessions. */
void ga10b_devd_mm_stat(uint32_t* tables, uint32_t* free_ranges,
                        uint64_t* va_next);
/* kernel name, for the daemon's launch profile; NULL when out of range */
const char* ga10b_devd_kernel_name(const ga10b_module_t* m, int kid);
/* release a module's uploaded code/banks (session teardown) */
void ga10b_dev_module_free(ga10b_module_t* m);

#endif