/* Actual BSP RX path with simulated DMA memory and MMIO. */
#include <assert.h>
#include <stdio.h>
#define main unused_main
#include DRIVER_SOURCE
#undef main

static uint32_t regs[0x4000 / 4];
static xgmac_desc_t rx[ETH_RING_SIZE], tx[ETH_RING_SIZE];
static uint8_t buffers[ETH_RING_SIZE][ETH_BUF_SIZE];

uint32_t get32(ewokos_addr_t addr) { assert(addr < sizeof(regs)); return regs[addr / 4]; }
void put32(ewokos_addr_t addr, uint32_t val) {
    assert(addr < sizeof(regs));
    if (addr == DMA_CH0_STATUS)
        regs[addr / 4] &= ~val;
    else
        regs[addr / 4] = val;
}
ewokos_addr_t dma_alloc(int id, uint32_t size) { (void)id; (void)size; return 0; }
ewokos_addr_t dma_phy_addr(int id, ewokos_addr_t addr) { (void)id; return addr; }
ewokos_addr_t syscall3(int op, ewokos_addr_t a, ewokos_addr_t b, ewokos_addr_t c) {
    (void)op; (void)b; (void)c; return a;
}
void klog(const char *fmt, ...) { (void)fmt; }

int main(void) {
    uint8_t out[ETH_BUF_SIZE];
    _present = true;
    _rx_desc = rx;
    _tx_desc = tx;
    _rx_desc_phy = 0x10000;
    for (uint32_t i = 0; i < ETH_RING_SIZE; i++) {
        _rx_buf[i] = buffers[i];
        _rx_buf_phy[i] = 0x20000 + i * ETH_BUF_SIZE;
        rx[i].des3 = DESC_OWN;
    }
    /* A completed frame must survive a full software queue unchanged. */
    memset(buffers[0], 0xa5, 1514);
    rx[0].des3 = DESC_RX_LD | 1514;
    _rxq_count = ETH_RXQ_SIZE;
    for (uint32_t i = 0; i < ETH_RXQ_SIZE; i++) {
        _rxq[i].len = 64;
        memset(_rxq[i].data, 0x7e, 64);
    }
    bsp_eth_poll();
    if (_rx_cons || (rx[0].des3 & DESC_OWN) || _rx_dropped) {
        fprintf(stderr, "FAIL: RX poll recycled/dropped a frame while the software queue was full\n");
        return 1;
    }
    for (uint32_t i = 0; i < ETH_RXQ_SIZE; i++) {
        assert(bsp_eth_read(out, sizeof(out)) == 64);
        for (uint32_t j = 0; j < 64; j++)
            assert(out[j] == 0x7e);
    }
    bsp_eth_poll();
    assert(bsp_eth_read(out, sizeof(out)) == 1514);
    assert(memcmp(out, buffers[0], 1514) == 0);
    assert(rx[0].des3 == DESC_OWN);
    assert(regs[DMA_CH0_RX_TAIL / 4] == _rx_desc_phy + ETH_DESC_SIZE);

    /* Drive repeated wraparound and make every payload distinguishable. */
    for (uint32_t n = 0; n < ETH_RING_SIZE * 10 + 7; n++) {
        uint32_t i = _rx_cons;
        memset(buffers[i], n & 0xff, 1514);
        rx[i].des3 = DESC_RX_LD | 1514;
        bsp_eth_poll();
        assert(bsp_eth_read(out, sizeof(out)) == 1514);
        for (uint32_t j = 0; j < 1514; j++)
            assert(out[j] == (uint8_t)n);
        assert(_rx_cons == (i + 1) % ETH_RING_SIZE);
        assert(_rxq_count == 0 && _rx_dropped == 0);
    }
    /* Bad DMA completion cannot leak a partial/error frame to netd. */
    rx[_rx_cons].des3 = DESC_RX_LD | DESC_RX_ES | 1514;
    bsp_eth_poll();
    assert(_rx_dropped == 1 && _rxq_count == 0);
    puts("PASS: full-queue retention, tail resume, 10 ring wraps, payloads and error rejection");
    return 0;
}
