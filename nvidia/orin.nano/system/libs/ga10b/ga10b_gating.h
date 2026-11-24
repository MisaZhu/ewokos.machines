#ifndef GA10B_GATING_H
#define GA10B_GATING_H

/* GENERATED from nvgpu rel-36.4 hal/power_features/cg/
 * ga10b_gating_reglist.c -- do not edit.  {addr, prod} pairs of the
 * lists nvgpu_cg_init_gr_load_gating_prod writes (cg.c:395-431). */

#include <stdint.h>

typedef struct { uint32_t addr, prod; } ga10b_gate_t;

static const ga10b_gate_t ga10b_slcg_bus[] = {
    { 0x00001c04u, 0x00000000u },
};

static const ga10b_gate_t ga10b_slcg_chiplet[] = {
    { 0x0010e07cu, 0x00000000u },
    { 0x0010e17cu, 0x00000000u },
};

static const ga10b_gate_t ga10b_slcg_gr[] = {
    { 0x004041f4u, 0x00200000u },
    { 0x00409134u, 0x00000140u },
    { 0x00409894u, 0x00000000u },
    { 0x00406004u, 0x00000000u },
    { 0x00405864u, 0x00000000u },
    { 0x00405bf4u, 0x00000000u },
    { 0x004078c4u, 0x00000000u },
    { 0x00405910u, 0xfffffff0u },
    { 0x00408044u, 0x00000000u },
    { 0x00407004u, 0x00000002u },
    { 0x0041a134u, 0x00000140u },
    { 0x0041a894u, 0x00000000u },
    { 0x00418974u, 0x00000000u },
    { 0x00418e10u, 0xfffffffeu },
    { 0x0041889cu, 0x00000000u },
    { 0x0041949cu, 0x00000000u },
    { 0x00418504u, 0x00000000u },
    { 0x0041860cu, 0x00000000u },
    { 0x0041868cu, 0x00000000u },
    { 0x0041871cu, 0x00000000u },
    { 0x00418388u, 0x00000000u },
    { 0x0041882cu, 0x00000000u },
    { 0x00418bc0u, 0x00000000u },
    { 0x00418c74u, 0xffffff80u },
    { 0x00418cf4u, 0xfffffff0u },
    { 0x00418d74u, 0xffffffe0u },
    { 0x00418f10u, 0xffffffe0u },
    { 0x00419024u, 0x000001feu },
    { 0x00419d24u, 0x00000000u },
    { 0x0041986cu, 0x00000104u },
    { 0x00419c84u, 0x0003fffcu },
    { 0x00419c8cu, 0xff71f384u },
    { 0x00419c94u, 0x38f00340u },
    { 0x00419ca4u, 0x00003ffcu },
    { 0x00419cacu, 0x0001f800u },
    { 0x00419cb4u, 0x00001d00u },
    { 0x00419a44u, 0x00000008u },
    { 0x00419a4cu, 0x000001f8u },
    { 0x00419a54u, 0x0000003cu },
    { 0x00419a5cu, 0x0000000cu },
    { 0x00419a64u, 0x00000182u },
    { 0x00419a7cu, 0x0000003cu },
    { 0x00419a84u, 0x0000000cu },
    { 0x00419c74u, 0x0000001eu },
    { 0x0041be2cu, 0x04115fc0u },
    { 0x0041bfecu, 0xfffffff0u },
    { 0x0041bed4u, 0xfffffff8u },
    { 0x0041b81cu, 0x00000000u },
    { 0x0041b878u, 0x00000000u },
    { 0x0041b89cu, 0x00000000u },
    { 0x0041bc1cu, 0x00000000u },
    { 0x0041bc78u, 0x00000000u },
    { 0x0041bc9cu, 0x00000000u },
};

static const ga10b_gate_t ga10b_slcg_perf[] = {
    { 0x00248018u, 0x00000000u },
    { 0x00251a18u, 0x00000000u },
    { 0x00251818u, 0x00000000u },
    { 0x0024a044u, 0x00000000u },
};

