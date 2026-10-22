/* Restricted Q8B GPU PAS ABI. No caller-selected SMC function or destination. */
#ifndef Q8B_GPU_PAS_H
#define Q8B_GPU_PAS_H
#define Q8B_GPU_PAS_ABI 0x51385001u
#define Q8B_GPU_PAS_QUERY 0x51380000u /* call ABI query; not authentication */
#define Q8B_GPU_PAS_INIT  0x51380001u /* owned DMA PA, metadata length */
#define Q8B_GPU_PAS_MEM   0x51380002u /* relocated footprint, 0 */
#define Q8B_GPU_PAS_STAGE 0x51380003u /* owned DMA PA, footprint */
#define Q8B_GPU_PAS_AUTH  0x51380004u /* 0, 0 */
#define Q8B_GPU_PAS_BUSY (-1001)
#define Q8B_GPU_PAS_UNSUPPORTED (-1002)
#define Q8B_GPU_PAS_INVALID (-1003)
#define Q8B_GPU_PAS_FAILED (-1004)
#define Q8B_GPU_PAS_WAITQ (-1005)
#endif
