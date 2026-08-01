#ifndef MI_SYS_H
#define MI_SYS_H

#include "mi_common.h"

MI_S32 MI_SYS_Init(void);
MI_S32 MI_SYS_Exit(void);
MI_S32 MI_SYS_Mmap(MI_PHY phyAddr, MI_U32 u32Size, void** ppVirtualAddress, MI_BOOL bCache);
MI_S32 MI_SYS_Munmap(void* pVirtualAddress, MI_U32 u32Size);
MI_S32 MI_SYS_MMA_Alloc(MI_U8* szMMAHeapName, MI_U32 blkSize, MI_PHY* pu64PhyAddr);
MI_S32 MI_SYS_MMA_Free(MI_PHY phyAddr);
MI_S32 MI_SYS_FlushInvCache(void* pVirtualAddress, MI_U32 u32Length);

#endif
