/* Bounded QRTR discovery decoder, matching Linux net/qrtr/af_qrtr.c. */
#ifndef QCOM8GEN3_QRTR_WIRE_H
#define QCOM8GEN3_QRTR_WIRE_H
#include <stdint.h>
#include <string.h>

struct qcom8gen3_qrtr_packet {
    uint32_t type, src_node, src_port, dst_node, dst_port, confirm;
    uint32_t size;
    const uint8_t *data;
};
static inline uint32_t qcom8gen3_qrtr_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint32_t qcom8gen3_qrtr_le16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static inline int qcom8gen3_qrtr_decode(const uint8_t *p, uint32_t n,
        struct qcom8gen3_qrtr_packet *out) {
    struct qcom8gen3_qrtr_packet v = {0};
    uint32_t hdr;
    if (!p || !out || !n) return -1;
    if (p[0] == 1) {
        if (n < 32 || qcom8gen3_qrtr_le32(p) != 1) return -1;
        hdr = 32; v.type = qcom8gen3_qrtr_le32(p + 4);
        v.src_node = qcom8gen3_qrtr_le32(p + 8); v.src_port = qcom8gen3_qrtr_le32(p + 12);
        v.confirm = !!qcom8gen3_qrtr_le32(p + 16); v.size = qcom8gen3_qrtr_le32(p + 20);
        v.dst_node = qcom8gen3_qrtr_le32(p + 24); v.dst_port = qcom8gen3_qrtr_le32(p + 28);
    } else if (p[0] == 3) {
        if (n < 16) return -1;
        hdr = 16u + p[3]; v.type = p[1]; v.confirm = p[2] & 1u;
        v.size = qcom8gen3_qrtr_le32(p + 4);
        v.src_node = qcom8gen3_qrtr_le16(p + 8); v.src_port = qcom8gen3_qrtr_le16(p + 10);
        v.dst_node = qcom8gen3_qrtr_le16(p + 12); v.dst_port = qcom8gen3_qrtr_le16(p + 14);
        if (v.src_port == 0xfffeu) v.src_port = 0xfffffffeu;
        if (v.dst_port == 0xfffeu) v.dst_port = 0xfffffffeu;
    } else return -1;
    if (v.dst_port == 0xffffu) v.dst_port = 0xfffffffeu;
    if (hdr > n || !v.size || v.size > UINT32_MAX - 3u ||
            ((v.size + 3u) & ~3u) != n - hdr) return -1;
    if ((v.type == 4 || v.type == 7) && v.size < 20) return -1;
    if (v.dst_port != 0xfffffffeu && v.type != 1 && v.type != 7) return -1;
    v.data = p + hdr;
    *out = v;
    return 0;
}

#define QCOM8GEN3_SSCTL_CLIENT_PORT 0x4000u
static inline void qcom8gen3_qrtr_put32(uint8_t *p, uint32_t v) {
    for(unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8u * i));
}
/* QMI request header (7 bytes), no TLVs; QRTR payload padded to 4 bytes. */
static inline void qcom8gen3_ssctl_shutdown_request(uint8_t packet[40],
        uint32_t node, uint32_t port, uint16_t txn) {
    memset(packet, 0, 40);
    const uint32_t hdr[] = {1, 1, 1, QCOM8GEN3_SSCTL_CLIENT_PORT, 0, 7, node, port};
    for(unsigned i = 0; i < 8; i++) qcom8gen3_qrtr_put32(packet + i * 4, hdr[i]);
    packet[33] = (uint8_t)txn; packet[34] = (uint8_t)(txn >> 8);
    packet[35] = 0x21;
}
/* Return 1 for a matching response (including malformed/rejected response),
 * 0 for unrelated traffic. Only a unique successful QMI result TLV is OK. */
static inline int qcom8gen3_ssctl_shutdown_result(const struct qcom8gen3_qrtr_packet *p,
        uint32_t node, uint32_t port, uint16_t txn, int32_t *result) {
    if(p->type != 1 || p->src_node != node || p->src_port != port ||
            p->dst_node != 1 || p->dst_port != QCOM8GEN3_SSCTL_CLIENT_PORT || p->size < 7 ||
            p->data[0] != 2 || qcom8gen3_qrtr_le16(p->data + 1) != txn ||
            qcom8gen3_qrtr_le16(p->data + 3) != 0x21) return 0;
    *result = -1;
    if(qcom8gen3_qrtr_le16(p->data + 5) != p->size - 7u) return 1;
    uint32_t pos = 7;
    int found = 0;
    int32_t status = -1;
    while(pos < p->size) {
        if(p->size - pos < 3) return 1;
        uint32_t type = p->data[pos], len = qcom8gen3_qrtr_le16(p->data + pos + 1);
        pos += 3;
        if(len > p->size - pos) return 1;
        if(type == 2) {
            if(found || len != 4) return 1;
            found = 1;
            status = (int32_t)(qcom8gen3_qrtr_le16(p->data + pos) << 16) |
                (int32_t)qcom8gen3_qrtr_le16(p->data + pos + 2);
        }
        pos += len;
    }
    if(found) *result = status;
    return 1;
}
#endif
