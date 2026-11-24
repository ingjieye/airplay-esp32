#!/bin/bash
# Run the simulator firmware in QEMU. Usage: [KEEP_FLASH=1] sim/qemu_run.sh [serial log file]
# host localhost:18080 -> board 80 (web/DLNA), localhost:17000 -> board 7000 (AirPlay RTSP), localhost:19152 -> board 49152 (DLNA)
# The board reaches the host at 10.0.2.2
set -e
cd "$(dirname "$0")/.."
B=build_sim
source ~/esp/esp-idf-v6.1/export.sh >/dev/null 2>&1
# KEEP_FLASH=1: reuse the previous flash image (keeps NVS, simulates a reboot)
[ "$KEEP_FLASH" = 1 ] || ( cd $B && python -m esptool --chip esp32 merge-bin --pad-to-size 8MB -o qemu_flash.bin @flash_args >/dev/null )
# Default eFuses for chip revision 3
python3 -c "import binascii;open('$B/qemu_efuse.bin','wb').write(binascii.unhexlify('00000000000000000000000000800000000000000000100000000000000000000000000000000000'+'0'*248))"
LOG=${1:-$B/qemu_serial.log}
exec qemu-system-xtensa -M esp32 -m 4M -nographic -no-reboot \
  -drive file=$B/qemu_flash.bin,if=mtd,format=raw \
  -drive file=$B/qemu_efuse.bin,if=none,format=raw,id=efuse \
  -global driver=nvram.esp32.efuse,property=drive,value=efuse \
  -nic user,model=open_eth,hostfwd=tcp:127.0.0.1:18080-:80,hostfwd=tcp:127.0.0.1:17000-:7000,hostfwd=tcp:127.0.0.1:19152-:49152 \
  -serial file:$LOG -monitor none
