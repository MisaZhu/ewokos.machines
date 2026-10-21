/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

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

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1


/* Substitute the variable and function names.  */
#define yyparse         ir3_yyparse
#define yylex           ir3_yylex
#define yyerror         ir3_yyerror
#define yydebug         ir3_yydebug
#define yynerrs         ir3_yynerrs
#define yylval          ir3_yylval
#define yychar          ir3_yychar

/* First part of user prologue.  */
#line 32 "ir3_parser.y"

#define YYDEBUG 0
#include "ir3/ir3_parser_support.c"

#line 83 "ir3_parser.c"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "ir3_parser.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_T_INT = 3,                      /* T_INT  */
  YYSYMBOL_T_HEX = 4,                      /* T_HEX  */
  YYSYMBOL_T_FLOAT = 5,                    /* T_FLOAT  */
  YYSYMBOL_T_IDENTIFIER = 6,               /* T_IDENTIFIER  */
  YYSYMBOL_T_REGISTER = 7,                 /* T_REGISTER  */
  YYSYMBOL_T_CONSTANT = 8,                 /* T_CONSTANT  */
  YYSYMBOL_T_RT = 9,                       /* T_RT  */
  YYSYMBOL_T_A_LOCALSIZE = 10,             /* T_A_LOCALSIZE  */
  YYSYMBOL_T_A_CONST = 11,                 /* T_A_CONST  */
  YYSYMBOL_T_A_BUF = 12,                   /* T_A_BUF  */
  YYSYMBOL_T_A_UBO = 13,                   /* T_A_UBO  */
  YYSYMBOL_T_A_INVOCATIONID = 14,          /* T_A_INVOCATIONID  */
  YYSYMBOL_T_A_WGID = 15,                  /* T_A_WGID  */
  YYSYMBOL_T_A_NUMWG = 16,                 /* T_A_NUMWG  */
  YYSYMBOL_T_A_BRANCHSTACK = 17,           /* T_A_BRANCHSTACK  */
  YYSYMBOL_T_A_IN = 18,                    /* T_A_IN  */
  YYSYMBOL_T_A_OUT = 19,                   /* T_A_OUT  */
  YYSYMBOL_T_A_TEX = 20,                   /* T_A_TEX  */
  YYSYMBOL_T_A_PVTMEM = 21,                /* T_A_PVTMEM  */
  YYSYMBOL_T_A_LOCALMEM = 22,              /* T_A_LOCALMEM  */
  YYSYMBOL_T_A_CONSTLEN = 23,              /* T_A_CONSTLEN  */
  YYSYMBOL_T_A_EARLYPREAMBLE = 24,         /* T_A_EARLYPREAMBLE  */
  YYSYMBOL_T_A_FULLNOPSTART = 25,          /* T_A_FULLNOPSTART  */
  YYSYMBOL_T_A_FULLNOPEND = 26,            /* T_A_FULLNOPEND  */
  YYSYMBOL_T_A_FULLSYNCSTART = 27,         /* T_A_FULLSYNCSTART  */
  YYSYMBOL_T_A_FULLSYNCEND = 28,           /* T_A_FULLSYNCEND  */
  YYSYMBOL_T_ABSNEG = 29,                  /* T_ABSNEG  */
  YYSYMBOL_T_NEG = 30,                     /* T_NEG  */
  YYSYMBOL_T_ABS = 31,                     /* T_ABS  */
  YYSYMBOL_T_R = 32,                       /* T_R  */
  YYSYMBOL_T_LAST = 33,                    /* T_LAST  */
  YYSYMBOL_T_HR = 34,                      /* T_HR  */
  YYSYMBOL_T_HC = 35,                      /* T_HC  */
  YYSYMBOL_T_EVEN = 36,                    /* T_EVEN  */
  YYSYMBOL_T_POS_INFINITY = 37,            /* T_POS_INFINITY  */
  YYSYMBOL_T_NEG_INFINITY = 38,            /* T_NEG_INFINITY  */
  YYSYMBOL_T_EI = 39,                      /* T_EI  */
  YYSYMBOL_T_WRMASK = 40,                  /* T_WRMASK  */
  YYSYMBOL_T_FLUT_0_0 = 41,                /* T_FLUT_0_0  */
  YYSYMBOL_T_FLUT_0_5 = 42,                /* T_FLUT_0_5  */
  YYSYMBOL_T_FLUT_1_0 = 43,                /* T_FLUT_1_0  */
  YYSYMBOL_T_FLUT_2_0 = 44,                /* T_FLUT_2_0  */
  YYSYMBOL_T_FLUT_E = 45,                  /* T_FLUT_E  */
  YYSYMBOL_T_FLUT_PI = 46,                 /* T_FLUT_PI  */
  YYSYMBOL_T_FLUT_INV_PI = 47,             /* T_FLUT_INV_PI  */
  YYSYMBOL_T_FLUT_INV_LOG2_E = 48,         /* T_FLUT_INV_LOG2_E  */
  YYSYMBOL_T_FLUT_LOG2_E = 49,             /* T_FLUT_LOG2_E  */
  YYSYMBOL_T_FLUT_INV_LOG2_10 = 50,        /* T_FLUT_INV_LOG2_10  */
  YYSYMBOL_T_FLUT_LOG2_10 = 51,            /* T_FLUT_LOG2_10  */
  YYSYMBOL_T_FLUT_4_0 = 52,                /* T_FLUT_4_0  */
  YYSYMBOL_T_SY = 53,                      /* T_SY  */
  YYSYMBOL_T_SS = 54,                      /* T_SS  */
  YYSYMBOL_T_JP = 55,                      /* T_JP  */
  YYSYMBOL_T_EQ_FLAG = 56,                 /* T_EQ_FLAG  */
  YYSYMBOL_T_SAT = 57,                     /* T_SAT  */
  YYSYMBOL_T_RPT = 58,                     /* T_RPT  */
  YYSYMBOL_T_UL = 59,                      /* T_UL  */
  YYSYMBOL_T_NOP = 60,                     /* T_NOP  */
  YYSYMBOL_T_EOLM = 61,                    /* T_EOLM  */
  YYSYMBOL_T_EOGM = 62,                    /* T_EOGM  */
  YYSYMBOL_T_EOSTSC = 63,                  /* T_EOSTSC  */
  YYSYMBOL_T_OP_NOP = 64,                  /* T_OP_NOP  */
  YYSYMBOL_T_OP_BR = 65,                   /* T_OP_BR  */
  YYSYMBOL_T_OP_BRAO = 66,                 /* T_OP_BRAO  */
  YYSYMBOL_T_OP_BRAA = 67,                 /* T_OP_BRAA  */
  YYSYMBOL_T_OP_BRAC = 68,                 /* T_OP_BRAC  */
  YYSYMBOL_T_OP_BANY = 69,                 /* T_OP_BANY  */
  YYSYMBOL_T_OP_BALL = 70,                 /* T_OP_BALL  */
  YYSYMBOL_T_OP_BRAX = 71,                 /* T_OP_BRAX  */
  YYSYMBOL_T_OP_JUMP = 72,                 /* T_OP_JUMP  */
  YYSYMBOL_T_OP_CALL = 73,                 /* T_OP_CALL  */
  YYSYMBOL_T_OP_RET = 74,                  /* T_OP_RET  */
  YYSYMBOL_T_OP_KILL = 75,                 /* T_OP_KILL  */
  YYSYMBOL_T_OP_END = 76,                  /* T_OP_END  */
  YYSYMBOL_T_OP_EMIT = 77,                 /* T_OP_EMIT  */
  YYSYMBOL_T_OP_CUT = 78,                  /* T_OP_CUT  */
  YYSYMBOL_T_OP_CHMASK = 79,               /* T_OP_CHMASK  */
  YYSYMBOL_T_OP_CHSH = 80,                 /* T_OP_CHSH  */
  YYSYMBOL_T_OP_FLOW_REV = 81,             /* T_OP_FLOW_REV  */
  YYSYMBOL_T_OP_BKT = 82,                  /* T_OP_BKT  */
  YYSYMBOL_T_OP_STKS = 83,                 /* T_OP_STKS  */
  YYSYMBOL_T_OP_STKR = 84,                 /* T_OP_STKR  */
  YYSYMBOL_T_OP_XSET = 85,                 /* T_OP_XSET  */
  YYSYMBOL_T_OP_XCLR = 86,                 /* T_OP_XCLR  */
  YYSYMBOL_T_OP_GETLAST = 87,              /* T_OP_GETLAST  */
  YYSYMBOL_T_OP_GETONE = 88,               /* T_OP_GETONE  */
  YYSYMBOL_T_OP_DBG = 89,                  /* T_OP_DBG  */
  YYSYMBOL_T_OP_SHPS = 90,                 /* T_OP_SHPS  */
  YYSYMBOL_T_OP_SHPE = 91,                 /* T_OP_SHPE  */
  YYSYMBOL_T_OP_PREDT = 92,                /* T_OP_PREDT  */
  YYSYMBOL_T_OP_PREDF = 93,                /* T_OP_PREDF  */
  YYSYMBOL_T_OP_PREDE = 94,                /* T_OP_PREDE  */
  YYSYMBOL_T_OP_MOVMSK = 95,               /* T_OP_MOVMSK  */
  YYSYMBOL_T_OP_MOVA1 = 96,                /* T_OP_MOVA1  */
  YYSYMBOL_T_OP_MOVA = 97,                 /* T_OP_MOVA  */
  YYSYMBOL_T_OP_MOVA_R = 98,               /* T_OP_MOVA_R  */
  YYSYMBOL_T_OP_MOV = 99,                  /* T_OP_MOV  */
  YYSYMBOL_T_OP_COV = 100,                 /* T_OP_COV  */
  YYSYMBOL_T_OP_SWZ = 101,                 /* T_OP_SWZ  */
  YYSYMBOL_T_OP_GAT = 102,                 /* T_OP_GAT  */
  YYSYMBOL_T_OP_SCT = 103,                 /* T_OP_SCT  */
  YYSYMBOL_T_OP_MOVS = 104,                /* T_OP_MOVS  */
  YYSYMBOL_T_MUL2 = 105,                   /* T_MUL2  */
  YYSYMBOL_T_DIV2 = 106,                   /* T_DIV2  */
  YYSYMBOL_T_OP_ADD_F = 107,               /* T_OP_ADD_F  */
  YYSYMBOL_T_OP_MIN_F = 108,               /* T_OP_MIN_F  */
  YYSYMBOL_T_OP_MAX_F = 109,               /* T_OP_MAX_F  */
  YYSYMBOL_T_OP_MUL_F = 110,               /* T_OP_MUL_F  */
  YYSYMBOL_T_OP_SIGN_F = 111,              /* T_OP_SIGN_F  */
  YYSYMBOL_T_OP_CMPS_F = 112,              /* T_OP_CMPS_F  */
  YYSYMBOL_T_OP_ABSNEG_F = 113,            /* T_OP_ABSNEG_F  */
  YYSYMBOL_T_OP_CMPV_F = 114,              /* T_OP_CMPV_F  */
  YYSYMBOL_T_OP_FLOOR_F = 115,             /* T_OP_FLOOR_F  */
  YYSYMBOL_T_OP_CEIL_F = 116,              /* T_OP_CEIL_F  */
  YYSYMBOL_T_OP_RNDNE_F = 117,             /* T_OP_RNDNE_F  */
  YYSYMBOL_T_OP_RNDAZ_F = 118,             /* T_OP_RNDAZ_F  */
  YYSYMBOL_T_OP_TRUNC_F = 119,             /* T_OP_TRUNC_F  */
  YYSYMBOL_T_OP_ADD_U = 120,               /* T_OP_ADD_U  */
  YYSYMBOL_T_OP_ADD_S = 121,               /* T_OP_ADD_S  */
  YYSYMBOL_T_OP_SUB_U = 122,               /* T_OP_SUB_U  */
  YYSYMBOL_T_OP_SUB_S = 123,               /* T_OP_SUB_S  */
  YYSYMBOL_T_OP_CMPS_U = 124,              /* T_OP_CMPS_U  */
  YYSYMBOL_T_OP_CMPS_S = 125,              /* T_OP_CMPS_S  */
  YYSYMBOL_T_OP_MIN_U = 126,               /* T_OP_MIN_U  */
  YYSYMBOL_T_OP_MIN_S = 127,               /* T_OP_MIN_S  */
  YYSYMBOL_T_OP_MAX_U = 128,               /* T_OP_MAX_U  */
  YYSYMBOL_T_OP_MAX_S = 129,               /* T_OP_MAX_S  */
  YYSYMBOL_T_OP_ABSNEG_S = 130,            /* T_OP_ABSNEG_S  */
  YYSYMBOL_T_OP_AND_B = 131,               /* T_OP_AND_B  */
  YYSYMBOL_T_OP_OR_B = 132,                /* T_OP_OR_B  */
  YYSYMBOL_T_OP_NOT_B = 133,               /* T_OP_NOT_B  */
  YYSYMBOL_T_OP_XOR_B = 134,               /* T_OP_XOR_B  */
  YYSYMBOL_T_OP_CMPV_U = 135,              /* T_OP_CMPV_U  */
  YYSYMBOL_T_OP_CMPV_S = 136,              /* T_OP_CMPV_S  */
  YYSYMBOL_T_OP_MUL_U24 = 137,             /* T_OP_MUL_U24  */
  YYSYMBOL_T_OP_MUL_S24 = 138,             /* T_OP_MUL_S24  */
  YYSYMBOL_T_OP_MULL_U = 139,              /* T_OP_MULL_U  */
  YYSYMBOL_T_OP_BFREV_B = 140,             /* T_OP_BFREV_B  */
  YYSYMBOL_T_OP_CLZ_S = 141,               /* T_OP_CLZ_S  */
  YYSYMBOL_T_OP_CLZ_B = 142,               /* T_OP_CLZ_B  */
  YYSYMBOL_T_OP_SHL_B = 143,               /* T_OP_SHL_B  */
  YYSYMBOL_T_OP_SHR_B = 144,               /* T_OP_SHR_B  */
  YYSYMBOL_T_OP_ASHR_B = 145,              /* T_OP_ASHR_B  */
  YYSYMBOL_T_OP_BARY_F = 146,              /* T_OP_BARY_F  */
  YYSYMBOL_T_OP_FLAT_B = 147,              /* T_OP_FLAT_B  */
  YYSYMBOL_T_OP_MGEN_B = 148,              /* T_OP_MGEN_B  */
  YYSYMBOL_T_OP_GETBIT_B = 149,            /* T_OP_GETBIT_B  */
  YYSYMBOL_T_OP_SETRM = 150,               /* T_OP_SETRM  */
  YYSYMBOL_T_OP_CBITS_B = 151,             /* T_OP_CBITS_B  */
  YYSYMBOL_T_OP_SHB = 152,                 /* T_OP_SHB  */
  YYSYMBOL_T_OP_MSAD = 153,                /* T_OP_MSAD  */
  YYSYMBOL_T_OP_MAD_U16 = 154,             /* T_OP_MAD_U16  */
  YYSYMBOL_T_OP_MADSH_U16 = 155,           /* T_OP_MADSH_U16  */
  YYSYMBOL_T_OP_MAD_S16 = 156,             /* T_OP_MAD_S16  */
  YYSYMBOL_T_OP_MADSH_M16 = 157,           /* T_OP_MADSH_M16  */
  YYSYMBOL_T_OP_MAD_U24 = 158,             /* T_OP_MAD_U24  */
  YYSYMBOL_T_OP_MAD_S24 = 159,             /* T_OP_MAD_S24  */
  YYSYMBOL_T_OP_MAD_F16 = 160,             /* T_OP_MAD_F16  */
  YYSYMBOL_T_OP_MAD_F32 = 161,             /* T_OP_MAD_F32  */
  YYSYMBOL_T_OP_SEL_B16 = 162,             /* T_OP_SEL_B16  */
  YYSYMBOL_T_OP_SEL_B32 = 163,             /* T_OP_SEL_B32  */
  YYSYMBOL_T_OP_SEL_S16 = 164,             /* T_OP_SEL_S16  */
  YYSYMBOL_T_OP_SEL_S32 = 165,             /* T_OP_SEL_S32  */
  YYSYMBOL_T_OP_SEL_F16 = 166,             /* T_OP_SEL_F16  */
  YYSYMBOL_T_OP_SEL_F32 = 167,             /* T_OP_SEL_F32  */
  YYSYMBOL_T_OP_SAD_S16 = 168,             /* T_OP_SAD_S16  */
  YYSYMBOL_T_OP_SAD_S32 = 169,             /* T_OP_SAD_S32  */
  YYSYMBOL_T_OP_SHRM = 170,                /* T_OP_SHRM  */
  YYSYMBOL_T_OP_SHLM = 171,                /* T_OP_SHLM  */
  YYSYMBOL_T_OP_SHRG = 172,                /* T_OP_SHRG  */
  YYSYMBOL_T_OP_SHLG = 173,                /* T_OP_SHLG  */
  YYSYMBOL_T_OP_ANDG = 174,                /* T_OP_ANDG  */
  YYSYMBOL_T_OP_DP2ACC = 175,              /* T_OP_DP2ACC  */
  YYSYMBOL_T_OP_DP4ACC = 176,              /* T_OP_DP4ACC  */
  YYSYMBOL_T_OP_WMM = 177,                 /* T_OP_WMM  */
  YYSYMBOL_T_OP_WMM_ACCU = 178,            /* T_OP_WMM_ACCU  */
  YYSYMBOL_T_OP_RCP = 179,                 /* T_OP_RCP  */
  YYSYMBOL_T_OP_RSQ = 180,                 /* T_OP_RSQ  */
  YYSYMBOL_T_OP_LOG2 = 181,                /* T_OP_LOG2  */
  YYSYMBOL_T_OP_EXP2 = 182,                /* T_OP_EXP2  */
  YYSYMBOL_T_OP_SIN = 183,                 /* T_OP_SIN  */
  YYSYMBOL_T_OP_COS = 184,                 /* T_OP_COS  */
  YYSYMBOL_T_OP_SQRT = 185,                /* T_OP_SQRT  */
  YYSYMBOL_T_OP_HRSQ = 186,                /* T_OP_HRSQ  */
  YYSYMBOL_T_OP_HLOG2 = 187,               /* T_OP_HLOG2  */
  YYSYMBOL_T_OP_HEXP2 = 188,               /* T_OP_HEXP2  */
  YYSYMBOL_T_OP_ISAM = 189,                /* T_OP_ISAM  */
  YYSYMBOL_T_OP_ISAML = 190,               /* T_OP_ISAML  */
  YYSYMBOL_T_OP_ISAMM = 191,               /* T_OP_ISAMM  */
  YYSYMBOL_T_OP_SAM = 192,                 /* T_OP_SAM  */
  YYSYMBOL_T_OP_SAMB = 193,                /* T_OP_SAMB  */
  YYSYMBOL_T_OP_SAML = 194,                /* T_OP_SAML  */
  YYSYMBOL_T_OP_SAMGQ = 195,               /* T_OP_SAMGQ  */
  YYSYMBOL_T_OP_GETLOD = 196,              /* T_OP_GETLOD  */
  YYSYMBOL_T_OP_CONV = 197,                /* T_OP_CONV  */
  YYSYMBOL_T_OP_CONVM = 198,               /* T_OP_CONVM  */
  YYSYMBOL_T_OP_GETSIZE = 199,             /* T_OP_GETSIZE  */
  YYSYMBOL_T_OP_GETBUF = 200,              /* T_OP_GETBUF  */
  YYSYMBOL_T_OP_GETPOS = 201,              /* T_OP_GETPOS  */
  YYSYMBOL_T_OP_GETINFO = 202,             /* T_OP_GETINFO  */
  YYSYMBOL_T_OP_DSX = 203,                 /* T_OP_DSX  */
  YYSYMBOL_T_OP_DSY = 204,                 /* T_OP_DSY  */
  YYSYMBOL_T_OP_GATHER4R = 205,            /* T_OP_GATHER4R  */
  YYSYMBOL_T_OP_GATHER4G = 206,            /* T_OP_GATHER4G  */
  YYSYMBOL_T_OP_GATHER4B = 207,            /* T_OP_GATHER4B  */
  YYSYMBOL_T_OP_GATHER4A = 208,            /* T_OP_GATHER4A  */
  YYSYMBOL_T_OP_SAMGP0 = 209,              /* T_OP_SAMGP0  */
  YYSYMBOL_T_OP_SAMGP1 = 210,              /* T_OP_SAMGP1  */
  YYSYMBOL_T_OP_SAMGP2 = 211,              /* T_OP_SAMGP2  */
  YYSYMBOL_T_OP_SAMGP3 = 212,              /* T_OP_SAMGP3  */
  YYSYMBOL_T_OP_DSXPP_1 = 213,             /* T_OP_DSXPP_1  */
  YYSYMBOL_T_OP_DSYPP_1 = 214,             /* T_OP_DSYPP_1  */
  YYSYMBOL_T_OP_RGETPOS = 215,             /* T_OP_RGETPOS  */
  YYSYMBOL_T_OP_RGETINFO = 216,            /* T_OP_RGETINFO  */
  YYSYMBOL_T_OP_BRCST_A = 217,             /* T_OP_BRCST_A  */
  YYSYMBOL_T_OP_QSHUFFLE_BRCST = 218,      /* T_OP_QSHUFFLE_BRCST  */
  YYSYMBOL_T_OP_QSHUFFLE_H = 219,          /* T_OP_QSHUFFLE_H  */
  YYSYMBOL_T_OP_QSHUFFLE_V = 220,          /* T_OP_QSHUFFLE_V  */
  YYSYMBOL_T_OP_QSHUFFLE_DIAG = 221,       /* T_OP_QSHUFFLE_DIAG  */
  YYSYMBOL_T_OP_TCINV = 222,               /* T_OP_TCINV  */
  YYSYMBOL_T_OP_IMG_BINDLESS_HOF = 223,    /* T_OP_IMG_BINDLESS_HOF  */
  YYSYMBOL_T_OP_IMG_BINDLESS_PCMN = 224,   /* T_OP_IMG_BINDLESS_PCMN  */
  YYSYMBOL_T_OP_IMG_BINDLESS = 225,        /* T_OP_IMG_BINDLESS  */
  YYSYMBOL_T_OP_LDG = 226,                 /* T_OP_LDG  */
  YYSYMBOL_T_OP_LDG_A = 227,               /* T_OP_LDG_A  */
  YYSYMBOL_T_OP_LDG_K = 228,               /* T_OP_LDG_K  */
  YYSYMBOL_T_OP_LDL = 229,                 /* T_OP_LDL  */
  YYSYMBOL_T_OP_LDP = 230,                 /* T_OP_LDP  */
  YYSYMBOL_T_OP_STG = 231,                 /* T_OP_STG  */
  YYSYMBOL_T_OP_STG_A = 232,               /* T_OP_STG_A  */
  YYSYMBOL_T_OP_STL = 233,                 /* T_OP_STL  */
  YYSYMBOL_T_OP_STP = 234,                 /* T_OP_STP  */
  YYSYMBOL_T_OP_LDIB = 235,                /* T_OP_LDIB  */
  YYSYMBOL_T_OP_G2L = 236,                 /* T_OP_G2L  */
  YYSYMBOL_T_OP_L2G = 237,                 /* T_OP_L2G  */
  YYSYMBOL_T_OP_PREFETCH = 238,            /* T_OP_PREFETCH  */
  YYSYMBOL_T_OP_LDLW = 239,                /* T_OP_LDLW  */
  YYSYMBOL_T_OP_STLW = 240,                /* T_OP_STLW  */
  YYSYMBOL_T_OP_RESFMT = 241,              /* T_OP_RESFMT  */
  YYSYMBOL_T_OP_RESINFO = 242,             /* T_OP_RESINFO  */
  YYSYMBOL_T_OP_RESBASE = 243,             /* T_OP_RESBASE  */
  YYSYMBOL_T_OP_ATOMIC_ADD = 244,          /* T_OP_ATOMIC_ADD  */
  YYSYMBOL_T_OP_ATOMIC_SUB = 245,          /* T_OP_ATOMIC_SUB  */
  YYSYMBOL_T_OP_ATOMIC_XCHG = 246,         /* T_OP_ATOMIC_XCHG  */
  YYSYMBOL_T_OP_ATOMIC_INC = 247,          /* T_OP_ATOMIC_INC  */
  YYSYMBOL_T_OP_ATOMIC_DEC = 248,          /* T_OP_ATOMIC_DEC  */
  YYSYMBOL_T_OP_ATOMIC_CMPXCHG = 249,      /* T_OP_ATOMIC_CMPXCHG  */
  YYSYMBOL_T_OP_ATOMIC_MIN = 250,          /* T_OP_ATOMIC_MIN  */
  YYSYMBOL_T_OP_ATOMIC_MAX = 251,          /* T_OP_ATOMIC_MAX  */
  YYSYMBOL_T_OP_ATOMIC_AND = 252,          /* T_OP_ATOMIC_AND  */
  YYSYMBOL_T_OP_ATOMIC_OR = 253,           /* T_OP_ATOMIC_OR  */
  YYSYMBOL_T_OP_ATOMIC_XOR = 254,          /* T_OP_ATOMIC_XOR  */
  YYSYMBOL_T_OP_RESINFO_B = 255,           /* T_OP_RESINFO_B  */
  YYSYMBOL_T_OP_LDIB_B = 256,              /* T_OP_LDIB_B  */
  YYSYMBOL_T_OP_STIB_B = 257,              /* T_OP_STIB_B  */
  YYSYMBOL_T_OP_ATOMIC_B_ADD = 258,        /* T_OP_ATOMIC_B_ADD  */
  YYSYMBOL_T_OP_ATOMIC_B_SUB = 259,        /* T_OP_ATOMIC_B_SUB  */
  YYSYMBOL_T_OP_ATOMIC_B_XCHG = 260,       /* T_OP_ATOMIC_B_XCHG  */
  YYSYMBOL_T_OP_ATOMIC_B_INC = 261,        /* T_OP_ATOMIC_B_INC  */
  YYSYMBOL_T_OP_ATOMIC_B_DEC = 262,        /* T_OP_ATOMIC_B_DEC  */
  YYSYMBOL_T_OP_ATOMIC_B_CMPXCHG = 263,    /* T_OP_ATOMIC_B_CMPXCHG  */
  YYSYMBOL_T_OP_ATOMIC_B_MIN = 264,        /* T_OP_ATOMIC_B_MIN  */
  YYSYMBOL_T_OP_ATOMIC_B_MAX = 265,        /* T_OP_ATOMIC_B_MAX  */
  YYSYMBOL_T_OP_ATOMIC_B_AND = 266,        /* T_OP_ATOMIC_B_AND  */
  YYSYMBOL_T_OP_ATOMIC_B_OR = 267,         /* T_OP_ATOMIC_B_OR  */
  YYSYMBOL_T_OP_ATOMIC_B_XOR = 268,        /* T_OP_ATOMIC_B_XOR  */
  YYSYMBOL_T_OP_ATOMIC_S_ADD = 269,        /* T_OP_ATOMIC_S_ADD  */
  YYSYMBOL_T_OP_ATOMIC_S_SUB = 270,        /* T_OP_ATOMIC_S_SUB  */
  YYSYMBOL_T_OP_ATOMIC_S_XCHG = 271,       /* T_OP_ATOMIC_S_XCHG  */
  YYSYMBOL_T_OP_ATOMIC_S_INC = 272,        /* T_OP_ATOMIC_S_INC  */
  YYSYMBOL_T_OP_ATOMIC_S_DEC = 273,        /* T_OP_ATOMIC_S_DEC  */
  YYSYMBOL_T_OP_ATOMIC_S_CMPXCHG = 274,    /* T_OP_ATOMIC_S_CMPXCHG  */
  YYSYMBOL_T_OP_ATOMIC_S_MIN = 275,        /* T_OP_ATOMIC_S_MIN  */
  YYSYMBOL_T_OP_ATOMIC_S_MAX = 276,        /* T_OP_ATOMIC_S_MAX  */
  YYSYMBOL_T_OP_ATOMIC_S_AND = 277,        /* T_OP_ATOMIC_S_AND  */
  YYSYMBOL_T_OP_ATOMIC_S_OR = 278,         /* T_OP_ATOMIC_S_OR  */
  YYSYMBOL_T_OP_ATOMIC_S_XOR = 279,        /* T_OP_ATOMIC_S_XOR  */
  YYSYMBOL_T_OP_ATOMIC_G_ADD = 280,        /* T_OP_ATOMIC_G_ADD  */
  YYSYMBOL_T_OP_ATOMIC_G_SUB = 281,        /* T_OP_ATOMIC_G_SUB  */
  YYSYMBOL_T_OP_ATOMIC_G_XCHG = 282,       /* T_OP_ATOMIC_G_XCHG  */
  YYSYMBOL_T_OP_ATOMIC_G_INC = 283,        /* T_OP_ATOMIC_G_INC  */
  YYSYMBOL_T_OP_ATOMIC_G_DEC = 284,        /* T_OP_ATOMIC_G_DEC  */
  YYSYMBOL_T_OP_ATOMIC_G_CMPXCHG = 285,    /* T_OP_ATOMIC_G_CMPXCHG  */
  YYSYMBOL_T_OP_ATOMIC_G_MIN = 286,        /* T_OP_ATOMIC_G_MIN  */
  YYSYMBOL_T_OP_ATOMIC_G_MAX = 287,        /* T_OP_ATOMIC_G_MAX  */
  YYSYMBOL_T_OP_ATOMIC_G_AND = 288,        /* T_OP_ATOMIC_G_AND  */
  YYSYMBOL_T_OP_ATOMIC_G_OR = 289,         /* T_OP_ATOMIC_G_OR  */
  YYSYMBOL_T_OP_ATOMIC_G_XOR = 290,        /* T_OP_ATOMIC_G_XOR  */
  YYSYMBOL_T_OP_LDGB = 291,                /* T_OP_LDGB  */
  YYSYMBOL_T_OP_STGB = 292,                /* T_OP_STGB  */
  YYSYMBOL_T_OP_STIB = 293,                /* T_OP_STIB  */
  YYSYMBOL_T_OP_LDC = 294,                 /* T_OP_LDC  */
  YYSYMBOL_T_OP_LDLV = 295,                /* T_OP_LDLV  */
  YYSYMBOL_T_OP_GETSPID = 296,             /* T_OP_GETSPID  */
  YYSYMBOL_T_OP_GETWID = 297,              /* T_OP_GETWID  */
  YYSYMBOL_T_OP_GETFIBERID = 298,          /* T_OP_GETFIBERID  */
  YYSYMBOL_T_OP_STC = 299,                 /* T_OP_STC  */
  YYSYMBOL_T_OP_STSC = 300,                /* T_OP_STSC  */
  YYSYMBOL_T_OP_SHFL = 301,                /* T_OP_SHFL  */
  YYSYMBOL_T_OP_RAY_INTERSECTION = 302,    /* T_OP_RAY_INTERSECTION  */
  YYSYMBOL_T_OP_BAR = 303,                 /* T_OP_BAR  */
  YYSYMBOL_T_OP_FENCE = 304,               /* T_OP_FENCE  */
  YYSYMBOL_T_OP_SLEEP = 305,               /* T_OP_SLEEP  */
  YYSYMBOL_T_OP_ICINV = 306,               /* T_OP_ICINV  */
  YYSYMBOL_T_OP_DCCLN = 307,               /* T_OP_DCCLN  */
  YYSYMBOL_T_OP_DCINV = 308,               /* T_OP_DCINV  */
  YYSYMBOL_T_OP_DCFLU = 309,               /* T_OP_DCFLU  */
  YYSYMBOL_T_OP_CCINV = 310,               /* T_OP_CCINV  */
  YYSYMBOL_T_OP_LOCK = 311,                /* T_OP_LOCK  */
  YYSYMBOL_T_OP_UNLOCK = 312,              /* T_OP_UNLOCK  */
  YYSYMBOL_T_OP_ALIAS = 313,               /* T_OP_ALIAS  */
  YYSYMBOL_T_RAW = 314,                    /* T_RAW  */
  YYSYMBOL_T_OP_PRINT = 315,               /* T_OP_PRINT  */
  YYSYMBOL_T_TYPE_F16 = 316,               /* T_TYPE_F16  */
  YYSYMBOL_T_TYPE_F32 = 317,               /* T_TYPE_F32  */
  YYSYMBOL_T_TYPE_U16 = 318,               /* T_TYPE_U16  */
  YYSYMBOL_T_TYPE_U32 = 319,               /* T_TYPE_U32  */
  YYSYMBOL_T_TYPE_S16 = 320,               /* T_TYPE_S16  */
  YYSYMBOL_T_TYPE_S32 = 321,               /* T_TYPE_S32  */
  YYSYMBOL_T_TYPE_U8 = 322,                /* T_TYPE_U8  */
  YYSYMBOL_T_TYPE_U8_32 = 323,             /* T_TYPE_U8_32  */
  YYSYMBOL_T_TYPE_U64 = 324,               /* T_TYPE_U64  */
  YYSYMBOL_T_TYPE_B16 = 325,               /* T_TYPE_B16  */
  YYSYMBOL_T_TYPE_B32 = 326,               /* T_TYPE_B32  */
  YYSYMBOL_T_UNTYPED = 327,                /* T_UNTYPED  */
  YYSYMBOL_T_TYPED = 328,                  /* T_TYPED  */
  YYSYMBOL_T_MIXED = 329,                  /* T_MIXED  */
  YYSYMBOL_T_UNSIGNED = 330,               /* T_UNSIGNED  */
  YYSYMBOL_T_LOW = 331,                    /* T_LOW  */
  YYSYMBOL_T_HIGH = 332,                   /* T_HIGH  */
  YYSYMBOL_T_1D = 333,                     /* T_1D  */
  YYSYMBOL_T_2D = 334,                     /* T_2D  */
  YYSYMBOL_T_3D = 335,                     /* T_3D  */
  YYSYMBOL_T_4D = 336,                     /* T_4D  */
  YYSYMBOL_T_SAD = 337,                    /* T_SAD  */
  YYSYMBOL_T_SSD = 338,                    /* T_SSD  */
  YYSYMBOL_T_LT = 339,                     /* T_LT  */
  YYSYMBOL_T_LE = 340,                     /* T_LE  */
  YYSYMBOL_T_GT = 341,                     /* T_GT  */
  YYSYMBOL_T_GE = 342,                     /* T_GE  */
  YYSYMBOL_T_EQ = 343,                     /* T_EQ  */
  YYSYMBOL_T_NE = 344,                     /* T_NE  */
  YYSYMBOL_T_S2EN = 345,                   /* T_S2EN  */
  YYSYMBOL_T_SAMP = 346,                   /* T_SAMP  */
  YYSYMBOL_T_TEX = 347,                    /* T_TEX  */
  YYSYMBOL_T_BASE = 348,                   /* T_BASE  */
  YYSYMBOL_T_OFFSET = 349,                 /* T_OFFSET  */
  YYSYMBOL_T_UNIFORM = 350,                /* T_UNIFORM  */
  YYSYMBOL_T_NONUNIFORM = 351,             /* T_NONUNIFORM  */
  YYSYMBOL_T_IMM = 352,                    /* T_IMM  */
  YYSYMBOL_T_RCK = 353,                    /* T_RCK  */
  YYSYMBOL_T_CLP = 354,                    /* T_CLP  */
  YYSYMBOL_T_NAN = 355,                    /* T_NAN  */
  YYSYMBOL_T_INF = 356,                    /* T_INF  */
  YYSYMBOL_T_A0 = 357,                     /* T_A0  */
  YYSYMBOL_T_A1 = 358,                     /* T_A1  */
  YYSYMBOL_T_P0 = 359,                     /* T_P0  */
  YYSYMBOL_T_UP0 = 360,                    /* T_UP0  */
  YYSYMBOL_T_W = 361,                      /* T_W  */
  YYSYMBOL_T_CAT1_TYPE_TYPE = 362,         /* T_CAT1_TYPE_TYPE  */
  YYSYMBOL_T_MOD_TEX = 363,                /* T_MOD_TEX  */
  YYSYMBOL_T_MOD_MEM = 364,                /* T_MOD_MEM  */
  YYSYMBOL_T_MOD_RT = 365,                 /* T_MOD_RT  */
  YYSYMBOL_T_MOD_XOR = 366,                /* T_MOD_XOR  */
  YYSYMBOL_T_MOD_UP = 367,                 /* T_MOD_UP  */
  YYSYMBOL_T_MOD_DOWN = 368,               /* T_MOD_DOWN  */
  YYSYMBOL_T_MOD_RUP = 369,                /* T_MOD_RUP  */
  YYSYMBOL_T_MOD_RDOWN = 370,              /* T_MOD_RDOWN  */
  YYSYMBOL_371_ = 371,                     /* '-'  */
  YYSYMBOL_372_ = 372,                     /* ','  */
  YYSYMBOL_373_ = 373,                     /* '('  */
  YYSYMBOL_374_ = 374,                     /* ')'  */
  YYSYMBOL_375_ = 375,                     /* '='  */
  YYSYMBOL_376_ = 376,                     /* ':'  */
  YYSYMBOL_377_ = 377,                     /* '!'  */
  YYSYMBOL_378_ = 378,                     /* '#'  */
  YYSYMBOL_379_ = 379,                     /* '.'  */
  YYSYMBOL_380_u_ = 380,                   /* 'u'  */
  YYSYMBOL_381_h_ = 381,                   /* 'h'  */
  YYSYMBOL_382_a_ = 382,                   /* 'a'  */
  YYSYMBOL_383_o_ = 383,                   /* 'o'  */
  YYSYMBOL_384_p_ = 384,                   /* 'p'  */
  YYSYMBOL_385_s_ = 385,                   /* 's'  */
  YYSYMBOL_386_v_ = 386,                   /* 'v'  */
  YYSYMBOL_387_ = 387,                     /* '+'  */
  YYSYMBOL_388_ = 388,                     /* '<'  */
  YYSYMBOL_389_g_ = 389,                   /* 'g'  */
  YYSYMBOL_390_ = 390,                     /* '['  */
  YYSYMBOL_391_ = 391,                     /* ']'  */
  YYSYMBOL_392_c_ = 392,                   /* 'c'  */
  YYSYMBOL_393_l_ = 393,                   /* 'l'  */
  YYSYMBOL_394_k_ = 394,                   /* 'k'  */
  YYSYMBOL_395_w_ = 395,                   /* 'w'  */
  YYSYMBOL_396_r_ = 396,                   /* 'r'  */
  YYSYMBOL_397_ = 397,                     /* '>'  */
  YYSYMBOL_YYACCEPT = 398,                 /* $accept  */
  YYSYMBOL_shader = 399,                   /* shader  */
  YYSYMBOL_400_1 = 400,                    /* $@1  */
  YYSYMBOL_headers = 401,                  /* headers  */
  YYSYMBOL_header = 402,                   /* header  */
  YYSYMBOL_const_val = 403,                /* const_val  */
  YYSYMBOL_localsize_header = 404,         /* localsize_header  */
  YYSYMBOL_const_header = 405,             /* const_header  */
  YYSYMBOL_buf_header_init_val = 406,      /* buf_header_init_val  */
  YYSYMBOL_buf_header_init_vals = 407,     /* buf_header_init_vals  */
  YYSYMBOL_buf_header_addr_reg = 408,      /* buf_header_addr_reg  */
  YYSYMBOL_buf_type = 409,                 /* buf_type  */
  YYSYMBOL_buf_header = 410,               /* buf_header  */
  YYSYMBOL_411_2 = 411,                    /* $@2  */
  YYSYMBOL_invocationid_header = 412,      /* invocationid_header  */
  YYSYMBOL_wgid_header = 413,              /* wgid_header  */
  YYSYMBOL_numwg_header = 414,             /* numwg_header  */
  YYSYMBOL_branchstack_header = 415,       /* branchstack_header  */
  YYSYMBOL_pvtmem_header = 416,            /* pvtmem_header  */
  YYSYMBOL_localmem_header = 417,          /* localmem_header  */
  YYSYMBOL_earlypreamble_header = 418,     /* earlypreamble_header  */
  YYSYMBOL_constlen_header = 419,          /* constlen_header  */
  YYSYMBOL_in_header = 420,                /* in_header  */
  YYSYMBOL_out_header = 421,               /* out_header  */
  YYSYMBOL_tex_header_opc = 422,           /* tex_header_opc  */
  YYSYMBOL_tex_header = 423,               /* tex_header  */
  YYSYMBOL_fullnop_start_section = 424,    /* fullnop_start_section  */
  YYSYMBOL_fullnop_end_section = 425,      /* fullnop_end_section  */
  YYSYMBOL_fullsync_start_section = 426,   /* fullsync_start_section  */
  YYSYMBOL_fullsync_end_section = 427,     /* fullsync_end_section  */
  YYSYMBOL_iflag = 428,                    /* iflag  */
  YYSYMBOL_iflags = 429,                   /* iflags  */
  YYSYMBOL_instrs = 430,                   /* instrs  */
  YYSYMBOL_instr = 431,                    /* instr  */
  YYSYMBOL_label = 432,                    /* label  */
  YYSYMBOL_cat0_src1 = 433,                /* cat0_src1  */
  YYSYMBOL_cat0_src2 = 434,                /* cat0_src2  */
  YYSYMBOL_cat0_immed = 435,               /* cat0_immed  */
  YYSYMBOL_cat0_instr = 436,               /* cat0_instr  */
  YYSYMBOL_437_3 = 437,                    /* $@3  */
  YYSYMBOL_438_4 = 438,                    /* $@4  */
  YYSYMBOL_439_5 = 439,                    /* $@5  */
  YYSYMBOL_440_6 = 440,                    /* $@6  */
  YYSYMBOL_441_7 = 441,                    /* $@7  */
  YYSYMBOL_442_8 = 442,                    /* $@8  */
  YYSYMBOL_443_9 = 443,                    /* $@9  */
  YYSYMBOL_444_10 = 444,                   /* $@10  */
  YYSYMBOL_445_11 = 445,                   /* $@11  */
  YYSYMBOL_446_12 = 446,                   /* $@12  */
  YYSYMBOL_447_13 = 447,                   /* $@13  */
  YYSYMBOL_448_14 = 448,                   /* $@14  */
  YYSYMBOL_449_15 = 449,                   /* $@15  */
  YYSYMBOL_450_16 = 450,                   /* $@16  */
  YYSYMBOL_cat1_opc = 451,                 /* cat1_opc  */
  YYSYMBOL_cat1_src = 452,                 /* cat1_src  */
  YYSYMBOL_cat1_movmsk = 453,              /* cat1_movmsk  */
  YYSYMBOL_454_17 = 454,                   /* $@17  */
  YYSYMBOL_mova_src = 455,                 /* mova_src  */
  YYSYMBOL_cat1_mova_flags = 456,          /* cat1_mova_flags  */
  YYSYMBOL_cat1_mova1 = 457,               /* cat1_mova1  */
  YYSYMBOL_458_18 = 458,                   /* $@18  */
  YYSYMBOL_cat1_mova = 459,                /* cat1_mova  */
  YYSYMBOL_460_19 = 460,                   /* $@19  */
  YYSYMBOL_cat1_mova_dst_flags = 461,      /* cat1_mova_dst_flags  */
  YYSYMBOL_cat1_mova_r = 462,              /* cat1_mova_r  */
  YYSYMBOL_463_20 = 463,                   /* $@20  */
  YYSYMBOL_cat1_swz = 464,                 /* cat1_swz  */
  YYSYMBOL_465_21 = 465,                   /* $@21  */
  YYSYMBOL_cat1_gat = 466,                 /* cat1_gat  */
  YYSYMBOL_467_22 = 467,                   /* $@22  */
  YYSYMBOL_cat1_sct = 468,                 /* cat1_sct  */
  YYSYMBOL_469_23 = 469,                   /* $@23  */
  YYSYMBOL_movs_invocation = 470,          /* movs_invocation  */
  YYSYMBOL_cat1_movs = 471,                /* cat1_movs  */
  YYSYMBOL_472_24 = 472,                   /* $@24  */
  YYSYMBOL_cat1_instr = 473,               /* cat1_instr  */
  YYSYMBOL_cat2_opc_1src = 474,            /* cat2_opc_1src  */
  YYSYMBOL_cat2_opc_2src_cnd = 475,        /* cat2_opc_2src_cnd  */
  YYSYMBOL_cat2_opc_2src = 476,            /* cat2_opc_2src  */
  YYSYMBOL_cond = 477,                     /* cond  */
  YYSYMBOL_cat2_instr = 478,               /* cat2_instr  */
  YYSYMBOL_cat3_dp_signedness = 479,       /* cat3_dp_signedness  */
  YYSYMBOL_cat3_dp_pack = 480,             /* cat3_dp_pack  */
  YYSYMBOL_cat3_opc = 481,                 /* cat3_opc  */
  YYSYMBOL_cat3_imm_reg_opc = 482,         /* cat3_imm_reg_opc  */
  YYSYMBOL_cat3_reg_or_const_or_rel_opc = 483, /* cat3_reg_or_const_or_rel_opc  */
  YYSYMBOL_cat3_wmm = 484,                 /* cat3_wmm  */
  YYSYMBOL_cat3_dp = 485,                  /* cat3_dp  */
  YYSYMBOL_src_reg_or_const_or_rel_or_flut = 486, /* src_reg_or_const_or_rel_or_flut  */
  YYSYMBOL_cat3_instr = 487,               /* cat3_instr  */
  YYSYMBOL_cat4_opc = 488,                 /* cat4_opc  */
  YYSYMBOL_cat4_instr = 489,               /* cat4_instr  */
  YYSYMBOL_cat5_opc_dsxypp = 490,          /* cat5_opc_dsxypp  */
  YYSYMBOL_cat5_opc_isam = 491,            /* cat5_opc_isam  */
  YYSYMBOL_cat5_opc = 492,                 /* cat5_opc  */
  YYSYMBOL_cat5_matchmode = 493,           /* cat5_matchmode  */
  YYSYMBOL_cat5_flag = 494,                /* cat5_flag  */
  YYSYMBOL_cat5_flags = 495,               /* cat5_flags  */
  YYSYMBOL_cat5_samp = 496,                /* cat5_samp  */
  YYSYMBOL_cat5_tex = 497,                 /* cat5_tex  */
  YYSYMBOL_cat5_type = 498,                /* cat5_type  */
  YYSYMBOL_cat5_a1 = 499,                  /* cat5_a1  */
  YYSYMBOL_cat5_samp_tex = 500,            /* cat5_samp_tex  */
  YYSYMBOL_cat5_samp_tex_all = 501,        /* cat5_samp_tex_all  */
  YYSYMBOL_cat5_instr = 502,               /* cat5_instr  */
  YYSYMBOL_cat6_typed = 503,               /* cat6_typed  */
  YYSYMBOL_cat6_dim = 504,                 /* cat6_dim  */
  YYSYMBOL_cat6_type = 505,                /* cat6_type  */
  YYSYMBOL_cat6_imm_offset = 506,          /* cat6_imm_offset  */
  YYSYMBOL_cat6_offset = 507,              /* cat6_offset  */
  YYSYMBOL_cat6_dst_offset = 508,          /* cat6_dst_offset  */
  YYSYMBOL_cat6_immed = 509,               /* cat6_immed  */
  YYSYMBOL_cat6_immed_or_gpr = 510,        /* cat6_immed_or_gpr  */
  YYSYMBOL_cat6_src_shift = 511,           /* cat6_src_shift  */
  YYSYMBOL_cat6_a6xx_global_address_pt2 = 512, /* cat6_a6xx_global_address_pt2  */
  YYSYMBOL_cat6_a6xx_global_address = 513, /* cat6_a6xx_global_address  */
  YYSYMBOL_cat6_load = 514,                /* cat6_load  */
  YYSYMBOL_515_25 = 515,                   /* $@25  */
  YYSYMBOL_516_26 = 516,                   /* $@26  */
  YYSYMBOL_517_27 = 517,                   /* $@27  */
  YYSYMBOL_518_28 = 518,                   /* $@28  */
  YYSYMBOL_519_29 = 519,                   /* $@29  */
  YYSYMBOL_520_30 = 520,                   /* $@30  */
  YYSYMBOL_521_31 = 521,                   /* $@31  */
  YYSYMBOL_522_32 = 522,                   /* $@32  */
  YYSYMBOL_cat6_store = 523,               /* cat6_store  */
  YYSYMBOL_524_33 = 524,                   /* $@33  */
  YYSYMBOL_525_34 = 525,                   /* $@34  */
  YYSYMBOL_526_35 = 526,                   /* $@35  */
  YYSYMBOL_527_36 = 527,                   /* $@36  */
  YYSYMBOL_528_37 = 528,                   /* $@37  */
  YYSYMBOL_cat6_loadib = 529,              /* cat6_loadib  */
  YYSYMBOL_530_38 = 530,                   /* $@38  */
  YYSYMBOL_cat6_storeib = 531,             /* cat6_storeib  */
  YYSYMBOL_532_39 = 532,                   /* $@39  */
  YYSYMBOL_cat6_prefetch = 533,            /* cat6_prefetch  */
  YYSYMBOL_534_40 = 534,                   /* $@40  */
  YYSYMBOL_cat6_atomic_opc = 535,          /* cat6_atomic_opc  */
  YYSYMBOL_cat6_a3xx_atomic_opc = 536,     /* cat6_a3xx_atomic_opc  */
  YYSYMBOL_cat6_a6xx_atomic_opc = 537,     /* cat6_a6xx_atomic_opc  */
  YYSYMBOL_cat6_a3xx_atomic_s = 538,       /* cat6_a3xx_atomic_s  */
  YYSYMBOL_cat6_a6xx_atomic_g = 539,       /* cat6_a6xx_atomic_g  */
  YYSYMBOL_cat6_atomic_l = 540,            /* cat6_atomic_l  */
  YYSYMBOL_cat6_atomic = 541,              /* cat6_atomic  */
  YYSYMBOL_cat6_ibo_opc_1src = 542,        /* cat6_ibo_opc_1src  */
  YYSYMBOL_cat6_ibo_opc_ldgb = 543,        /* cat6_ibo_opc_ldgb  */
  YYSYMBOL_cat6_ibo_opc_stgb = 544,        /* cat6_ibo_opc_stgb  */
  YYSYMBOL_cat6_ibo = 545,                 /* cat6_ibo  */
  YYSYMBOL_546_41 = 546,                   /* $@41  */
  YYSYMBOL_cat6_id_opc = 547,              /* cat6_id_opc  */
  YYSYMBOL_cat6_id = 548,                  /* cat6_id  */
  YYSYMBOL_cat6_bindless_base = 549,       /* cat6_bindless_base  */
  YYSYMBOL_cat6_bindless_mode = 550,       /* cat6_bindless_mode  */
  YYSYMBOL_cat6_reg_or_immed = 551,        /* cat6_reg_or_immed  */
  YYSYMBOL_cat6_bindless_ibo_opc_1src = 552, /* cat6_bindless_ibo_opc_1src  */
  YYSYMBOL_cat6_bindless_ibo_opc_2src = 553, /* cat6_bindless_ibo_opc_2src  */
  YYSYMBOL_cat6_bindless_ibo_opc_3src = 554, /* cat6_bindless_ibo_opc_3src  */
  YYSYMBOL_cat6_bindless_ibo_opc_3src_dst = 555, /* cat6_bindless_ibo_opc_3src_dst  */
  YYSYMBOL_cat6_rck = 556,                 /* cat6_rck  */
  YYSYMBOL_cat6_bindless_ibo = 557,        /* cat6_bindless_ibo  */
  YYSYMBOL_cat6_bindless_ldc_opc = 558,    /* cat6_bindless_ldc_opc  */
  YYSYMBOL_cat6_bindless_ldc_middle = 559, /* cat6_bindless_ldc_middle  */
  YYSYMBOL_cat6_bindless_ldc = 560,        /* cat6_bindless_ldc  */
  YYSYMBOL_const_dst = 561,                /* const_dst  */
  YYSYMBOL_cat6_stc = 562,                 /* cat6_stc  */
  YYSYMBOL_563_42 = 563,                   /* $@42  */
  YYSYMBOL_564_43 = 564,                   /* $@43  */
  YYSYMBOL_cat6_shfl_mode = 565,           /* cat6_shfl_mode  */
  YYSYMBOL_cat6_shfl = 566,                /* cat6_shfl  */
  YYSYMBOL_567_44 = 567,                   /* $@44  */
  YYSYMBOL_cat6_ray_intersection = 568,    /* cat6_ray_intersection  */
  YYSYMBOL_569_45 = 569,                   /* $@45  */
  YYSYMBOL_cat6_todo = 570,                /* cat6_todo  */
  YYSYMBOL_cat6_instr = 571,               /* cat6_instr  */
  YYSYMBOL_cat7_scope = 572,               /* cat7_scope  */
  YYSYMBOL_cat7_scopes = 573,              /* cat7_scopes  */
  YYSYMBOL_cat7_barrier = 574,             /* cat7_barrier  */
  YYSYMBOL_575_46 = 575,                   /* $@46  */
  YYSYMBOL_576_47 = 576,                   /* $@47  */
  YYSYMBOL_cat7_data_cache = 577,          /* cat7_data_cache  */
  YYSYMBOL_cat7_alias_dst = 578,           /* cat7_alias_dst  */
  YYSYMBOL_cat7_alias_src = 579,           /* cat7_alias_src  */
  YYSYMBOL_cat7_alias_scope = 580,         /* cat7_alias_scope  */
  YYSYMBOL_cat7_alias_int_type = 581,      /* cat7_alias_int_type  */
  YYSYMBOL_cat7_alias_float_type = 582,    /* cat7_alias_float_type  */
  YYSYMBOL_cat7_alias_type = 583,          /* cat7_alias_type  */
  YYSYMBOL_cat7_alias_table_size_minus_one = 584, /* cat7_alias_table_size_minus_one  */
  YYSYMBOL_cat7_instr = 585,               /* cat7_instr  */
  YYSYMBOL_586_48 = 586,                   /* $@48  */
  YYSYMBOL_raw_instr = 587,                /* raw_instr  */
  YYSYMBOL_meta_print_regs = 588,          /* meta_print_regs  */
  YYSYMBOL_meta_print_reg = 589,           /* meta_print_reg  */
  YYSYMBOL_meta_print_start = 590,         /* meta_print_start  */
  YYSYMBOL_meta_print = 591,               /* meta_print  */
  YYSYMBOL_src_gpr = 592,                  /* src_gpr  */
  YYSYMBOL_src_a0 = 593,                   /* src_a0  */
  YYSYMBOL_src_a1 = 594,                   /* src_a1  */
  YYSYMBOL_src_p0 = 595,                   /* src_p0  */
  YYSYMBOL_src = 596,                      /* src  */
  YYSYMBOL_dst = 597,                      /* dst  */
  YYSYMBOL_const = 598,                    /* const  */
  YYSYMBOL_dst_reg_flag = 599,             /* dst_reg_flag  */
  YYSYMBOL_dst_reg_flags = 600,            /* dst_reg_flags  */
  YYSYMBOL_dst_reg = 601,                  /* dst_reg  */
  YYSYMBOL_src_reg_flag = 602,             /* src_reg_flag  */
  YYSYMBOL_src_reg_flags = 603,            /* src_reg_flags  */
  YYSYMBOL_src_reg = 604,                  /* src_reg  */
  YYSYMBOL_src_reg_gpr = 605,              /* src_reg_gpr  */
  YYSYMBOL_src_const = 606,                /* src_const  */
  YYSYMBOL_src_reg_or_const = 607,         /* src_reg_or_const  */
  YYSYMBOL_src_reg_or_const_or_rel = 608,  /* src_reg_or_const_or_rel  */
  YYSYMBOL_src_reg_or_const_or_rel_or_imm = 609, /* src_reg_or_const_or_rel_or_imm  */
  YYSYMBOL_src_reg_or_rel_or_imm = 610,    /* src_reg_or_rel_or_imm  */
  YYSYMBOL_uoffset = 611,                  /* uoffset  */
  YYSYMBOL_offset = 612,                   /* offset  */
  YYSYMBOL_src_uoffset = 613,              /* src_uoffset  */
  YYSYMBOL_relative_gpr_src = 614,         /* relative_gpr_src  */
  YYSYMBOL_relative_gpr_dst = 615,         /* relative_gpr_dst  */
  YYSYMBOL_relative_const = 616,           /* relative_const  */
  YYSYMBOL_relative = 617,                 /* relative  */
  YYSYMBOL_immediate_cat1 = 618,           /* immediate_cat1  */
  YYSYMBOL_immediate = 619,                /* immediate  */
  YYSYMBOL_flut_immed = 620,               /* flut_immed  */
  YYSYMBOL_uinteger = 621,                 /* uinteger  */
  YYSYMBOL_integer = 622,                  /* integer  */
  YYSYMBOL_float = 623,                    /* float  */
  YYSYMBOL_type = 624                      /* type  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;


/* Second part of user prologue.  */
#line 52 "ir3_parser.y"

