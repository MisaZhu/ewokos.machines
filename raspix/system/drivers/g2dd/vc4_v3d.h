#ifndef RASPIX_VC4_V3D_H
#define RASPIX_VC4_V3D_H

#include <stdint.h>

typedef struct {
	volatile uint32_t* regs;
	uint32_t ident0;
	uint32_t ident1;
	uint32_t last_ct0cs;
	uint32_t last_ct1cs;
	uint32_t last_ct0ea;
	uint32_t last_ct1ea;
	uint32_t last_ct0ca;
	uint32_t last_ct1ca;
	uint32_t last_ct0ra0;
	uint32_t last_ct1ra0;
	uint32_t last_ct0lc;
	uint32_t last_ct1lc;
	uint32_t last_ct0pc;
	uint32_t last_ct1pc;
	uint32_t last_pcs;
	uint32_t last_bfc;
	uint32_t last_rfc;
	uint32_t last_bpca;
	uint32_t last_bpcs;
	uint32_t last_bpoa;
	uint32_t last_bpos;
	uint32_t last_errstat;
	/* Binner overflow (overspill) memory handed to the PTB via BPOA/BPOS. */
	uint32_t bin_overflow_addr;
	uint32_t bin_overflow_size;
	uint32_t bin_overflow_chunk;
	uint32_t bin_overflow_used;
} vc4_v3d_t;

int32_t vc4_v3d_init(vc4_v3d_t* v3d, volatile uint32_t* regs, uint32_t ident0, uint32_t ident1);
void vc4_v3d_set_bin_overflow(vc4_v3d_t* v3d, uint32_t bus_addr, uint32_t size, uint32_t chunk_size);
void vc4_v3d_reset(vc4_v3d_t* v3d);
int32_t vc4_v3d_wait_idle(vc4_v3d_t* v3d, uint32_t timeout_us);
int32_t vc4_v3d_submit_ct(vc4_v3d_t* v3d, uint32_t thread, uint32_t start_bus_addr, uint32_t end_bus_addr, uint32_t timeout_us);
int32_t vc4_v3d_submit_ct0(vc4_v3d_t* v3d, uint32_t start_bus_addr, uint32_t end_bus_addr, uint32_t timeout_us);
int32_t vc4_v3d_submit_ct1(vc4_v3d_t* v3d, uint32_t start_bus_addr, uint32_t end_bus_addr, uint32_t timeout_us);

#endif
