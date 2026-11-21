#!/bin/bash
# Deploy the UART Bluetooth firmware and enable the BT stack in the live
# init.rd.  /mnt/ewokos/etc is root-owned, so run this with sudo:
#   sudo bash machines/qcom/8gen3/deploy_bt_uart.sh
# The btd binary itself is deployed separately (drivers/8gen3 is user-owned).
set -eu
ROOT=/mnt/ewokos
SRC="$(cd "$(dirname "$0")" && pwd)"
FW="$SRC/firmware/qca"

[ -d "$ROOT/etc/firmware/qca" ] || { echo "live rootfs not mounted?"; exit 1; }

# 1. UART firmware (rampatch TLV + board NVM set, incl. g variants)
for f in "$FW"/hpbtfw21.tlv "$FW"/wcnhpbtfw21.tlv "$FW"/hpnv21* "$FW"/wcnhpnv21*; do
    cp --remove-destination "$f" "$ROOT/etc/firmware/qca/$(basename "$f")"
    chmod 644 "$ROOT/etc/firmware/qca/$(basename "$f")"
done

# 2. Enable btd + BT HID subscribers in the live boot script (idempotent)
if [ ! -f "$ROOT/etc/init.rd.bak-bt" ]; then
    cp -p "$ROOT/etc/init.rd" "$ROOT/etc/init.rd.bak-bt"
fi
sed -i \
    -e 's/^# USB Bluetooth setup/# UART Bluetooth setup/' \
    -e '\|^#@/bin/ipcserv /drivers/8gen3/btd |s/^#//' \
    -e '\|^#@/bin/ipcserv /drivers/8gen3/hid_moused .*/dev/mouse1|s/^#//' \
    -e '\|^#@/bin/ipcserv /drivers/8gen3/hid_keybd .*/dev/keyb1|s/^#//' \
    -e '\|^#@/bin/ipcserv /drivers/8gen3/hid_joystickd|s/^#//' \
    "$ROOT/etc/init.rd"
chmod 644 "$ROOT/etc/init.rd"
sync

# 3. Verify
fail=0
for f in "$FW"/hpbtfw21.tlv "$FW"/wcnhpbtfw21.tlv "$FW"/hpnv21* "$FW"/wcnhpnv21*; do
    b=$(basename "$f")
    a=$(md5sum "$f" | cut -d' ' -f1)
    c=$(md5sum "$ROOT/etc/firmware/qca/$b" | cut -d' ' -f1)
    [ "$a" = "$c" ] || { echo "MD5 MISMATCH: $b"; fail=1; }
done
grep -q '^@/bin/ipcserv /drivers/8gen3/btd' "$ROOT/etc/init.rd" || { echo "init.rd: btd still disabled"; fail=1; }
[ "$fail" = 0 ] && echo "deploy OK: firmware verified, btd enabled in init.rd"
exit "$fail"