#if YYDEBUG
static void print_token(FILE *file, int type, YYSTYPE value)
{
	fprintf(file, "\ntype: %d\n", type);
}

#define YYPRINT(file, type, value) print_token(file, type, value)
#endif

#line 752 "ir3_parser.c"


#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int16 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if 1

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* 1 */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  3
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   1921

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  398
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  227
/* YYNRULES -- Number of rules.  */
#define YYNRULES  683
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  1412

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   625


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int16 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,   377,     2,   378,     2,     2,     2,     2,
     373,   374,     2,   387,   372,   371,   379,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,   376,     2,
     388,   375,   397,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,   390,     2,   391,     2,     2,     2,   382,     2,   392,
       2,     2,     2,   389,   381,     2,     2,   394,   393,     2,
       2,   383,   384,     2,   396,   385,     2,   380,   386,   395,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73,    74,
      75,    76,    77,    78,    79,    80,    81,    82,    83,    84,
      85,    86,    87,    88,    89,    90,    91,    92,    93,    94,
      95,    96,    97,    98,    99,   100,   101,   102,   103,   104,
     105,   106,   107,   108,   109,   110,   111,   112,   113,   114,
     115,   116,   117,   118,   119,   120,   121,   122,   123,   124,
     125,   126,   127,   128,   129,   130,   131,   132,   133,   134,
     135,   136,   137,   138,   139,   140,   141,   142,   143,   144,
     145,   146,   147,   148,   149,   150,   151,   152,   153,   154,
     155,   156,   157,   158,   159,   160,   161,   162,   163,   164,
     165,   166,   167,   168,   169,   170,   171,   172,   173,   174,
     175,   176,   177,   178,   179,   180,   181,   182,   183,   184,
     185,   186,   187,   188,   189,   190,   191,   192,   193,   194,
     195,   196,   197,   198,   199,   200,   201,   202,   203,   204,
     205,   206,   207,   208,   209,   210,   211,   212,   213,   214,
     215,   216,   217,   218,   219,   220,   221,   222,   223,   224,
     225,   226,   227,   228,   229,   230,   231,   232,   233,   234,
     235,   236,   237,   238,   239,   240,   241,   242,   243,   244,
     245,   246,   247,   248,   249,   250,   251,   252,   253,   254,
     255,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   267,   268,   269,   270,   271,   272,   273,   274,
     275,   276,   277,   278,   279,   280,   281,   282,   283,   284,
     285,   286,   287,   288,   289,   290,   291,   292,   293,   294,
     295,   296,   297,   298,   299,   300,   301,   302,   303,   304,
     305,   306,   307,   308,   309,   310,   311,   312,   313,   314,
     315,   316,   317,   318,   319,   320,   321,   322,   323,   324,
     325,   326,   327,   328,   329,   330,   331,   332,   333,   334,
     335,   336,   337,   338,   339,   340,   341,   342,   343,   344,
     345,   346,   347,   348,   349,   350,   351,   352,   353,   354,
     355,   356,   357,   358,   359,   360,   361,   362,   363,   364,
     365,   366,   367,   368,   369,   370
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,   494,   494,   494,   496,   497,   499,   500,   501,   502,
     503,   504,   505,   506,   507,   508,   509,   510,   511,   512,
     514,   515,   516,   517,   519,   525,   529,   530,   531,   532,
     535,   543,   545,   546,   548,   548,   555,   561,   568,   575,
     584,   586,   588,   590,   592,   595,   597,   600,   602,   610,
     611,   612,   613,   615,   616,   617,   618,   619,   620,   621,
     622,   623,   624,   626,   627,   629,   630,   632,   633,   634,
     635,   636,   637,   638,   639,   640,   641,   642,   643,   644,
     645,   646,   648,   650,   651,   653,   654,   656,   657,   659,
     660,   660,   661,   661,   662,   662,   663,   663,   664,   664,
     665,   665,   666,   666,   667,   667,   668,   668,   669,   670,
     670,   671,   672,   673,   674,   675,   676,   677,   677,   678,
     679,   680,   681,   682,   682,   683,   684,   684,   685,   686,
     687,   688,   689,   689,   691,   694,   698,   699,   701,   701,
     717,   718,   719,   721,   722,   724,   724,   731,   731,   738,
     739,   741,   741,   749,   749,   751,   751,   753,   753,   755,
     756,   758,   758,   761,   762,   763,   764,   765,   766,   767,
     768,   769,   770,   772,   773,   774,   775,   776,   777,   778,
     779,   780,   781,   782,   783,   784,   785,   787,   788,   789,
     790,   791,   792,   793,   794,   795,   796,   798,   799,   800,
     801,   802,   803,   804,   805,   806,   807,   808,   809,   810,
     811,   812,   813,   814,   815,   816,   817,   818,   819,   820,
     821,   822,   823,   824,   826,   827,   828,   829,   830,   831,
     833,   834,   835,   837,   838,   840,   841,   843,   844,   845,
     846,   847,   848,   849,   850,   852,   853,   854,   855,   856,
     858,   859,   860,   861,   862,   863,   864,   865,   866,   867,
     868,   869,   870,   872,   873,   875,   876,   878,   879,   880,
     882,   883,   884,   885,   886,   888,   889,   890,   891,   892,
     893,   894,   895,   896,   897,   899,   901,   902,   904,   906,
     907,   908,   909,   910,   911,   912,   913,   914,   915,   916,
     917,   918,   919,   920,   921,   922,   923,   924,   925,   926,
     927,   928,   929,   930,   931,   932,   933,   934,   935,   936,
     937,   938,   940,   941,   943,   944,   945,   946,   947,   948,
     949,   950,   951,   952,   953,   954,   955,   956,   957,   958,
     960,   961,   962,   963,   964,   966,   967,   968,   969,   971,
     972,   973,   974,   976,   977,   978,   979,   980,   981,   982,
     983,   984,   986,   987,   989,   990,   991,   992,   994,   995,
     996,   997,   998,   999,  1001,  1003,  1004,  1006,  1007,  1010,
    1015,  1020,  1026,  1038,  1040,  1040,  1041,  1041,  1042,  1042,
    1043,  1043,  1044,  1044,  1045,  1045,  1046,  1046,  1046,  1050,
    1050,  1051,  1051,  1052,  1052,  1053,  1053,  1054,  1054,  1056,
    1056,  1057,  1057,  1059,  1059,  1061,  1062,  1063,  1064,  1065,
    1066,  1067,  1068,  1069,  1070,  1071,  1073,  1074,  1075,  1076,
    1077,  1078,  1079,  1080,  1081,  1082,  1083,  1085,  1086,  1087,
    1088,  1089,  1090,  1091,  1092,  1093,  1094,  1095,  1097,  1099,
    1101,  1103,  1104,  1105,  1107,  1109,  1110,  1112,  1113,  1114,
    1114,  1117,  1118,  1119,  1121,  1123,  1124,  1126,  1127,  1128,
    1130,  1131,  1133,  1134,  1136,  1137,  1138,  1139,  1140,  1141,
    1142,  1143,  1144,  1145,  1146,  1148,  1150,  1152,  1153,  1155,
    1156,  1157,  1158,  1160,  1164,  1165,  1166,  1168,  1174,  1175,
    1176,  1179,  1179,  1180,  1180,  1182,  1183,  1184,  1185,  1186,
    1190,  1193,  1193,  1195,  1195,  1199,  1200,  1201,  1203,  1204,
    1205,  1206,  1207,  1208,  1209,  1210,  1211,  1212,  1213,  1214,
    1215,  1216,  1218,  1219,  1220,  1221,  1223,  1224,  1226,  1226,
    1227,  1227,  1229,  1230,  1231,  1233,  1234,  1236,  1237,  1239,
    1240,  1241,  1243,  1244,  1246,  1247,  1249,  1250,  1252,  1254,
    1255,  1256,  1257,  1258,  1259,  1260,  1261,  1261,  1265,  1267,
    1268,  1270,  1274,  1281,  1339,  1340,  1341,  1342,  1344,  1345,
    1346,  1347,  1349,  1350,  1351,  1352,  1353,  1355,  1357,  1358,
    1359,  1360,  1361,  1362,  1364,  1365,  1368,  1369,  1371,  1372,
    1373,  1374,  1375,  1377,  1378,  1380,  1381,  1383,  1384,  1386,
    1387,  1389,  1390,  1392,  1393,  1394,  1396,  1397,  1398,  1400,
    1401,  1402,  1404,  1405,  1407,  1408,  1410,  1412,  1413,  1415,
    1416,  1418,  1419,  1421,  1422,  1431,  1432,  1433,  1434,  1435,
    1436,  1437,  1438,  1439,  1440,  1441,  1442,  1443,  1444,  1445,
    1446,  1447,  1449,  1450,  1451,  1452,  1453,  1456,  1457,  1458,
    1459,  1460,  1461,  1462,  1463,  1464,  1465,  1466,  1467,  1469,
    1470,  1472,  1473,  1475,  1476,  1478,  1479,  1480,  1481,  1482,
    1483,  1484,  1485,  1486
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if 1
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "T_INT", "T_HEX",
  "T_FLOAT", "T_IDENTIFIER", "T_REGISTER", "T_CONSTANT", "T_RT",
  "T_A_LOCALSIZE", "T_A_CONST", "T_A_BUF", "T_A_UBO", "T_A_INVOCATIONID",
  "T_A_WGID", "T_A_NUMWG", "T_A_BRANCHSTACK", "T_A_IN", "T_A_OUT",
  "T_A_TEX", "T_A_PVTMEM", "T_A_LOCALMEM", "T_A_CONSTLEN",
  "T_A_EARLYPREAMBLE", "T_A_FULLNOPSTART", "T_A_FULLNOPEND",
  "T_A_FULLSYNCSTART", "T_A_FULLSYNCEND", "T_ABSNEG", "T_NEG", "T_ABS",
  "T_R", "T_LAST", "T_HR", "T_HC", "T_EVEN", "T_POS_INFINITY",
  "T_NEG_INFINITY", "T_EI", "T_WRMASK", "T_FLUT_0_0", "T_FLUT_0_5",
  "T_FLUT_1_0", "T_FLUT_2_0", "T_FLUT_E", "T_FLUT_PI", "T_FLUT_INV_PI",
  "T_FLUT_INV_LOG2_E", "T_FLUT_LOG2_E", "T_FLUT_INV_LOG2_10",
  "T_FLUT_LOG2_10", "T_FLUT_4_0", "T_SY", "T_SS", "T_JP", "T_EQ_FLAG",
  "T_SAT", "T_RPT", "T_UL", "T_NOP", "T_EOLM", "T_EOGM", "T_EOSTSC",
  "T_OP_NOP", "T_OP_BR", "T_OP_BRAO", "T_OP_BRAA", "T_OP_BRAC",
  "T_OP_BANY", "T_OP_BALL", "T_OP_BRAX", "T_OP_JUMP", "T_OP_CALL",
  "T_OP_RET", "T_OP_KILL", "T_OP_END", "T_OP_EMIT", "T_OP_CUT",
  "T_OP_CHMASK", "T_OP_CHSH", "T_OP_FLOW_REV", "T_OP_BKT", "T_OP_STKS",
  "T_OP_STKR", "T_OP_XSET", "T_OP_XCLR", "T_OP_GETLAST", "T_OP_GETONE",
  "T_OP_DBG", "T_OP_SHPS", "T_OP_SHPE", "T_OP_PREDT", "T_OP_PREDF",
  "T_OP_PREDE", "T_OP_MOVMSK", "T_OP_MOVA1", "T_OP_MOVA", "T_OP_MOVA_R",
  "T_OP_MOV", "T_OP_COV", "T_OP_SWZ", "T_OP_GAT", "T_OP_SCT", "T_OP_MOVS",
  "T_MUL2", "T_DIV2", "T_OP_ADD_F", "T_OP_MIN_F", "T_OP_MAX_F",
  "T_OP_MUL_F", "T_OP_SIGN_F", "T_OP_CMPS_F", "T_OP_ABSNEG_F",
  "T_OP_CMPV_F", "T_OP_FLOOR_F", "T_OP_CEIL_F", "T_OP_RNDNE_F",
  "T_OP_RNDAZ_F", "T_OP_TRUNC_F", "T_OP_ADD_U", "T_OP_ADD_S", "T_OP_SUB_U",
  "T_OP_SUB_S", "T_OP_CMPS_U", "T_OP_CMPS_S", "T_OP_MIN_U", "T_OP_MIN_S",
  "T_OP_MAX_U", "T_OP_MAX_S", "T_OP_ABSNEG_S", "T_OP_AND_B", "T_OP_OR_B",
  "T_OP_NOT_B", "T_OP_XOR_B", "T_OP_CMPV_U", "T_OP_CMPV_S", "T_OP_MUL_U24",
  "T_OP_MUL_S24", "T_OP_MULL_U", "T_OP_BFREV_B", "T_OP_CLZ_S",
  "T_OP_CLZ_B", "T_OP_SHL_B", "T_OP_SHR_B", "T_OP_ASHR_B", "T_OP_BARY_F",
  "T_OP_FLAT_B", "T_OP_MGEN_B", "T_OP_GETBIT_B", "T_OP_SETRM",
  "T_OP_CBITS_B", "T_OP_SHB", "T_OP_MSAD", "T_OP_MAD_U16",
  "T_OP_MADSH_U16", "T_OP_MAD_S16", "T_OP_MADSH_M16", "T_OP_MAD_U24",
  "T_OP_MAD_S24", "T_OP_MAD_F16", "T_OP_MAD_F32", "T_OP_SEL_B16",
  "T_OP_SEL_B32", "T_OP_SEL_S16", "T_OP_SEL_S32", "T_OP_SEL_F16",
  "T_OP_SEL_F32", "T_OP_SAD_S16", "T_OP_SAD_S32", "T_OP_SHRM", "T_OP_SHLM",
  "T_OP_SHRG", "T_OP_SHLG", "T_OP_ANDG", "T_OP_DP2ACC", "T_OP_DP4ACC",
  "T_OP_WMM", "T_OP_WMM_ACCU", "T_OP_RCP", "T_OP_RSQ", "T_OP_LOG2",
  "T_OP_EXP2", "T_OP_SIN", "T_OP_COS", "T_OP_SQRT", "T_OP_HRSQ",
  "T_OP_HLOG2", "T_OP_HEXP2", "T_OP_ISAM", "T_OP_ISAML", "T_OP_ISAMM",
  "T_OP_SAM", "T_OP_SAMB", "T_OP_SAML", "T_OP_SAMGQ", "T_OP_GETLOD",
  "T_OP_CONV", "T_OP_CONVM", "T_OP_GETSIZE", "T_OP_GETBUF", "T_OP_GETPOS",
  "T_OP_GETINFO", "T_OP_DSX", "T_OP_DSY", "T_OP_GATHER4R", "T_OP_GATHER4G",
  "T_OP_GATHER4B", "T_OP_GATHER4A", "T_OP_SAMGP0", "T_OP_SAMGP1",
  "T_OP_SAMGP2", "T_OP_SAMGP3", "T_OP_DSXPP_1", "T_OP_DSYPP_1",
  "T_OP_RGETPOS", "T_OP_RGETINFO", "T_OP_BRCST_A", "T_OP_QSHUFFLE_BRCST",
  "T_OP_QSHUFFLE_H", "T_OP_QSHUFFLE_V", "T_OP_QSHUFFLE_DIAG", "T_OP_TCINV",
  "T_OP_IMG_BINDLESS_HOF", "T_OP_IMG_BINDLESS_PCMN", "T_OP_IMG_BINDLESS",
  "T_OP_LDG", "T_OP_LDG_A", "T_OP_LDG_K", "T_OP_LDL", "T_OP_LDP",
  "T_OP_STG", "T_OP_STG_A", "T_OP_STL", "T_OP_STP", "T_OP_LDIB",
  "T_OP_G2L", "T_OP_L2G", "T_OP_PREFETCH", "T_OP_LDLW", "T_OP_STLW",
  "T_OP_RESFMT", "T_OP_RESINFO", "T_OP_RESBASE", "T_OP_ATOMIC_ADD",
  "T_OP_ATOMIC_SUB", "T_OP_ATOMIC_XCHG", "T_OP_ATOMIC_INC",
  "T_OP_ATOMIC_DEC", "T_OP_ATOMIC_CMPXCHG", "T_OP_ATOMIC_MIN",
  "T_OP_ATOMIC_MAX", "T_OP_ATOMIC_AND", "T_OP_ATOMIC_OR",
  "T_OP_ATOMIC_XOR", "T_OP_RESINFO_B", "T_OP_LDIB_B", "T_OP_STIB_B",
  "T_OP_ATOMIC_B_ADD", "T_OP_ATOMIC_B_SUB", "T_OP_ATOMIC_B_XCHG",
  "T_OP_ATOMIC_B_INC", "T_OP_ATOMIC_B_DEC", "T_OP_ATOMIC_B_CMPXCHG",
  "T_OP_ATOMIC_B_MIN", "T_OP_ATOMIC_B_MAX", "T_OP_ATOMIC_B_AND",
  "T_OP_ATOMIC_B_OR", "T_OP_ATOMIC_B_XOR", "T_OP_ATOMIC_S_ADD",
  "T_OP_ATOMIC_S_SUB", "T_OP_ATOMIC_S_XCHG", "T_OP_ATOMIC_S_INC",
  "T_OP_ATOMIC_S_DEC", "T_OP_ATOMIC_S_CMPXCHG", "T_OP_ATOMIC_S_MIN",
  "T_OP_ATOMIC_S_MAX", "T_OP_ATOMIC_S_AND", "T_OP_ATOMIC_S_OR",
  "T_OP_ATOMIC_S_XOR", "T_OP_ATOMIC_G_ADD", "T_OP_ATOMIC_G_SUB",
  "T_OP_ATOMIC_G_XCHG", "T_OP_ATOMIC_G_INC", "T_OP_ATOMIC_G_DEC",
  "T_OP_ATOMIC_G_CMPXCHG", "T_OP_ATOMIC_G_MIN", "T_OP_ATOMIC_G_MAX",
  "T_OP_ATOMIC_G_AND", "T_OP_ATOMIC_G_OR", "T_OP_ATOMIC_G_XOR",
  "T_OP_LDGB", "T_OP_STGB", "T_OP_STIB", "T_OP_LDC", "T_OP_LDLV",
  "T_OP_GETSPID", "T_OP_GETWID", "T_OP_GETFIBERID", "T_OP_STC",
  "T_OP_STSC", "T_OP_SHFL", "T_OP_RAY_INTERSECTION", "T_OP_BAR",
  "T_OP_FENCE", "T_OP_SLEEP", "T_OP_ICINV", "T_OP_DCCLN", "T_OP_DCINV",
  "T_OP_DCFLU", "T_OP_CCINV", "T_OP_LOCK", "T_OP_UNLOCK", "T_OP_ALIAS",
  "T_RAW", "T_OP_PRINT", "T_TYPE_F16", "T_TYPE_F32", "T_TYPE_U16",
  "T_TYPE_U32", "T_TYPE_S16", "T_TYPE_S32", "T_TYPE_U8", "T_TYPE_U8_32",
  "T_TYPE_U64", "T_TYPE_B16", "T_TYPE_B32", "T_UNTYPED", "T_TYPED",
  "T_MIXED", "T_UNSIGNED", "T_LOW", "T_HIGH", "T_1D", "T_2D", "T_3D",
  "T_4D", "T_SAD", "T_SSD", "T_LT", "T_LE", "T_GT", "T_GE", "T_EQ", "T_NE",
  "T_S2EN", "T_SAMP", "T_TEX", "T_BASE", "T_OFFSET", "T_UNIFORM",
  "T_NONUNIFORM", "T_IMM", "T_RCK", "T_CLP", "T_NAN", "T_INF", "T_A0",
  "T_A1", "T_P0", "T_UP0", "T_W", "T_CAT1_TYPE_TYPE", "T_MOD_TEX",
  "T_MOD_MEM", "T_MOD_RT", "T_MOD_XOR", "T_MOD_UP", "T_MOD_DOWN",
  "T_MOD_RUP", "T_MOD_RDOWN", "'-'", "','", "'('", "')'", "'='", "':'",
  "'!'", "'#'", "'.'", "'u'", "'h'", "'a'", "'o'", "'p'", "'s'", "'v'",
  "'+'", "'<'", "'g'", "'['", "']'", "'c'", "'l'", "'k'", "'w'", "'r'",
  "'>'", "$accept", "shader", "$@1", "headers", "header", "const_val",
  "localsize_header", "const_header", "buf_header_init_val",
  "buf_header_init_vals", "buf_header_addr_reg", "buf_type", "buf_header",
  "$@2", "invocationid_header", "wgid_header", "numwg_header",
  "branchstack_header", "pvtmem_header", "localmem_header",
  "earlypreamble_header", "constlen_header", "in_header", "out_header",
  "tex_header_opc", "tex_header", "fullnop_start_section",
  "fullnop_end_section", "fullsync_start_section", "fullsync_end_section",
  "iflag", "iflags", "instrs", "instr", "label", "cat0_src1", "cat0_src2",
  "cat0_immed", "cat0_instr", "$@3", "$@4", "$@5", "$@6", "$@7", "$@8",
  "$@9", "$@10", "$@11", "$@12", "$@13", "$@14", "$@15", "$@16",
  "cat1_opc", "cat1_src", "cat1_movmsk", "$@17", "mova_src",
  "cat1_mova_flags", "cat1_mova1", "$@18", "cat1_mova", "$@19",
  "cat1_mova_dst_flags", "cat1_mova_r", "$@20", "cat1_swz", "$@21",
  "cat1_gat", "$@22", "cat1_sct", "$@23", "movs_invocation", "cat1_movs",
  "$@24", "cat1_instr", "cat2_opc_1src", "cat2_opc_2src_cnd",
  "cat2_opc_2src", "cond", "cat2_instr", "cat3_dp_signedness",
  "cat3_dp_pack", "cat3_opc", "cat3_imm_reg_opc",
  "cat3_reg_or_const_or_rel_opc", "cat3_wmm", "cat3_dp",
  "src_reg_or_const_or_rel_or_flut", "cat3_instr", "cat4_opc",
  "cat4_instr", "cat5_opc_dsxypp", "cat5_opc_isam", "cat5_opc",
  "cat5_matchmode", "cat5_flag", "cat5_flags", "cat5_samp", "cat5_tex",
  "cat5_type", "cat5_a1", "cat5_samp_tex", "cat5_samp_tex_all",
  "cat5_instr", "cat6_typed", "cat6_dim", "cat6_type", "cat6_imm_offset",
  "cat6_offset", "cat6_dst_offset", "cat6_immed", "cat6_immed_or_gpr",
  "cat6_src_shift", "cat6_a6xx_global_address_pt2",
  "cat6_a6xx_global_address", "cat6_load", "$@25", "$@26", "$@27", "$@28",
  "$@29", "$@30", "$@31", "$@32", "cat6_store", "$@33", "$@34", "$@35",
  "$@36", "$@37", "cat6_loadib", "$@38", "cat6_storeib", "$@39",
  "cat6_prefetch", "$@40", "cat6_atomic_opc", "cat6_a3xx_atomic_opc",
  "cat6_a6xx_atomic_opc", "cat6_a3xx_atomic_s", "cat6_a6xx_atomic_g",
  "cat6_atomic_l", "cat6_atomic", "cat6_ibo_opc_1src", "cat6_ibo_opc_ldgb",
  "cat6_ibo_opc_stgb", "cat6_ibo", "$@41", "cat6_id_opc", "cat6_id",
  "cat6_bindless_base", "cat6_bindless_mode", "cat6_reg_or_immed",
  "cat6_bindless_ibo_opc_1src", "cat6_bindless_ibo_opc_2src",
  "cat6_bindless_ibo_opc_3src", "cat6_bindless_ibo_opc_3src_dst",
  "cat6_rck", "cat6_bindless_ibo", "cat6_bindless_ldc_opc",
  "cat6_bindless_ldc_middle", "cat6_bindless_ldc", "const_dst", "cat6_stc",
  "$@42", "$@43", "cat6_shfl_mode", "cat6_shfl", "$@44",
  "cat6_ray_intersection", "$@45", "cat6_todo", "cat6_instr", "cat7_scope",
  "cat7_scopes", "cat7_barrier", "$@46", "$@47", "cat7_data_cache",
  "cat7_alias_dst", "cat7_alias_src", "cat7_alias_scope",
  "cat7_alias_int_type", "cat7_alias_float_type", "cat7_alias_type",
  "cat7_alias_table_size_minus_one", "cat7_instr", "$@48", "raw_instr",
  "meta_print_regs", "meta_print_reg", "meta_print_start", "meta_print",
  "src_gpr", "src_a0", "src_a1", "src_p0", "src", "dst", "const",
  "dst_reg_flag", "dst_reg_flags", "dst_reg", "src_reg_flag",
  "src_reg_flags", "src_reg", "src_reg_gpr", "src_const",
  "src_reg_or_const", "src_reg_or_const_or_rel",
  "src_reg_or_const_or_rel_or_imm", "src_reg_or_rel_or_imm", "uoffset",
  "offset", "src_uoffset", "relative_gpr_src", "relative_gpr_dst",
  "relative_const", "relative", "immediate_cat1", "immediate",
  "flut_immed", "uinteger", "integer", "float", "type", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-1248)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-4)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
   -1248,    53,  1303, -1248,    69,  -330, -1248, -1248,  -289,  -286,
    -280,    69,  -277,  -260,  -252,    69,    69,    69, -1248,  1356,
    1303, -1248, -1248,    69, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,   123,
    -220,   148,   151,   128,   152, -1248,   184,   201,   223, -1248,
   -1248, -1248,  -140, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,   227, -1248,
   -1248, -1248, -1248,   699,  1608,  1249, -1248, -1248, -1248,  -115,
   -1248, -1248, -1248, -1248,    69,  -114,  -101,   -95,   -92,   -73,
     -57,   -55,   -49, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
     -50, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,   -46,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248,   -44,   -42,   -42,
     -42,   -36,   -35,   -34,   -27,   -26,    -7,    79, -1248, -1248,
      88, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
     117,   159, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248,    31, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248,   538,    -5,   538, -1248,   538,
     538,   538,   538,     0, -1248,   538, -1248,    13,    14,    13,
   -1248, -1248, -1248, -1248, -1248, -1248,    18,    18,    18, -1248,
   -1248, -1248, -1248,    20,    18,    18, -1248,    20, -1248,    18,
      18,    18,    18, -1248,    30, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248,   368, -1248,  -115,    38,    48,
      69, -1248, -1248, -1248, -1248,   409,   411,   415,  -258,  -258,
    -258,     6,  -258,  -258,    44,    44,    44,  -258,    44,    63,
      44,    44,    90,    59,    96,    98, -1248,   102,   104,   106,
     107,   110,   111, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248,    20,    20,    20,    20,    20,    20,    20,    20,    20,
      18,   115,    20,    20,    18,    20,    20,    20,   132,   538,
     136,   136,   137, -1248,    87, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248,    89, -1248,   516,   122,   145,
     149,   157,   308,   162,   163,   166,   167,   168,   -61,   141,
     175,  1141, -1248,    13,   538,  1087,   173,   173,   -56,   170,
     170,   170,   350,   170,   170,   170,   538,   170,   170,   170,
     170,    24, -1248, -1248,   543,    69,    69,   185,   186,   189,
     183, -1248,   205,   196,   197,   199, -1248, -1248,   274, -1248,
   -1248,   210,   216,    36, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248,   217,   218,   522, -1248, -1248, -1248,
   -1248, -1248, -1248,   538,   538,   206,   538,   538,   211,   225,
     226,   215,   170,   228,   538,   230,   170,   538,   224,   238,
      91,   259,  -142,   136, -1248, -1248,  -196,   280,   281, -1248,
   -1248,  1160,  1160,   827, -1248, -1248, -1248, -1248, -1248, -1248,
     538,   827,  1009,   979,   827,    78, -1248, -1248,   -43,   538,
     827, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248,   271,    13,   350,
     538,   538, -1248, -1248,   275,    20,    20,    20, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,   538,    20,
      20, -1248,    20,    20,    20,    20,   266,   277,   278,   282,
   -1248,   279, -1248,   288, -1248, -1248,    69,   655,   657,     6,
   -1248,    44,  -255,  -255, -1248,    44,    44,    44, -1248, -1248,
      44,   538, -1248, -1248, -1248,   318,   538,   538,   538,   538,
     306,   307,   290,   313,   315,   298,   299,   301,   303,    20,
     174,   323,   311,    20,   324,   312,   316, -1248, -1248, -1248,
   -1248, -1248,    20, -1248,   325, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248,   326,  -248,  -248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248,   309,   310, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248,    94,   177,   328,   331, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248,   679,    71, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,     6,   154,
    1104, -1248, -1248, -1248, -1248, -1248,   342,   351,   946,   357,
   -1248, -1248,   174, -1248,   358, -1248, -1248,   362, -1248,   363,
   -1248, -1248, -1248,   364, -1248,   730,   173,   370,   366,   376,
   -1248, -1248, -1248, -1248,   371,   372,   386,   377,   387,   389,
     390,   391,   392,   393,     6,   424,   380,    43, -1248,    69,
     403,   401,   412,   406, -1248, -1248,   425,   416,   417, -1248,
   -1248, -1248, -1248, -1248,  1160,  1160,   418,   419,   423,   426,
     428,   407,   413,    45,   408,   420,   174,   267,   174,   174,
     429,  -246,   410,   174,   431,   414,    45,    45,   122,   267,
    -146,     6,     6, -1248,   421,   422,   448,   454, -1248,   438,
     439,   171,   443,   447, -1248, -1248, -1248, -1248, -1248,   113,
     466,   467, -1248, -1248, -1248, -1248,   451,     6, -1248, -1248,
     827,   827, -1248,   267,   267,   267,   283,   979, -1248,   538,
   -1248,   730,    10,     6,     6,     6,   437,     6,     6,     6,
       6,     6,   474,   457,   458,   459,   461, -1248, -1248, -1248,
      69,     6,     6,   833, -1248,    44,    44, -1248,   899, -1248,
   -1248, -1248,  1160,   538,   283,   538,   283,   450,   452,   456,
     453, -1248,   460,   462,  -248,   455,    74,   464,  -240,  -240,
       6,    43, -1248,   472, -1248,   465,  -240,     6,   475,   473,
     489,   469,   491, -1248, -1248, -1248, -1248, -1248, -1248,   487,
   -1248, -1248, -1248, -1248,  -248,  -248, -1248, -1248, -1248, -1248,
   -1248,   510,   511,  -248,  -248, -1248,   512,   515, -1248,   518,
     519,   521,   527,   528,   529,   532, -1248, -1248,   533, -1248,
   -1248,   536,   509,   530,   531,   525,   538, -1248,   537,   540,
     541,   542,     6,  -106,     6,  -106,    43,   539,   549,   551,
     554, -1248, -1248, -1248,   545,   558,   559,   563,   566,   174,
     267,     6,   567,   174,   174,   553,   573,    23,    43,   555,
   -1248,   557,   538, -1248,   578,   174,   561,   523,     6,   581,
     584,   174,   585,   955,   562,   564, -1248, -1248,   565,   568,
   -1248,   827,  1009,   979,   827,  1128,   267,   730,    10,   613,
      10,   571,   577,   579,    43,   595,   582,  -106,  -106,  -106,
   -1248,   591,   593,   593,   593,   538,   596,   586, -1248,    69,
   -1248, -1248,     6,     6,   283,   283,   538,    86,  -246,   588,
   -1248,   587,  -246,  -246,   605,   174,   130, -1248,  -248, -1248,
     608,   609,   612,     6,  -246,   627,   610,   616,   283,  1128,
     631,   283, -1248,   150, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248,   643,   632,   646,   660, -1248, -1248,   661,
   -1248, -1248,   662,   538,   538,   538,   644,   647,   656,   538,
     283,   283,  -106,   689, -1248, -1248, -1248, -1248,  -106,   658,
   -1248,   673,   690,   691,   692,   693, -1248, -1248, -1248,   675,
     696,   683,   678,   685,   174,   705,   133,   694, -1248,   174,
     174,   695, -1248,   687,   174,  1128, -1248,   707,   708,    43,
     709, -1248,   711, -1248,   979, -1248,   713,  -209,   728,   139,
     139,   715,   716,   717, -1248,   701,    43,   720,   722,   724,
     538, -1248,   538,   739,  1092,     6,   283,   283,   538,   729,
    1128,   174,   731,   737,   738,  1128,   174,   694,   714,   740,
     741,   746,   732,   747,   748,   734,   749,     6,     6, -1248,
     283,  1202, -1248,    10, -1248, -1248, -1248, -1248, -1248,   754,
   -1248,   736,   744,   174,    43,   745,    43,    43,    43,   755,
   -1248,   751,   760, -1248, -1248,   758,   765,  1128, -1248,  -246,
    1128,  1128,  1128, -1248,   694,  -248,     6, -1248,  1128,  1128,
    1128,  1128,  1128,   768,  1128, -1248, -1248,   771, -1248, -1248,
   -1248, -1248,   728,   767,   769,   772,   770,   786, -1248,   788,
     632,    43, -1248,     6,   283,   283, -1248,   774, -1248, -1248,
   -1248,   792,   807, -1248, -1248, -1248,   791, -1248, -1248,   174,
   -1248,   283,    43,    43,   174,   790,   174,    43,   811,   632,
     815, -1248, -1248,   816,  -248,   808,   825,   835, -1248,   820,
     822, -1248,   174,   842, -1248,    43,   843,   836,   319,   844,
     828,   174,   174,   845,   849,   850,    43, -1248,    43,   851,
   -1248, -1248, -1248,   837,     6,   852,   855,   174,   174,   174,
     856, -1248,     6,   841,   862,   174,   174, -1248,   858, -1248,
     174,   866,     6, -1248, -1248, -1248,   174, -1248,  1233,   867,
     868,   872, -1248,   174,     6, -1248,   870,  1242,   875,  1059,
   -1248, -1248
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int16 yydefact[] =
{
       2,     0,     4,     1,     0,     0,    32,    33,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    43,    63,
       4,     6,     7,     0,     8,     9,    10,    11,    12,    16,
      17,    18,    19,    13,    14,    15,    21,    23,    20,     0,
       0,     0,     0,     0,     0,    40,     0,     0,     0,    41,
      42,    44,     0,    49,    50,    51,    52,    53,    54,    55,
      56,    57,    58,    59,    60,    61,    62,   568,     0,    78,
      79,    80,    81,    63,   250,    63,    66,    77,    75,     0,
      76,     5,    34,    22,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    82,   572,    64,    89,    90,    92,    94,
       0,    98,   100,   102,   104,   106,   108,   109,   111,   112,
     113,   114,   115,   116,   117,   119,   120,   121,   122,     0,
     123,   125,   126,   128,   129,   130,   131,     0,   143,   143,
     143,     0,     0,     0,     0,     0,     0,   197,   198,   199,
     200,   177,   187,   173,   190,   178,   179,   180,   181,   182,
     201,   202,   203,   204,   188,   189,   205,   206,   207,   208,
     174,   209,   210,   183,   211,   191,   192,   212,   213,   214,
     184,   176,   175,   215,   216,   217,   218,   219,   220,   221,
     185,   186,   222,   223,   251,   252,   253,   254,   255,   256,
     241,   242,   257,   258,   259,   260,   243,   244,   261,   262,
     245,   246,   247,   248,   249,   265,   266,   263,   264,   275,
     276,   277,   278,   279,   280,   281,   282,   283,   284,   288,
     289,   290,   291,   292,   293,   294,   295,   296,   297,   298,
     299,   300,   301,   302,   303,   304,   305,   306,   307,   308,
     309,   310,   311,   286,   287,   312,   313,   314,   315,   316,
     317,   318,   361,   320,   319,   321,   384,   386,   388,   392,
     390,   399,   401,   405,   403,   409,   515,   516,   413,   394,
     407,   517,   454,   473,   415,   416,   417,   418,   419,   420,
     421,   422,   423,   424,   425,   472,   486,   485,   474,   475,
     476,   477,   478,   479,   480,   481,   482,   483,   484,   426,
     427,   428,   429,   430,   431,   432,   433,   434,   435,   436,
     437,   438,   439,   440,   441,   442,   443,   444,   445,   446,
     447,   455,   456,   411,   493,   396,   461,   462,   463,   501,
     503,   511,   513,   538,   540,   561,   563,   542,   543,   544,
     562,   564,   565,   566,    67,     0,   163,   164,   165,   166,
     167,   168,   169,   172,    68,     0,     0,     0,    69,     0,
       0,     0,     0,     0,    70,     0,    71,   338,   338,   338,
      72,   518,   520,   519,   521,   522,     0,     0,     0,   452,
     453,   451,   523,     0,     0,     0,   524,     0,   525,     0,
       0,     0,     0,   527,     0,   526,   528,   529,   530,   531,
      73,   559,   560,    74,    65,     0,   573,   569,    31,     0,
       0,    36,    37,    38,    39,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,   151,     0,     0,     0,
       0,     0,     0,   194,   193,   196,   195,   237,   239,   238,
     240,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     536,   536,     0,   582,     0,   588,   589,   590,   592,   593,
     591,   583,   584,   585,   586,     0,   596,   594,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,   337,   338,     0,     0,   343,   343,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,   571,   570,     0,    29,     0,     0,     0,     0,
       0,    84,     0,     0,     0,     0,   669,   670,     0,   671,
      96,     0,     0,     0,   103,   105,   107,   110,   118,   132,
     124,   127,   138,   144,     0,     0,   149,   134,   135,   153,
     155,   157,   161,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,   536,   539,   541,     0,     0,     0,   595,
     597,     0,     0,     0,   224,   225,   226,   227,   228,   229,
       0,     0,     0,     0,     0,     0,   233,   234,     0,     0,
       0,   330,   324,   322,   323,   329,   333,   331,   332,   335,
     336,   334,   325,   326,   327,   328,   339,     0,   338,     0,
       0,     0,   362,   363,     0,     0,     0,     0,   675,   676,
     677,   678,   679,   680,   681,   682,   683,   368,     0,     0,
       0,   464,     0,     0,     0,     0,     0,     0,     0,     0,
     374,     0,    26,    27,    35,    24,     0,     0,     0,     0,
      83,     0,     0,     0,   672,     0,     0,     0,    88,    87,
       0,     0,   145,   147,   150,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,   505,   506,   507,
     508,   509,     0,   510,     0,   535,   534,   532,   533,   537,
     549,   550,   551,     0,   622,   622,   574,   587,   598,   599,
     600,   601,   602,     0,     0,   647,   648,   649,   650,   651,
     575,   576,   577,     0,     0,     0,     0,   170,   578,   579,
     580,   581,   605,   609,   603,     0,   611,   612,   613,   136,
     633,   634,   614,   137,   635,   171,   657,   658,   659,   660,
     661,   662,   663,   664,   665,   666,   667,   668,     0,     0,
       0,   616,   230,   618,   654,   652,     0,     0,     0,     0,
     267,   268,     0,   619,     0,   620,   621,     0,   607,     0,
     608,   235,   236,     0,   285,     0,   343,     0,     0,   357,
     364,   365,   366,   367,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    30,    29,
       0,     0,     0,     0,    91,    86,     0,     0,     0,    97,
      99,   101,   133,   139,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,   622,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,   624,     0,     0,     0,     0,   673,     0,
       0,     0,     0,     0,   640,   641,   642,   643,   644,     0,
       0,     0,   604,   606,   610,   615,     0,     0,   656,   617,
       0,     0,   269,     0,     0,     0,     0,     0,   353,     0,
     342,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,   487,     0,     0,     0,     0,   470,   471,    28,
       0,     0,     0,     0,    85,     0,     0,   146,     0,   140,
     141,   148,     0,     0,     0,     0,     0,     0,     0,   499,
       0,   498,     0,     0,   622,     0,     0,     0,   622,   622,
       0,     0,   370,     0,   369,     0,   622,     0,     0,     0,
       0,     0,     0,   554,   555,   552,   553,   556,   557,     0,
     625,   623,   630,   629,   622,   622,   645,   646,   674,   636,
     637,     0,     0,   622,   622,   653,     0,     0,   232,     0,
       0,     0,     0,     0,     0,     0,   340,   341,   347,   348,
     356,   345,     0,     0,     0,     0,     0,   459,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,    93,    95,   142,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     372,     0,     0,   371,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,   638,   639,     0,     0,
     655,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     488,     0,   465,   465,   465,     0,     0,     0,   497,     0,
      45,    46,     0,     0,     0,     0,     0,     0,   622,     0,
     500,     0,   622,   622,     0,     0,     0,   383,   622,   373,
       0,     0,     0,     0,   622,     0,     0,     0,     0,     0,
       0,     0,   558,     0,   628,   632,   631,   627,   231,   270,
     272,   271,   273,     0,   622,   347,   348,   349,   359,   345,
     346,   355,   345,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   468,   469,   467,   494,     0,     0,
      25,     0,     0,     0,     0,     0,   162,   160,   159,     0,
       0,     0,     0,     0,     0,     0,     0,   378,   382,     0,
       0,     0,   414,     0,     0,     0,   397,     0,     0,     0,
       0,   546,     0,   545,     0,   626,     0,     0,     0,     0,
       0,     0,     0,     0,   457,     0,     0,     0,     0,     0,
       0,   466,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,   378,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,   512,
       0,     0,   274,     0,   350,   344,   351,   352,   358,   345,
     354,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     495,     0,     0,   152,   154,     0,     0,     0,   387,   622,
       0,     0,     0,   402,   378,   622,     0,   381,     0,     0,
       0,     0,     0,     0,     0,   502,   504,     0,   567,   547,
     548,   360,     0,     0,     0,     0,     0,     0,   489,     0,
     622,     0,   496,     0,     0,     0,   385,     0,   393,   391,
     400,     0,     0,   377,   406,   404,     0,   395,   408,     0,
     398,     0,     0,     0,     0,     0,     0,     0,     0,   622,
       0,   156,   158,     0,   622,     0,     0,     0,   514,     0,
       0,   449,     0,     0,   490,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,   491,     0,     0,
     389,   376,   375,     0,     0,     0,     0,     0,     0,     0,
       0,   492,     0,     0,     0,     0,     0,   450,     0,   458,
       0,     0,     0,   380,   410,   412,     0,   460,     0,     0,
       0,     0,   379,     0,     0,   448,     0,     0,     0,     0,
      47,    48
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
   -1248, -1248, -1248,  1232, -1248,    -4, -1248, -1248, -1248,   430,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248,  1180, -1248,  1185, -1248,   164,   589,  -369, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248,   669, -1248, -1248,  -794,   198, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,   191, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248,  -241,   352,  -897,
    -476,  -904,   353,  -977, -1248,   243,   207,   941,   314,  -958,
    -914,  -704, -1248, -1201, -1248,   229, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248,  -762,  -925,    21, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248,  -526, -1248, -1248, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248,  -439, -1248, -1248, -1248, -1248,
   -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248, -1248,
     859, -1248, -1248, -1248,  -797,   160,  -968, -1248,  -262,  -474,
    -689, -1248,   784,  -339, -1248,  -459,  -568, -1248, -1248,  -828,
    -558,  -546,  -821, -1084,  -700, -1247,   668, -1248, -1248,  -602,
    -587,  -549,  -596,  -535,  -419,   394,   649
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
       0,     1,     2,    19,    20,   662,    21,    22,   663,   664,
     525,    23,    24,   408,    25,    26,    27,    28,    29,    30,
      31,    32,    33,    34,  1411,    35,    69,    70,    71,    72,
      73,    74,    75,    76,    77,   533,   837,   544,   344,   418,
     419,   420,   675,   422,   423,   424,   425,   426,   427,   428,
     430,   431,   680,   345,   747,   346,   681,   937,   434,   347,
     844,   348,   845,   685,   349,   556,   350,   686,   351,   687,
     352,   688,  1176,   353,   689,   354,   355,   356,   357,   600,
     358,   499,   609,   359,   360,   361,   362,   363,   789,   364,
     365,   366,   367,   368,   369,   502,   503,   504,  1145,  1146,
     630,  1257,  1147,  1148,   370,   509,   635,   513,   962,   963,
    1049,   658,  1370,  1239,  1117,   955,   371,   451,   452,   453,
     455,   454,   462,   465,  1246,   372,   456,   457,   459,   458,
     463,   373,   460,   374,   464,   375,   461,   376,   377,   378,
     379,   380,   381,   382,   383,   384,   385,   386,  1086,   387,
     388,  1164,  1095,   926,   389,   390,   391,   392,  1022,   393,
     394,   659,   395,   950,   396,   466,   467,   712,   397,   468,
     398,   469,   399,   400,   583,   584,   401,   470,   471,   402,
    1202,  1298,   723,   977,   978,   979,  1133,   403,   472,    78,
     406,   407,    79,    80,   748,   749,   750,   751,   752,   486,
     753,   487,   488,   489,   754,   792,   756,   799,   757,   758,
     781,   782,   794,   873,   964,  1206,   760,   490,   761,   762,
     940,   783,   784,   539,   785,   883,   647
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      40,   795,   540,   674,   763,   763,   791,    45,   908,   536,
     537,    49,    50,    51,   590,  1009,   491,   726,   493,    82,
     494,   495,   496,   497,   874,   875,   500,   536,   537,   957,
     726,   631,   585,   759,   759,   793,  1285,   798,   473,   536,
     537,   972,   678,    41,   790,  1051,   536,   537,   536,   537,
     726,   941,  1056,     3,   796,   787,   545,   546,   797,   548,
    1205,   550,   551,  1338,   804,   474,   894,   475,   476,   477,
     478,   479,    36,    37,    38,   999,  1000,  1001,   726,   727,
     409,   726,   727,  1321,    42,   726,  1003,    43,   480,   536,
     537,   894,  1356,    44,   536,   537,    46,   536,   537,   878,
    1097,   531,   660,  1151,   835,   733,   734,   728,   729,   730,
     731,   732,   733,    47,  1005,  1011,   536,   537,   878,   532,
     923,    48,   836,   871,   679,   871,    83,   506,   507,   473,
     581,   871,   755,   755,   780,    87,    88,   726,  1007,   872,
     726,   961,   780,   755,   719,   780,   726,  1048,  1034,   741,
    1179,   780,    84,   895,  1182,  1183,    85,   473,    86,  1201,
      89,   713,  1159,  1160,  1161,   627,  1193,   720,   721,   722,
     973,   974,   764,   764,   536,   537,   988,   651,   895,   975,
     976,   726,  1150,   898,   443,   444,   475,   476,   477,   478,
     479,    90,   902,   445,   446,   766,   767,   768,   769,   770,
     771,   772,   773,   774,   775,   776,   777,   480,    91,  1012,
    1013,  1014,   957,  1016,  1017,  1018,  1019,  1020,   884,   885,
     886,   887,   447,   448,   690,   691,  1205,   693,   694,   888,
      92,   899,  1258,  1260,    94,   701,    93,  1220,   704,  1255,
    1255,  1255,  1255,  1222,  1092,  1093,  1094,   715,  1143,   894,
     833,   716,  1140,   717,   718,  1205,  1052,   405,  1050,  1050,
     410,   786,   626,  1057,   449,   450,  1050,   894,   606,   607,
     803,   632,   633,   411,   726,   727,  1301,   536,   537,   412,
    1144,  1149,   413,  1152,  1064,  1065,   939,   939,   801,   802,
     726,   808,   809,  1068,  1069,   892,   728,   729,   730,   731,
     732,   414,   834,  1254,  1256,   795,   839,   840,   841,   817,
    1150,   842,   728,   729,   730,   731,   732,   415,  1091,   416,
    1096,  1317,   536,   537,   882,   417,   726,   435,   436,   421,
     909,  1165,  1166,   429,  1255,   432,   895,   433,  1002,   793,
     969,   970,   843,   437,   438,   439,   674,   847,   848,   849,
     850,  1033,   440,   441,   997,   998,  1006,  1007,   796,   896,
     766,   767,   768,   769,   770,   771,   772,   773,   774,   775,
     776,   777,   442,   656,   492,   522,  1036,   538,  1038,   498,
     740,   741,   742,  1252,   939,   938,   938,   806,   481,   482,
     483,   484,   501,   505,   971,   538,  1116,   508,   956,   512,
     740,   741,   742,   949,   657,   660,   527,   538,   928,   521,
     956,   524,  1259,  1259,   538,   528,   538,   529,  1188,  1192,
     526,   530,   543,  1299,   549,   764,   764,   485,   740,   741,
     742,   740,   741,   742,   951,   740,   741,   742,   861,   553,
      39,   780,   780,   740,   956,   956,   956,   951,   951,   879,
     880,   552,   980,   981,   554,   555,  1259,   707,   708,   709,
     710,   711,   538,   745,   557,   881,   558,   746,   559,   560,
     991,   795,   561,   562,   746,   587,   791,   588,   996,   481,
     482,   483,   484,   938,   881,  1006,  1007,   740,   741,   742,
     740,   741,   742,   893,   660,   660,   660,   741,   660,   660,
     660,   660,   660,  1186,   573,   793,  1236,   481,   482,   483,
     484,   580,  1028,  1029,   790,   582,   586,   591,   893,   764,
     608,   592,   665,   764,   796,  1138,  1142,   897,  1141,   593,
     893,   740,   741,   742,   601,   602,  1173,  1174,   603,   604,
     605,   660,   981,  1295,  1296,   473,   629,   610,   660,   634,
     889,   661,   475,   476,   477,   478,   479,   666,   669,   667,
    1197,  1371,   668,  1200,   670,   927,  1031,  1032,   671,   672,
    1004,   673,  1178,   480,   475,   476,   477,   478,   479,   684,
    1198,   956,   676,   534,   535,  1322,   541,   542,   677,   682,
     683,   547,  1218,  1219,   954,   480,   958,   959,   692,   698,
     695,   966,   795,   660,  1035,   660,  1037,   928,   810,   811,
     812,   813,   780,   755,   696,   780,   705,   956,   700,   697,
     510,   511,  1110,   702,   740,   741,   742,   514,   515,   981,
     706,   714,   517,   518,   519,   520,   793,   724,   725,  1127,
     740,   741,   742,   805,  1359,   824,  1245,   594,   595,   596,
     597,   598,   599,   828,   827,   796,   825,   826,  1274,  1275,
     829,   831,   830,   832,  1300,   928,   638,   639,   640,   641,
     642,   643,   644,   645,   646,   846,   893,  1085,   851,   852,
     853,  1278,  1297,  1171,  1172,   854,  1283,   855,   856,   857,
     538,   858,   778,   859,   893,   862,   865,   876,   877,  1053,
     779,   863,   866,   572,   660,   870,   867,   576,   728,   729,
     730,   731,   732,  1122,   900,   869,   890,   636,   637,   891,
     648,   649,   650,   901,   652,   653,   654,   655,  1316,   903,
     904,  1318,  1319,  1320,   905,   906,   907,   726,   911,  1324,
    1325,  1326,  1327,  1328,   910,  1330,  1341,  1342,   912,   916,
     913,   914,    57,    58,    59,    60,  1167,    61,    62,    63,
      64,    65,    66,  1348,   927,   915,   917,  1175,   918,   919,
     920,   921,   922,   924,   925,   930,   931,  1108,   933,   699,
     928,  1112,  1113,   703,   934,  1118,  1119,   932,   935,   936,
     942,   943,   956,  1124,  1203,   944,   947,   928,   945,  1130,
     946,   952,   948,   965,   953,   984,  1273,   968,   960,  1372,
     967,   985,   986,   987,  1211,  1212,  1213,   989,   982,   983,
    1217,   990,   927,   993,   994,   995,  1015,  1021,   660,   660,
     536,   537,   764,  1026,   726,   727,  1023,  1024,  1025,  1030,
    1039,  1061,  1040,  1041,  1042,   928,  1046,   928,   928,   928,
    1043,  1047,  1044,  1185,  1187,  1055,   728,   729,   730,   731,
     732,   733,   734,  1054,  1059,  1058,  1063,  1323,   766,   767,
     768,   769,   770,   771,   772,   773,   774,   775,   776,   777,
    1060,  1269,  1062,  1270,  1066,  1067,  1070,  1071,  1081,  1276,
    1072,  1073,   928,  1074,  1340,   481,   482,   483,   484,  1075,
    1076,  1077,   536,   537,  1078,  1079,   726,   727,  1080,  1082,
    1083,  1099,  1126,   928,   928,  1084,  1087,  1103,   928,  1088,
    1089,  1090,  1234,  1100,  1237,  1101,  1027,  1240,  1241,  1102,
    1104,  1105,  1244,   733,   734,  1106,   928,   927,  1107,  1111,
     735,   736,   737,   738,  1114,  1115,  1120,   928,  1121,   928,
    1123,   739,  1125,  1128,   927,  1384,  1129,  1131,  1132,  1134,
    1007,  1135,  1136,  1391,  1153,  1137,  1154,  1157,  1155,  1279,
    1162,  1158,  1163,  1399,  1284,  1168,  1181,  1184,  1169,  1180,
    1189,  1190,   536,   537,  1191,  1406,   726,   766,   767,   768,
     769,   770,   771,   772,   773,   774,   775,   776,   777,  1194,
    1195,  1305,   927,  1199,   927,   927,   927,  1196,   728,   729,
     730,   731,   732,   733,   734,  1204,   726,   727,  1207,   872,
     766,   767,   768,   769,   770,   771,   772,   773,   774,   775,
     776,   777,  1208,  1209,  1210,  1214,  1215,  1221,   728,   729,
     730,   731,   732,   733,   734,  1224,  1216,  1098,  1223,   927,
     766,   767,   768,   769,   770,   771,   772,   773,   774,   775,
     776,   777,  1225,  1226,  1227,  1228,  1229,  1347,  1230,  1232,
     927,   927,  1351,  1231,  1353,   927,  1233,  1235,  1243,  1247,
    1248,  1250,  1238,  1251,  1242,  1253,   741,  1261,  1262,  1263,
    1365,  1264,  1266,   927,  1267,  1170,  1268,  1271,  1272,  1375,
    1376,  1277,  1286,  1280,   927,  1156,   927,   536,   537,  1281,
    1282,   726,   727,  1288,  1287,  1387,  1388,  1389,  1289,  1291,
    1292,  1294,  1290,  1394,  1395,  1293,  1302,  1311,  1397,  1303,
    1314,   536,   537,  1304,  1400,  1313,  1307,  1315,   733,   734,
    1329,  1405,  1312,  1331,  1334,   766,   767,   768,   769,   770,
     771,   772,   773,   774,   775,   776,   777,  1332,  1336,  1333,
    1337,  1335,  1352,   536,   537,  1343,  1344,   726,   727,   766,
     767,   768,   769,   770,   771,   772,   773,   774,   775,   776,
     777,  1345,  1346,  1355,   740,   741,   742,  1357,  1358,   728,
     729,   730,   731,   732,   733,   734,  1360,  1361,   538,  1369,
     778,   735,   736,   737,   738,   536,   537,  1362,   779,   726,
     727,  1363,   739,  1364,  1366,  1368,  1374,  1377,  1373,   745,
    1249,  1378,  1379,   746,  1385,  1383,  1382,  1386,  1390,  1392,
    1396,   728,   729,   730,   731,   732,  1393,  1265,  1398,  1401,
    1403,  1402,  1407,   735,   736,   737,   738,  1404,  1408,    -3,
    1409,  1410,    81,    95,   739,    52,   740,   741,   742,   929,
     404,   765,   838,  1139,  1008,  1010,   523,  1177,  1045,  1109,
     538,   589,   743,   800,    53,    54,    55,    56,   807,     0,
     744,     0,     0,   992,     0,  1306,     0,  1308,  1309,  1310,
       0,   745,     0,     0,     0,   746,     0,     0,     0,     0,
       0,     0,    57,    58,    59,    60,     0,    61,    62,    63,
      64,    65,    66,     4,     5,     6,     7,     8,     9,    10,
      11,    12,    13,    14,    15,    16,    17,    18,   516,     0,
       0,     0,  1339,     0,     0,     0,   740,   741,   742,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     538,     0,   778,  1349,  1350,     0,     0,     0,  1354,     0,
     779,     0,    52,     0,     0,     0,   740,   741,   742,     0,
       0,   745,     0,     0,     0,   746,  1367,     0,     0,     0,
       0,    53,    54,    55,    56,     0,     0,  1380,     0,  1381,
     788,     0,   563,   564,   565,   566,   567,   568,   569,   570,
     571,   745,     0,   574,   575,   746,   577,   578,   579,    57,
      58,    59,    60,     0,    61,    62,    63,    64,    65,    66,
     611,     0,   612,     0,   613,   614,     0,     0,     0,     0,
       0,     0,   615,     0,     0,   616,     0,   617,   618,     0,
     619,   620,     0,     0,     0,     0,     0,     0,   621,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,   740,   741,   742,     0,     0,     0,     0,     0,   622,
     623,   624,   625,   628,   611,   538,   612,   778,   613,   614,
       0,     0,     0,     0,     0,   779,   615,     0,     0,   616,
       0,   617,   618,     0,   619,   620,   745,     0,     0,   538,
     746,   778,   621,     0,     0,     0,     0,     0,     0,   779,
       0,     0,     0,     0,     0,     0,     0,   740,   741,   742,
       0,     0,     0,   622,   623,   624,   625,     0,     0,     0,
       0,   538,     0,   743,     0,     0,     0,     0,     0,     0,
       0,   744,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,   745,     0,     0,     0,   746,     0,     0,   740,
     741,   742,     0,    67,    68,     0,     0,     0,     0,     0,
       0,     0,     0,   538,     0,   743,   814,   815,   816,     0,
       0,     0,     0,   744,     0,     0,     0,     0,     0,     0,
     818,   819,     0,   820,   821,   822,   823,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     860,     0,     0,     0,   864,     0,     0,     0,     0,     0,
       0,     0,     0,   868,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      67,    68,    96,    97,    98,    99,   100,   101,   102,   103,
     104,   105,   106,   107,   108,   109,   110,   111,   112,   113,
     114,   115,   116,   117,   118,   119,   120,   121,   122,   123,
     124,   125,   126,   127,   128,   129,   130,   131,   132,   133,
     134,   135,   136,     0,     0,   137,   138,   139,   140,   141,
     142,   143,   144,   145,   146,   147,   148,   149,   150,   151,
     152,   153,   154,   155,   156,   157,   158,   159,   160,   161,
     162,   163,   164,   165,   166,   167,   168,   169,   170,   171,
     172,   173,   174,   175,   176,   177,   178,   179,   180,   181,
     182,   183,   184,   185,   186,   187,   188,   189,   190,   191,
     192,   193,   194,   195,   196,   197,   198,   199,   200,   201,
     202,   203,   204,   205,   206,   207,   208,   209,   210,   211,
     212,   213,   214,   215,   216,   217,   218,   219,   220,   221,
     222,   223,   224,   225,   226,   227,   228,   229,   230,   231,
     232,   233,   234,   235,   236,   237,   238,   239,   240,   241,
     242,   243,   244,   245,   246,   247,   248,   249,   250,   251,
     252,   253,   254,   255,   256,   257,   258,   259,   260,   261,
     262,   263,   264,   265,   266,   267,   268,   269,   270,   271,
     272,   273,   274,   275,   276,   277,   278,   279,   280,   281,
     282,   283,   284,   285,   286,   287,   288,   289,   290,   291,
     292,   293,   294,   295,   296,   297,   298,   299,   300,   301,
     302,   303,   304,   305,   306,   307,   308,   309,   310,   311,
     312,   313,   314,   315,   316,   317,   318,   319,   320,   321,
     322,   323,   324,   325,   326,   327,   328,   329,   330,   331,
     332,   333,   334,   335,   336,   337,   338,   339,   340,   341,
     342,   343
};