static const ga10b_gate_t ga10b_slcg_xbar[] = {
    { 0x0013c824u, 0x00000000u },
    { 0x0013dc08u, 0x00000000u },
    { 0x0013c924u, 0x00000000u },
    { 0x0013cb84u, 0x00000000u },
    { 0x0013cb8cu, 0x00000000u },
};

static const ga10b_gate_t ga10b_slcg_hshub[] = {
    { 0x000043f4u, 0x00000000u },
    { 0x000047f4u, 0x00000000u },
    { 0x00004bf4u, 0x00000000u },
};

static const ga10b_gate_t ga10b_blcg_bus[] = {
    { 0x00001c00u, 0x00000042u },
};

static const ga10b_gate_t ga10b_blcg_gr[] = {
    { 0x004041f0u, 0x0000c646u },
    { 0x00409890u, 0x0000007fu },
    { 0x004098b0u, 0x0000007fu },
    { 0x00406000u, 0x0000c444u },
    { 0x00405860u, 0x0000c242u },
    { 0x00405bf0u, 0x0000c444u },
    { 0x004078c0u, 0x00004242u },
    { 0x0040590cu, 0x0000c444u },
    { 0x00408040u, 0x0000c444u },
    { 0x00407000u, 0x4000c242u },
    { 0x0041a890u, 0x0000427fu },
    { 0x0041a8b0u, 0x0000007fu },
    { 0x00418970u, 0x0000c242u },
    { 0x00418e0cu, 0x0000c444u },
    { 0x00418898u, 0x00004242u },
    { 0x00419498u, 0x00004242u },
    { 0x00418500u, 0x0000c244u },
    { 0x00418608u, 0x0000c242u },
    { 0x00418688u, 0x0000c242u },
    { 0x00418718u, 0x00000042u },
    { 0x00418828u, 0x00008444u },
    { 0x00418bbcu, 0x0000c242u },
    { 0x00418c70u, 0x0000c444u },
    { 0x00418cf0u, 0x0000c444u },
    { 0x00418d70u, 0x0000c444u },
    { 0x00418f0cu, 0x0000c444u },
    { 0x00419020u, 0x0000c242u },
    { 0x00419868u, 0x00008242u },
    { 0x00419c80u, 0x00004045u },
    { 0x00419c88u, 0x00004047u },
    { 0x00419c90u, 0x00004046u },
    { 0x00419c98u, 0x00000042u },
    { 0x00419ca0u, 0x00004047u },
    { 0x00419ca8u, 0x00000046u },
    { 0x00419cb0u, 0x00000045u },
    { 0x00419a40u, 0x00000242u },
    { 0x00419a48u, 0x00000242u },
    { 0x00419a50u, 0x00000242u },
    { 0x00419a58u, 0x00000242u },
    { 0x00419a60u, 0x00000244u },
    { 0x00419a68u, 0x00000242u },
    { 0x00419a78u, 0x00000242u },
    { 0x00419a80u, 0x00000242u },
    { 0x00419a70u, 0x00000545u },
    { 0x00419a88u, 0x00000545u },
    { 0x00419c70u, 0x0000c444u },
    { 0x0041be28u, 0x00008242u },
    { 0x0041bfe8u, 0x0000c444u },
    { 0x0041bed0u, 0x0000c444u },
    { 0x0041b818u, 0x0000c242u },
    { 0x0041b874u, 0x0000c242u },
    { 0x0041b898u, 0x0000c242u },
    { 0x0041bc18u, 0x0000c242u },
    { 0x0041bc74u, 0x0000c242u },
    { 0x0041bc98u, 0x0000c242u },
};

static const ga10b_gate_t ga10b_blcg_xbar[] = {
    { 0x0013c820u, 0x0001004au },
    { 0x0013dc04u, 0x0001004au },
    { 0x0013c920u, 0x0000004au },
    { 0x0013cb80u, 0x00000042u },
    { 0x0013cb88u, 0x00000042u },
};

static const ga10b_gate_t ga10b_blcg_hshub[] = {
    { 0x000043f0u, 0x0000c242u },
    { 0x000047f0u, 0x0000c242u },
    { 0x00004bf0u, 0x0000c242u },
};

#define GA10B_GATE_N(t) ((uint32_t)(sizeof(t) / sizeof((t)[0])))

#endif
