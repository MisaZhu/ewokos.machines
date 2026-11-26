/* UVC (USB Video Class) webcam support for the Orin Nano bsp.
 *
 * Claimed by usbhostd through bsp_usb_uvc_probe() during enumeration:
 * the configuration descriptor is parsed for a video-streaming interface
 * and the best format/frame/alt-setting combination is parked. Streaming
 * itself starts on USB_UVC_CMD_START (probe/commit + SET_INTERFACE +
 * isochronous endpoint open) and is driven from bsp_usb_uvc_poll() in the
 * usbhostd loop. Completed frames are published through a keyed shm
 * double buffer (ewoksys/usb_uvc.h) that the preview app attaches to.
 *
 * Formats: uncompressed YUY2 and MJPEG are both delivered RAW to the
 * consumer (the preview app converts/decodes); the driver only assembles
 * UVC payload packets into whole frames.
 */
#include <usb/bsp_usb.h>
#include <usb/usb_defs.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <ewoksys/klog.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/proto.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/usb_uvc.h>

/* isochronous transport + speed query live in bsp_usb.c */
extern int  bsp_usb_isoc_in_open(bsp_usb_dev_t* dev, uint8_t ep_addr,
        uint16_t mps, uint8_t interval);
extern int  bsp_usb_isoc_in_poll(bsp_usb_dev_t* dev, uint8_t ep_addr,
        void* buf, int size);
extern void bsp_usb_isoc_in_close(bsp_usb_dev_t* dev, uint8_t ep_addr);
extern int  bsp_usb_device_speed(bsp_usb_dev_t* dev);

#define UVC_FRAME_CAP   (2u * 1024u * 1024u) /* per-frame shm buffer cap */
#define UVC_MAX_FRAME_W 1920
#define UVC_MAX_FRAME_H 1080
#define UVC_ISOC_PKT    3072

static inline uint16_t uvc_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t uvc_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void uvc_put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void uvc_put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* one alternate setting of a video-streaming interface carrying an
   isochronous IN endpoint */
typedef struct {
    bool valid;
    uint8_t iface;
    uint8_t alt;
    uint8_t ep;
    uint16_t mps;      /* full wMaxPacketSize incl. mult bits */
    uint8_t interval;  /* raw bInterval */
} uvc_alt_t;

#define UVC_MAX_ALTS 32

/* one format/frame combination found in the descriptors */
typedef struct {
    bool valid;
    bool mjpeg;
    uint8_t iface;
    uint8_t fmt_index;
    uint8_t frame_index;
    uint16_t w, h;
    uint32_t max_frame_size;
    uint32_t def_interval; /* 100ns units */
} uvc_frame_t;

#define UVC_MAX_FRAMES 48

static struct {
    bool claimed;
    bool streaming;
    bsp_usb_dev_t* dev;

    /* parked selection */
    uvc_frame_t frame;
    uvc_alt_t alt;

    /* live stream */
    uint8_t ep;
    uint16_t mps;
    uint8_t interval;
    uint32_t max_payload;
    uint8_t* asm_buf;
    uint32_t asm_cap;
    uint32_t asm_used;
    uint8_t asm_fid;
    bool asm_active;
    bool asm_overflow;

    /* shm double buffer */
    int shm_id;
    usb_uvc_shm_header_t* shm_hdr;
    uint8_t* shm_buf[2];

    /* stats */
    uint32_t frames;
    uint32_t dropped;
    uint32_t bytes;
    uint32_t errors;
} _uvc;

/* ---------------- descriptor parsing ---------------- */