static const yytype_int16 yycheck[] =
{
       4,   603,   421,   538,   591,   592,   602,    11,   805,     3,
       4,    15,    16,    17,   488,   912,   355,     7,   357,    23,
     359,   360,   361,   362,   724,   725,   365,     3,     4,   857,
       7,   507,   471,   591,   592,   603,  1237,   605,     7,     3,
       4,   869,     6,   373,   602,   959,     3,     4,     3,     4,
       7,   845,   966,     0,   603,   601,   425,   426,   604,   428,
    1144,   430,   431,  1310,   610,    34,   755,    36,    37,    38,
      39,    40,     3,     4,     5,   903,   904,   905,     7,     8,
      84,     7,     8,  1284,   373,     7,   907,   373,    57,     3,
       4,   780,  1339,   373,     3,     4,   373,     3,     4,     5,
    1025,   359,   521,  1080,   359,    34,    35,    29,    30,    31,
      32,    33,    34,   373,   911,   912,     3,     4,     5,   377,
     824,   373,   377,   371,   543,   371,     3,   368,   369,     7,
     469,   371,   591,   592,   593,     7,     8,     7,   347,   387,
       7,   387,   601,   602,   583,   604,     7,   387,   942,   358,
    1108,   610,   372,   755,  1112,  1113,     8,     7,     7,     9,
       8,   580,  1087,  1088,  1089,   504,  1124,   363,   364,   365,
     316,   317,   591,   592,     3,     4,     5,   516,   780,   325,
     326,     7,  1079,   779,   105,   106,    36,    37,    38,    39,
      40,     7,   788,   105,   106,    41,    42,    43,    44,    45,
      46,    47,    48,    49,    50,    51,    52,    57,     7,   913,
     914,   915,  1040,   917,   918,   919,   920,   921,    41,    42,
      43,    44,   105,   106,   563,   564,  1310,   566,   567,    52,
       7,   780,  1209,  1210,     7,   574,   376,  1162,   577,  1207,
    1208,  1209,  1210,  1168,   350,   351,   352,   389,  1076,   938,
     669,   393,  1073,   395,   396,  1339,   960,   372,   958,   959,
     374,   600,   503,   967,   105,   106,   966,   956,   329,   330,
     609,   327,   328,   374,     7,     8,  1253,     3,     4,   374,
    1077,  1078,   374,  1080,   984,   985,   844,   845,   331,   332,
       7,   630,   631,   993,   994,   754,    29,    30,    31,    32,
      33,   374,   671,  1207,  1208,   907,   675,   676,   677,   648,
    1207,   680,    29,    30,    31,    32,    33,   374,  1022,   374,
    1024,  1279,     3,     4,   743,   374,     7,   129,   130,   379,
     806,  1093,  1094,   379,  1302,   379,   938,   379,   906,   907,
     866,   867,   681,   379,   379,   379,   881,   686,   687,   688,
     689,   938,   379,   379,   900,   901,   346,   347,   907,   778,
      41,    42,    43,    44,    45,    46,    47,    48,    49,    50,
      51,    52,   379,   349,   379,     7,   944,   371,   946,   379,
     357,   358,   359,  1204,   942,   844,   845,   628,   357,   358,
     359,   360,   379,   379,   868,   371,   373,   379,   857,   379,
     357,   358,   359,   358,   380,   824,   410,   371,   827,   379,
     869,   373,  1209,  1210,   371,     6,   371,     6,  1118,  1123,
     372,     6,   378,  1251,   361,   844,   845,   396,   357,   358,
     359,   357,   358,   359,   853,   357,   358,   359,   700,   380,
     371,   900,   901,   357,   903,   904,   905,   866,   867,   355,
     356,   361,   871,   872,   358,   357,  1253,   366,   367,   368,
     369,   370,   371,   392,   362,   371,   362,   396,   362,   362,
     889,  1073,   362,   362,   396,   388,  1072,   388,   897,   357,
     358,   359,   360,   942,   371,   346,   347,   357,   358,   359,
     357,   358,   359,   755,   913,   914,   915,   358,   917,   918,
     919,   920,   921,   373,   389,  1073,   373,   357,   358,   359,
     360,   379,   931,   932,  1072,   379,   379,   372,   780,   938,
     379,   372,   526,   942,  1073,  1071,  1075,   373,  1074,   372,
     792,   357,   358,   359,   372,   372,  1104,  1105,   372,   372,
     372,   960,   961,  1247,  1248,     7,   373,   372,   967,   379,
     373,     8,    36,    37,    38,    39,    40,   372,   375,   373,
    1128,  1358,   373,  1131,   359,   827,   935,   936,   372,   372,
     909,   372,  1107,    57,    36,    37,    38,    39,    40,    57,
    1129,  1040,   372,   419,   420,  1285,   422,   423,   372,   372,
     372,   427,  1160,  1161,   856,    57,   858,   859,   392,   384,
     389,   863,  1204,  1022,   943,  1024,   945,  1026,   333,   334,
     335,   336,  1071,  1072,   389,  1074,   392,  1076,   390,   393,
     377,   378,  1041,   393,   357,   358,   359,   384,   385,  1048,
     392,   372,   389,   390,   391,   392,  1204,   357,   357,  1058,
     357,   358,   359,   372,  1344,   379,  1195,   339,   340,   341,
     342,   343,   344,   374,   372,  1204,   379,   379,  1226,  1227,
     372,     6,   666,     6,  1251,  1084,   316,   317,   318,   319,
     320,   321,   322,   323,   324,   357,   938,  1016,   372,   372,
     390,  1230,  1250,  1102,  1103,   372,  1235,   372,   390,   390,
     371,   390,   373,   390,   956,   372,   372,   388,   388,   961,
     381,   390,   390,   460,  1123,   379,   390,   464,    29,    30,
      31,    32,    33,  1052,   372,   390,   388,   510,   511,   388,
     513,   514,   515,   372,   517,   518,   519,   520,  1277,   372,
     372,  1280,  1281,  1282,   372,   372,   372,     7,   372,  1288,
    1289,  1290,  1291,  1292,   374,  1294,  1314,  1315,   372,   372,
     379,   379,    53,    54,    55,    56,  1095,    58,    59,    60,
      61,    62,    63,  1331,  1026,   379,   379,  1106,   379,   379,
     379,   379,   379,   349,   394,   372,   375,  1039,   372,   572,
    1199,  1043,  1044,   576,   359,  1047,  1048,   375,   372,   372,
     372,   372,  1251,  1055,  1133,   372,   389,  1216,   372,  1061,
     372,   393,   389,   393,   384,   357,  1225,   393,   379,  1358,
     379,   357,   374,   374,  1153,  1154,  1155,   374,   397,   397,
    1159,   374,  1084,   357,   357,   374,   389,   353,  1247,  1248,
       3,     4,  1251,   372,     7,     8,   379,   379,   379,     6,
     390,   372,   390,   387,   391,  1264,   391,  1266,  1267,  1268,
     390,   387,   390,  1115,  1116,   390,    29,    30,    31,    32,
      33,    34,    35,   391,   391,   390,   379,  1286,    41,    42,
      43,    44,    45,    46,    47,    48,    49,    50,    51,    52,
     391,  1220,   391,  1222,   374,   374,   374,   372,   379,  1228,
     372,   372,  1311,   372,  1313,   357,   358,   359,   360,   372,
     372,   372,     3,     4,   372,   372,     7,     8,   372,   379,
     379,   372,   389,  1332,  1333,   390,   379,   372,  1337,   379,
     379,   379,  1184,   374,  1186,   374,   930,  1189,  1190,   375,
     372,   372,  1194,    34,    35,   372,  1355,  1199,   372,   372,
      41,    42,    43,    44,   391,   372,   391,  1366,   391,  1368,
     372,    52,   391,   372,  1216,  1374,   372,   372,     3,   397,
     347,   397,   397,  1382,   393,   397,   389,   372,   389,  1231,
     379,   389,   379,  1392,  1236,   379,   389,   372,   392,   391,
     372,   372,     3,     4,   372,  1404,     7,    41,    42,    43,
      44,    45,    46,    47,    48,    49,    50,    51,    52,   372,
     390,  1263,  1264,   372,  1266,  1267,  1268,   391,    29,    30,
      31,    32,    33,    34,    35,   372,     7,     8,   372,   387,
      41,    42,    43,    44,    45,    46,    47,    48,    49,    50,
      51,    52,   372,   372,   372,   391,   389,   348,    29,    30,
      31,    32,    33,    34,    35,   372,   390,  1026,   390,  1311,
      41,    42,    43,    44,    45,    46,    47,    48,    49,    50,
      51,    52,   372,   372,   372,   372,   391,  1329,   372,   391,
    1332,  1333,  1334,   390,  1336,  1337,   391,   372,   391,   372,
     372,   372,   388,   372,   389,   372,   358,   372,   372,   372,
    1352,   390,   372,  1355,   372,  1099,   372,   358,     6,  1361,
    1362,   372,   388,   372,  1366,  1084,  1368,     3,     4,   372,
     372,     7,     8,   372,   374,  1377,  1378,  1379,   372,   372,
     372,   372,   390,  1385,  1386,   391,   372,   372,  1390,   393,
     372,     3,     4,   389,  1396,   375,   391,   372,    34,    35,
     372,  1403,   391,   372,   372,    41,    42,    43,    44,    45,
      46,    47,    48,    49,    50,    51,    52,   390,   372,   390,
     372,   391,   372,     3,     4,   391,   374,     7,     8,    41,
      42,    43,    44,    45,    46,    47,    48,    49,    50,    51,
      52,   374,   391,   372,   357,   358,   359,   372,   372,    29,
      30,    31,    32,    33,    34,    35,   388,   372,   371,   363,
     373,    41,    42,    43,    44,     3,     4,   372,   381,     7,
       8,   391,    52,   391,   372,   372,   388,   372,   374,   392,
    1199,   372,   372,   396,   372,   388,   375,   372,   372,   388,
     372,    29,    30,    31,    32,    33,   374,  1216,   372,     6,
     372,   374,   372,    41,    42,    43,    44,   375,     6,     0,
     375,   192,    20,    73,    52,     6,   357,   358,   359,   829,
      75,   592,   673,  1072,   912,   912,   407,  1107,   954,  1040,
     371,   487,   373,   605,    25,    26,    27,    28,   629,    -1,
     381,    -1,    -1,   889,    -1,  1264,    -1,  1266,  1267,  1268,
      -1,   392,    -1,    -1,    -1,   396,    -1,    -1,    -1,    -1,
      -1,    -1,    53,    54,    55,    56,    -1,    58,    59,    60,
      61,    62,    63,    10,    11,    12,    13,    14,    15,    16,
      17,    18,    19,    20,    21,    22,    23,    24,   387,    -1,
      -1,    -1,  1311,    -1,    -1,    -1,   357,   358,   359,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
     371,    -1,   373,  1332,  1333,    -1,    -1,    -1,  1337,    -1,
     381,    -1,     6,    -1,    -1,    -1,   357,   358,   359,    -1,
      -1,   392,    -1,    -1,    -1,   396,  1355,    -1,    -1,    -1,
      -1,    25,    26,    27,    28,    -1,    -1,  1366,    -1,  1368,
     381,    -1,   451,   452,   453,   454,   455,   456,   457,   458,
     459,   392,    -1,   462,   463,   396,   465,   466,   467,    53,
      54,    55,    56,    -1,    58,    59,    60,    61,    62,    63,
     333,    -1,   335,    -1,   337,   338,    -1,    -1,    -1,    -1,
      -1,    -1,   345,    -1,    -1,   348,    -1,   350,   351,    -1,
     353,   354,    -1,    -1,    -1,    -1,    -1,    -1,   361,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,   357,   358,   359,    -1,    -1,    -1,    -1,    -1,   382,
     383,   384,   385,   386,   333,   371,   335,   373,   337,   338,
      -1,    -1,    -1,    -1,    -1,   381,   345,    -1,    -1,   348,
      -1,   350,   351,    -1,   353,   354,   392,    -1,    -1,   371,
     396,   373,   361,    -1,    -1,    -1,    -1,    -1,    -1,   381,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,   357,   358,   359,
      -1,    -1,    -1,   382,   383,   384,   385,    -1,    -1,    -1,
      -1,   371,    -1,   373,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,   381,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   392,    -1,    -1,    -1,   396,    -1,    -1,   357,
     358,   359,    -1,   314,   315,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   371,    -1,   373,   635,   636,   637,    -1,
      -1,    -1,    -1,   381,    -1,    -1,    -1,    -1,    -1,    -1,
     649,   650,    -1,   652,   653,   654,   655,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
     699,    -1,    -1,    -1,   703,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   712,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
     314,   315,    64,    65,    66,    67,    68,    69,    70,    71,
      72,    73,    74,    75,    76,    77,    78,    79,    80,    81,
      82,    83,    84,    85,    86,    87,    88,    89,    90,    91,
      92,    93,    94,    95,    96,    97,    98,    99,   100,   101,
     102,   103,   104,    -1,    -1,   107,   108,   109,   110,   111,
     112,   113,   114,   115,   116,   117,   118,   119,   120,   121,
     122,   123,   124,   125,   126,   127,   128,   129,   130,   131,
     132,   133,   134,   135,   136,   137,   138,   139,   140,   141,
     142,   143,   144,   145,   146,   147,   148,   149,   150,   151,
     152,   153,   154,   155,   156,   157,   158,   159,   160,   161,
     162,   163,   164,   165,   166,   167,   168,   169,   170,   171,
     172,   173,   174,   175,   176,   177,   178,   179,   180,   181,
     182,   183,   184,   185,   186,   187,   188,   189,   190,   191,
     192,   193,   194,   195,   196,   197,   198,   199,   200,   201,
     202,   203,   204,   205,   206,   207,   208,   209,   210,   211,
     212,   213,   214,   215,   216,   217,   218,   219,   220,   221,
     222,   223,   224,   225,   226,   227,   228,   229,   230,   231,
     232,   233,   234,   235,   236,   237,   238,   239,   240,   241,
     242,   243,   244,   245,   246,   247,   248,   249,   250,   251,
     252,   253,   254,   255,   256,   257,   258,   259,   260,   261,
     262,   263,   264,   265,   266,   267,   268,   269,   270,   271,
     272,   273,   274,   275,   276,   277,   278,   279,   280,   281,
     282,   283,   284,   285,   286,   287,   288,   289,   290,   291,
     292,   293,   294,   295,   296,   297,   298,   299,   300,   301,
     302,   303,   304,   305,   306,   307,   308,   309,   310,   311,
     312,   313
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int16 yystos[] =
{
       0,   399,   400,     0,    10,    11,    12,    13,    14,    15,
      16,    17,    18,    19,    20,    21,    22,    23,    24,   401,
     402,   404,   405,   409,   410,   412,   413,   414,   415,   416,
     417,   418,   419,   420,   421,   423,     3,     4,     5,   371,
     403,   373,   373,   373,   373,   403,   373,   373,   373,   403,
     403,   403,     6,    25,    26,    27,    28,    53,    54,    55,
      56,    58,    59,    60,    61,    62,    63,   314,   315,   424,
     425,   426,   427,   428,   429,   430,   431,   432,   587,   590,
     591,   401,   403,     3,   372,     8,     7,     7,     8,     8,
       7,     7,     7,   376,     7,   429,    64,    65,    66,    67,
      68,    69,    70,    71,    72,    73,    74,    75,    76,    77,
      78,    79,    80,    81,    82,    83,    84,    85,    86,    87,
      88,    89,    90,    91,    92,    93,    94,    95,    96,    97,
      98,    99,   100,   101,   102,   103,   104,   107,   108,   109,
     110,   111,   112,   113,   114,   115,   116,   117,   118,   119,
     120,   121,   122,   123,   124,   125,   126,   127,   128,   129,
     130,   131,   132,   133,   134,   135,   136,   137,   138,   139,
     140,   141,   142,   143,   144,   145,   146,   147,   148,   149,
     150,   151,   152,   153,   154,   155,   156,   157,   158,   159,
     160,   161,   162,   163,   164,   165,   166,   167,   168,   169,
     170,   171,   172,   173,   174,   175,   176,   177,   178,   179,
     180,   181,   182,   183,   184,   185,   186,   187,   188,   189,
     190,   191,   192,   193,   194,   195,   196,   197,   198,   199,
     200,   201,   202,   203,   204,   205,   206,   207,   208,   209,
     210,   211,   212,   213,   214,   215,   216,   217,   218,   219,
     220,   221,   222,   223,   224,   225,   226,   227,   228,   229,
     230,   231,   232,   233,   234,   235,   236,   237,   238,   239,
     240,   241,   242,   243,   244,   245,   246,   247,   248,   249,
     250,   251,   252,   253,   254,   255,   256,   257,   258,   259,
     260,   261,   262,   263,   264,   265,   266,   267,   268,   269,
     270,   271,   272,   273,   274,   275,   276,   277,   278,   279,
     280,   281,   282,   283,   284,   285,   286,   287,   288,   289,
     290,   291,   292,   293,   294,   295,   296,   297,   298,   299,
     300,   301,   302,   303,   304,   305,   306,   307,   308,   309,
     310,   311,   312,   313,   436,   451,   453,   457,   459,   462,
     464,   466,   468,   471,   473,   474,   475,   476,   478,   481,
     482,   483,   484,   485,   487,   488,   489,   490,   491,   492,
     502,   514,   523,   529,   531,   533,   535,   536,   537,   538,
     539,   540,   541,   542,   543,   544,   545,   547,   548,   552,
     553,   554,   555,   557,   558,   560,   562,   566,   568,   570,
     571,   574,   577,   585,   431,   372,   588,   589,   411,   403,
     374,   374,   374,   374,   374,   374,   374,   374,   437,   438,
     439,   379,   441,   442,   443,   444,   445,   446,   447,   379,
     448,   449,   379,   379,   456,   456,   456,   379,   379,   379,
     379,   379,   379,   105,   106,   105,   106,   105,   106,   105,
     106,   515,   516,   517,   519,   518,   524,   525,   527,   526,
     530,   534,   520,   528,   532,   521,   563,   564,   567,   569,
     575,   576,   586,     7,    34,    36,    37,    38,    39,    40,
      57,   357,   358,   359,   360,   396,   597,   599,   600,   601,
     615,   601,   379,   601,   601,   601,   601,   601,   379,   479,
     601,   379,   493,   494,   495,   379,   495,   495,   379,   503,
     503,   503,   379,   505,   503,   503,   505,   503,   503,   503,
     503,   379,     7,   588,   373,   408,   372,   403,     6,     6,
       6,   359,   377,   433,   433,   433,     3,     4,   371,   621,
     622,   433,   433,   378,   435,   435,   435,   433,   435,   361,
     435,   435,   361,   380,   358,   357,   463,   362,   362,   362,
     362,   362,   362,   505,   505,   505,   505,   505,   505,   505,
     505,   505,   503,   389,   505,   505,   503,   505,   505,   505,
     379,   601,   379,   572,   573,   573,   379,   388,   388,   600,
     597,   372,   372,   372,   339,   340,   341,   342,   343,   344,
     477,   372,   372,   372,   372,   372,   329,   330,   379,   480,
     372,   333,   335,   337,   338,   345,   348,   350,   351,   353,
     354,   361,   382,   383,   384,   385,   495,   601,   386,   373,
     498,   498,   327,   328,   379,   504,   504,   504,   316,   317,
     318,   319,   320,   321,   322,   323,   324,   624,   504,   504,
     504,   601,   504,   504,   504,   504,   349,   380,   509,   559,
     622,     8,   403,   406,   407,   403,   372,   373,   373,   375,
     359,   372,   372,   372,   621,   440,   372,   372,     6,   622,
     450,   454,   372,   372,    57,   461,   465,   467,   469,   472,
     601,   601,   392,   601,   601,   389,   389,   393,   384,   504,
     390,   601,   393,   504,   601,   392,   392,   366,   367,   368,
     369,   370,   565,   622,   372,   389,   393,   395,   396,   573,
     363,   364,   365,   580,   357,   357,     7,     8,    29,    30,
      31,    32,    33,    34,    35,    41,    42,    43,    44,    52,
     357,   358,   359,   373,   381,   392,   396,   452,   592,   593,
     594,   595,   596,   598,   602,   603,   604,   606,   607,   608,
     614,   616,   617,   618,   622,   452,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,   373,   381,
     603,   608,   609,   619,   620,   622,   601,   609,   381,   486,
     608,   620,   603,   604,   610,   617,   619,   609,   604,   605,
     614,   331,   332,   601,   609,   372,   495,   624,   601,   601,
     333,   334,   335,   336,   505,   505,   505,   601,   505,   505,
     505,   505,   505,   505,   379,   379,   379,   372,   374,   372,
     403,     6,     6,   622,   435,   359,   377,   434,   434,   435,
     435,   435,   435,   601,   458,   460,   357,   601,   601,   601,
     601,   372,   372,   390,   372,   372,   390,   390,   390,   390,
     505,   596,   372,   390,   505,   372,   390,   390,   505,   390,
     379,   371,   387,   611,   612,   612,   388,   388,     5,   355,
     356,   371,   622,   623,    41,    42,    43,    44,    52,   373,
     388,   388,   603,   596,   598,   617,   622,   373,   620,   619,
     372,   372,   620,   372,   372,   372,   372,   372,   592,   498,
     374,   372,   372,   379,   379,   379,   372,   379,   379,   379,
     379,   379,   379,   509,   349,   394,   551,   596,   622,   407,
     372,   375,   375,   372,   359,   372,   372,   455,   603,   608,
     618,   455,   372,   372,   372,   372,   372,   389,   389,   358,
     561,   622,   393,   384,   596,   513,   603,   607,   596,   596,
     379,   387,   506,   507,   612,   393,   596,   379,   393,   561,
     561,   597,   607,   316,   317,   325,   326,   581,   582,   583,
     622,   622,   397,   397,   357,   357,   374,   374,     5,   374,
     374,   622,   623,   357,   357,   374,   622,   609,   609,   607,
     607,   607,   604,   610,   601,   592,   346,   347,   496,   497,
     500,   592,   509,   509,   509,   389,   509,   509,   509,   509,
     509,   353,   556,   379,   379,   379,   372,   403,   622,   622,
       6,   435,   435,   618,   455,   601,   604,   601,   604,   390,
     390,   387,   391,   390,   390,   506,   391,   387,   387,   508,
     612,   508,   509,   596,   391,   390,   508,   509,   390,   391,
     391,   372,   391,   379,   612,   612,   374,   374,   612,   612,
     374,   372,   372,   372,   372,   372,   372,   372,   372,   372,
     372,   379,   379,   379,   390,   601,   546,   379,   379,   379,
     379,   509,   350,   351,   352,   550,   509,   550,   551,   372,
     374,   374,   375,   372,   372,   372,   372,   372,   596,   513,
     622,   372,   596,   596,   391,   372,   373,   512,   596,   596,
     391,   391,   601,   372,   596,   391,   389,   622,   372,   372,
     596,   372,     3,   584,   397,   397,   397,   397,   609,   486,
     610,   609,   619,   607,   592,   496,   497,   500,   501,   592,
     497,   501,   592,   393,   389,   389,   551,   372,   389,   550,
     550,   550,   379,   379,   549,   549,   549,   601,   379,   392,
     403,   622,   622,   604,   604,   601,   470,   593,   621,   507,
     391,   389,   507,   507,   372,   596,   373,   596,   612,   372,
     372,   372,   509,   507,   372,   390,   391,   604,   619,   372,
     604,     9,   578,   601,   372,   611,   613,   372,   372,   372,
     372,   601,   601,   601,   391,   389,   390,   601,   604,   604,
     550,   348,   550,   390,   372,   372,   372,   372,   372,   391,
     372,   390,   391,   391,   596,   372,   373,   596,   388,   511,
     596,   596,   389,   391,   596,   619,   522,   372,   372,   551,
     372,   372,   610,   372,   499,   594,   499,   499,   501,   592,
     501,   372,   372,   372,   390,   551,   372,   372,   372,   601,
     601,   358,     6,   622,   604,   604,   601,   372,   619,   596,
     372,   372,   372,   619,   596,   511,   388,   374,   372,   372,
     390,   372,   372,   391,   372,   509,   509,   604,   579,   607,
     618,   501,   372,   393,   389,   596,   551,   391,   551,   551,
     551,   372,   391,   375,   372,   372,   619,   507,   619,   619,
     619,   511,   612,   622,   619,   619,   619,   619,   619,   372,
     619,   372,   390,   390,   372,   391,   372,   372,   613,   551,
     622,   604,   604,   391,   374,   374,   391,   596,   604,   551,
     551,   596,   372,   596,   551,   372,   613,   372,   372,   612,
     388,   372,   372,   391,   391,   596,   372,   551,   372,   363,
     510,   592,   619,   374,   388,   596,   596,   372,   372,   372,
     551,   551,   375,   388,   622,   372,   372,   596,   596,   596,
     372,   622,   388,   374,   596,   596,   372,   596,   372,   622,
     596,     6,   374,   372,   375,   596,   622,   372,     6,   375,
     192,   422
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int16 yyr1[] =
{
       0,   398,   400,   399,   401,   401,   402,   402,   402,   402,
     402,   402,   402,   402,   402,   402,   402,   402,   402,   402,
     403,   403,   403,   403,   404,   405,   406,   407,   407,   407,
     408,   408,   409,   409,   411,   410,   412,   413,   413,   414,
     415,   416,   417,   418,   419,   420,   421,   422,   423,   424,
     425,   426,   427,   428,   428,   428,   428,   428,   428,   428,
     428,   428,   428,   429,   429,   430,   430,   431,   431,   431,
     431,   431,   431,   431,   431,   431,   431,   431,   431,   431,
     431,   431,   432,   433,   433,   434,   434,   435,   435,   436,
     437,   436,   438,   436,   439,   436,   440,   436,   441,   436,
     442,   436,   443,   436,   444,   436,   445,   436,   436,   446,
     436,   436,   436,   436,   436,   436,   436,   447,   436,   436,
     436,   436,   436,   448,   436,   436,   449,   436,   436,   436,
     436,   436,   450,   436,   451,   451,   452,   452,   454,   453,
     455,   455,   455,   456,   456,   458,   457,   460,   459,   461,
     461,   463,   462,   465,   464,   467,   466,   469,   468,   470,
     470,   472,   471,   473,   473,   473,   473,   473,   473,   473,
     473,   473,   473,   474,   474,   474,   474,   474,   474,   474,
     474,   474,   474,   474,   474,   474,   474,   475,   475,   475,
     475,   475,   475,   475,   475,   475,   475,   476,   476,   476,
     476,   476,   476,   476,   476,   476,   476,   476,   476,   476,
     476,   476,   476,   476,   476,   476,   476,   476,   476,   476,
     476,   476,   476,   476,   477,   477,   477,   477,   477,   477,
     478,   478,   478,   479,   479,   480,   480,   481,   481,   481,
     481,   481,   481,   481,   481,   482,   482,   482,   482,   482,
     483,   483,   483,   483,   483,   483,   483,   483,   483,   483,
     483,   483,   483,   484,   484,   485,   485,   486,   486,   486,
     487,   487,   487,   487,   487,   488,   488,   488,   488,   488,
     488,   488,   488,   488,   488,   489,   490,   490,   491,   492,
     492,   492,   492,   492,   492,   492,   492,   492,   492,   492,
     492,   492,   492,   492,   492,   492,   492,   492,   492,   492,
     492,   492,   492,   492,   492,   492,   492,   492,   492,   492,
     492,   492,   493,   493,   494,   494,   494,   494,   494,   494,
     494,   494,   494,   494,   494,   494,   494,   494,   495,   495,
     496,   497,   498,   498,   499,   500,   500,   500,   500,   501,
     501,   501,   501,   502,   502,   502,   502,   502,   502,   502,
     502,   502,   503,   503,   504,   504,   504,   504,   505,   506,
     507,   507,   508,   508,   509,   510,   510,   511,   511,   512,
     512,   512,   512,   513,   515,   514,   516,   514,   517,   514,
     518,   514,   519,   514,   520,   514,   521,   522,   514,   524,
     523,   525,   523,   526,   523,   527,   523,   528,   523,   530,
     529,   532,   531,   534,   533,   535,   535,   535,   535,   535,
     535,   535,   535,   535,   535,   535,   536,   536,   536,   536,
     536,   536,   536,   536,   536,   536,   536,   537,   537,   537,
     537,   537,   537,   537,   537,   537,   537,   537,   538,   539,
     540,   541,   541,   541,   542,   543,   544,   545,   545,   546,
     545,   547,   547,   547,   548,   549,   549,   550,   550,   550,
     551,   551,   552,   552,   553,   553,   553,   553,   553,   553,
     553,   553,   553,   553,   553,   554,   555,   556,   556,   557,
     557,   557,   557,   558,   559,   559,   559,   560,   561,   561,
     561,   563,   562,   564,   562,   565,   565,   565,   565,   565,
     565,   567,   566,   569,   568,   570,   570,   570,   571,   571,
     571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
     571,   571,   572,   572,   572,   572,   573,   573,   575,   574,
     576,   574,   577,   577,   577,   578,   578,   579,   579,   580,
     580,   580,   581,   581,   582,   582,   583,   583,   584,   585,
     585,   585,   585,   585,   585,   585,   586,   585,   587,   588,
     588,   589,   590,   591,   592,   593,   594,   595,   596,   596,
     596,   596,   597,   597,   597,   597,   597,   598,   599,   599,
     599,   599,   599,   599,   600,   600,   601,   601,   602,   602,
     602,   602,   602,   603,   603,   604,   604,   605,   605,   606,
     606,   607,   607,   608,   608,   608,   609,   609,   609,   610,
     610,   610,   611,   611,   612,   612,   613,   614,   614,   615,
     615,   616,   616,   617,   617,   618,   618,   618,   618,   618,
     618,   618,   618,   618,   618,   618,   618,   618,   618,   618,
     618,   618,   619,   619,   619,   619,   619,   620,   620,   620,
     620,   620,   620,   620,   620,   620,   620,   620,   620,   621,
     621,   622,   622,   623,   623,   624,   624,   624,   624,   624,
     624,   624,   624,   624
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     0,     3,     0,     2,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     2,     1,     6,    11,     1,     1,     3,     0,
       3,     0,     1,     1,     0,     5,     4,     4,     4,     4,
       2,     2,     2,     1,     2,    10,    10,     1,    27,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     0,     2,     2,     1,     2,     2,     2,
       2,     2,     2,     2,     2,     1,     1,     1,     1,     1,
       1,     1,     2,     2,     1,     2,     1,     2,     2,     1,
       0,     5,     0,     7,     0,     7,     0,     5,     0,     5,
       0,     5,     0,     3,     0,     3,     0,     3,     1,     0,
       3,     1,     1,     1,     1,     1,     1,     0,     3,     1,
       1,     1,     1,     0,     3,     1,     0,     3,     1,     1,
       1,     1,     0,     5,     3,     3,     1,     1,     0,     5,
       1,     1,     2,     0,     2,     0,     6,     0,     6,     0,
       1,     0,    11,     0,    11,     0,    13,     0,    13,     1,
       1,     0,     9,     1,     1,     1,     1,     1,     1,     1,
       4,     4,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     2,     2,     2,     2,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       4,     8,     6,     2,     2,     2,     2,     2,     2,     2,
       2,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       0,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     2,
       8,     8,     8,     8,    10,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     4,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     1,     0,     2,
       1,     1,     3,     0,     1,     1,     3,     1,     1,     1,
       3,     3,     3,     5,    10,     8,     6,     4,    10,     8,
      11,     1,     2,     2,     2,     2,     2,     2,     2,     1,
       1,     2,     1,     2,     1,     1,     1,     3,     0,    12,
      10,     4,     2,     3,     0,    12,     0,    11,     0,    15,
       0,    12,     0,    12,     0,    12,     0,     0,    12,     0,
      12,     0,    11,     0,    12,     0,    12,     0,    12,     0,
      17,     0,    17,     0,     9,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,    20,    13,
      16,     1,     1,     1,     1,     1,     1,     9,    16,     0,
      17,     1,     1,     1,     3,     0,     2,     2,     2,     2,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     0,     2,    11,
      13,    14,    15,     1,     6,     8,     9,     7,     1,     1,
       3,     0,    11,     0,    11,     1,     1,     1,     1,     1,
       1,     0,    10,     0,    13,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     2,     2,     2,     2,     0,     2,     0,     3,
       0,     3,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     0,    11,     1,     1,
       2,     2,     2,     2,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     2,     1,     2,     1,     1,
       1,     1,     1,     1,     2,     1,     2,     1,     1,     1,
       2,     1,     1,     1,     1,     2,     1,     2,     1,     1,
       1,     1,     0,     2,     1,     2,     1,     5,     5,     5,
       5,     5,     5,     1,     1,     1,     3,     3,     4,     4,
       2,     2,     2,     2,     2,     3,     3,     1,     1,     1,
       1,     1,     1,     3,     1,     4,     2,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     2,     1,     2,     1,     1,     1,     1,     1,
       1,     1,     1,     1
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)




# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  yy_symbol_value_print (yyo, yykind, yyvaluep);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp,
                 int yyrule)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)]);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


