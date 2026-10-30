# Robo & Kala TW220 Goodix Berlin touch driver

`goodixd` implements the TW220 DTS-described `goodix,gt9916` device:

- SPI22 at `0x00898000`, mode 0, 8-bit transfers;
- GPIO83/84/85/86 QUP22 pinmux;
- IRQ GPIO175 (active low), reset GPIO99;
- GPIO90 (1.8 V VDDIO) and GPIO56 (3.3 V AVDD);
- 2560 x 1600 touch coordinates.

The daemon registers the existing EwokOS six-byte `/dev/touch0` protocol, so
the normal `xtouch` client can consume it.  A TW220 graphical init profile
should start:

```
/bin/ipcserv /drivers/q8b/goodixd /dev/touch0
/bin/bgrun /sbin/x/xtouch -c /etc/x/xtouch-tw220.json /dev/touch0
```

The Q8B default `init.rd` keeps its USB HID touch path; this SPI daemon is
installed as `/drivers/q8b/goodixd` and is selected explicitly by a TW220
profile.