static void uvc_parse(const uint8_t* cfg, int cfg_len,
        uvc_frame_t* frames, int* n_frames,
        uvc_alt_t* alts, int* n_alts) {
    int fi = 0, ai = 0;
    uint8_t cur_iface = 0, cur_alt = 0, cur_class = 0, cur_subclass = 0;
    bool in_vs = false;
    bool cur_mjpeg = false;
    uint8_t cur_fmt_index = 0;
    bool have_fmt = false;

    for (int off = 0; off + 2 <= cfg_len; ) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len < 2 || off + len > cfg_len) {
            break;
        }
        const uint8_t* d = cfg + off;

        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_iface_desc_t)) {
            const usb_iface_desc_t* iface = (const usb_iface_desc_t*)d;
            cur_iface = iface->bInterfaceNumber;
            cur_alt = iface->bAlternateSetting;
            cur_class = iface->bInterfaceClass;
            cur_subclass = iface->bInterfaceSubClass;
            in_vs = (cur_class == USB_CLASS_VIDEO &&
                     cur_subclass == USB_VIDEO_SUBCLASS_STREAMING);
            have_fmt = false;
        }
        else if (type == USB_DESC_CS_INTERFACE && in_vs && len >= 3) {
            uint8_t sub = d[2];
            if (sub == UVC_VS_FORMAT_UNCOMPRESSED && len >= 21) {
                /* guidFormat "YUY2" only; other uncompressed GUIDs
                   (NV12, YV12, ...) are not handled */
                if (d[5] == 'Y' && d[6] == 'U' && d[7] == 'Y' && d[8] == '2') {
                    cur_mjpeg = false;
                    cur_fmt_index = d[3];
                    have_fmt = true;
                }
                else {
                    have_fmt = false;
                }
            }
            else if (sub == UVC_VS_FORMAT_MJPEG && len >= 5) {
                cur_mjpeg = true;
                cur_fmt_index = d[3];
                have_fmt = true;
            }
            else if ((sub == UVC_VS_FRAME_UNCOMPRESSED ||
                      sub == UVC_VS_FRAME_MJPEG) && len >= 26 && have_fmt) {
                bool mjpeg = (sub == UVC_VS_FRAME_MJPEG);
                if (mjpeg != cur_mjpeg) {
                    off += len;
                    continue;
                }
                if (fi < UVC_MAX_FRAMES) {
                    uvc_frame_t* f = &frames[fi];
                    memset(f, 0, sizeof(*f));
                    f->valid = true;
                    f->mjpeg = mjpeg;
                    f->iface = cur_iface;
                    f->fmt_index = cur_fmt_index;
                    f->frame_index = d[3];
                    f->w = uvc_le16(d + 5);
                    f->h = uvc_le16(d + 7);
                    f->max_frame_size = uvc_le32(d + 17);
                    f->def_interval = uvc_le32(d + 21);
                    if (f->def_interval == 0) {
                        f->def_interval = 333333; /* 30fps fallback */
                    }
                    fi++;
                }
            }
        }
        else if (type == USB_DESC_ENDPOINT && in_vs && cur_alt > 0 &&
                 len >= sizeof(usb_ep_desc_t)) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)d;
            if ((ep->bEndpointAddress & USB_ENDPOINT_IN) != 0 &&
                    (ep->bmAttributes & 0x3u) == USB_ENDPOINT_XFER_ISOCHRONOUS &&
                    ai < UVC_MAX_ALTS) {
                uvc_alt_t* a = &alts[ai];
                a->valid = true;
                a->iface = cur_iface;
                a->alt = cur_alt;
                a->ep = ep->bEndpointAddress;
                a->mps = ep->wMaxPacketSize;
                a->interval = ep->bInterval == 0 ? 1 : ep->bInterval;
                ai++;
            }
        }
        off += len;
    }
    *n_frames = fi;
    *n_alts = ai;
}

/* bytes/sec an alt setting can carry at the device's current speed */
static uint32_t uvc_alt_bandwidth(const uvc_alt_t* a, int speed) {
    uint32_t base = a->mps & 0x07ffu;
    uint32_t mult = 1u + ((a->mps >> 11) & 0x3u);
    uint32_t per_xfer = base * mult;
    if (speed == BSP_USB_SPEED_HIGH || speed == BSP_USB_SPEED_SUPER ||
            speed == BSP_USB_SPEED_SUPER_PLUS) {
        uint32_t bi = a->interval > 16 ? 16 : a->interval;
        /* 8000 microframes/s divided by 2^(bInterval-1) */
        return per_xfer * (8000u >> (bi - 1u));
    }
    /* FS/LS: bInterval is in ms */
    return per_xfer * (1000u / (a->interval == 0 ? 1 : a->interval));
}