/* Context of a parse error.  */
typedef struct
{
  yy_state_t *yyssp;
  yysymbol_kind_t yytoken;
} yypcontext_t;

/* Put in YYARG at most YYARGN of the expected tokens given the
   current YYCTX, and return the number of tokens stored in YYARG.  If
   YYARG is null, return the number of expected tokens (guaranteed to
   be less than YYNTOKENS).  Return YYENOMEM on memory exhaustion.
   Return 0 if there are more than YYARGN expected tokens, yet fill
   YYARG up to YYARGN. */
static int
yypcontext_expected_tokens (const yypcontext_t *yyctx,
                            yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  int yyn = yypact[+*yyctx->yyssp];
  if (!yypact_value_is_default (yyn))
    {
      /* Start YYX at -YYN if negative to avoid negative indexes in
         YYCHECK.  In other words, skip the first -YYN actions for
         this state because they are default actions.  */
      int yyxbegin = yyn < 0 ? -yyn : 0;
      /* Stay within bounds of both yycheck and yytname.  */
      int yychecklim = YYLAST - yyn + 1;
      int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
      int yyx;
      for (yyx = yyxbegin; yyx < yyxend; ++yyx)
        if (yycheck[yyx + yyn] == yyx && yyx != YYSYMBOL_YYerror
            && !yytable_value_is_error (yytable[yyx + yyn]))
          {
            if (!yyarg)
              ++yycount;
            else if (yycount == yyargn)
              return 0;
            else
              yyarg[yycount++] = YY_CAST (yysymbol_kind_t, yyx);
          }
    }
  if (yyarg && yycount == 0 && 0 < yyargn)
    yyarg[0] = YYSYMBOL_YYEMPTY;
  return yycount;
}




