/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_IR3_YY_IR3_PARSER_H_INCLUDED
# define YY_IR3_YY_IR3_PARSER_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int ir3_yydebug;
#endif
/* "%code requires" blocks.  */
#line 24 "ir3_parser.y"

#include "ir3/ir3_assembler.h"
#include "ir3/ir3_shader.h"

struct ir3 * ir3_parse(struct ir3_shader_variant *v,
		struct ir3_kernel_info *k, FILE *f);

#line 57 "ir3_parser.h"

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    T_INT = 258,                   /* T_INT  */
    T_HEX = 259,                   /* T_HEX  */
    T_FLOAT = 260,                 /* T_FLOAT  */
    T_IDENTIFIER = 261,            /* T_IDENTIFIER  */
    T_REGISTER = 262,              /* T_REGISTER  */
    T_CONSTANT = 263,              /* T_CONSTANT  */
    T_RT = 264,                    /* T_RT  */
    T_A_LOCALSIZE = 265,           /* T_A_LOCALSIZE  */
    T_A_CONST = 266,               /* T_A_CONST  */
    T_A_BUF = 267,                 /* T_A_BUF  */
    T_A_UBO = 268,                 /* T_A_UBO  */
    T_A_INVOCATIONID = 269,        /* T_A_INVOCATIONID  */
    T_A_WGID = 270,                /* T_A_WGID  */
    T_A_NUMWG = 271,               /* T_A_NUMWG  */
    T_A_BRANCHSTACK = 272,         /* T_A_BRANCHSTACK  */
    T_A_IN = 273,                  /* T_A_IN  */
    T_A_OUT = 274,                 /* T_A_OUT  */
    T_A_TEX = 275,                 /* T_A_TEX  */
    T_A_PVTMEM = 276,              /* T_A_PVTMEM  */
    T_A_LOCALMEM = 277,            /* T_A_LOCALMEM  */
    T_A_CONSTLEN = 278,            /* T_A_CONSTLEN  */
    T_A_EARLYPREAMBLE = 279,       /* T_A_EARLYPREAMBLE  */
    T_A_FULLNOPSTART = 280,        /* T_A_FULLNOPSTART  */
    T_A_FULLNOPEND = 281,          /* T_A_FULLNOPEND  */
    T_A_FULLSYNCSTART = 282,       /* T_A_FULLSYNCSTART  */
    T_A_FULLSYNCEND = 283,         /* T_A_FULLSYNCEND  */
    T_ABSNEG = 284,                /* T_ABSNEG  */
    T_NEG = 285,                   /* T_NEG  */
    T_ABS = 286,                   /* T_ABS  */
    T_R = 287,                     /* T_R  */
    T_LAST = 288,                  /* T_LAST  */
    T_HR = 289,                    /* T_HR  */
    T_HC = 290,                    /* T_HC  */
    T_EVEN = 291,                  /* T_EVEN  */
    T_POS_INFINITY = 292,          /* T_POS_INFINITY  */
    T_NEG_INFINITY = 293,          /* T_NEG_INFINITY  */
    T_EI = 294,                    /* T_EI  */
    T_WRMASK = 295,                /* T_WRMASK  */
    T_FLUT_0_0 = 296,              /* T_FLUT_0_0  */
    T_FLUT_0_5 = 297,              /* T_FLUT_0_5  */
    T_FLUT_1_0 = 298,              /* T_FLUT_1_0  */
    T_FLUT_2_0 = 299,              /* T_FLUT_2_0  */
    T_FLUT_E = 300,                /* T_FLUT_E  */
    T_FLUT_PI = 301,               /* T_FLUT_PI  */
    T_FLUT_INV_PI = 302,           /* T_FLUT_INV_PI  */
    T_FLUT_INV_LOG2_E = 303,       /* T_FLUT_INV_LOG2_E  */
    T_FLUT_LOG2_E = 304,           /* T_FLUT_LOG2_E  */
    T_FLUT_INV_LOG2_10 = 305,      /* T_FLUT_INV_LOG2_10  */
    T_FLUT_LOG2_10 = 306,          /* T_FLUT_LOG2_10  */
    T_FLUT_4_0 = 307,              /* T_FLUT_4_0  */
    T_SY = 308,                    /* T_SY  */
    T_SS = 309,                    /* T_SS  */
    T_JP = 310,                    /* T_JP  */
    T_EQ_FLAG = 311,               /* T_EQ_FLAG  */
    T_SAT = 312,                   /* T_SAT  */
    T_RPT = 313,                   /* T_RPT  */
    T_UL = 314,                    /* T_UL  */
    T_NOP = 315,                   /* T_NOP  */
    T_EOLM = 316,                  /* T_EOLM  */
    T_EOGM = 317,                  /* T_EOGM  */
    T_EOSTSC = 318,                /* T_EOSTSC  */
    T_OP_NOP = 319,                /* T_OP_NOP  */
    T_OP_BR = 320,                 /* T_OP_BR  */
    T_OP_BRAO = 321,               /* T_OP_BRAO  */
    T_OP_BRAA = 322,               /* T_OP_BRAA  */
    T_OP_BRAC = 323,               /* T_OP_BRAC  */
    T_OP_BANY = 324,               /* T_OP_BANY  */
    T_OP_BALL = 325,               /* T_OP_BALL  */
    T_OP_BRAX = 326,               /* T_OP_BRAX  */
    T_OP_JUMP = 327,               /* T_OP_JUMP  */
    T_OP_CALL = 328,               /* T_OP_CALL  */
    T_OP_RET = 329,                /* T_OP_RET  */
    T_OP_KILL = 330,               /* T_OP_KILL  */
    T_OP_END = 331,                /* T_OP_END  */
    T_OP_EMIT = 332,               /* T_OP_EMIT  */
    T_OP_CUT = 333,                /* T_OP_CUT  */
    T_OP_CHMASK = 334,             /* T_OP_CHMASK  */
    T_OP_CHSH = 335,               /* T_OP_CHSH  */
    T_OP_FLOW_REV = 336,           /* T_OP_FLOW_REV  */
    T_OP_BKT = 337,                /* T_OP_BKT  */
    T_OP_STKS = 338,               /* T_OP_STKS  */
    T_OP_STKR = 339,               /* T_OP_STKR  */
    T_OP_XSET = 340,               /* T_OP_XSET  */
    T_OP_XCLR = 341,               /* T_OP_XCLR  */
    T_OP_GETLAST = 342,            /* T_OP_GETLAST  */
    T_OP_GETONE = 343,             /* T_OP_GETONE  */
    T_OP_DBG = 344,                /* T_OP_DBG  */
    T_OP_SHPS = 345,               /* T_OP_SHPS  */
    T_OP_SHPE = 346,               /* T_OP_SHPE  */
    T_OP_PREDT = 347,              /* T_OP_PREDT  */
    T_OP_PREDF = 348,              /* T_OP_PREDF  */
    T_OP_PREDE = 349,              /* T_OP_PREDE  */
    T_OP_MOVMSK = 350,             /* T_OP_MOVMSK  */
    T_OP_MOVA1 = 351,              /* T_OP_MOVA1  */
    T_OP_MOVA = 352,               /* T_OP_MOVA  */
    T_OP_MOVA_R = 353,             /* T_OP_MOVA_R  */
    T_OP_MOV = 354,                /* T_OP_MOV  */
    T_OP_COV = 355,                /* T_OP_COV  */
    T_OP_SWZ = 356,                /* T_OP_SWZ  */
    T_OP_GAT = 357,                /* T_OP_GAT  */
    T_OP_SCT = 358,                /* T_OP_SCT  */
    T_OP_MOVS = 359,               /* T_OP_MOVS  */
    T_MUL2 = 360,                  /* T_MUL2  */
    T_DIV2 = 361,                  /* T_DIV2  */
    T_OP_ADD_F = 362,              /* T_OP_ADD_F  */
    T_OP_MIN_F = 363,              /* T_OP_MIN_F  */
    T_OP_MAX_F = 364,              /* T_OP_MAX_F  */
    T_OP_MUL_F = 365,              /* T_OP_MUL_F  */
    T_OP_SIGN_F = 366,             /* T_OP_SIGN_F  */
    T_OP_CMPS_F = 367,             /* T_OP_CMPS_F  */
    T_OP_ABSNEG_F = 368,           /* T_OP_ABSNEG_F  */
    T_OP_CMPV_F = 369,             /* T_OP_CMPV_F  */
    T_OP_FLOOR_F = 370,            /* T_OP_FLOOR_F  */
    T_OP_CEIL_F = 371,             /* T_OP_CEIL_F  */
    T_OP_RNDNE_F = 372,            /* T_OP_RNDNE_F  */
    T_OP_RNDAZ_F = 373,            /* T_OP_RNDAZ_F  */
    T_OP_TRUNC_F = 374,            /* T_OP_TRUNC_F  */
    T_OP_ADD_U = 375,              /* T_OP_ADD_U  */
    T_OP_ADD_S = 376,              /* T_OP_ADD_S  */
    T_OP_SUB_U = 377,              /* T_OP_SUB_U  */
    T_OP_SUB_S = 378,              /* T_OP_SUB_S  */
    T_OP_CMPS_U = 379,             /* T_OP_CMPS_U  */
    T_OP_CMPS_S = 380,             /* T_OP_CMPS_S  */
    T_OP_MIN_U = 381,              /* T_OP_MIN_U  */
    T_OP_MIN_S = 382,              /* T_OP_MIN_S  */
    T_OP_MAX_U = 383,              /* T_OP_MAX_U  */
    T_OP_MAX_S = 384,              /* T_OP_MAX_S  */
    T_OP_ABSNEG_S = 385,           /* T_OP_ABSNEG_S  */
    T_OP_AND_B = 386,              /* T_OP_AND_B  */
    T_OP_OR_B = 387,               /* T_OP_OR_B  */
    T_OP_NOT_B = 388,              /* T_OP_NOT_B  */
    T_OP_XOR_B = 389,              /* T_OP_XOR_B  */
    T_OP_CMPV_U = 390,             /* T_OP_CMPV_U  */
    T_OP_CMPV_S = 391,             /* T_OP_CMPV_S  */
    T_OP_MUL_U24 = 392,            /* T_OP_MUL_U24  */
    T_OP_MUL_S24 = 393,            /* T_OP_MUL_S24  */
    T_OP_MULL_U = 394,             /* T_OP_MULL_U  */
    T_OP_BFREV_B = 395,            /* T_OP_BFREV_B  */
    T_OP_CLZ_S = 396,              /* T_OP_CLZ_S  */
    T_OP_CLZ_B = 397,              /* T_OP_CLZ_B  */
    T_OP_SHL_B = 398,              /* T_OP_SHL_B  */
    T_OP_SHR_B = 399,              /* T_OP_SHR_B  */
    T_OP_ASHR_B = 400,             /* T_OP_ASHR_B  */
    T_OP_BARY_F = 401,             /* T_OP_BARY_F  */
    T_OP_FLAT_B = 402,             /* T_OP_FLAT_B  */
    T_OP_MGEN_B = 403,             /* T_OP_MGEN_B  */
    T_OP_GETBIT_B = 404,           /* T_OP_GETBIT_B  */
    T_OP_SETRM = 405,              /* T_OP_SETRM  */
    T_OP_CBITS_B = 406,            /* T_OP_CBITS_B  */
    T_OP_SHB = 407,                /* T_OP_SHB  */
    T_OP_MSAD = 408,               /* T_OP_MSAD  */
    T_OP_MAD_U16 = 409,            /* T_OP_MAD_U16  */
    T_OP_MADSH_U16 = 410,          /* T_OP_MADSH_U16  */
    T_OP_MAD_S16 = 411,            /* T_OP_MAD_S16  */
    T_OP_MADSH_M16 = 412,          /* T_OP_MADSH_M16  */
    T_OP_MAD_U24 = 413,            /* T_OP_MAD_U24  */
    T_OP_MAD_S24 = 414,            /* T_OP_MAD_S24  */
    T_OP_MAD_F16 = 415,            /* T_OP_MAD_F16  */
    T_OP_MAD_F32 = 416,            /* T_OP_MAD_F32  */
    T_OP_SEL_B16 = 417,            /* T_OP_SEL_B16  */
    T_OP_SEL_B32 = 418,            /* T_OP_SEL_B32  */
    T_OP_SEL_S16 = 419,            /* T_OP_SEL_S16  */
    T_OP_SEL_S32 = 420,            /* T_OP_SEL_S32  */
    T_OP_SEL_F16 = 421,            /* T_OP_SEL_F16  */
    T_OP_SEL_F32 = 422,            /* T_OP_SEL_F32  */
    T_OP_SAD_S16 = 423,            /* T_OP_SAD_S16  */
    T_OP_SAD_S32 = 424,            /* T_OP_SAD_S32  */
    T_OP_SHRM = 425,               /* T_OP_SHRM  */
    T_OP_SHLM = 426,               /* T_OP_SHLM  */
    T_OP_SHRG = 427,               /* T_OP_SHRG  */
    T_OP_SHLG = 428,               /* T_OP_SHLG  */
    T_OP_ANDG = 429,               /* T_OP_ANDG  */
    T_OP_DP2ACC = 430,             /* T_OP_DP2ACC  */
    T_OP_DP4ACC = 431,             /* T_OP_DP4ACC  */
    T_OP_WMM = 432,                /* T_OP_WMM  */
    T_OP_WMM_ACCU = 433,           /* T_OP_WMM_ACCU  */
    T_OP_RCP = 434,                /* T_OP_RCP  */
    T_OP_RSQ = 435,                /* T_OP_RSQ  */
    T_OP_LOG2 = 436,               /* T_OP_LOG2  */
    T_OP_EXP2 = 437,               /* T_OP_EXP2  */
    T_OP_SIN = 438,                /* T_OP_SIN  */
    T_OP_COS = 439,                /* T_OP_COS  */
    T_OP_SQRT = 440,               /* T_OP_SQRT  */
    T_OP_HRSQ = 441,               /* T_OP_HRSQ  */
    T_OP_HLOG2 = 442,              /* T_OP_HLOG2  */
    T_OP_HEXP2 = 443,              /* T_OP_HEXP2  */
    T_OP_ISAM = 444,               /* T_OP_ISAM  */
    T_OP_ISAML = 445,              /* T_OP_ISAML  */
    T_OP_ISAMM = 446,              /* T_OP_ISAMM  */
    T_OP_SAM = 447,                /* T_OP_SAM  */
    T_OP_SAMB = 448,               /* T_OP_SAMB  */
    T_OP_SAML = 449,               /* T_OP_SAML  */
    T_OP_SAMGQ = 450,              /* T_OP_SAMGQ  */
    T_OP_GETLOD = 451,             /* T_OP_GETLOD  */
    T_OP_CONV = 452,               /* T_OP_CONV  */
    T_OP_CONVM = 453,              /* T_OP_CONVM  */
    T_OP_GETSIZE = 454,            /* T_OP_GETSIZE  */
    T_OP_GETBUF = 455,             /* T_OP_GETBUF  */
    T_OP_GETPOS = 456,             /* T_OP_GETPOS  */
    T_OP_GETINFO = 457,            /* T_OP_GETINFO  */
    T_OP_DSX = 458,                /* T_OP_DSX  */
    T_OP_DSY = 459,                /* T_OP_DSY  */
    T_OP_GATHER4R = 460,           /* T_OP_GATHER4R  */
    T_OP_GATHER4G = 461,           /* T_OP_GATHER4G  */
    T_OP_GATHER4B = 462,           /* T_OP_GATHER4B  */
    T_OP_GATHER4A = 463,           /* T_OP_GATHER4A  */
    T_OP_SAMGP0 = 464,             /* T_OP_SAMGP0  */
    T_OP_SAMGP1 = 465,             /* T_OP_SAMGP1  */
    T_OP_SAMGP2 = 466,             /* T_OP_SAMGP2  */
    T_OP_SAMGP3 = 467,             /* T_OP_SAMGP3  */
    T_OP_DSXPP_1 = 468,            /* T_OP_DSXPP_1  */
    T_OP_DSYPP_1 = 469,            /* T_OP_DSYPP_1  */
    T_OP_RGETPOS = 470,            /* T_OP_RGETPOS  */
    T_OP_RGETINFO = 471,           /* T_OP_RGETINFO  */
    T_OP_BRCST_A = 472,            /* T_OP_BRCST_A  */
    T_OP_QSHUFFLE_BRCST = 473,     /* T_OP_QSHUFFLE_BRCST  */
    T_OP_QSHUFFLE_H = 474,         /* T_OP_QSHUFFLE_H  */
    T_OP_QSHUFFLE_V = 475,         /* T_OP_QSHUFFLE_V  */
    T_OP_QSHUFFLE_DIAG = 476,      /* T_OP_QSHUFFLE_DIAG  */
    T_OP_TCINV = 477,              /* T_OP_TCINV  */
    T_OP_IMG_BINDLESS_HOF = 478,   /* T_OP_IMG_BINDLESS_HOF  */
    T_OP_IMG_BINDLESS_PCMN = 479,  /* T_OP_IMG_BINDLESS_PCMN  */
    T_OP_IMG_BINDLESS = 480,       /* T_OP_IMG_BINDLESS  */
    T_OP_LDG = 481,                /* T_OP_LDG  */
    T_OP_LDG_A = 482,              /* T_OP_LDG_A  */
    T_OP_LDG_K = 483,              /* T_OP_LDG_K  */
    T_OP_LDL = 484,                /* T_OP_LDL  */
    T_OP_LDP = 485,                /* T_OP_LDP  */
    T_OP_STG = 486,                /* T_OP_STG  */
    T_OP_STG_A = 487,              /* T_OP_STG_A  */
    T_OP_STL = 488,                /* T_OP_STL  */
    T_OP_STP = 489,                /* T_OP_STP  */
    T_OP_LDIB = 490,               /* T_OP_LDIB  */
    T_OP_G2L = 491,                /* T_OP_G2L  */
    T_OP_L2G = 492,                /* T_OP_L2G  */
    T_OP_PREFETCH = 493,           /* T_OP_PREFETCH  */
    T_OP_LDLW = 494,               /* T_OP_LDLW  */
    T_OP_STLW = 495,               /* T_OP_STLW  */
    T_OP_RESFMT = 496,             /* T_OP_RESFMT  */
    T_OP_RESINFO = 497,            /* T_OP_RESINFO  */
    T_OP_RESBASE = 498,            /* T_OP_RESBASE  */
    T_OP_ATOMIC_ADD = 499,         /* T_OP_ATOMIC_ADD  */
    T_OP_ATOMIC_SUB = 500,         /* T_OP_ATOMIC_SUB  */
    T_OP_ATOMIC_XCHG = 501,        /* T_OP_ATOMIC_XCHG  */
    T_OP_ATOMIC_INC = 502,         /* T_OP_ATOMIC_INC  */
    T_OP_ATOMIC_DEC = 503,         /* T_OP_ATOMIC_DEC  */
    T_OP_ATOMIC_CMPXCHG = 504,     /* T_OP_ATOMIC_CMPXCHG  */
    T_OP_ATOMIC_MIN = 505,         /* T_OP_ATOMIC_MIN  */
    T_OP_ATOMIC_MAX = 506,         /* T_OP_ATOMIC_MAX  */
    T_OP_ATOMIC_AND = 507,         /* T_OP_ATOMIC_AND  */
    T_OP_ATOMIC_OR = 508,          /* T_OP_ATOMIC_OR  */
    T_OP_ATOMIC_XOR = 509,         /* T_OP_ATOMIC_XOR  */
    T_OP_RESINFO_B = 510,          /* T_OP_RESINFO_B  */
    T_OP_LDIB_B = 511,             /* T_OP_LDIB_B  */
    T_OP_STIB_B = 512,             /* T_OP_STIB_B  */
    T_OP_ATOMIC_B_ADD = 513,       /* T_OP_ATOMIC_B_ADD  */
    T_OP_ATOMIC_B_SUB = 514,       /* T_OP_ATOMIC_B_SUB  */
    T_OP_ATOMIC_B_XCHG = 515,      /* T_OP_ATOMIC_B_XCHG  */
    T_OP_ATOMIC_B_INC = 516,       /* T_OP_ATOMIC_B_INC  */
    T_OP_ATOMIC_B_DEC = 517,       /* T_OP_ATOMIC_B_DEC  */
    T_OP_ATOMIC_B_CMPXCHG = 518,   /* T_OP_ATOMIC_B_CMPXCHG  */
    T_OP_ATOMIC_B_MIN = 519,       /* T_OP_ATOMIC_B_MIN  */
    T_OP_ATOMIC_B_MAX = 520,       /* T_OP_ATOMIC_B_MAX  */
    T_OP_ATOMIC_B_AND = 521,       /* T_OP_ATOMIC_B_AND  */
    T_OP_ATOMIC_B_OR = 522,        /* T_OP_ATOMIC_B_OR  */
    T_OP_ATOMIC_B_XOR = 523,       /* T_OP_ATOMIC_B_XOR  */
    T_OP_ATOMIC_S_ADD = 524,       /* T_OP_ATOMIC_S_ADD  */
    T_OP_ATOMIC_S_SUB = 525,       /* T_OP_ATOMIC_S_SUB  */
    T_OP_ATOMIC_S_XCHG = 526,      /* T_OP_ATOMIC_S_XCHG  */
    T_OP_ATOMIC_S_INC = 527,       /* T_OP_ATOMIC_S_INC  */
    T_OP_ATOMIC_S_DEC = 528,       /* T_OP_ATOMIC_S_DEC  */
    T_OP_ATOMIC_S_CMPXCHG = 529,   /* T_OP_ATOMIC_S_CMPXCHG  */
    T_OP_ATOMIC_S_MIN = 530,       /* T_OP_ATOMIC_S_MIN  */
    T_OP_ATOMIC_S_MAX = 531,       /* T_OP_ATOMIC_S_MAX  */
    T_OP_ATOMIC_S_AND = 532,       /* T_OP_ATOMIC_S_AND  */
    T_OP_ATOMIC_S_OR = 533,        /* T_OP_ATOMIC_S_OR  */
    T_OP_ATOMIC_S_XOR = 534,       /* T_OP_ATOMIC_S_XOR  */
    T_OP_ATOMIC_G_ADD = 535,       /* T_OP_ATOMIC_G_ADD  */
    T_OP_ATOMIC_G_SUB = 536,       /* T_OP_ATOMIC_G_SUB  */
    T_OP_ATOMIC_G_XCHG = 537,      /* T_OP_ATOMIC_G_XCHG  */
    T_OP_ATOMIC_G_INC = 538,       /* T_OP_ATOMIC_G_INC  */
    T_OP_ATOMIC_G_DEC = 539,       /* T_OP_ATOMIC_G_DEC  */
    T_OP_ATOMIC_G_CMPXCHG = 540,   /* T_OP_ATOMIC_G_CMPXCHG  */
    T_OP_ATOMIC_G_MIN = 541,       /* T_OP_ATOMIC_G_MIN  */
    T_OP_ATOMIC_G_MAX = 542,       /* T_OP_ATOMIC_G_MAX  */
    T_OP_ATOMIC_G_AND = 543,       /* T_OP_ATOMIC_G_AND  */
    T_OP_ATOMIC_G_OR = 544,        /* T_OP_ATOMIC_G_OR  */
    T_OP_ATOMIC_G_XOR = 545,       /* T_OP_ATOMIC_G_XOR  */
    T_OP_LDGB = 546,               /* T_OP_LDGB  */
    T_OP_STGB = 547,               /* T_OP_STGB  */
    T_OP_STIB = 548,               /* T_OP_STIB  */
    T_OP_LDC = 549,                /* T_OP_LDC  */
    T_OP_LDLV = 550,               /* T_OP_LDLV  */
    T_OP_GETSPID = 551,            /* T_OP_GETSPID  */
    T_OP_GETWID = 552,             /* T_OP_GETWID  */
    T_OP_GETFIBERID = 553,         /* T_OP_GETFIBERID  */
    T_OP_STC = 554,                /* T_OP_STC  */
    T_OP_STSC = 555,               /* T_OP_STSC  */
    T_OP_SHFL = 556,               /* T_OP_SHFL  */
    T_OP_RAY_INTERSECTION = 557,   /* T_OP_RAY_INTERSECTION  */
    T_OP_BAR = 558,                /* T_OP_BAR  */
    T_OP_FENCE = 559,              /* T_OP_FENCE  */
    T_OP_SLEEP = 560,              /* T_OP_SLEEP  */
    T_OP_ICINV = 561,              /* T_OP_ICINV  */
    T_OP_DCCLN = 562,              /* T_OP_DCCLN  */
    T_OP_DCINV = 563,              /* T_OP_DCINV  */
    T_OP_DCFLU = 564,              /* T_OP_DCFLU  */
    T_OP_CCINV = 565,              /* T_OP_CCINV  */
    T_OP_LOCK = 566,               /* T_OP_LOCK  */
    T_OP_UNLOCK = 567,             /* T_OP_UNLOCK  */
    T_OP_ALIAS = 568,              /* T_OP_ALIAS  */
    T_RAW = 569,                   /* T_RAW  */
    T_OP_PRINT = 570,              /* T_OP_PRINT  */
    T_TYPE_F16 = 571,              /* T_TYPE_F16  */
    T_TYPE_F32 = 572,              /* T_TYPE_F32  */
    T_TYPE_U16 = 573,              /* T_TYPE_U16  */
    T_TYPE_U32 = 574,              /* T_TYPE_U32  */
    T_TYPE_S16 = 575,              /* T_TYPE_S16  */
    T_TYPE_S32 = 576,              /* T_TYPE_S32  */
    T_TYPE_U8 = 577,               /* T_TYPE_U8  */
    T_TYPE_U8_32 = 578,            /* T_TYPE_U8_32  */
    T_TYPE_U64 = 579,              /* T_TYPE_U64  */
    T_TYPE_B16 = 580,              /* T_TYPE_B16  */
    T_TYPE_B32 = 581,              /* T_TYPE_B32  */
    T_UNTYPED = 582,               /* T_UNTYPED  */
    T_TYPED = 583,                 /* T_TYPED  */
    T_MIXED = 584,                 /* T_MIXED  */
    T_UNSIGNED = 585,              /* T_UNSIGNED  */
    T_LOW = 586,                   /* T_LOW  */
    T_HIGH = 587,                  /* T_HIGH  */
    T_1D = 588,                    /* T_1D  */
    T_2D = 589,                    /* T_2D  */
    T_3D = 590,                    /* T_3D  */
    T_4D = 591,                    /* T_4D  */
    T_SAD = 592,                   /* T_SAD  */
    T_SSD = 593,                   /* T_SSD  */
    T_LT = 594,                    /* T_LT  */
    T_LE = 595,                    /* T_LE  */
    T_GT = 596,                    /* T_GT  */
    T_GE = 597,                    /* T_GE  */
    T_EQ = 598,                    /* T_EQ  */
    T_NE = 599,                    /* T_NE  */
    T_S2EN = 600,                  /* T_S2EN  */
    T_SAMP = 601,                  /* T_SAMP  */
    T_TEX = 602,                   /* T_TEX  */
    T_BASE = 603,                  /* T_BASE  */
    T_OFFSET = 604,                /* T_OFFSET  */
    T_UNIFORM = 605,               /* T_UNIFORM  */
    T_NONUNIFORM = 606,            /* T_NONUNIFORM  */
    T_IMM = 607,                   /* T_IMM  */
    T_RCK = 608,                   /* T_RCK  */
    T_CLP = 609,                   /* T_CLP  */
    T_NAN = 610,                   /* T_NAN  */
    T_INF = 611,                   /* T_INF  */
    T_A0 = 612,                    /* T_A0  */
    T_A1 = 613,                    /* T_A1  */
    T_P0 = 614,                    /* T_P0  */
    T_UP0 = 615,                   /* T_UP0  */
    T_W = 616,                     /* T_W  */
    T_CAT1_TYPE_TYPE = 617,        /* T_CAT1_TYPE_TYPE  */
    T_MOD_TEX = 618,               /* T_MOD_TEX  */
    T_MOD_MEM = 619,               /* T_MOD_MEM  */
    T_MOD_RT = 620,                /* T_MOD_RT  */
    T_MOD_XOR = 621,               /* T_MOD_XOR  */
    T_MOD_UP = 622,                /* T_MOD_UP  */
    T_MOD_DOWN = 623,              /* T_MOD_DOWN  */
    T_MOD_RUP = 624,               /* T_MOD_RUP  */
    T_MOD_RDOWN = 625              /* T_MOD_RDOWN  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 37 "ir3_parser.y"

	int tok;
	int num;
	uint32_t unum;
	uint64_t u64;
	double flt;
	const char *str;
	struct ir3_register *reg;
	struct {
		int start;
		int num;
	} range;
	type_t type;

#line 459 "ir3_parser.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE ir3_yylval;


int ir3_yyparse (void);


#endif /* !YY_IR3_YY_IR3_PARSER_H_INCLUDED  */
