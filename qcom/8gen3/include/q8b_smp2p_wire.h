/* SMP2P v1/v2 wire decoder; layout and named entry semantics from Linux
 * drivers/soc/qcom/smp2p.c and sc8280xp.dtsi's slave-kernel IRQ bits 0..3. */
#ifndef Q8B_SMP2P_WIRE_H
#define Q8B_SMP2P_WIRE_H
#include <stdint.h>
#include <stddef.h>

#define Q8B_SMP2P_IN_ITEM 94u
#define Q8B_SMP2P_OUT_ITEM 432u

/* Prepare AP-owned master-kernel; publish version after the caller's write
 * barrier. qcom,smem orders INBOUND before OUTBOUND in the Linux driver. */
static inline int q8b_smp2p_init_master(uint8_t *data, size_t size) {
    static const char name[] = "master-kernel";
    if (!data || size < 340) return -1;
    for (size_t i = 0; i < size; i++) data[i] = 0;
    data[0] = 0x24; data[1] = 0x53; data[2] = 0x4d; data[3] = 0x50;
    data[5] = 1; /* SSR_ACK */
    data[10] = 5; /* local=AP(0), remote=DSP(5) */
    data[12] = 16;
    data[14] = 1;
    for (size_t i = 0; i < sizeof(name); i++) data[20 + i] = (uint8_t)name[i];
    return 0;
}

static inline uint32_t q8b_smp2p_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
            (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline int q8b_smp2p_slave_bits(const uint8_t *data, size_t size,
        uint32_t *bits) {
    static const char name[] = "slave-kernel";
    unsigned total, valid;
    if (!data || !bits || size < 20 ||
            q8b_smp2p_le32(data) != 0x504d5324u ||
            (data[4] != 1 && data[4] != 2) ||
            data[8] != 5 || data[9] || data[10] || data[11])
        return -1;
    total = (unsigned)data[12] | (unsigned)data[13] << 8;
    valid = (unsigned)data[14] | (unsigned)data[15] << 8;
    if (!total || total > 16 || valid > total || size < 20 + total * 20)
        return -1;
    for (unsigned i = 0; i < valid; i++) {
        const uint8_t *entry = data + 20 + i * 20;
        unsigned k;
        for (k = 0; k < sizeof(name); k++)
            if (entry[k] != (uint8_t)name[k]) break;
        if (k == sizeof(name)) {
            *bits = q8b_smp2p_le32(entry + 16);
            return 0;
        }
    }
    return -1;
}
#endif