static bool uvc_select(bsp_usb_dev_t* dev, const uint8_t* cfg, int cfg_len) {
    static uvc_frame_t frames[UVC_MAX_FRAMES];
    static uvc_alt_t alts[UVC_MAX_ALTS];
    int n_frames = 0, n_alts = 0;

    uvc_parse(cfg, cfg_len, frames, &n_frames, alts, &n_alts);
    if (n_frames == 0 || n_alts == 0) {
        return false;
    }
    int speed = bsp_usb_device_speed(dev);

    /* frame preference: YUY2 <= 640x480 (largest first), then any YUY2
       (smallest first), then MJPEG <= 1280x720, then any MJPEG */
    const uvc_frame_t* best = NULL;
    int best_score = -1;
    for (int pass = 0; pass < 4 && best == NULL; ++pass) {
        for (int i = 0; i < n_frames; ++i) {
            const uvc_frame_t* f = &frames[i];
            if (!f->valid || f->w == 0 || f->h == 0 ||
                    f->w > UVC_MAX_FRAME_W || f->h > UVC_MAX_FRAME_H) {
                continue;
            }
            uint32_t raw = f->mjpeg ? f->max_frame_size :
                    (uint32_t)f->w * f->h * 2u;
            if (raw == 0 || raw > UVC_FRAME_CAP) {
                continue;
            }
            bool ok = false;
            int score = 0;
            switch (pass) {
            case 0:
                ok = !f->mjpeg && f->w <= 640 && f->h <= 480;
                score = (int)f->w * f->h;
                break;
            case 1:
                ok = !f->mjpeg;
                score = -(int)f->w * f->h;
                break;
            case 2:
                ok = f->mjpeg && f->w <= 1280 && f->h <= 720;
                score = (int)f->w * f->h;
                break;
            default:
                ok = f->mjpeg;
                score = -(int)f->w * f->h;
                break;
            }
            if (ok && score > best_score) {
                best = f;
                best_score = score;
            }
        }
    }
    if (best == NULL) {
        return false;
    }

    /* alt setting: smallest bandwidth that still covers the frame rate,
       else the largest available (best effort) */
    uint32_t fps_x100 = 10000000u / best->def_interval; /* fps * 100 */
    uint32_t need = (uint32_t)((uint64_t)best->max_frame_size * fps_x100 / 100u);
    need += need / 4; /* headroom for payload headers/scheduling */
    const uvc_alt_t* best_alt = NULL;
    uint32_t best_bw = 0;
    for (int i = 0; i < n_alts; ++i) {
        const uvc_alt_t* a = &alts[i];
        if (!a->valid || a->iface != best->iface) {
            continue;
        }
        uint32_t bw = uvc_alt_bandwidth(a, speed);
        if (bw >= need) {
            if (best_alt == NULL || bw < best_bw) {
                best_alt = a;
                best_bw = bw;
            }
        }
        else if (best_alt == NULL && bw > best_bw) {
            best_alt = a;
            best_bw = bw;
        }
    }
    if (best_alt == NULL) {
        return false;
    }

    _uvc.frame = *best;
    _uvc.alt = *best_alt;
    klog("bsp_uvc: selected %s %ux%u fmt=%u frame=%u iface=%u alt=%u ep=%02x mps=%u bw=%u need=%u\n",
            best->mjpeg ? "MJPEG" : "YUY2", best->w, best->h,
            best->fmt_index, best->frame_index, best->iface, best_alt->alt,
            best_alt->ep, best_alt->mps, best_bw, need);
    return true;
}

/* ---------------- probe / commit / streaming ---------------- */

static int uvc_vs_ctrl(bsp_usb_dev_t* dev, uint8_t req, uint8_t cs,
        uint8_t iface, void* data, uint16_t len, bool dir_in) {
    usb_setup_pkt_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = dir_in ? USB_REQTYPE_CLASS_IFACE_IN :
            USB_REQTYPE_CLASS_IFACE_OUT;
    setup.bRequest = req;
    setup.wValue = (uint16_t)(cs << 8);
    setup.wIndex = iface;
    setup.wLength = len;
    return bsp_usb_control_xfer(dev, &setup, data, dir_in);
}

static int uvc_set_interface(bsp_usb_dev_t* dev, uint8_t iface, uint8_t alt) {
    usb_setup_pkt_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_STD_IFACE_OUT;
    setup.bRequest = USB_REQ_SET_INTERFACE;
    setup.wValue = alt;
    setup.wIndex = iface;
    return bsp_usb_control_xfer(dev, &setup, NULL, false);
}

static void uvc_shm_destroy(void) {
    if (_uvc.shm_id > 0) {
        if (_uvc.shm_hdr != NULL) {
            shmdt(_uvc.shm_hdr);
        }
        shmctl(_uvc.shm_id, IPC_RMID, NULL);
    }
    _uvc.shm_id = -1;
    _uvc.shm_hdr = NULL;
    _uvc.shm_buf[0] = NULL;
    _uvc.shm_buf[1] = NULL;
}