#ifndef yystrlen
# if defined __GLIBC__ && defined _STRING_H
#  define yystrlen(S) (YY_CAST (YYPTRDIFF_T, strlen (S)))
# else
/* Return the length of YYSTR.  */
static YYPTRDIFF_T
yystrlen (const char *yystr)
{
  YYPTRDIFF_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
# endif
#endif

#ifndef yystpcpy
# if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#  define yystpcpy stpcpy
# else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
# endif
#endif

#ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYPTRDIFF_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYPTRDIFF_T yyn = 0;
      char const *yyp = yystr;
      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            else
              goto append;

          append:
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (yyres)
    return yystpcpy (yyres, yystr) - yyres;
  else
    return yystrlen (yystr);
}
#endif


static int
yy_syntax_error_arguments (const yypcontext_t *yyctx,
                           yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yyctx->yytoken != YYSYMBOL_YYEMPTY)
    {
      int yyn;
      if (yyarg)
        yyarg[yycount] = yyctx->yytoken;
      ++yycount;
      yyn = yypcontext_expected_tokens (yyctx,
                                        yyarg ? yyarg + 1 : yyarg, yyargn - 1);
      if (yyn == YYENOMEM)
        return YYENOMEM;
      else
        yycount += yyn;
    }
  return yycount;
}

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return -1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return YYENOMEM if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYPTRDIFF_T *yymsg_alloc, char **yymsg,
                const yypcontext_t *yyctx)
{
  enum { YYARGS_MAX = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat: reported tokens (one for the "unexpected",
     one per "expected"). */
  yysymbol_kind_t yyarg[YYARGS_MAX];
  /* Cumulated lengths of YYARG.  */
  YYPTRDIFF_T yysize = 0;

  /* Actual size of YYARG. */
  int yycount = yy_syntax_error_arguments (yyctx, yyarg, YYARGS_MAX);
  if (yycount == YYENOMEM)
    return YYENOMEM;

  switch (yycount)
    {
#define YYCASE_(N, S)                       \
      case N:                               \
        yyformat = S;                       \
        break
    default: /* Avoid compiler warnings. */
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
    }

  /* Compute error message size.  Don't count the "%s"s, but reserve
     room for the terminator.  */
  yysize = yystrlen (yyformat) - 2 * yycount + 1;
  {
    int yyi;
    for (yyi = 0; yyi < yycount; ++yyi)
      {
        YYPTRDIFF_T yysize1
          = yysize + yytnamerr (YY_NULLPTR, yytname[yyarg[yyi]]);
        if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
          yysize = yysize1;
        else
          return YYENOMEM;
      }
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return -1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yytname[yyarg[yyi++]]);
          yyformat += 2;
        }
      else
        {
          ++yyp;
          ++yyformat;
        }
  }
  return 0;
}


