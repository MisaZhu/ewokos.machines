#ifndef MI_GFX_DATATYPE_H
#define MI_GFX_DATATYPE_H

#include "mi_common.h"

typedef enum {
	E_MI_GFX_FMT_RGB565 = 0x06,
	E_MI_GFX_FMT_ARGB8888 = 0x0b
} MI_GFX_ColorFmt_e;

typedef enum {
	E_MI_GFX_DFB_BLD_ZERO = 0x00,
	E_MI_GFX_DFB_BLD_ONE = 0x01,
	E_MI_GFX_DFB_BLD_SRCALPHA = 0x04,
	E_MI_GFX_DFB_BLD_INVSRCALPHA = 0x05
} MI_GFX_DfbBldOp_e;

typedef enum {
	E_MI_GFX_MIRROR_NONE = 0x00
} MI_GFX_Mirror_e;

typedef enum {
	E_MI_GFX_ROTATE_0 = 0x00
} MI_GFX_Rotate_e;

typedef enum {
	E_MI_GFX_DFB_BLEND_NOFX = 0x00,
	E_MI_GFX_DFB_BLEND_COLORALPHA = 0x01,
	E_MI_GFX_DFB_BLEND_ALPHACHANNEL = 0x02
} MI_Gfx_DfbBlendFlags_e;

typedef enum {
	E_MI_GFX_RGB_OP_EQUAL = 0x00
} MI_GFX_ColorKeyOp_e;

typedef struct {
	MI_S32 s32Xpos;
	MI_S32 s32Ypos;
	MI_U32 u32Width;
	MI_U32 u32Height;
} MI_GFX_Rect_t;

typedef struct {
	MI_U32 u32ColorStart;
	MI_U32 u32ColorEnd;
} MI_GFX_ColorKey_t;

typedef struct {
	MI_BOOL bEnColorKey;
	MI_GFX_ColorKeyOp_e eCKeyOp;
	MI_GFX_ColorFmt_e eCKeyFmt;
	MI_GFX_ColorKey_t stCKeyVal;
} MI_GFX_ColorKeyInfo_t;

typedef struct {
	MI_PHY phyAddr;
	MI_GFX_ColorFmt_e eColorFmt;
	MI_U32 u32Width;
	MI_U32 u32Height;
	MI_U32 u32Stride;
} MI_GFX_Surface_t;

typedef struct {
	MI_GFX_Rect_t stClipRect;
	MI_GFX_ColorKeyInfo_t stSrcColorKeyInfo;
	MI_GFX_ColorKeyInfo_t stDstColorKeyInfo;
	MI_GFX_DfbBldOp_e eSrcDfbBldOp;
	MI_GFX_DfbBldOp_e eDstDfbBldOp;
	MI_GFX_Mirror_e eMirror;
	MI_GFX_Rotate_e eRotate;
	MI_Gfx_DfbBlendFlags_e eDFBBlendFlag;
	MI_U32 u32GlobalSrcConstColor;
	MI_U32 u32GlobalDstConstColor;
} MI_GFX_Opt_t;

#endif