static bool uvc_shm_create(uint32_t cap) {
    uvc_shm_destroy();
    uint32_t total = (uint32_t)sizeof(usb_uvc_shm_header_t) + cap * 2u;
    int id = -1;
    for (int i = 0; i < 8 && id <= 0; ++i) {
        key_t key = (key_t)(0x55564300u + ((uint32_t)getpid() & 0xffffu) + i);
        id = shmget(key, (int)total, 0666 | IPC_CREAT | IPC_EXCL);
    }
    if (id <= 0) {
        klog("bsp_uvc: shm create failed\n");
        return false;
    }
    uint8_t* base = (uint8_t*)shmat(id, 0, 0);
    if (base == (void*)-1) {
        shmctl(id, IPC_RMID, NULL);
        return false;
    }
    memset(base, 0, total);
    _uvc.shm_id = id;
    _uvc.shm_hdr = (usb_uvc_shm_header_t*)base;
    _uvc.shm_buf[0] = base + sizeof(usb_uvc_shm_header_t);
    _uvc.shm_buf[1] = _uvc.shm_buf[0] + cap;

    usb_uvc_shm_header_t* h = _uvc.shm_hdr;
    h->magic = USB_UVC_SHM_MAGIC;
    h->version = 1;
    h->w = _uvc.frame.w;
    h->h = _uvc.frame.h;
    h->format = _uvc.frame.mjpeg ? UVC_FRAME_FMT_MJPEG : UVC_FRAME_FMT_YUY2;
    h->cap = cap;
    h->index = 0;
    h->seq = 0;
    h->size = 0;
    return true;
}

static void uvc_stop_stream(void) {
    if (!_uvc.streaming) {
        return;
    }
    _uvc.streaming = false;
    if (_uvc.dev != NULL) {
        uvc_set_interface(_uvc.dev, _uvc.alt.iface, 0);
        bsp_usb_isoc_in_close(_uvc.dev, _uvc.ep);
    }
    if (_uvc.asm_buf != NULL) {
        free(_uvc.asm_buf);
        _uvc.asm_buf = NULL;
    }
    _uvc.asm_active = false;
    _uvc.asm_used = 0;
    klog("bsp_uvc: stream stopped frames=%u dropped=%u\n",
            _uvc.frames, _uvc.dropped);
}

static int uvc_start_stream(void) {
    if (!_uvc.claimed || _uvc.dev == NULL) {
        return -1;
    }
    if (_uvc.streaming) {
        return 0;
    }
    bsp_usb_dev_t* dev = _uvc.dev;
    const uvc_frame_t* f = &_uvc.frame;
    const uvc_alt_t* a = &_uvc.alt;

    uint32_t cap = f->mjpeg ? f->max_frame_size :
            (uint32_t)f->w * f->h * 2u;
    if (cap == 0 || cap > UVC_FRAME_CAP) {
        return -1;
    }
    if (!uvc_shm_create(cap)) {
        return -1;
    }

    /* VS probe: propose the parked format/frame/interval, let the device
       answer with its actual buffer/payload limits (UVC 1.1 = 34 bytes,
       1.0 devices answer 26) */
    uint8_t pc[34];
    memset(pc, 0, sizeof(pc));
    uvc_put16(pc + 0, 0x0001);       /* bmHint: dwFrameInterval fixed */
    pc[2] = f->fmt_index;
    pc[3] = f->frame_index;
    uvc_put32(pc + 4, f->def_interval);
    uvc_put32(pc + 18, cap);         /* dwMaxVideoFrameSize */
    uvc_put32(pc + 22, a->mps);      /* dwMaxPayloadTransferSize */

    if (uvc_vs_ctrl(dev, UVC_REQ_SET_CUR, UVC_VS_PROBE_CONTROL,
            a->iface, pc, sizeof(pc), false) < 0) {
        klog("bsp_uvc: probe SET_CUR failed\n");
        uvc_shm_destroy();
        return -1;
    }
    memset(pc, 0, sizeof(pc));
    int got = uvc_vs_ctrl(dev, UVC_REQ_GET_CUR, UVC_VS_PROBE_CONTROL,
            a->iface, pc, sizeof(pc), true);
    if (got < 26) {
        klog("bsp_uvc: probe GET_CUR failed got=%d\n", got);
        uvc_shm_destroy();
        return -1;
    }
    uint32_t interval = uvc_le32(pc + 4);
    uint32_t max_frame = uvc_le32(pc + 18);
    uint32_t max_payload = uvc_le32(pc + 22);
    if (interval == 0) {
        interval = f->def_interval;
    }
    if (max_frame == 0 || max_frame > cap) {
        max_frame = cap;
    }
    if (max_payload == 0 || max_payload > a->mps) {
        max_payload = a->mps;
    }

    /* commit the negotiated parameters */
    memset(pc, 0, sizeof(pc));
    uvc_put16(pc + 0, 0x0001);
    pc[2] = f->fmt_index;
    pc[3] = f->frame_index;
    uvc_put32(pc + 4, interval);
    uvc_put32(pc + 18, max_frame);
    uvc_put32(pc + 22, max_payload);
    if (uvc_vs_ctrl(dev, UVC_REQ_SET_CUR, UVC_VS_COMMIT_CONTROL,
            a->iface, pc, sizeof(pc), false) < 0) {
        klog("bsp_uvc: commit SET_CUR failed\n");
        uvc_shm_destroy();
        return -1;
    }

    if (uvc_set_interface(dev, a->iface, a->alt) < 0) {
        klog("bsp_uvc: set_interface alt=%u failed\n", a->alt);
        uvc_shm_destroy();
        return -1;
    }
    if (bsp_usb_isoc_in_open(dev, a->ep, a->mps, a->interval) != 0) {
        klog("bsp_uvc: isoc open failed ep=%02x\n", a->ep);
        uvc_set_interface(dev, a->iface, 0);
        uvc_shm_destroy();
        return -1;
    }

    _uvc.asm_buf = (uint8_t*)malloc(max_frame);
    if (_uvc.asm_buf == NULL) {
        bsp_usb_isoc_in_close(dev, a->ep);
        uvc_set_interface(dev, a->iface, 0);
        uvc_shm_destroy();
        return -1;
    }
    _uvc.asm_cap = max_frame;
    _uvc.asm_used = 0;
    _uvc.asm_active = false;
    _uvc.asm_overflow = false;
    _uvc.ep = a->ep;
    _uvc.mps = a->mps;
    _uvc.interval = a->interval;
    _uvc.max_payload = max_payload;
    _uvc.streaming = true;
    klog("bsp_uvc: stream started %s %ux%u interval=%u max_frame=%u payload=%u shm=%d\n",
            f->mjpeg ? "MJPEG" : "YUY2", f->w, f->h, interval,
            max_frame, max_payload, _uvc.shm_id);
    return 0;
}