/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep)
{
  YY_USE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/* Lookahead token kind.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
int yynerrs;




/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;

  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYPTRDIFF_T yymsg_alloc = sizeof yymsgbuf;

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex ();
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2: /* $@1: %empty  */
#line 494 "ir3_parser.y"
                   { new_shader(); }
#line 3190 "ir3_parser.c"
    break;

  case 20: /* const_val: T_FLOAT  */
#line 514 "ir3_parser.y"
                             { (yyval.unum) = fui((yyvsp[0].flt)); }
#line 3196 "ir3_parser.c"
    break;

  case 21: /* const_val: T_INT  */
#line 515 "ir3_parser.y"
                             { (yyval.unum) = (yyvsp[0].num);      }
#line 3202 "ir3_parser.c"
    break;

  case 22: /* const_val: '-' T_INT  */
#line 516 "ir3_parser.y"
                             { (yyval.unum) = -(yyvsp[0].num);     }
#line 3208 "ir3_parser.c"
    break;

  case 23: /* const_val: T_HEX  */
#line 517 "ir3_parser.y"
                             { (yyval.unum) = (yyvsp[0].unum);      }
#line 3214 "ir3_parser.c"
    break;

  case 24: /* localsize_header: T_A_LOCALSIZE const_val ',' const_val ',' const_val  */
#line 519 "ir3_parser.y"
                                                                       {
                       variant->local_size[0] = (yyvsp[-4].unum);
                       variant->local_size[1] = (yyvsp[-2].unum);
                       variant->local_size[2] = (yyvsp[0].unum);
}
#line 3224 "ir3_parser.c"
    break;

  case 25: /* const_header: T_A_CONST '(' T_CONSTANT ')' const_val ',' const_val ',' const_val ',' const_val  */
#line 525 "ir3_parser.y"
                                                                                                    {
                       add_const((yyvsp[-8].num), (yyvsp[-6].unum), (yyvsp[-4].unum), (yyvsp[-2].unum), (yyvsp[0].unum));
}
#line 3232 "ir3_parser.c"
    break;

  case 26: /* buf_header_init_val: const_val  */
#line 529 "ir3_parser.y"
                                { add_buf_init_val((yyvsp[0].unum)); }
#line 3238 "ir3_parser.c"
    break;

  case 30: /* buf_header_addr_reg: '(' T_CONSTANT ')'  */
#line 535 "ir3_parser.y"
                                      {
                       assert(((yyvsp[-1].num) & 0x1) == 0);  /* half-reg not allowed */
                       unsigned reg = (yyvsp[-1].num) >> 1;

                       info->buf_addr_regs[info->num_bufs - 1] = reg;
                       /* reserve space in immediates for the actual value to be plugged in later: */
                       add_const((yyvsp[-1].num), 0, 0, 0, 0);
}
#line 3251 "ir3_parser.c"
    break;

  case 32: /* buf_type: T_A_BUF  */
#line 545 "ir3_parser.y"
                  { (yyval.num) = KERNEL_BUF_UAV; }
#line 3257 "ir3_parser.c"
    break;

  case 33: /* buf_type: T_A_UBO  */
#line 546 "ir3_parser.y"
                  { (yyval.num) = KERNEL_BUF_UBO; }
#line 3263 "ir3_parser.c"
    break;

  case 34: /* $@2: %empty  */
#line 548 "ir3_parser.y"
                                      {
                       int idx = info->num_bufs++;
                       assert(idx < MAX_BUFS);
                       info->buf_types[idx] = (yyvsp[-1].num);
                       info->buf_sizes[idx] = (yyvsp[0].unum);
}
#line 3274 "ir3_parser.c"
    break;

  case 36: /* invocationid_header: T_A_INVOCATIONID '(' T_REGISTER ')'  */
#line 555 "ir3_parser.y"
                                                         {
                       assert(((yyvsp[-1].num) & 0x1) == 0);  /* half-reg not allowed */
                       unsigned reg = (yyvsp[-1].num) >> 1;
                       add_sysval(reg, 0x7, SYSTEM_VALUE_LOCAL_INVOCATION_ID);
}
#line 3284 "ir3_parser.c"
    break;

  case 37: /* wgid_header: T_A_WGID '(' T_REGISTER ')'  */
#line 561 "ir3_parser.y"
                                               {
                       assert(((yyvsp[-1].num) & 0x1) == 0);  /* half-reg not allowed */
                       unsigned reg = (yyvsp[-1].num) >> 1;
                       assert(variant->compiler->gen >= 5);
                       assert(reg >= regid(48, 0)); /* must be a high reg */
                       add_sysval(reg, 0x7, SYSTEM_VALUE_WORKGROUP_ID);
}
#line 3296 "ir3_parser.c"
    break;

  case 38: /* wgid_header: T_A_WGID '(' T_CONSTANT ')'  */
#line 568 "ir3_parser.y"
                                               {
                       assert(((yyvsp[-1].num) & 0x1) == 0);  /* half-reg not allowed */
                       unsigned reg = (yyvsp[-1].num) >> 1;
                       assert(variant->compiler->gen < 5);
                       info->wgid = reg;
}
#line 3307 "ir3_parser.c"
    break;

  case 39: /* numwg_header: T_A_NUMWG '(' T_CONSTANT ')'  */
#line 575 "ir3_parser.y"
                                                {
                       assert(((yyvsp[-1].num) & 0x1) == 0);  /* half-reg not allowed */
                       unsigned reg = (yyvsp[-1].num) >> 1;
                       info->numwg = reg;
                       /* reserve space in immediates for the actual value to be plugged in later: */
                       if (variant->compiler->gen >= 5)
                          add_const((yyvsp[-1].num), 0, 0, 0, 0);
}
#line 3320 "ir3_parser.c"
    break;

  case 40: /* branchstack_header: T_A_BRANCHSTACK const_val  */
#line 584 "ir3_parser.y"
                                              { variant->branchstack = (yyvsp[0].unum); }
#line 3326 "ir3_parser.c"
    break;

  case 41: /* pvtmem_header: T_A_PVTMEM const_val  */
#line 586 "ir3_parser.y"
                                    { variant->pvtmem_size = (yyvsp[0].unum); }
#line 3332 "ir3_parser.c"
    break;

  case 42: /* localmem_header: T_A_LOCALMEM const_val  */
#line 588 "ir3_parser.y"
                                        { variant->shared_size = (yyvsp[0].unum); }
#line 3338 "ir3_parser.c"
    break;

  case 43: /* earlypreamble_header: T_A_EARLYPREAMBLE  */
#line 590 "ir3_parser.y"
                                        { variant->early_preamble = 1; }
#line 3344 "ir3_parser.c"
    break;

  case 44: /* constlen_header: T_A_CONSTLEN const_val  */
#line 592 "ir3_parser.y"
                                        { variant->constlen = (yyvsp[0].unum); }
#line 3350 "ir3_parser.c"
    break;

  case 45: /* in_header: T_A_IN '(' T_REGISTER ')' T_IDENTIFIER '(' T_IDENTIFIER '=' integer ')'  */
#line 595 "ir3_parser.y"
                                                                                           { }
#line 3356 "ir3_parser.c"
    break;

  case 46: /* out_header: T_A_OUT '(' T_REGISTER ')' T_IDENTIFIER '(' T_IDENTIFIER '=' integer ')'  */
#line 597 "ir3_parser.y"
                                                                                            { }
#line 3362 "ir3_parser.c"
    break;

  case 48: /* tex_header: T_A_TEX '(' T_REGISTER ')' T_IDENTIFIER '=' integer ',' T_IDENTIFIER '=' integer ',' T_IDENTIFIER '=' integer ',' T_MOD_TEX '=' integer ',' T_IDENTIFIER '=' integer ',' T_IDENTIFIER '=' tex_header_opc  */
#line 608 "ir3_parser.y"
                                                                 { }
#line 3368 "ir3_parser.c"
    break;

  case 49: /* fullnop_start_section: T_A_FULLNOPSTART  */
#line 610 "ir3_parser.y"
                                        { is_in_fullnop_section = true; }
#line 3374 "ir3_parser.c"
    break;

  case 50: /* fullnop_end_section: T_A_FULLNOPEND  */
#line 611 "ir3_parser.y"
                                    { is_in_fullnop_section = false; }
#line 3380 "ir3_parser.c"
    break;

  case 51: /* fullsync_start_section: T_A_FULLSYNCSTART  */
#line 612 "ir3_parser.y"
                                          { is_in_fullsync_section = true; }
#line 3386 "ir3_parser.c"
    break;

  case 52: /* fullsync_end_section: T_A_FULLSYNCEND  */
#line 613 "ir3_parser.y"
                                      { is_in_fullsync_section = false; }
#line 3392 "ir3_parser.c"
    break;

  case 53: /* iflag: T_SY  */
#line 615 "ir3_parser.y"
                          { iflags.flags |= IR3_INSTR_SY; }
#line 3398 "ir3_parser.c"
    break;

  case 54: /* iflag: T_SS  */
#line 616 "ir3_parser.y"
                          { iflags.flags |= IR3_INSTR_SS; }
#line 3404 "ir3_parser.c"
    break;

  case 55: /* iflag: T_JP  */
#line 617 "ir3_parser.y"
                          { iflags.flags |= IR3_INSTR_JP; }
#line 3410 "ir3_parser.c"
    break;

  case 56: /* iflag: T_EQ_FLAG  */
#line 618 "ir3_parser.y"
                             { iflags.flags |= IR3_INSTR_EQ; }
#line 3416 "ir3_parser.c"
    break;

  case 57: /* iflag: T_RPT  */
#line 619 "ir3_parser.y"
                          { iflags.repeat = (yyvsp[0].num); }
#line 3422 "ir3_parser.c"
    break;

  case 58: /* iflag: T_UL  */
#line 620 "ir3_parser.y"
                          { iflags.flags |= IR3_INSTR_UL; }
#line 3428 "ir3_parser.c"
    break;

  case 59: /* iflag: T_NOP  */
#line 621 "ir3_parser.y"
                          { iflags.nop = (yyvsp[0].tok); }
#line 3434 "ir3_parser.c"
    break;

  case 60: /* iflag: T_EOLM  */
#line 622 "ir3_parser.y"
                          { iflags.flags |= IR3_INSTR_EOLM; }
#line 3440 "ir3_parser.c"
    break;

  case 61: /* iflag: T_EOGM  */
#line 623 "ir3_parser.y"
                          { iflags.flags |= IR3_INSTR_EOGM; }
#line 3446 "ir3_parser.c"
    break;

  case 62: /* iflag: T_EOSTSC  */
#line 624 "ir3_parser.y"
                            { iflags.flags |= IR3_INSTR_EOSTSC; }
#line 3452 "ir3_parser.c"
    break;

  case 72: /* instr: iflags cat5_instr  */
#line 637 "ir3_parser.y"
                                     { fixup_cat5_s2en(); }
#line 3458 "ir3_parser.c"
    break;

  case 82: /* label: T_IDENTIFIER ':'  */
#line 648 "ir3_parser.y"
                                    { new_label((yyvsp[-1].str)); }
#line 3464 "ir3_parser.c"
    break;

  case 83: /* cat0_src1: '!' T_P0  */
#line 650 "ir3_parser.y"
                                   { instr->cat0.inv1 = true; (yyval.reg) = new_src((62 << 3) + (yyvsp[0].num), IR3_REG_PREDICATE); }
#line 3470 "ir3_parser.c"
    break;

  case 84: /* cat0_src1: T_P0  */
#line 651 "ir3_parser.y"
                                   { (yyval.reg) = new_src((62 << 3) + (yyvsp[0].num), IR3_REG_PREDICATE); }
#line 3476 "ir3_parser.c"
    break;

  case 85: /* cat0_src2: '!' T_P0  */
#line 653 "ir3_parser.y"
                                   { instr->cat0.inv2 = true; (yyval.reg) = new_src((62 << 3) + (yyvsp[0].num), IR3_REG_PREDICATE); }
#line 3482 "ir3_parser.c"
    break;

  case 86: /* cat0_src2: T_P0  */
#line 654 "ir3_parser.y"
                                   { (yyval.reg) = new_src((62 << 3) + (yyvsp[0].num), IR3_REG_PREDICATE); }
#line 3488 "ir3_parser.c"
    break;

  case 87: /* cat0_immed: '#' integer  */
#line 656 "ir3_parser.y"
                                   { instr->cat0.immed = (yyvsp[0].num); }
#line 3494 "ir3_parser.c"
    break;

  case 88: /* cat0_immed: '#' T_IDENTIFIER  */
#line 657 "ir3_parser.y"
                                    { ralloc_steal(variant->ir, (void *)(yyvsp[0].str)); instr->cat0.target_label = (yyvsp[0].str); }
#line 3500 "ir3_parser.c"
    break;

  case 89: /* cat0_instr: T_OP_NOP  */
#line 659 "ir3_parser.y"
                                   { new_instr(OPC_NOP); }
#line 3506 "ir3_parser.c"
    break;

  case 90: /* $@3: %empty  */
#line 660 "ir3_parser.y"
                                   { new_instr(OPC_BR);   }
#line 3512 "ir3_parser.c"
    break;

  case 92: /* $@4: %empty  */
#line 661 "ir3_parser.y"
                                   { new_instr(OPC_BRAO); }
#line 3518 "ir3_parser.c"
    break;

  case 94: /* $@5: %empty  */
#line 662 "ir3_parser.y"
                                   { new_instr(OPC_BRAA); }
#line 3524 "ir3_parser.c"
    break;

  case 96: /* $@6: %empty  */
#line 663 "ir3_parser.y"
                                         { new_instr(OPC_BRAC)->cat0.idx = (yyvsp[0].num); }
#line 3530 "ir3_parser.c"
    break;

  case 98: /* $@7: %empty  */
#line 664 "ir3_parser.y"
                                   { new_instr(OPC_BANY); }
#line 3536 "ir3_parser.c"
    break;

  case 100: /* $@8: %empty  */
#line 665 "ir3_parser.y"
                                   { new_instr(OPC_BALL); }
#line 3542 "ir3_parser.c"
    break;

  case 102: /* $@9: %empty  */
#line 666 "ir3_parser.y"
                                   { new_instr(OPC_BRAX); }
#line 3548 "ir3_parser.c"
    break;

  case 104: /* $@10: %empty  */
#line 667 "ir3_parser.y"
                                   { new_instr(OPC_JUMP); }
#line 3554 "ir3_parser.c"
    break;

  case 106: /* $@11: %empty  */
#line 668 "ir3_parser.y"
                                   { new_instr(OPC_CALL); }
#line 3560 "ir3_parser.c"
    break;

  case 108: /* cat0_instr: T_OP_RET  */
#line 669 "ir3_parser.y"
                                   { new_instr(OPC_RET); }
#line 3566 "ir3_parser.c"
    break;

  case 109: /* $@12: %empty  */
#line 670 "ir3_parser.y"
                                   { new_instr(OPC_KILL); }
#line 3572 "ir3_parser.c"
    break;

  case 111: /* cat0_instr: T_OP_END  */
#line 671 "ir3_parser.y"
                                   { new_instr(OPC_END); }
#line 3578 "ir3_parser.c"
    break;

  case 112: /* cat0_instr: T_OP_EMIT  */
#line 672 "ir3_parser.y"
                                   { new_instr(OPC_EMIT); }
#line 3584 "ir3_parser.c"
    break;

  case 113: /* cat0_instr: T_OP_CUT  */
#line 673 "ir3_parser.y"
                                   { new_instr(OPC_CUT); }
#line 3590 "ir3_parser.c"
    break;

  case 114: /* cat0_instr: T_OP_CHMASK  */
#line 674 "ir3_parser.y"
                                   { new_instr(OPC_CHMASK); }
#line 3596 "ir3_parser.c"
    break;

  case 115: /* cat0_instr: T_OP_CHSH  */
#line 675 "ir3_parser.y"
                                   { new_instr(OPC_CHSH); }
#line 3602 "ir3_parser.c"
    break;

  case 116: /* cat0_instr: T_OP_FLOW_REV  */
#line 676 "ir3_parser.y"
                                   { new_instr(OPC_FLOW_REV); }
#line 3608 "ir3_parser.c"
    break;

  case 117: /* $@13: %empty  */
#line 677 "ir3_parser.y"
                                   { new_instr(OPC_BKT); }
#line 3614 "ir3_parser.c"
    break;

  case 119: /* cat0_instr: T_OP_STKS  */
#line 678 "ir3_parser.y"
                                   { new_instr(OPC_STKS); }
#line 3620 "ir3_parser.c"
    break;

  case 120: /* cat0_instr: T_OP_STKR  */
#line 679 "ir3_parser.y"
                                   { new_instr(OPC_STKR); }
#line 3626 "ir3_parser.c"
    break;

  case 121: /* cat0_instr: T_OP_XSET  */
#line 680 "ir3_parser.y"
                                   { new_instr(OPC_XSET); }
#line 3632 "ir3_parser.c"
    break;

  case 122: /* cat0_instr: T_OP_XCLR  */
#line 681 "ir3_parser.y"
                                   { new_instr(OPC_XCLR); }
#line 3638 "ir3_parser.c"
    break;

  case 123: /* $@14: %empty  */
#line 682 "ir3_parser.y"
                                   { new_instr(OPC_GETONE); }
#line 3644 "ir3_parser.c"
    break;

  case 125: /* cat0_instr: T_OP_DBG  */
#line 683 "ir3_parser.y"
                                   { new_instr(OPC_DBG); }
#line 3650 "ir3_parser.c"
    break;

  case 126: /* $@15: %empty  */
#line 684 "ir3_parser.y"
                                   { new_instr(OPC_SHPS); }
#line 3656 "ir3_parser.c"
    break;

  case 128: /* cat0_instr: T_OP_SHPE  */
#line 685 "ir3_parser.y"
                                   { new_instr(OPC_SHPE); }
#line 3662 "ir3_parser.c"
    break;

  case 129: /* cat0_instr: T_OP_PREDT  */
#line 686 "ir3_parser.y"
                                   { new_instr(OPC_PREDT); }
#line 3668 "ir3_parser.c"
    break;

  case 130: /* cat0_instr: T_OP_PREDF  */
#line 687 "ir3_parser.y"
                                   { new_instr(OPC_PREDF); }
#line 3674 "ir3_parser.c"
    break;

  case 131: /* cat0_instr: T_OP_PREDE  */
#line 688 "ir3_parser.y"
                                   { new_instr(OPC_PREDE); }
#line 3680 "ir3_parser.c"
    break;

  case 132: /* $@16: %empty  */
#line 689 "ir3_parser.y"
                                        { new_instr(OPC_GETLAST); }
#line 3686 "ir3_parser.c"
    break;

  case 134: /* cat1_opc: T_OP_MOV '.' T_CAT1_TYPE_TYPE  */
#line 691 "ir3_parser.y"
                                                 {
                       parse_type_type(new_instr(OPC_MOV), (yyvsp[0].str));
}
#line 3694 "ir3_parser.c"
    break;

  case 135: /* cat1_opc: T_OP_COV '.' T_CAT1_TYPE_TYPE  */
#line 694 "ir3_parser.y"
                                                 {
                       parse_type_type(new_instr(OPC_MOV), (yyvsp[0].str));
}
#line 3702 "ir3_parser.c"
    break;

  case 138: /* $@17: %empty  */
#line 701 "ir3_parser.y"
                                       {
                       new_instr(OPC_MOVMSK);
                       instr->cat1.src_type = TYPE_U32;
                       instr->cat1.dst_type = TYPE_U32;
                   }
#line 3712 "ir3_parser.c"
    break;

  case 139: /* cat1_movmsk: T_OP_MOVMSK '.' T_W $@17 dst_reg  */
#line 705 "ir3_parser.y"
                             {
                       if (((yyvsp[-2].num) % 32) != 0)
                          yyerror("w# must be multiple of 32");
                       if ((yyvsp[-2].num) < 32)
                          yyerror("w# must be at least 32");

                       int num = (yyvsp[-2].num) / 32;

                       instr->repeat = num - 1;
                       instr->dsts[0]->wrmask = (1 << num) - 1;
                   }
#line 3728 "ir3_parser.c"
    break;

  case 144: /* cat1_mova_flags: '.' 'u'  */
#line 722 "ir3_parser.y"
                           { iflags.flags |= IR3_INSTR_U; }
#line 3734 "ir3_parser.c"
    break;

  case 145: /* $@18: %empty  */
#line 724 "ir3_parser.y"
                                                       {
                       new_instr(OPC_MOV);
                       instr->cat1.src_type = TYPE_U16;
                       instr->cat1.dst_type = TYPE_U16;
                       new_dst((61 << 3) + 2, IR3_REG_HALF);
                   }
#line 3745 "ir3_parser.c"
    break;

  case 147: /* $@19: %empty  */
#line 731 "ir3_parser.y"
                                                      {
                       new_instr(OPC_MOV);
                       instr->cat1.src_type = TYPE_S16;
                       instr->cat1.dst_type = TYPE_S16;
                       new_dst((61 << 3), IR3_REG_HALF);
                   }
#line 3756 "ir3_parser.c"
    break;

  case 150: /* cat1_mova_dst_flags: T_SAT  */
#line 739 "ir3_parser.y"
                         { instr->flags |= IR3_INSTR_SAT; }
#line 3762 "ir3_parser.c"
    break;

  case 151: /* $@20: %empty  */
#line 741 "ir3_parser.y"
                                               { new_instr(OPC_MOV); }
#line 3768 "ir3_parser.c"
    break;

  case 152: /* cat1_mova_r: T_OP_MOVA_R cat1_mova_flags $@20 cat1_mova_dst_flags T_A0 ',' mova_src ',' integer ',' integer  */
#line 741 "ir3_parser.y"
                                                                                                                                     {
                       instr->cat1.src_type = TYPE_S16;
                       instr->cat1.dst_type = TYPE_S16;
                       new_dst((61 << 3), IR3_REG_HALF);
                       instr->cat1.r[0] = (yyvsp[-2].num);
                       instr->cat1.r[1] = (yyvsp[0].num);
                   }
#line 3780 "ir3_parser.c"
    break;

  case 153: /* $@21: %empty  */
#line 749 "ir3_parser.y"
                                                 { parse_type_type(new_instr(OPC_SWZ), (yyvsp[0].str)); }
#line 3786 "ir3_parser.c"
    break;

  case 155: /* $@22: %empty  */
#line 751 "ir3_parser.y"
                                                 { parse_type_type(new_instr(OPC_GAT), (yyvsp[0].str)); }
#line 3792 "ir3_parser.c"
    break;

  case 157: /* $@23: %empty  */
#line 753 "ir3_parser.y"
                                                 { parse_type_type(new_instr(OPC_SCT), (yyvsp[0].str)); }
#line 3798 "ir3_parser.c"
    break;

  case 159: /* movs_invocation: uinteger  */
#line 755 "ir3_parser.y"
                          { new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[0].num); }
#line 3804 "ir3_parser.c"
    break;

  case 161: /* $@24: %empty  */
#line 758 "ir3_parser.y"
                                          { parse_type_type(new_instr(OPC_MOVS), (yyvsp[0].str)); }
#line 3810 "ir3_parser.c"
    break;

  case 173: /* cat2_opc_1src: T_OP_ABSNEG_F  */
#line 772 "ir3_parser.y"
                                  { new_instr(OPC_ABSNEG_F); }
#line 3816 "ir3_parser.c"
    break;

  case 174: /* cat2_opc_1src: T_OP_ABSNEG_S  */
#line 773 "ir3_parser.y"
                                  { new_instr(OPC_ABSNEG_S); }
#line 3822 "ir3_parser.c"
    break;

  case 175: /* cat2_opc_1src: T_OP_CLZ_B  */
#line 774 "ir3_parser.y"
                                  { new_instr(OPC_CLZ_B); }
#line 3828 "ir3_parser.c"
    break;

  case 176: /* cat2_opc_1src: T_OP_CLZ_S  */
#line 775 "ir3_parser.y"
                                  { new_instr(OPC_CLZ_S); }
#line 3834 "ir3_parser.c"
    break;

  case 177: /* cat2_opc_1src: T_OP_SIGN_F  */
#line 776 "ir3_parser.y"
                                  { new_instr(OPC_SIGN_F); }
#line 3840 "ir3_parser.c"
    break;

  case 178: /* cat2_opc_1src: T_OP_FLOOR_F  */
#line 777 "ir3_parser.y"
                                  { new_instr(OPC_FLOOR_F); }
#line 3846 "ir3_parser.c"
    break;

  case 179: /* cat2_opc_1src: T_OP_CEIL_F  */
#line 778 "ir3_parser.y"
                                  { new_instr(OPC_CEIL_F); }
#line 3852 "ir3_parser.c"
    break;

  case 180: /* cat2_opc_1src: T_OP_RNDNE_F  */
#line 779 "ir3_parser.y"
                                  { new_instr(OPC_RNDNE_F); }
#line 3858 "ir3_parser.c"
    break;

  case 181: /* cat2_opc_1src: T_OP_RNDAZ_F  */
#line 780 "ir3_parser.y"
                                  { new_instr(OPC_RNDAZ_F); }
#line 3864 "ir3_parser.c"
    break;

  case 182: /* cat2_opc_1src: T_OP_TRUNC_F  */
#line 781 "ir3_parser.y"
                                  { new_instr(OPC_TRUNC_F); }
#line 3870 "ir3_parser.c"
    break;

  case 183: /* cat2_opc_1src: T_OP_NOT_B  */
#line 782 "ir3_parser.y"
                                  { new_instr(OPC_NOT_B); }
#line 3876 "ir3_parser.c"
    break;

  case 184: /* cat2_opc_1src: T_OP_BFREV_B  */
#line 783 "ir3_parser.y"
                                  { new_instr(OPC_BFREV_B); }
#line 3882 "ir3_parser.c"
    break;

  case 185: /* cat2_opc_1src: T_OP_SETRM  */
#line 784 "ir3_parser.y"
                                  { new_instr(OPC_SETRM); }
#line 3888 "ir3_parser.c"
    break;

  case 186: /* cat2_opc_1src: T_OP_CBITS_B  */
#line 785 "ir3_parser.y"
                                  { new_instr(OPC_CBITS_B); }
#line 3894 "ir3_parser.c"
    break;

  case 187: /* cat2_opc_2src_cnd: T_OP_CMPS_F  */
#line 787 "ir3_parser.y"
                                  { new_instr(OPC_CMPS_F); }
#line 3900 "ir3_parser.c"
    break;

  case 188: /* cat2_opc_2src_cnd: T_OP_CMPS_U  */
#line 788 "ir3_parser.y"
                                  { new_instr(OPC_CMPS_U); }
#line 3906 "ir3_parser.c"
    break;

  case 189: /* cat2_opc_2src_cnd: T_OP_CMPS_S  */
#line 789 "ir3_parser.y"
                                  { new_instr(OPC_CMPS_S); }
#line 3912 "ir3_parser.c"
    break;

  case 190: /* cat2_opc_2src_cnd: T_OP_CMPV_F  */
#line 790 "ir3_parser.y"
                                  { new_instr(OPC_CMPV_F); }
#line 3918 "ir3_parser.c"
    break;

  case 191: /* cat2_opc_2src_cnd: T_OP_CMPV_U  */
#line 791 "ir3_parser.y"
                                  { new_instr(OPC_CMPV_U); }
#line 3924 "ir3_parser.c"
    break;

  case 192: /* cat2_opc_2src_cnd: T_OP_CMPV_S  */
#line 792 "ir3_parser.y"
                                  { new_instr(OPC_CMPV_S); }
#line 3930 "ir3_parser.c"
    break;

  case 193: /* cat2_opc_2src_cnd: T_OP_ADD_F T_DIV2  */
#line 793 "ir3_parser.y"
                                     { new_instr(OPC_ADD_F_DIV2); }
#line 3936 "ir3_parser.c"
    break;

  case 194: /* cat2_opc_2src_cnd: T_OP_ADD_F T_MUL2  */
#line 794 "ir3_parser.y"
                                     { new_instr(OPC_ADD_F_MUL2); }
#line 3942 "ir3_parser.c"
    break;

  case 195: /* cat2_opc_2src_cnd: T_OP_MUL_F T_DIV2  */
#line 795 "ir3_parser.y"
                                     { new_instr(OPC_MUL_F_DIV2); }
#line 3948 "ir3_parser.c"
    break;

  case 196: /* cat2_opc_2src_cnd: T_OP_MUL_F T_MUL2  */
#line 796 "ir3_parser.y"
                                     { new_instr(OPC_MUL_F_MUL2); }
#line 3954 "ir3_parser.c"
    break;

  case 197: /* cat2_opc_2src: T_OP_ADD_F  */
#line 798 "ir3_parser.y"
                                  { new_instr(OPC_ADD_F); }
#line 3960 "ir3_parser.c"
    break;

  case 198: /* cat2_opc_2src: T_OP_MIN_F  */
#line 799 "ir3_parser.y"
                                  { new_instr(OPC_MIN_F); }
#line 3966 "ir3_parser.c"
    break;

  case 199: /* cat2_opc_2src: T_OP_MAX_F  */
#line 800 "ir3_parser.y"
                                  { new_instr(OPC_MAX_F); }
#line 3972 "ir3_parser.c"
    break;

  case 200: /* cat2_opc_2src: T_OP_MUL_F  */
#line 801 "ir3_parser.y"
                                  { new_instr(OPC_MUL_F); }
#line 3978 "ir3_parser.c"
    break;

  case 201: /* cat2_opc_2src: T_OP_ADD_U  */
#line 802 "ir3_parser.y"
                                  { new_instr(OPC_ADD_U); }
#line 3984 "ir3_parser.c"
    break;

  case 202: /* cat2_opc_2src: T_OP_ADD_S  */
#line 803 "ir3_parser.y"
                                  { new_instr(OPC_ADD_S); }
#line 3990 "ir3_parser.c"
    break;

  case 203: /* cat2_opc_2src: T_OP_SUB_U  */
#line 804 "ir3_parser.y"
                                  { new_instr(OPC_SUB_U); }
#line 3996 "ir3_parser.c"
    break;

  case 204: /* cat2_opc_2src: T_OP_SUB_S  */
#line 805 "ir3_parser.y"
                                  { new_instr(OPC_SUB_S); }
#line 4002 "ir3_parser.c"
    break;

  case 205: /* cat2_opc_2src: T_OP_MIN_U  */
#line 806 "ir3_parser.y"
                                  { new_instr(OPC_MIN_U); }
#line 4008 "ir3_parser.c"
    break;

  case 206: /* cat2_opc_2src: T_OP_MIN_S  */
#line 807 "ir3_parser.y"
                                  { new_instr(OPC_MIN_S); }
#line 4014 "ir3_parser.c"
    break;

  case 207: /* cat2_opc_2src: T_OP_MAX_U  */
#line 808 "ir3_parser.y"
                                  { new_instr(OPC_MAX_U); }
#line 4020 "ir3_parser.c"
    break;

  case 208: /* cat2_opc_2src: T_OP_MAX_S  */
#line 809 "ir3_parser.y"
                                  { new_instr(OPC_MAX_S); }
#line 4026 "ir3_parser.c"
    break;

  case 209: /* cat2_opc_2src: T_OP_AND_B  */
#line 810 "ir3_parser.y"
                                  { new_instr(OPC_AND_B); }
#line 4032 "ir3_parser.c"
    break;

  case 210: /* cat2_opc_2src: T_OP_OR_B  */
#line 811 "ir3_parser.y"
                                  { new_instr(OPC_OR_B); }
#line 4038 "ir3_parser.c"
    break;

  case 211: /* cat2_opc_2src: T_OP_XOR_B  */
#line 812 "ir3_parser.y"
                                  { new_instr(OPC_XOR_B); }
#line 4044 "ir3_parser.c"
    break;

  case 212: /* cat2_opc_2src: T_OP_MUL_U24  */
#line 813 "ir3_parser.y"
                                  { new_instr(OPC_MUL_U24); }
#line 4050 "ir3_parser.c"
    break;

  case 213: /* cat2_opc_2src: T_OP_MUL_S24  */
#line 814 "ir3_parser.y"
                                  { new_instr(OPC_MUL_S24); }
#line 4056 "ir3_parser.c"
    break;

  case 214: /* cat2_opc_2src: T_OP_MULL_U  */
#line 815 "ir3_parser.y"
                                  { new_instr(OPC_MULL_U); }
#line 4062 "ir3_parser.c"
    break;

  case 215: /* cat2_opc_2src: T_OP_SHL_B  */
#line 816 "ir3_parser.y"
                                  { new_instr(OPC_SHL_B); }
#line 4068 "ir3_parser.c"
    break;

  case 216: /* cat2_opc_2src: T_OP_SHR_B  */
#line 817 "ir3_parser.y"
                                  { new_instr(OPC_SHR_B); }
#line 4074 "ir3_parser.c"
    break;

  case 217: /* cat2_opc_2src: T_OP_ASHR_B  */
#line 818 "ir3_parser.y"
                                  { new_instr(OPC_ASHR_B); }
#line 4080 "ir3_parser.c"
    break;

  case 218: /* cat2_opc_2src: T_OP_BARY_F  */
#line 819 "ir3_parser.y"
                                  { new_instr(OPC_BARY_F); }
#line 4086 "ir3_parser.c"
    break;

  case 219: /* cat2_opc_2src: T_OP_FLAT_B  */
#line 820 "ir3_parser.y"
                                  { new_instr(OPC_FLAT_B); }
#line 4092 "ir3_parser.c"
    break;

  case 220: /* cat2_opc_2src: T_OP_MGEN_B  */
#line 821 "ir3_parser.y"
                                  { new_instr(OPC_MGEN_B); }
#line 4098 "ir3_parser.c"
    break;

  case 221: /* cat2_opc_2src: T_OP_GETBIT_B  */
#line 822 "ir3_parser.y"
                                  { new_instr(OPC_GETBIT_B); }
#line 4104 "ir3_parser.c"
    break;

  case 222: /* cat2_opc_2src: T_OP_SHB  */
#line 823 "ir3_parser.y"
                                  { new_instr(OPC_SHB); }
#line 4110 "ir3_parser.c"
    break;

  case 223: /* cat2_opc_2src: T_OP_MSAD  */
#line 824 "ir3_parser.y"
                                  { new_instr(OPC_MSAD); }
#line 4116 "ir3_parser.c"
    break;

  case 224: /* cond: T_LT  */
#line 826 "ir3_parser.y"
                                  { instr->cat2.condition = IR3_COND_LT; }
#line 4122 "ir3_parser.c"
    break;

  case 225: /* cond: T_LE  */
#line 827 "ir3_parser.y"
                                  { instr->cat2.condition = IR3_COND_LE; }
#line 4128 "ir3_parser.c"
    break;

  case 226: /* cond: T_GT  */
#line 828 "ir3_parser.y"
                                  { instr->cat2.condition = IR3_COND_GT; }
#line 4134 "ir3_parser.c"
    break;

  case 227: /* cond: T_GE  */
#line 829 "ir3_parser.y"
                                  { instr->cat2.condition = IR3_COND_GE; }
#line 4140 "ir3_parser.c"
    break;

  case 228: /* cond: T_EQ  */
#line 830 "ir3_parser.y"
                                  { instr->cat2.condition = IR3_COND_EQ; }
#line 4146 "ir3_parser.c"
    break;

  case 229: /* cond: T_NE  */
#line 831 "ir3_parser.y"
                                  { instr->cat2.condition = IR3_COND_NE; }
#line 4152 "ir3_parser.c"
    break;

  case 233: /* cat3_dp_signedness: '.' T_MIXED  */
#line 837 "ir3_parser.y"
                                 { instr->cat3.signedness = IR3_SRC_MIXED; }
#line 4158 "ir3_parser.c"
    break;

  case 234: /* cat3_dp_signedness: '.' T_UNSIGNED  */
#line 838 "ir3_parser.y"
                                 { instr->cat3.signedness = IR3_SRC_UNSIGNED; }
#line 4164 "ir3_parser.c"
    break;

  case 235: /* cat3_dp_pack: '.' T_LOW  */
#line 840 "ir3_parser.y"
                                 { instr->cat3.packed = IR3_SRC_PACKED_LOW; }
#line 4170 "ir3_parser.c"
    break;

  case 236: /* cat3_dp_pack: '.' T_HIGH  */
#line 841 "ir3_parser.y"
                                 { instr->cat3.packed = IR3_SRC_PACKED_HIGH; }
#line 4176 "ir3_parser.c"
    break;

  case 237: /* cat3_opc: T_OP_MAD_F16 T_MUL2  */
#line 843 "ir3_parser.y"
                                       { new_instr(OPC_MAD_F16_MUL2); }
#line 4182 "ir3_parser.c"
    break;

  case 238: /* cat3_opc: T_OP_MAD_F32 T_MUL2  */
#line 844 "ir3_parser.y"
                                       { new_instr(OPC_MAD_F32_MUL2); }
#line 4188 "ir3_parser.c"
    break;

  case 239: /* cat3_opc: T_OP_MAD_F16 T_DIV2  */
#line 845 "ir3_parser.y"
                                       { new_instr(OPC_MAD_F16_DIV2); }
#line 4194 "ir3_parser.c"
    break;

  case 240: /* cat3_opc: T_OP_MAD_F32 T_DIV2  */
#line 846 "ir3_parser.y"
                                       { new_instr(OPC_MAD_F32_DIV2); }
#line 4200 "ir3_parser.c"
    break;

  case 241: /* cat3_opc: T_OP_MAD_F16  */
#line 847 "ir3_parser.y"
                                  { new_instr(OPC_MAD_F16); }
#line 4206 "ir3_parser.c"
    break;

  case 242: /* cat3_opc: T_OP_MAD_F32  */
#line 848 "ir3_parser.y"
                                  { new_instr(OPC_MAD_F32); }
#line 4212 "ir3_parser.c"
    break;

  case 243: /* cat3_opc: T_OP_SEL_F16  */
#line 849 "ir3_parser.y"
                                  { new_instr(OPC_SEL_F16); }
#line 4218 "ir3_parser.c"
    break;

  case 244: /* cat3_opc: T_OP_SEL_F32  */
#line 850 "ir3_parser.y"
                                  { new_instr(OPC_SEL_F32); }
#line 4224 "ir3_parser.c"
    break;

  case 245: /* cat3_imm_reg_opc: T_OP_SHRM  */
#line 852 "ir3_parser.y"
                                  { new_instr(OPC_SHRM); }
#line 4230 "ir3_parser.c"
    break;

  case 246: /* cat3_imm_reg_opc: T_OP_SHLM  */
#line 853 "ir3_parser.y"
                                  { new_instr(OPC_SHLM); }
#line 4236 "ir3_parser.c"
    break;

  case 247: /* cat3_imm_reg_opc: T_OP_SHRG  */
#line 854 "ir3_parser.y"
                                  { new_instr(OPC_SHRG); }
#line 4242 "ir3_parser.c"
    break;

  case 248: /* cat3_imm_reg_opc: T_OP_SHLG  */
#line 855 "ir3_parser.y"
                                  { new_instr(OPC_SHLG); }
#line 4248 "ir3_parser.c"
    break;

  case 249: /* cat3_imm_reg_opc: T_OP_ANDG  */
#line 856 "ir3_parser.y"
                                  { new_instr(OPC_ANDG); }
#line 4254 "ir3_parser.c"
    break;

  case 251: /* cat3_reg_or_const_or_rel_opc: T_OP_MAD_U16  */
#line 859 "ir3_parser.y"
                                  { new_instr(OPC_MAD_U16); }
#line 4260 "ir3_parser.c"
    break;

  case 252: /* cat3_reg_or_const_or_rel_opc: T_OP_MADSH_U16  */
#line 860 "ir3_parser.y"
                                  { new_instr(OPC_MADSH_U16); }
#line 4266 "ir3_parser.c"
    break;

  case 253: /* cat3_reg_or_const_or_rel_opc: T_OP_MAD_S16  */
#line 861 "ir3_parser.y"
                                  { new_instr(OPC_MAD_S16); }
#line 4272 "ir3_parser.c"
    break;

  case 254: /* cat3_reg_or_const_or_rel_opc: T_OP_MADSH_M16  */
#line 862 "ir3_parser.y"
                                  { new_instr(OPC_MADSH_M16); }
#line 4278 "ir3_parser.c"
    break;

  case 255: /* cat3_reg_or_const_or_rel_opc: T_OP_MAD_U24  */
#line 863 "ir3_parser.y"
                                  { new_instr(OPC_MAD_U24); }
#line 4284 "ir3_parser.c"
    break;

  case 256: /* cat3_reg_or_const_or_rel_opc: T_OP_MAD_S24  */
#line 864 "ir3_parser.y"
                                  { new_instr(OPC_MAD_S24); }
#line 4290 "ir3_parser.c"
    break;

  case 257: /* cat3_reg_or_const_or_rel_opc: T_OP_SEL_B16  */
