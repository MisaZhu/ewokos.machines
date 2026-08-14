## Waveshare LCD 3.5A on Raspberry Pi 5

This port keeps the original user-space `ili9486 + xpt2046` driver stack and
runs it on Raspberry Pi 5 through the `raspi5` BSP. The Pi 5 specific SPI/GPIO
mapping is handled by the BCM2712/RP1 backend in `system/libs`.

### Wiring

The driver assumes the standard Waveshare 3.5A 40-pin header wiring:

- LCD DC: GPIO24
- LCD CS: GPIO8
- LCD RST: GPIO25
- LCD BL: GPIO18
- TP CS: GPIO7
- TP IRQ: GPIO17

### Runtime

- framebuffer mount: `/dev/disp0`
- driver binary: `/drivers/waveshare/lcdhatd`
- console UART: `/drivers/raspi5/uartd`

### Configurable Arguments

`lcdhatd` keeps the Waveshare defaults, but the GPIO and SPI routing can now be
overridden from the command line:

- `-d <div>`: LCD SPI clock divider
- `-D <gpio>`: LCD DC pin
- `-C <gpio>`: LCD CS pin (`-1` to skip manual GPIO CS)
- `-R <gpio>`: LCD reset pin
- `-B <gpio>`: LCD backlight pin (`-1` to disable)
- `-S <sel>`: LCD SPI select (`0/1`, `-1` to skip hardware CE)
- `-p <gpio>`: touch CS pin
- `-i <gpio>`: touch IRQ pin
- `-t <div>`: touch SPI clock divider
- `-T <sel>`: touch SPI select (`0/1`, `-1` to skip hardware CE)

### Notes

- Default wiring remains `LCD CE0(GPIO8)` and `TP CE1(GPIO7)`.
- The LCD init path uses the `WS35A` profile before panel startup, matching the
  official `waveshare35a` overlay flow.
- Raspberry Pi 5 support depends on the `raspi5` BSP implementation rather than
  a separate LCD driver rewrite.