/* ---------------- frame assembly ---------------- */

static void uvc_publish(const uint8_t* data, uint32_t size) {
    usb_uvc_shm_header_t* h = _uvc.shm_hdr;
    if (h == NULL || size == 0 || size > h->cap) {
        _uvc.dropped++;
        return;
    }
    uint32_t back = h->index ^ 1u;
    memcpy(_uvc.shm_buf[back], data, size);
    h->size = size;
    __sync_synchronize();
    h->seq++;
    h->index = back;
    __sync_synchronize();
    _uvc.frames++;
    _uvc.bytes += size;
}

static void uvc_process_packet(const uint8_t* pkt, int n) {
    if (n < 2) {
        return;
    }
    uint8_t hlen = pkt[0];
    uint8_t bf = pkt[1];
    if (hlen < 2 || hlen > n) {
        return;
    }
    if (bf & 0x40) { /* ERR: device flagged a bad frame */
        if (_uvc.asm_active) {
            _uvc.asm_active = false;
            _uvc.asm_used = 0;
            _uvc.dropped++;
        }
        return;
    }
    uint8_t fid = bf & 0x01u;
    bool eof = (bf & 0x02u) != 0;
    const uint8_t* payload = pkt + hlen;
    uint32_t plen = (uint32_t)(n - hlen);

    if (!_uvc.asm_active) {
        if (plen == 0 && !eof) {
            return; /* idle packet between frames */
        }
        _uvc.asm_active = true;
        _uvc.asm_fid = fid;
        _uvc.asm_used = 0;
        _uvc.asm_overflow = false;
    }
    else if (fid != _uvc.asm_fid) {
        /* new frame started before the previous one finished */
        _uvc.dropped++;
        _uvc.asm_fid = fid;
        _uvc.asm_used = 0;
        _uvc.asm_overflow = false;
    }

    if (plen > 0) {
        if (_uvc.asm_used + plen > _uvc.asm_cap) {
            _uvc.asm_overflow = true;
        }
        else if (!_uvc.asm_overflow) {
            memcpy(_uvc.asm_buf + _uvc.asm_used, payload, plen);
            _uvc.asm_used += plen;
        }
    }
    if (eof) {
        if (!_uvc.asm_overflow && _uvc.asm_used > 0) {
            uvc_publish(_uvc.asm_buf, _uvc.asm_used);
        }
        else {
            _uvc.dropped++;
        }
        _uvc.asm_active = false;
        _uvc.asm_used = 0;
        _uvc.asm_overflow = false;
    }
}

