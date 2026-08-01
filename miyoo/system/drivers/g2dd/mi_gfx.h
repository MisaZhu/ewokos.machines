#ifndef MI_GFX_H
#define MI_GFX_H

#include "mi_gfx_datatype.h"

MI_S32 MI_GFX_Open(void);
MI_S32 MI_GFX_Close(void);
MI_S32 MI_GFX_WaitAllDone(MI_BOOL bWaitAllDone, MI_U16 u16TargetFence);
MI_S32 MI_GFX_QuickFill(MI_GFX_Surface_t* pstDst,
		MI_GFX_Rect_t* pstDstRect,
		MI_U32 u32ColorVal,
		MI_U16* pu16Fence);
MI_S32 MI_GFX_BitBlit(MI_GFX_Surface_t* pstSrc,
		MI_GFX_Rect_t* pstSrcRect,
		MI_GFX_Surface_t* pstDst,
		MI_GFX_Rect_t* pstDstRect,
		MI_GFX_Opt_t* pstOpt,
		MI_U16* pu16Fence);

#endif