#line 865 "ir3_parser.y"
                                  { new_instr(OPC_SEL_B16); }
#line 4296 "ir3_parser.c"
    break;

  case 258: /* cat3_reg_or_const_or_rel_opc: T_OP_SEL_B32  */
#line 866 "ir3_parser.y"
                                  { new_instr(OPC_SEL_B32); }
#line 4302 "ir3_parser.c"
    break;

  case 259: /* cat3_reg_or_const_or_rel_opc: T_OP_SEL_S16  */
#line 867 "ir3_parser.y"
                                  { new_instr(OPC_SEL_S16); }
#line 4308 "ir3_parser.c"
    break;

  case 260: /* cat3_reg_or_const_or_rel_opc: T_OP_SEL_S32  */
#line 868 "ir3_parser.y"
                                  { new_instr(OPC_SEL_S32); }
#line 4314 "ir3_parser.c"
    break;

  case 261: /* cat3_reg_or_const_or_rel_opc: T_OP_SAD_S16  */
#line 869 "ir3_parser.y"
                                  { new_instr(OPC_SAD_S16); }
#line 4320 "ir3_parser.c"
    break;

  case 262: /* cat3_reg_or_const_or_rel_opc: T_OP_SAD_S32  */
#line 870 "ir3_parser.y"
                                  { new_instr(OPC_SAD_S32); }
#line 4326 "ir3_parser.c"
    break;

  case 263: /* cat3_wmm: T_OP_WMM  */
#line 872 "ir3_parser.y"
                                  { new_instr(OPC_WMM); }
#line 4332 "ir3_parser.c"
    break;

  case 264: /* cat3_wmm: T_OP_WMM_ACCU  */
#line 873 "ir3_parser.y"
                                  { new_instr(OPC_WMM_ACCU); }
#line 4338 "ir3_parser.c"
    break;

  case 265: /* cat3_dp: T_OP_DP2ACC  */
#line 875 "ir3_parser.y"
                                  { new_instr(OPC_DP2ACC); }
#line 4344 "ir3_parser.c"
    break;

  case 266: /* cat3_dp: T_OP_DP4ACC  */
#line 876 "ir3_parser.y"
                                  { new_instr(OPC_DP4ACC); }
#line 4350 "ir3_parser.c"
    break;

  case 268: /* src_reg_or_const_or_rel_or_flut: flut_immed  */
#line 879 "ir3_parser.y"
                                  { new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[0].num); }
#line 4356 "ir3_parser.c"
    break;

  case 269: /* src_reg_or_const_or_rel_or_flut: 'h' flut_immed  */
#line 880 "ir3_parser.y"
                                  { new_src(0, IR3_REG_IMMED | IR3_REG_HALF)->uim_val = (yyvsp[0].num); }
#line 4362 "ir3_parser.c"
    break;

  case 275: /* cat4_opc: T_OP_RCP  */
#line 888 "ir3_parser.y"
                                  { new_instr(OPC_RCP); }
#line 4368 "ir3_parser.c"
    break;

  case 276: /* cat4_opc: T_OP_RSQ  */
#line 889 "ir3_parser.y"
                                  { new_instr(OPC_RSQ); }
#line 4374 "ir3_parser.c"
    break;

  case 277: /* cat4_opc: T_OP_LOG2  */
#line 890 "ir3_parser.y"
                                  { new_instr(OPC_LOG2); }
#line 4380 "ir3_parser.c"
    break;

  case 278: /* cat4_opc: T_OP_EXP2  */
#line 891 "ir3_parser.y"
                                  { new_instr(OPC_EXP2); }
#line 4386 "ir3_parser.c"
    break;

  case 279: /* cat4_opc: T_OP_SIN  */
#line 892 "ir3_parser.y"
                                  { new_instr(OPC_SIN); }
#line 4392 "ir3_parser.c"
    break;

  case 280: /* cat4_opc: T_OP_COS  */
#line 893 "ir3_parser.y"
                                  { new_instr(OPC_COS); }
#line 4398 "ir3_parser.c"
    break;

  case 281: /* cat4_opc: T_OP_SQRT  */
#line 894 "ir3_parser.y"
                                  { new_instr(OPC_SQRT); }
#line 4404 "ir3_parser.c"
    break;

  case 282: /* cat4_opc: T_OP_HRSQ  */
#line 895 "ir3_parser.y"
                                  { new_instr(OPC_HRSQ); }
#line 4410 "ir3_parser.c"
    break;

  case 283: /* cat4_opc: T_OP_HLOG2  */
#line 896 "ir3_parser.y"
                                  { new_instr(OPC_HLOG2); }
#line 4416 "ir3_parser.c"
    break;

  case 284: /* cat4_opc: T_OP_HEXP2  */
#line 897 "ir3_parser.y"
                                  { new_instr(OPC_HEXP2); }
#line 4422 "ir3_parser.c"
    break;

  case 286: /* cat5_opc_dsxypp: T_OP_DSXPP_1  */
#line 901 "ir3_parser.y"
                                  { new_instr(OPC_DSXPP_1)->cat5.type = TYPE_F32; }
#line 4428 "ir3_parser.c"
    break;

  case 287: /* cat5_opc_dsxypp: T_OP_DSYPP_1  */
#line 902 "ir3_parser.y"
                                  { new_instr(OPC_DSYPP_1)->cat5.type = TYPE_F32; }
#line 4434 "ir3_parser.c"
    break;

  case 288: /* cat5_opc_isam: T_OP_ISAM  */
#line 904 "ir3_parser.y"
                                  { new_instr(OPC_ISAM)->flags |= IR3_INSTR_INV_1D; }
#line 4440 "ir3_parser.c"
    break;

  case 289: /* cat5_opc: T_OP_ISAML  */
#line 906 "ir3_parser.y"
                                  { new_instr(OPC_ISAML); }
#line 4446 "ir3_parser.c"
    break;

  case 290: /* cat5_opc: T_OP_ISAMM  */
#line 907 "ir3_parser.y"
                                  { new_instr(OPC_ISAMM); }
#line 4452 "ir3_parser.c"
    break;

  case 291: /* cat5_opc: T_OP_SAM  */
#line 908 "ir3_parser.y"
                                  { new_instr(OPC_SAM); }
#line 4458 "ir3_parser.c"
    break;

  case 292: /* cat5_opc: T_OP_SAMB  */
#line 909 "ir3_parser.y"
                                  { new_instr(OPC_SAMB); }
#line 4464 "ir3_parser.c"
    break;

  case 293: /* cat5_opc: T_OP_SAML  */
#line 910 "ir3_parser.y"
                                  { new_instr(OPC_SAML); }
#line 4470 "ir3_parser.c"
    break;

  case 294: /* cat5_opc: T_OP_SAMGQ  */
#line 911 "ir3_parser.y"
                                  { new_instr(OPC_SAMGQ); }
#line 4476 "ir3_parser.c"
    break;

  case 295: /* cat5_opc: T_OP_GETLOD  */
#line 912 "ir3_parser.y"
                                  { new_instr(OPC_GETLOD); }
#line 4482 "ir3_parser.c"
    break;

  case 296: /* cat5_opc: T_OP_CONV  */
#line 913 "ir3_parser.y"
                                  { new_instr(OPC_CONV); }
#line 4488 "ir3_parser.c"
    break;

  case 297: /* cat5_opc: T_OP_CONVM  */
#line 914 "ir3_parser.y"
                                  { new_instr(OPC_CONVM); }
#line 4494 "ir3_parser.c"
    break;

  case 298: /* cat5_opc: T_OP_GETSIZE  */
#line 915 "ir3_parser.y"
                                  { new_instr(OPC_GETSIZE); }
#line 4500 "ir3_parser.c"
    break;

  case 299: /* cat5_opc: T_OP_GETBUF  */
#line 916 "ir3_parser.y"
                                  { new_instr(OPC_GETBUF); }
#line 4506 "ir3_parser.c"
    break;

  case 300: /* cat5_opc: T_OP_GETPOS  */
#line 917 "ir3_parser.y"
                                  { new_instr(OPC_GETPOS); }
#line 4512 "ir3_parser.c"
    break;

  case 301: /* cat5_opc: T_OP_GETINFO  */
#line 918 "ir3_parser.y"
                                  { new_instr(OPC_GETINFO); }
#line 4518 "ir3_parser.c"
    break;

  case 302: /* cat5_opc: T_OP_DSX  */
#line 919 "ir3_parser.y"
                                  { new_instr(OPC_DSX); }
#line 4524 "ir3_parser.c"
    break;

  case 303: /* cat5_opc: T_OP_DSY  */
#line 920 "ir3_parser.y"
                                  { new_instr(OPC_DSY); }
#line 4530 "ir3_parser.c"
    break;

  case 304: /* cat5_opc: T_OP_GATHER4R  */
#line 921 "ir3_parser.y"
                                  { new_instr(OPC_GATHER4R); }
#line 4536 "ir3_parser.c"
    break;

  case 305: /* cat5_opc: T_OP_GATHER4G  */
#line 922 "ir3_parser.y"
                                  { new_instr(OPC_GATHER4G); }
#line 4542 "ir3_parser.c"
    break;

  case 306: /* cat5_opc: T_OP_GATHER4B  */
#line 923 "ir3_parser.y"
                                  { new_instr(OPC_GATHER4B); }
#line 4548 "ir3_parser.c"
    break;

  case 307: /* cat5_opc: T_OP_GATHER4A  */
#line 924 "ir3_parser.y"
                                  { new_instr(OPC_GATHER4A); }
#line 4554 "ir3_parser.c"
    break;

  case 308: /* cat5_opc: T_OP_SAMGP0  */
#line 925 "ir3_parser.y"
                                  { new_instr(OPC_SAMGP0); }
#line 4560 "ir3_parser.c"
    break;

  case 309: /* cat5_opc: T_OP_SAMGP1  */
#line 926 "ir3_parser.y"
                                  { new_instr(OPC_SAMGP1); }
#line 4566 "ir3_parser.c"
    break;

  case 310: /* cat5_opc: T_OP_SAMGP2  */
#line 927 "ir3_parser.y"
                                  { new_instr(OPC_SAMGP2); }
#line 4572 "ir3_parser.c"
    break;

  case 311: /* cat5_opc: T_OP_SAMGP3  */
#line 928 "ir3_parser.y"
                                  { new_instr(OPC_SAMGP3); }
#line 4578 "ir3_parser.c"
    break;

  case 312: /* cat5_opc: T_OP_RGETPOS  */
#line 929 "ir3_parser.y"
                                  { new_instr(OPC_RGETPOS); }
#line 4584 "ir3_parser.c"
    break;

  case 313: /* cat5_opc: T_OP_RGETINFO  */
#line 930 "ir3_parser.y"
                                  { new_instr(OPC_RGETINFO); }
#line 4590 "ir3_parser.c"
    break;

  case 314: /* cat5_opc: T_OP_BRCST_A  */
#line 931 "ir3_parser.y"
                                  { new_instr(OPC_BRCST_ACTIVE); }
#line 4596 "ir3_parser.c"
    break;

  case 315: /* cat5_opc: T_OP_QSHUFFLE_BRCST  */
#line 932 "ir3_parser.y"
                                       { new_instr(OPC_QUAD_SHUFFLE_BRCST); }
#line 4602 "ir3_parser.c"
    break;

  case 316: /* cat5_opc: T_OP_QSHUFFLE_H  */
#line 933 "ir3_parser.y"
                                       { new_instr(OPC_QUAD_SHUFFLE_HORIZ); }
#line 4608 "ir3_parser.c"
    break;

  case 317: /* cat5_opc: T_OP_QSHUFFLE_V  */
#line 934 "ir3_parser.y"
                                       { new_instr(OPC_QUAD_SHUFFLE_VERT); }
#line 4614 "ir3_parser.c"
    break;

  case 318: /* cat5_opc: T_OP_QSHUFFLE_DIAG  */
#line 935 "ir3_parser.y"
                                       { new_instr(OPC_QUAD_SHUFFLE_DIAG); }
#line 4620 "ir3_parser.c"
    break;

  case 319: /* cat5_opc: T_OP_IMG_BINDLESS_PCMN  */
#line 936 "ir3_parser.y"
                                          { new_instr(OPC_IMG_BINDLESS_PCMN); }
#line 4626 "ir3_parser.c"
    break;

  case 320: /* cat5_opc: T_OP_IMG_BINDLESS_HOF  */
#line 937 "ir3_parser.y"
                                              { new_instr(OPC_IMG_BINDLESS_HOF); }
#line 4632 "ir3_parser.c"
    break;

  case 321: /* cat5_opc: T_OP_IMG_BINDLESS  */
#line 938 "ir3_parser.y"
                                         { new_instr(OPC_IMG_BINDLESS); }
#line 4638 "ir3_parser.c"
    break;

  case 322: /* cat5_matchmode: '.' T_SAD  */
#line 940 "ir3_parser.y"
                               { instr->cat5.match_mode = IR3_MATCH_MODE_SAD; }
#line 4644 "ir3_parser.c"
    break;

  case 323: /* cat5_matchmode: '.' T_SSD  */
#line 941 "ir3_parser.y"
                               { instr->cat5.match_mode = IR3_MATCH_MODE_SSD; }
#line 4650 "ir3_parser.c"
    break;

  case 324: /* cat5_flag: '.' T_3D  */
#line 943 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_3D; }
#line 4656 "ir3_parser.c"
    break;

  case 325: /* cat5_flag: '.' 'a'  */
#line 944 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_A; }
#line 4662 "ir3_parser.c"
    break;

  case 326: /* cat5_flag: '.' 'o'  */
#line 945 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_O; }
#line 4668 "ir3_parser.c"
    break;

  case 327: /* cat5_flag: '.' 'p'  */
#line 946 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_P; }
#line 4674 "ir3_parser.c"
    break;

  case 328: /* cat5_flag: '.' 's'  */
#line 947 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_S; }
#line 4680 "ir3_parser.c"
    break;

  case 329: /* cat5_flag: '.' T_S2EN  */
#line 948 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_S2EN; }
#line 4686 "ir3_parser.c"
    break;

  case 330: /* cat5_flag: '.' T_1D  */
#line 949 "ir3_parser.y"
                                  { instr->flags &= ~IR3_INSTR_INV_1D; }
#line 4692 "ir3_parser.c"
    break;

  case 331: /* cat5_flag: '.' T_UNIFORM  */
#line 950 "ir3_parser.y"
                                  { }
#line 4698 "ir3_parser.c"
    break;

  case 332: /* cat5_flag: '.' T_NONUNIFORM  */
#line 951 "ir3_parser.y"
                                     { instr->flags |= IR3_INSTR_NONUNIF; }
#line 4704 "ir3_parser.c"
    break;

  case 333: /* cat5_flag: '.' T_BASE  */
#line 952 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_B; instr->cat5.tex_base = (yyvsp[0].tok); }
#line 4710 "ir3_parser.c"
    break;

  case 334: /* cat5_flag: '.' T_W  */
#line 953 "ir3_parser.y"
                                  { instr->cat5.cluster_size = (yyvsp[0].num); }
#line 4716 "ir3_parser.c"
    break;

  case 335: /* cat5_flag: '.' T_RCK  */
#line 954 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_RCK; }
#line 4722 "ir3_parser.c"
    break;

  case 336: /* cat5_flag: '.' T_CLP  */
#line 955 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_CLP; }
#line 4728 "ir3_parser.c"
    break;

  case 340: /* cat5_samp: T_SAMP  */
#line 960 "ir3_parser.y"
                                  { instr->cat5.samp = (yyvsp[0].tok); }
#line 4734 "ir3_parser.c"
    break;

  case 341: /* cat5_tex: T_TEX  */
#line 961 "ir3_parser.y"
                                  { instr->cat5.tex = (yyvsp[0].tok); }
#line 4740 "ir3_parser.c"
    break;

  case 342: /* cat5_type: '(' type ')'  */
#line 962 "ir3_parser.y"
                                  { instr->cat5.type = (yyvsp[-1].type); }
#line 4746 "ir3_parser.c"
    break;

  case 343: /* cat5_type: %empty  */
#line 963 "ir3_parser.y"
                                  { }
#line 4752 "ir3_parser.c"
    break;

  case 344: /* cat5_a1: src_a1  */
#line 964 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_A1EN; }
#line 4758 "ir3_parser.c"
    break;

  case 360: /* cat5_instr: cat5_opc_isam '.' 'v' cat5_flags cat5_type dst_reg ',' src_gpr src_uoffset ',' cat5_samp_tex_all  */
#line 983 "ir3_parser.y"
                                                                                                                    { instr->flags |= IR3_INSTR_V; }
#line 4764 "ir3_parser.c"
    break;

  case 361: /* cat5_instr: T_OP_TCINV  */
#line 984 "ir3_parser.y"
                              { new_instr(OPC_TCINV); }
#line 4770 "ir3_parser.c"
    break;

  case 362: /* cat6_typed: '.' T_UNTYPED  */
#line 986 "ir3_parser.y"
                                  { instr->cat6.typed = 0; }
#line 4776 "ir3_parser.c"
    break;

  case 363: /* cat6_typed: '.' T_TYPED  */
#line 987 "ir3_parser.y"
                                  { instr->cat6.typed = 1; }
#line 4782 "ir3_parser.c"
    break;

  case 364: /* cat6_dim: '.' T_1D  */
#line 989 "ir3_parser.y"
                             { instr->cat6.d = 1; }
#line 4788 "ir3_parser.c"
    break;

  case 365: /* cat6_dim: '.' T_2D  */
#line 990 "ir3_parser.y"
                             { instr->cat6.d = 2; }
#line 4794 "ir3_parser.c"
    break;

  case 366: /* cat6_dim: '.' T_3D  */
#line 991 "ir3_parser.y"
                             { instr->cat6.d = 3; }
#line 4800 "ir3_parser.c"
    break;

  case 367: /* cat6_dim: '.' T_4D  */
#line 992 "ir3_parser.y"
                             { instr->cat6.d = 4; }
#line 4806 "ir3_parser.c"
    break;

  case 368: /* cat6_type: '.' type  */
#line 994 "ir3_parser.y"
                             { instr->cat6.type = (yyvsp[0].type); }
#line 4812 "ir3_parser.c"
    break;

  case 369: /* cat6_imm_offset: offset  */
#line 995 "ir3_parser.y"
                             { new_src(0, IR3_REG_IMMED)->iim_val = (yyvsp[0].num); }
#line 4818 "ir3_parser.c"
    break;

  case 372: /* cat6_dst_offset: offset  */
#line 998 "ir3_parser.y"
                             { instr->cat6.dst_offset = (yyvsp[0].num); }
#line 4824 "ir3_parser.c"
    break;

  case 374: /* cat6_immed: integer  */
#line 1001 "ir3_parser.y"
                             { instr->cat6.iim_val = (yyvsp[0].num); }
#line 4830 "ir3_parser.c"
    break;

  case 377: /* cat6_src_shift: '<' '<' integer  */
#line 1006 "ir3_parser.y"
                                {(yyval.unum) = (yyvsp[0].num);}
#line 4836 "ir3_parser.c"
    break;

  case 378: /* cat6_src_shift: %empty  */
#line 1007 "ir3_parser.y"
                                {(yyval.unum) = 0;}
#line 4842 "ir3_parser.c"
    break;

  case 379: /* cat6_a6xx_global_address_pt2: '(' '(' '(' src cat6_src_shift ')' offset ')' '<' '<' integer ')'  */
#line 1010 "ir3_parser.y"
                                                                                     {
                        illegal_syntax_from(7, "pre-a7xx global offset syntax");
                        new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[-7].unum);
                        new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[-5].num);
                   }
#line 4852 "ir3_parser.c"
    break;

  case 380: /* cat6_a6xx_global_address_pt2: '(' '(' src cat6_src_shift offset ')' '<' '<' integer ')'  */
#line 1015 "ir3_parser.y"
                                                                             {
                        illegal_syntax_from(7, "pre-a7xx global offset syntax");
                        new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[-6].unum);
                        new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[-5].num);
                   }
#line 4862 "ir3_parser.c"
    break;

  case 381: /* cat6_a6xx_global_address_pt2: '(' src cat6_src_shift ')'  */
#line 1020 "ir3_parser.y"
                                              {
                        illegal_syntax_from(7, "pre-a7xx global offset syntax");
                        // The shift contains the implicit type shift, subtract it.
                        new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[-1].unum) - cat6_type_shift();
                        new_src(0, IR3_REG_IMMED)->uim_val = 0;
                   }
#line 4873 "ir3_parser.c"
    break;

  case 382: /* cat6_a6xx_global_address_pt2: src offset  */
#line 1026 "ir3_parser.y"
                              {
                        if (variant->compiler->gen < 7) {
                            new_src(0, IR3_REG_IMMED)->uim_val = 0;
                            new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[0].num);
                        } else {
                            // Dummy src to smooth the difference between a6xx and a7xx
                            new_src(0, IR3_REG_IMMED)->uim_val = 0;
                            new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[0].num);
                        }
                   }
#line 4888 "ir3_parser.c"
    break;

  case 384: /* $@25: %empty  */
#line 1040 "ir3_parser.y"
                              { new_instr(OPC_LDG); }
#line 4894 "ir3_parser.c"
    break;

  case 386: /* $@26: %empty  */
#line 1041 "ir3_parser.y"
                              { new_instr(OPC_LDG_A); }
#line 4900 "ir3_parser.c"
    break;

  case 388: /* $@27: %empty  */
#line 1042 "ir3_parser.y"
                              { new_instr(OPC_LDG_K); }
#line 4906 "ir3_parser.c"
    break;

  case 390: /* $@28: %empty  */
#line 1043 "ir3_parser.y"
                              { new_instr(OPC_LDP); }
#line 4912 "ir3_parser.c"
    break;

  case 392: /* $@29: %empty  */
#line 1044 "ir3_parser.y"
                              { new_instr(OPC_LDL); }
#line 4918 "ir3_parser.c"
    break;

  case 394: /* $@30: %empty  */
#line 1045 "ir3_parser.y"
                              { new_instr(OPC_LDLW); }
#line 4924 "ir3_parser.c"
    break;

  case 396: /* $@31: %empty  */
#line 1046 "ir3_parser.y"
                              { new_instr(OPC_LDLV); }
#line 4930 "ir3_parser.c"
    break;

  case 397: /* $@32: %empty  */
#line 1046 "ir3_parser.y"
                                                                                                  {
                       new_src(0, IR3_REG_IMMED)->iim_val = (yyvsp[-1].num);
                   }
#line 4938 "ir3_parser.c"
    break;

  case 399: /* $@33: %empty  */
#line 1050 "ir3_parser.y"
                              { new_instr(OPC_STG); dummy_dst(); }
#line 4944 "ir3_parser.c"
    break;

  case 401: /* $@34: %empty  */
#line 1051 "ir3_parser.y"
                              { new_instr(OPC_STG_A); dummy_dst(); }
#line 4950 "ir3_parser.c"
    break;

  case 403: /* $@35: %empty  */
#line 1052 "ir3_parser.y"
                             { new_instr(OPC_STP); dummy_dst(); }
#line 4956 "ir3_parser.c"
    break;

  case 405: /* $@36: %empty  */
#line 1053 "ir3_parser.y"
                             { new_instr(OPC_STL); dummy_dst(); }
#line 4962 "ir3_parser.c"
    break;

  case 407: /* $@37: %empty  */
#line 1054 "ir3_parser.y"
                             { new_instr(OPC_STLW); dummy_dst(); }
#line 4968 "ir3_parser.c"
    break;

  case 409: /* $@38: %empty  */
#line 1056 "ir3_parser.y"
                             { new_instr(OPC_LDIB); }
#line 4974 "ir3_parser.c"
    break;

  case 411: /* $@39: %empty  */
#line 1057 "ir3_parser.y"
                             { new_instr(OPC_STIB); dummy_dst(); }
#line 4980 "ir3_parser.c"
    break;

  case 413: /* $@40: %empty  */
#line 1059 "ir3_parser.y"
                                 { new_instr(OPC_PREFETCH); new_dst(0,0); /* dummy dst */ }
#line 4986 "ir3_parser.c"
    break;

  case 415: /* cat6_atomic_opc: T_OP_ATOMIC_ADD  */
#line 1061 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_ADD); }
#line 4992 "ir3_parser.c"
    break;

  case 416: /* cat6_atomic_opc: T_OP_ATOMIC_SUB  */
#line 1062 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_SUB); }
#line 4998 "ir3_parser.c"
    break;

  case 417: /* cat6_atomic_opc: T_OP_ATOMIC_XCHG  */
#line 1063 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_XCHG); }
#line 5004 "ir3_parser.c"
    break;

  case 418: /* cat6_atomic_opc: T_OP_ATOMIC_INC  */
#line 1064 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_INC); }
#line 5010 "ir3_parser.c"
    break;

  case 419: /* cat6_atomic_opc: T_OP_ATOMIC_DEC  */
#line 1065 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_DEC); }
#line 5016 "ir3_parser.c"
    break;

  case 420: /* cat6_atomic_opc: T_OP_ATOMIC_CMPXCHG  */
#line 1066 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_CMPXCHG); }
#line 5022 "ir3_parser.c"
    break;

  case 421: /* cat6_atomic_opc: T_OP_ATOMIC_MIN  */
#line 1067 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_MIN); }
#line 5028 "ir3_parser.c"
    break;

  case 422: /* cat6_atomic_opc: T_OP_ATOMIC_MAX  */
#line 1068 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_MAX); }
#line 5034 "ir3_parser.c"
    break;

  case 423: /* cat6_atomic_opc: T_OP_ATOMIC_AND  */
#line 1069 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_AND); }
#line 5040 "ir3_parser.c"
    break;

  case 424: /* cat6_atomic_opc: T_OP_ATOMIC_OR  */
#line 1070 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_OR); }
#line 5046 "ir3_parser.c"
    break;

  case 425: /* cat6_atomic_opc: T_OP_ATOMIC_XOR  */
#line 1071 "ir3_parser.y"
                                       { new_instr(OPC_ATOMIC_XOR); }
#line 5052 "ir3_parser.c"
    break;

  case 426: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_ADD  */
#line 1073 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_ADD); }
#line 5058 "ir3_parser.c"
    break;

  case 427: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_SUB  */
#line 1074 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_SUB); }
#line 5064 "ir3_parser.c"
    break;

  case 428: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_XCHG  */
#line 1075 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_XCHG); }
#line 5070 "ir3_parser.c"
    break;

  case 429: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_INC  */
#line 1076 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_INC); }
#line 5076 "ir3_parser.c"
    break;

  case 430: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_DEC  */
#line 1077 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_DEC); }
#line 5082 "ir3_parser.c"
    break;

  case 431: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_CMPXCHG  */
#line 1078 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_CMPXCHG); }
#line 5088 "ir3_parser.c"
    break;

  case 432: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_MIN  */
#line 1079 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_MIN); }
#line 5094 "ir3_parser.c"
    break;

  case 433: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_MAX  */
#line 1080 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_MAX); }
#line 5100 "ir3_parser.c"
    break;

  case 434: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_AND  */
#line 1081 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_AND); }
#line 5106 "ir3_parser.c"
    break;

  case 435: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_OR  */
#line 1082 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_OR); }
#line 5112 "ir3_parser.c"
    break;

  case 436: /* cat6_a3xx_atomic_opc: T_OP_ATOMIC_S_XOR  */
#line 1083 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_S_XOR); }
#line 5118 "ir3_parser.c"
    break;

  case 437: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_ADD  */
#line 1085 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_ADD); }
#line 5124 "ir3_parser.c"
    break;

  case 438: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_SUB  */
#line 1086 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_SUB); }
#line 5130 "ir3_parser.c"
    break;

  case 439: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_XCHG  */
#line 1087 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_XCHG); }
#line 5136 "ir3_parser.c"
    break;

  case 440: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_INC  */
#line 1088 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_INC); }
#line 5142 "ir3_parser.c"
    break;

  case 441: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_DEC  */
#line 1089 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_DEC); }
#line 5148 "ir3_parser.c"
    break;

  case 442: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_CMPXCHG  */
#line 1090 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_CMPXCHG); }
#line 5154 "ir3_parser.c"
    break;

  case 443: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_MIN  */
#line 1091 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_MIN); }
#line 5160 "ir3_parser.c"
    break;

  case 444: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_MAX  */
#line 1092 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_MAX); }
#line 5166 "ir3_parser.c"
    break;

  case 445: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_AND  */
#line 1093 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_AND); }
#line 5172 "ir3_parser.c"
    break;

  case 446: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_OR  */
#line 1094 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_OR); }
#line 5178 "ir3_parser.c"
    break;

  case 447: /* cat6_a6xx_atomic_opc: T_OP_ATOMIC_G_XOR  */
#line 1095 "ir3_parser.y"
                                              { new_instr(OPC_ATOMIC_G_XOR); }
#line 5184 "ir3_parser.c"
    break;

  case 454: /* cat6_ibo_opc_1src: T_OP_RESINFO  */
#line 1107 "ir3_parser.y"
                                  { new_instr(OPC_RESINFO); }
#line 5190 "ir3_parser.c"
    break;

  case 455: /* cat6_ibo_opc_ldgb: T_OP_LDGB  */
#line 1109 "ir3_parser.y"
                                  { new_instr(OPC_LDGB); }
#line 5196 "ir3_parser.c"
    break;

  case 456: /* cat6_ibo_opc_stgb: T_OP_STGB  */
#line 1110 "ir3_parser.y"
                                  { new_instr(OPC_STGB); }
#line 5202 "ir3_parser.c"
    break;

  case 459: /* $@41: %empty  */
#line 1114 "ir3_parser.y"
                                                                                  { dummy_dst(); }
#line 5208 "ir3_parser.c"
    break;

  case 461: /* cat6_id_opc: T_OP_GETSPID  */
#line 1117 "ir3_parser.y"
                                { new_instr(OPC_GETSPID); }
#line 5214 "ir3_parser.c"
    break;

  case 462: /* cat6_id_opc: T_OP_GETWID  */
#line 1118 "ir3_parser.y"
                                { new_instr(OPC_GETWID); }
#line 5220 "ir3_parser.c"
    break;

  case 463: /* cat6_id_opc: T_OP_GETFIBERID  */
#line 1119 "ir3_parser.y"
                                   { new_instr(OPC_GETFIBERID); }
#line 5226 "ir3_parser.c"
    break;

  case 466: /* cat6_bindless_base: '.' T_BASE  */
#line 1124 "ir3_parser.y"
                              { instr->flags |= IR3_INSTR_B; instr->cat6.base = (yyvsp[0].tok); }
#line 5232 "ir3_parser.c"
    break;

  case 469: /* cat6_bindless_mode: T_NONUNIFORM cat6_bindless_base  */
#line 1128 "ir3_parser.y"
                                                   { instr->flags |= IR3_INSTR_NONUNIF; }
#line 5238 "ir3_parser.c"
    break;

  case 471: /* cat6_reg_or_immed: integer  */
#line 1131 "ir3_parser.y"
                           { new_src(0, IR3_REG_IMMED)->iim_val = (yyvsp[0].num); }
#line 5244 "ir3_parser.c"
    break;

  case 472: /* cat6_bindless_ibo_opc_1src: T_OP_RESINFO_B  */
#line 1133 "ir3_parser.y"
                                                 { new_instr(OPC_RESINFO); }
#line 5250 "ir3_parser.c"
    break;

  case 473: /* cat6_bindless_ibo_opc_1src: T_OP_RESBASE  */
#line 1134 "ir3_parser.y"
                                                 { new_instr(OPC_RESBASE); }
#line 5256 "ir3_parser.c"
    break;

  case 474: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_ADD  */
#line 1136 "ir3_parser.y"
                                                     { new_instr(OPC_ATOMIC_B_ADD); dummy_dst(); }
#line 5262 "ir3_parser.c"
    break;

  case 475: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_SUB  */
#line 1137 "ir3_parser.y"
                                            { new_instr(OPC_ATOMIC_B_SUB); dummy_dst(); }
#line 5268 "ir3_parser.c"
    break;

  case 476: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_XCHG  */
#line 1138 "ir3_parser.y"
                                            { new_instr(OPC_ATOMIC_B_XCHG); dummy_dst(); }
#line 5274 "ir3_parser.c"
    break;

  case 477: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_INC  */
#line 1139 "ir3_parser.y"
                                            { new_instr(OPC_ATOMIC_B_INC); dummy_dst(); }
#line 5280 "ir3_parser.c"
    break;

  case 478: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_DEC  */
#line 1140 "ir3_parser.y"
                                            { new_instr(OPC_ATOMIC_B_DEC); dummy_dst(); }