void bsp_usb_uvc_poll(void) {
    if (!_uvc.streaming || _uvc.dev == NULL) {
        return;
    }
    uint8_t pkt[UVC_ISOC_PKT];
    for (int i = 0; i < 64; ++i) {
        int n = bsp_usb_isoc_in_poll(_uvc.dev, _uvc.ep, pkt, sizeof(pkt));
        if (n < 0) {
            _uvc.errors++;
            klog("bsp_uvc: isoc poll error, stopping stream\n");
            uvc_stop_stream();
            return;
        }
        if (n == 0) {
            return;
        }
        uvc_process_packet(pkt, n);
    }
}

bool bsp_usb_uvc_streaming(void) {
    return _uvc.streaming;
}

/* ---------------- claim / teardown / control ---------------- */

int bsp_usb_uvc_probe(bsp_usb_dev_t* dev, const uint8_t* cfg, int cfg_len) {
    if (dev == NULL || cfg == NULL || _uvc.claimed) {
        return -1;
    }
    /* only claim devices that expose a video-control interface */
    bool has_vc = false;
    for (int off = 0; off + 2 <= cfg_len; ) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len < 2 || off + len > cfg_len) {
            break;
        }
        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_iface_desc_t)) {
            const usb_iface_desc_t* iface = (const usb_iface_desc_t*)(cfg + off);
            if (iface->bInterfaceClass == USB_CLASS_VIDEO &&
                    iface->bInterfaceSubClass == USB_VIDEO_SUBCLASS_CONTROL) {
                has_vc = true;
                break;
            }
        }
        off += len;
    }
    if (!has_vc) {
        return -1;
    }
    if (!uvc_select(dev, cfg, cfg_len)) {
        klog("bsp_uvc: video device found but no usable format/alt\n");
        return -1;
    }
    _uvc.claimed = true;
    _uvc.dev = dev;
    _uvc.shm_id = -1;
    return 0;
}

void bsp_usb_uvc_detach(bsp_usb_dev_t* dev) {
    if (!_uvc.claimed || _uvc.dev != dev) {
        return;
    }
    uvc_stop_stream();
    uvc_shm_destroy();
    memset(&_uvc, 0, sizeof(_uvc));
    _uvc.shm_id = -1;
    klog("bsp_uvc: device detached\n");
}

bool bsp_usb_uvc_attached(bsp_usb_dev_t* dev) {
    return _uvc.claimed && _uvc.dev == dev;
}

/* controller re-init forgot every device handle: drop the claim without
   touching the (now dead) device */
void bsp_usb_uvc_reinit(void) {
    if (!_uvc.claimed) {
        return;
    }
    _uvc.streaming = false;
    if (_uvc.asm_buf != NULL) {
        free(_uvc.asm_buf);
    }
    uvc_shm_destroy();
    memset(&_uvc, 0, sizeof(_uvc));
    _uvc.shm_id = -1;
}

int bsp_usb_uvc_cntl(vdevice_t* vdev, int from_pid, int cmd,
        proto_t* in, proto_t* out) {
    (void)vdev;
    (void)from_pid;
    (void)in;

    switch (cmd) {
    case USB_UVC_CMD_INFO:
        PF->addi(out, _uvc.claimed ? 1 : 0);
        PF->addi(out, _uvc.claimed ? 1 : 0);
        PF->addi(out, _uvc.streaming ? 1 : 0);
        PF->addi(out, _uvc.frame.mjpeg ? UVC_FRAME_FMT_MJPEG : UVC_FRAME_FMT_YUY2);
        PF->addi(out, _uvc.frame.w);
        PF->addi(out, _uvc.frame.h);
        PF->addi(out, _uvc.shm_id);
        return 0;
    case USB_UVC_CMD_START:
        return uvc_start_stream();
    case USB_UVC_CMD_STOP:
        uvc_stop_stream();
        return 0;
    case USB_UVC_CMD_STATS:
        PF->addi(out, (int32_t)_uvc.frames);
        PF->addi(out, (int32_t)_uvc.dropped);
        PF->addi(out, (int32_t)_uvc.bytes);
        PF->addi(out, (int32_t)_uvc.errors);
        return 0;
    default:
        return -1;
    }
}