#line 5286 "ir3_parser.c"
    break;

  case 479: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_CMPXCHG  */
#line 1141 "ir3_parser.y"
                                            { new_instr(OPC_ATOMIC_B_CMPXCHG); dummy_dst(); }
#line 5292 "ir3_parser.c"
    break;

  case 480: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_MIN  */
#line 1142 "ir3_parser.y"
                                            { new_instr(OPC_ATOMIC_B_MIN); dummy_dst(); }
#line 5298 "ir3_parser.c"
    break;

  case 481: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_MAX  */
#line 1143 "ir3_parser.y"
                                            { new_instr(OPC_ATOMIC_B_MAX); dummy_dst(); }
#line 5304 "ir3_parser.c"
    break;

  case 482: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_AND  */
#line 1144 "ir3_parser.y"
                                            { new_instr(OPC_ATOMIC_B_AND); dummy_dst(); }
#line 5310 "ir3_parser.c"
    break;

  case 483: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_OR  */
#line 1145 "ir3_parser.y"
                                            { new_instr(OPC_ATOMIC_B_OR); dummy_dst(); }
#line 5316 "ir3_parser.c"
    break;

  case 484: /* cat6_bindless_ibo_opc_2src: T_OP_ATOMIC_B_XOR  */
#line 1146 "ir3_parser.y"
                                            { new_instr(OPC_ATOMIC_B_XOR); dummy_dst(); }
#line 5322 "ir3_parser.c"
    break;

  case 485: /* cat6_bindless_ibo_opc_3src: T_OP_STIB_B  */
#line 1148 "ir3_parser.y"
                                            { new_instr(OPC_STIB); dummy_dst(); }
#line 5328 "ir3_parser.c"
    break;

  case 486: /* cat6_bindless_ibo_opc_3src_dst: T_OP_LDIB_B  */
#line 1150 "ir3_parser.y"
                                                         { new_instr(OPC_LDIB); }
#line 5334 "ir3_parser.c"
    break;

  case 488: /* cat6_rck: T_RCK '.'  */
#line 1153 "ir3_parser.y"
                             { instr->flags |= IR3_INSTR_RCK; }
#line 5340 "ir3_parser.c"
    break;

  case 490: /* cat6_bindless_ibo: cat6_bindless_ibo_opc_2src cat6_typed cat6_dim cat6_type '.' cat6_immed '.' cat6_bindless_mode src_reg ',' cat6_reg_or_immed ',' cat6_reg_or_immed  */
#line 1156 "ir3_parser.y"
                                                                                                                                                                      { swap(instr->srcs[0], instr->srcs[2]); }
#line 5346 "ir3_parser.c"
    break;

  case 491: /* cat6_bindless_ibo: cat6_bindless_ibo_opc_3src cat6_typed cat6_dim cat6_type '.' cat6_immed '.' cat6_bindless_mode src_reg ',' cat6_reg_or_immed src_uoffset ',' cat6_reg_or_immed  */
#line 1157 "ir3_parser.y"
                                                                                                                                                                                  { swap(instr->srcs[0], instr->srcs[3]); }
#line 5352 "ir3_parser.c"
    break;

  case 492: /* cat6_bindless_ibo: cat6_bindless_ibo_opc_3src_dst cat6_typed cat6_dim cat6_type '.' cat6_rck cat6_immed '.' cat6_bindless_mode dst_reg ',' cat6_reg_or_immed src_uoffset ',' cat6_reg_or_immed  */
#line 1158 "ir3_parser.y"
                                                                                                                                                                                               { swap(instr->srcs[0], instr->srcs[2]); swap(instr->srcs[1], instr->srcs[2]); }
#line 5358 "ir3_parser.c"
    break;

  case 493: /* cat6_bindless_ldc_opc: T_OP_LDC  */
#line 1160 "ir3_parser.y"
                                 { new_instr(OPC_LDC); }
#line 5364 "ir3_parser.c"
    break;

  case 494: /* cat6_bindless_ldc_middle: T_OFFSET '.' cat6_immed '.' cat6_bindless_mode dst_reg  */
#line 1164 "ir3_parser.y"
                                                                               { instr->cat6.d = (yyvsp[-5].tok); }
#line 5370 "ir3_parser.c"
    break;

  case 495: /* cat6_bindless_ldc_middle: 'u' '.' T_OFFSET '.' cat6_immed '.' cat6_bindless_mode dst_reg  */
#line 1165 "ir3_parser.y"
                                                                                       { instr->flags |= IR3_INSTR_U; instr->cat6.d = (yyvsp[-5].tok); }
#line 5376 "ir3_parser.c"
    break;

  case 496: /* cat6_bindless_ldc_middle: cat6_immed '.' 'k' '.' cat6_bindless_mode 'c' '[' T_A1 ']'  */
#line 1166 "ir3_parser.y"
                                                                                   { instr->opc = OPC_LDC_K; }
#line 5382 "ir3_parser.c"
    break;

  case 497: /* cat6_bindless_ldc: cat6_bindless_ldc_opc '.' cat6_bindless_ldc_middle ',' cat6_reg_or_immed ',' cat6_reg_or_immed  */
#line 1168 "ir3_parser.y"
                                                                                                                  {
                      instr->cat6.type = TYPE_U32;
                      /* TODO cleanup ir3 src order: */
                      swap(instr->srcs[0], instr->srcs[1]);
                   }
#line 5392 "ir3_parser.c"
    break;

  case 498: /* const_dst: integer  */
#line 1174 "ir3_parser.y"
                          { new_src(0, IR3_REG_IMMED)->iim_val = (yyvsp[0].num); }
#line 5398 "ir3_parser.c"
    break;

  case 499: /* const_dst: T_A1  */
#line 1175 "ir3_parser.y"
                       { new_src(0, IR3_REG_IMMED)->iim_val = 0; instr->flags |= IR3_INSTR_A1EN; }
#line 5404 "ir3_parser.c"
    break;

  case 500: /* const_dst: T_A1 '+' integer  */
#line 1176 "ir3_parser.y"
                                   { new_src(0, IR3_REG_IMMED)->iim_val = (yyvsp[0].num); instr->flags |= IR3_INSTR_A1EN; }
#line 5410 "ir3_parser.c"
    break;

  case 501: /* $@42: %empty  */
#line 1179 "ir3_parser.y"
                        { new_instr(OPC_STC); }
#line 5416 "ir3_parser.c"
    break;

  case 503: /* $@43: %empty  */
#line 1180 "ir3_parser.y"
                        { new_instr(OPC_STSC); }
#line 5422 "ir3_parser.c"
    break;

  case 505: /* cat6_shfl_mode: T_MOD_XOR  */
#line 1182 "ir3_parser.y"
                            { instr->cat6.shfl_mode = SHFL_XOR;   }
#line 5428 "ir3_parser.c"
    break;

  case 506: /* cat6_shfl_mode: T_MOD_UP  */
#line 1183 "ir3_parser.y"
                            { instr->cat6.shfl_mode = SHFL_UP;    }
#line 5434 "ir3_parser.c"
    break;

  case 507: /* cat6_shfl_mode: T_MOD_DOWN  */
#line 1184 "ir3_parser.y"
                            { instr->cat6.shfl_mode = SHFL_DOWN;  }
#line 5440 "ir3_parser.c"
    break;

  case 508: /* cat6_shfl_mode: T_MOD_RUP  */
#line 1185 "ir3_parser.y"
                            { instr->cat6.shfl_mode = SHFL_RUP;   }
#line 5446 "ir3_parser.c"
    break;

  case 509: /* cat6_shfl_mode: T_MOD_RDOWN  */
#line 1186 "ir3_parser.y"
                            { instr->cat6.shfl_mode = SHFL_RDOWN; }
#line 5452 "ir3_parser.c"
    break;

  case 510: /* cat6_shfl_mode: integer  */
#line 1190 "ir3_parser.y"
                            { instr->cat6.shfl_mode = (yyvsp[0].num); }
#line 5458 "ir3_parser.c"
    break;

  case 511: /* $@44: %empty  */
#line 1193 "ir3_parser.y"
                   { new_instr(OPC_SHFL); }
#line 5464 "ir3_parser.c"
    break;

  case 513: /* $@45: %empty  */
#line 1195 "ir3_parser.y"
                                             {
                     new_instr(OPC_RAY_INTERSECTION);
                     }
#line 5472 "ir3_parser.c"
    break;

  case 515: /* cat6_todo: T_OP_G2L  */
#line 1199 "ir3_parser.y"
                                            { new_instr(OPC_G2L); }
#line 5478 "ir3_parser.c"
    break;

  case 516: /* cat6_todo: T_OP_L2G  */
#line 1200 "ir3_parser.y"
                                            { new_instr(OPC_L2G); }
#line 5484 "ir3_parser.c"
    break;

  case 517: /* cat6_todo: T_OP_RESFMT  */
#line 1201 "ir3_parser.y"
                                            { new_instr(OPC_RESFMT); }
#line 5490 "ir3_parser.c"
    break;

  case 532: /* cat7_scope: '.' 'w'  */
#line 1218 "ir3_parser.y"
                            { instr->cat7.w = true; }
#line 5496 "ir3_parser.c"
    break;

  case 533: /* cat7_scope: '.' 'r'  */
#line 1219 "ir3_parser.y"
                            { instr->cat7.r = true; }
#line 5502 "ir3_parser.c"
    break;

  case 534: /* cat7_scope: '.' 'l'  */
#line 1220 "ir3_parser.y"
                            { instr->cat7.l = true; }
#line 5508 "ir3_parser.c"
    break;

  case 535: /* cat7_scope: '.' 'g'  */
#line 1221 "ir3_parser.y"
                            { instr->cat7.g = true; }
#line 5514 "ir3_parser.c"
    break;

  case 538: /* $@46: %empty  */
#line 1226 "ir3_parser.y"
                                           { new_instr(OPC_BAR); }
#line 5520 "ir3_parser.c"
    break;

  case 540: /* $@47: %empty  */
#line 1227 "ir3_parser.y"
                                           { new_instr(OPC_FENCE); }
#line 5526 "ir3_parser.c"
    break;

  case 542: /* cat7_data_cache: T_OP_DCCLN  */
#line 1229 "ir3_parser.y"
                                           { new_instr(OPC_DCCLN); }
#line 5532 "ir3_parser.c"
    break;

  case 543: /* cat7_data_cache: T_OP_DCINV  */
#line 1230 "ir3_parser.y"
                                           { new_instr(OPC_DCINV); }
#line 5538 "ir3_parser.c"
    break;

  case 544: /* cat7_data_cache: T_OP_DCFLU  */
#line 1231 "ir3_parser.y"
                                           { new_instr(OPC_DCFLU); }
#line 5544 "ir3_parser.c"
    break;

  case 546: /* cat7_alias_dst: T_RT  */
#line 1234 "ir3_parser.y"
                        { new_dst((yyvsp[0].num), IR3_REG_RT); }
#line 5550 "ir3_parser.c"
    break;

  case 549: /* cat7_alias_scope: T_MOD_TEX  */
#line 1239 "ir3_parser.y"
                                { instr->cat7.alias_scope = ALIAS_TEX; }
#line 5556 "ir3_parser.c"
    break;

  case 550: /* cat7_alias_scope: T_MOD_MEM  */
#line 1240 "ir3_parser.y"
                                { instr->cat7.alias_scope = ALIAS_MEM; }
#line 5562 "ir3_parser.c"
    break;

  case 551: /* cat7_alias_scope: T_MOD_RT  */
#line 1241 "ir3_parser.y"
                                { instr->cat7.alias_scope = ALIAS_RT; }
#line 5568 "ir3_parser.c"
    break;

  case 557: /* cat7_alias_type: cat7_alias_float_type  */
#line 1250 "ir3_parser.y"
                                        { instr->cat7.alias_type_float = true; }
#line 5574 "ir3_parser.c"
    break;

  case 558: /* cat7_alias_table_size_minus_one: T_INT  */
#line 1252 "ir3_parser.y"
                                       { instr->cat7.alias_table_size_minus_one = (yyvsp[0].num); }
#line 5580 "ir3_parser.c"
    break;

  case 561: /* cat7_instr: T_OP_SLEEP  */
#line 1256 "ir3_parser.y"
                                           { new_instr(OPC_SLEEP); }
#line 5586 "ir3_parser.c"
    break;

  case 562: /* cat7_instr: T_OP_CCINV  */
#line 1257 "ir3_parser.y"
                                           { new_instr(OPC_CCINV); }
#line 5592 "ir3_parser.c"
    break;

  case 563: /* cat7_instr: T_OP_ICINV  */
#line 1258 "ir3_parser.y"
                                           { new_instr(OPC_ICINV); }
#line 5598 "ir3_parser.c"
    break;

  case 564: /* cat7_instr: T_OP_LOCK  */
#line 1259 "ir3_parser.y"
                                           { new_instr(OPC_LOCK); }
#line 5604 "ir3_parser.c"
    break;

  case 565: /* cat7_instr: T_OP_UNLOCK  */
#line 1260 "ir3_parser.y"
                                           { new_instr(OPC_UNLOCK); }
#line 5610 "ir3_parser.c"
    break;

  case 566: /* $@48: %empty  */
#line 1261 "ir3_parser.y"
                              {
                       new_instr(OPC_ALIAS);
                   }
#line 5618 "ir3_parser.c"
    break;

  case 568: /* raw_instr: T_RAW  */
#line 1265 "ir3_parser.y"
                   {new_instr(OPC_META_RAW)->raw.value = (yyvsp[0].u64);}
#line 5624 "ir3_parser.c"
    break;

  case 571: /* meta_print_reg: ',' T_REGISTER  */
#line 1270 "ir3_parser.y"
                               {
	meta_print_data.regs_to_dump[meta_print_data.regs_count++] = (yyvsp[0].num);
}
#line 5632 "ir3_parser.c"
    break;

  case 572: /* meta_print_start: T_OP_PRINT T_REGISTER  */
#line 1274 "ir3_parser.y"
                                        {
	meta_print_data.reg_address_lo = (yyvsp[0].num);
	meta_print_data.reg_address_hi = (yyvsp[0].num) + 2;
	meta_print_data.reg_tmp = (yyvsp[0].num) + 4;
	meta_print_data.regs_count = 0;
}
#line 5643 "ir3_parser.c"
    break;

  case 573: /* meta_print: meta_print_start meta_print_regs  */
#line 1281 "ir3_parser.y"
                                             {
	/* low */
	new_instr(OPC_MOV);
	instr->cat1.src_type = TYPE_U32;
	instr->cat1.dst_type = TYPE_U32;
	new_dst(meta_print_data.reg_address_lo, 0);
	new_src(0, IR3_REG_IMMED)->uim_val = info->shader_print_buffer_iova & 0xffffffff;

	/* high */
	new_instr(OPC_MOV);
	instr->cat1.src_type = TYPE_U32;
	instr->cat1.dst_type = TYPE_U32;
	new_dst(meta_print_data.reg_address_hi, 0);
	new_src(0, IR3_REG_IMMED)->uim_val = info->shader_print_buffer_iova >> 32;

	/* offset */
	new_instr(OPC_MOV);
	instr->cat1.src_type = TYPE_U32;
	instr->cat1.dst_type = TYPE_U32;
	new_dst(meta_print_data.reg_tmp, 0);
	new_src(0, IR3_REG_IMMED)->uim_val = 4 * meta_print_data.regs_count;

	new_instr(OPC_NOP);
	instr->repeat = 5;

	/* Increment and get current offset into print buffer */
	new_instr(OPC_ATOMIC_G_ADD);
	instr->cat6.d = 1;
	instr->cat6.typed = 0;
	instr->cat6.type = TYPE_U32;
	instr->cat6.iim_val = 1;

	new_dst(meta_print_data.reg_address_lo, 0);
	new_src(meta_print_data.reg_address_lo, 0);
	new_src(meta_print_data.reg_tmp, 0);

	/* Store all regs */
	for (uint32_t i = 0; i < meta_print_data.regs_count; i++) {
		new_instr(OPC_STG);
		dummy_dst();
		instr->cat6.type = TYPE_U32;
		instr->flags = IR3_INSTR_SY;
		new_src(meta_print_data.reg_address_lo, 0);
		new_src(0, IR3_REG_IMMED)->iim_val = 0;
		new_src(meta_print_data.regs_to_dump[i], IR3_REG_R);
		new_src(0, IR3_REG_IMMED)->iim_val = 1;

		new_instr(OPC_ADD_U);
		instr->flags = IR3_INSTR_SS;
		new_dst(meta_print_data.reg_address_lo, 0);
		new_src(meta_print_data.reg_address_lo, 0);
		new_src(0, IR3_REG_IMMED)->uim_val = 4;

		new_instr(OPC_NOP);
		instr->repeat = 5;
	}
}
#line 5705 "ir3_parser.c"
    break;

  case 574: /* src_gpr: T_REGISTER  */
#line 1339 "ir3_parser.y"
                                  { (yyval.reg) = new_src((yyvsp[0].num), 0); }
#line 5711 "ir3_parser.c"
    break;

  case 575: /* src_a0: T_A0  */
#line 1340 "ir3_parser.y"
                                  { (yyval.reg) = new_src((61 << 3), IR3_REG_HALF); }
#line 5717 "ir3_parser.c"
    break;

  case 576: /* src_a1: T_A1  */
#line 1341 "ir3_parser.y"
                                  { (yyval.reg) = new_src((61 << 3) + 1, IR3_REG_HALF); }
#line 5723 "ir3_parser.c"
    break;

  case 577: /* src_p0: T_P0  */
#line 1342 "ir3_parser.y"
                                  { (yyval.reg) = new_src((62 << 3) + (yyvsp[0].num), IR3_REG_PREDICATE); }
#line 5729 "ir3_parser.c"
    break;

  case 582: /* dst: T_REGISTER  */
#line 1349 "ir3_parser.y"
                                  { (yyval.reg) = new_dst((yyvsp[0].num), 0); }
#line 5735 "ir3_parser.c"
    break;

  case 583: /* dst: T_A0  */
#line 1350 "ir3_parser.y"
                                  { (yyval.reg) = new_dst((61 << 3), IR3_REG_HALF); }
#line 5741 "ir3_parser.c"
    break;

  case 584: /* dst: T_A1  */
#line 1351 "ir3_parser.y"
                                  { (yyval.reg) = new_dst((61 << 3) + 1, IR3_REG_HALF); }
#line 5747 "ir3_parser.c"
    break;

  case 585: /* dst: T_P0  */
#line 1352 "ir3_parser.y"
                                  { (yyval.reg) = new_dst((62 << 3) + (yyvsp[0].num), IR3_REG_PREDICATE); }
#line 5753 "ir3_parser.c"
    break;

  case 586: /* dst: T_UP0  */
#line 1353 "ir3_parser.y"
                                  { (yyval.reg) = new_dst((62 << 3) + (yyvsp[0].num), IR3_REG_PREDICATE | IR3_REG_UNIFORM); }
#line 5759 "ir3_parser.c"
    break;

  case 587: /* const: T_CONSTANT  */
#line 1355 "ir3_parser.y"
                                  { (yyval.reg) = new_src((yyvsp[0].num), IR3_REG_CONST); }
#line 5765 "ir3_parser.c"
    break;

  case 588: /* dst_reg_flag: T_EVEN  */
#line 1357 "ir3_parser.y"
                                  { instr->cat1.round = ROUND_EVEN; }
#line 5771 "ir3_parser.c"
    break;

  case 589: /* dst_reg_flag: T_POS_INFINITY  */
#line 1358 "ir3_parser.y"
                                  { instr->cat1.round = ROUND_POS_INF; }
#line 5777 "ir3_parser.c"
    break;

  case 590: /* dst_reg_flag: T_NEG_INFINITY  */
#line 1359 "ir3_parser.y"
                                  { instr->cat1.round = ROUND_NEG_INF; }
#line 5783 "ir3_parser.c"
    break;

  case 591: /* dst_reg_flag: T_SAT  */
#line 1360 "ir3_parser.y"
                                  { instr->flags |= IR3_INSTR_SAT; }
#line 5789 "ir3_parser.c"
    break;

  case 592: /* dst_reg_flag: T_EI  */
#line 1361 "ir3_parser.y"
                                  { rflags.flags |= IR3_REG_EI; }
#line 5795 "ir3_parser.c"
    break;

  case 593: /* dst_reg_flag: T_WRMASK  */
#line 1362 "ir3_parser.y"
                                  { rflags.wrmask = (yyvsp[0].num); }
#line 5801 "ir3_parser.c"
    break;

  case 596: /* dst_reg: dst  */
#line 1368 "ir3_parser.y"
                                       { (yyvsp[0].reg)->flags |= IR3_REG_R; }
#line 5807 "ir3_parser.c"
    break;

  case 597: /* dst_reg: dst_reg_flags dst  */
#line 1369 "ir3_parser.y"
                                       { (yyvsp[0].reg)->flags |= IR3_REG_R; }
#line 5813 "ir3_parser.c"
    break;

  case 598: /* src_reg_flag: T_ABSNEG  */
#line 1371 "ir3_parser.y"
                                  { rflags.flags |= IR3_REG_ABS|IR3_REG_NEGATE; }
#line 5819 "ir3_parser.c"
    break;

  case 599: /* src_reg_flag: T_NEG  */
#line 1372 "ir3_parser.y"
                                  { rflags.flags |= IR3_REG_NEGATE; }
#line 5825 "ir3_parser.c"
    break;

  case 600: /* src_reg_flag: T_ABS  */
#line 1373 "ir3_parser.y"
                                  { rflags.flags |= IR3_REG_ABS; }
#line 5831 "ir3_parser.c"
    break;

  case 601: /* src_reg_flag: T_R  */
#line 1374 "ir3_parser.y"
                                  { rflags.flags |= IR3_REG_R; }
#line 5837 "ir3_parser.c"
    break;

  case 602: /* src_reg_flag: T_LAST  */
#line 1375 "ir3_parser.y"
                                  { rflags.flags |= IR3_REG_LAST_USE; }
#line 5843 "ir3_parser.c"
    break;

  case 622: /* uoffset: %empty  */
#line 1404 "ir3_parser.y"
                   { (yyval.num) = 0; }
#line 5849 "ir3_parser.c"
    break;

  case 623: /* uoffset: '+' integer  */
#line 1405 "ir3_parser.y"
                               { (yyval.num) = (yyvsp[0].num); }
#line 5855 "ir3_parser.c"
    break;

  case 625: /* offset: '-' integer  */
#line 1408 "ir3_parser.y"
                               { (yyval.num) = -(yyvsp[0].num); }
#line 5861 "ir3_parser.c"
    break;

  case 626: /* src_uoffset: uoffset  */
#line 1410 "ir3_parser.y"
                           { new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[0].num); if ((yyvsp[0].num)) instr->flags |= IR3_INSTR_IMM_OFFSET; }
#line 5867 "ir3_parser.c"
    break;

  case 627: /* relative_gpr_src: 'r' '<' T_A0 offset '>'  */
#line 1412 "ir3_parser.y"
                                            { new_src(0, IR3_REG_RELATIV)->array.offset = (yyvsp[-1].num); }
#line 5873 "ir3_parser.c"
    break;

  case 628: /* relative_gpr_src: T_HR '<' T_A0 offset '>'  */
#line 1413 "ir3_parser.y"
                                             { new_src(0, IR3_REG_RELATIV | IR3_REG_HALF)->array.offset = (yyvsp[-1].num); }
#line 5879 "ir3_parser.c"
    break;

  case 629: /* relative_gpr_dst: 'r' '<' T_A0 offset '>'  */
#line 1415 "ir3_parser.y"
                                            { new_dst(0, IR3_REG_RELATIV)->array.offset = (yyvsp[-1].num); }
#line 5885 "ir3_parser.c"
    break;

  case 630: /* relative_gpr_dst: T_HR '<' T_A0 offset '>'  */
#line 1416 "ir3_parser.y"
                                             { new_dst(0, IR3_REG_RELATIV | IR3_REG_HALF)->array.offset = (yyvsp[-1].num); }
#line 5891 "ir3_parser.c"
    break;

  case 631: /* relative_const: 'c' '<' T_A0 offset '>'  */
#line 1418 "ir3_parser.y"
                                            { new_src(0, IR3_REG_RELATIV | IR3_REG_CONST)->array.offset = (yyvsp[-1].num); }
#line 5897 "ir3_parser.c"
    break;

  case 632: /* relative_const: T_HC '<' T_A0 offset '>'  */
#line 1419 "ir3_parser.y"
                                             { new_src(0, IR3_REG_RELATIV | IR3_REG_CONST | IR3_REG_HALF)->array.offset = (yyvsp[-1].num); }
#line 5903 "ir3_parser.c"
    break;

  case 635: /* immediate_cat1: integer  */
#line 1431 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->iim_val = type_size(instr->cat1.src_type) < 32 ? (yyvsp[0].num) & 0xffff : (yyvsp[0].num); }
#line 5909 "ir3_parser.c"
    break;

  case 636: /* immediate_cat1: '(' integer ')'  */
#line 1432 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->iim_val = (yyvsp[-1].num); }
#line 5915 "ir3_parser.c"
    break;

  case 637: /* immediate_cat1: '(' float ')'  */
#line 1433 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->fim_val = (yyvsp[-1].flt); }
#line 5921 "ir3_parser.c"
    break;

  case 638: /* immediate_cat1: 'h' '(' integer ')'  */
#line 1434 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED | IR3_REG_HALF)->iim_val = (yyvsp[-1].num) & 0xffff; }
#line 5927 "ir3_parser.c"
    break;

  case 639: /* immediate_cat1: 'h' '(' float ')'  */
#line 1435 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED | IR3_REG_HALF)->uim_val = _mesa_float_to_half((yyvsp[-1].flt)); }
#line 5933 "ir3_parser.c"
    break;

  case 640: /* immediate_cat1: 'h' T_FLUT_0_0  */
#line 1436 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED | IR3_REG_HALF)->uim_val = _mesa_float_to_half(0.0); }
#line 5939 "ir3_parser.c"
    break;

  case 641: /* immediate_cat1: 'h' T_FLUT_0_5  */
#line 1437 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED | IR3_REG_HALF)->uim_val = _mesa_float_to_half(0.5); }
#line 5945 "ir3_parser.c"
    break;

  case 642: /* immediate_cat1: 'h' T_FLUT_1_0  */
#line 1438 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED | IR3_REG_HALF)->uim_val = _mesa_float_to_half(1.0); }
#line 5951 "ir3_parser.c"
    break;

  case 643: /* immediate_cat1: 'h' T_FLUT_2_0  */
#line 1439 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED | IR3_REG_HALF)->uim_val = _mesa_float_to_half(2.0); }
#line 5957 "ir3_parser.c"
    break;

  case 644: /* immediate_cat1: 'h' T_FLUT_4_0  */
#line 1440 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED | IR3_REG_HALF)->uim_val = _mesa_float_to_half(4.0); }
#line 5963 "ir3_parser.c"
    break;

  case 645: /* immediate_cat1: '(' T_NAN ')'  */
#line 1441 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->fim_val = NAN; }
#line 5969 "ir3_parser.c"
    break;

  case 646: /* immediate_cat1: '(' T_INF ')'  */
#line 1442 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->fim_val = INFINITY; }
#line 5975 "ir3_parser.c"
    break;

  case 647: /* immediate_cat1: T_FLUT_0_0  */
#line 1443 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->fim_val = 0.0; }
#line 5981 "ir3_parser.c"
    break;

  case 648: /* immediate_cat1: T_FLUT_0_5  */
#line 1444 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->fim_val = 0.5; }
#line 5987 "ir3_parser.c"
    break;

  case 649: /* immediate_cat1: T_FLUT_1_0  */
#line 1445 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->fim_val = 1.0; }
#line 5993 "ir3_parser.c"
    break;

  case 650: /* immediate_cat1: T_FLUT_2_0  */
#line 1446 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->fim_val = 2.0; }
#line 5999 "ir3_parser.c"
    break;

  case 651: /* immediate_cat1: T_FLUT_4_0  */
#line 1447 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->fim_val = 4.0; }
#line 6005 "ir3_parser.c"
    break;

  case 652: /* immediate: integer  */
#line 1449 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->iim_val = (yyvsp[0].num); }
#line 6011 "ir3_parser.c"
    break;

  case 653: /* immediate: '(' integer ')'  */
#line 1450 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->iim_val = (yyvsp[-1].num); }
#line 6017 "ir3_parser.c"
    break;

  case 654: /* immediate: flut_immed  */
#line 1451 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED)->uim_val = (yyvsp[0].num); }
#line 6023 "ir3_parser.c"
    break;

  case 655: /* immediate: 'h' '(' integer ')'  */
#line 1452 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED | IR3_REG_HALF)->iim_val = (yyvsp[-1].num); }
#line 6029 "ir3_parser.c"
    break;

  case 656: /* immediate: 'h' flut_immed  */
#line 1453 "ir3_parser.y"
                                       { new_src(0, IR3_REG_IMMED | IR3_REG_HALF)->uim_val = (yyvsp[0].num); }
#line 6035 "ir3_parser.c"
    break;

  case 669: /* uinteger: T_INT  */
#line 1469 "ir3_parser.y"
                               { (yyval.num) = (yyvsp[0].num); }
#line 6041 "ir3_parser.c"
    break;

  case 670: /* uinteger: T_HEX  */
#line 1470 "ir3_parser.y"
                               { (yyval.num) = (yyvsp[0].unum); }
#line 6047 "ir3_parser.c"
    break;

  case 671: /* integer: uinteger  */
#line 1472 "ir3_parser.y"
                                { (yyval.num) = (yyvsp[0].num); }
#line 6053 "ir3_parser.c"
    break;

  case 672: /* integer: '-' uinteger  */
#line 1473 "ir3_parser.y"
                                { (yyval.num) = -(yyvsp[0].num); }
#line 6059 "ir3_parser.c"
    break;

  case 673: /* float: T_FLOAT  */
#line 1475 "ir3_parser.y"
                               { (yyval.flt) = (yyvsp[0].flt); }
#line 6065 "ir3_parser.c"
    break;

  case 674: /* float: '-' T_FLOAT  */
#line 1476 "ir3_parser.y"
                               { (yyval.flt) = -(yyvsp[0].flt); }
#line 6071 "ir3_parser.c"
    break;

  case 675: /* type: T_TYPE_F16  */
#line 1478 "ir3_parser.y"
                                { (yyval.type) = TYPE_F16;   }
#line 6077 "ir3_parser.c"
    break;

  case 676: /* type: T_TYPE_F32  */
#line 1479 "ir3_parser.y"
                                { (yyval.type) = TYPE_F32;   }
#line 6083 "ir3_parser.c"
    break;

  case 677: /* type: T_TYPE_U16  */
#line 1480 "ir3_parser.y"
                                { (yyval.type) = TYPE_U16;   }
#line 6089 "ir3_parser.c"
    break;

  case 678: /* type: T_TYPE_U32  */
#line 1481 "ir3_parser.y"
                                { (yyval.type) = TYPE_U32;   }
#line 6095 "ir3_parser.c"
    break;

  case 679: /* type: T_TYPE_S16  */
#line 1482 "ir3_parser.y"
                                { (yyval.type) = TYPE_S16;   }
#line 6101 "ir3_parser.c"
    break;

  case 680: /* type: T_TYPE_S32  */
#line 1483 "ir3_parser.y"
                                { (yyval.type) = TYPE_S32;   }
#line 6107 "ir3_parser.c"
    break;

  case 681: /* type: T_TYPE_U8  */
#line 1484 "ir3_parser.y"
                                { (yyval.type) = TYPE_U8;    }
#line 6113 "ir3_parser.c"
    break;

  case 682: /* type: T_TYPE_U8_32  */
#line 1485 "ir3_parser.y"
                                { (yyval.type) = TYPE_U8_32; }
#line 6119 "ir3_parser.c"
    break;

  case 683: /* type: T_TYPE_U64  */
#line 1486 "ir3_parser.y"
                                { (yyval.type) = TYPE_ATOMIC_U64;  }
#line 6125 "ir3_parser.c"
    break;


#line 6129 "ir3_parser.c"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      {
        yypcontext_t yyctx
          = {yyssp, yytoken};
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == -1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = YY_CAST (char *,
                             YYSTACK_ALLOC (YY_CAST (YYSIZE_T, yymsg_alloc)));
            if (yymsg)
              {
                yysyntax_error_status
                  = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
                yymsgp = yymsg;
              }
            else
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = YYENOMEM;
              }
          }
        yyerror (yymsgp);
        if (yysyntax_error_status == YYENOMEM)
          YYNOMEM;
      }
    }

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
  return yyresult;
}

