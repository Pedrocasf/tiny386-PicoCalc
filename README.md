# Tiny386

## Introduction
Tiny386 is an x86 PC emulator written in C99. The highlight of the project is its portability. It now boots Windows 9x/NT on MCU such as ESP32-S3.

The core of the project is a built-from-scratch, simple and stupid i386 cpu emulator. Some features are missing, e.g. debugging, hardware tasking and some permission checks, but it should be able to run most 16/32 bit software. To boot modern linux kernel and windows, some 486 and 586 instrutions are added. The cpu emulator is kept in ~6K LOC. There is also an optional x87 fpu emulator.

To assemble a complete PC system, we have ported many peripherals from TinyEMU and QEMU, it now includes:
 - 8259 PIC
 - 8254 PIT
 - 8042 Keyboard Controller
 - CMOS RTC
 - ISA VGA with Bochs VBE
 - IDE Disk Controller
 - NE2000 ISA Network Card
 - 8257 ISA DMA
 - PC Speaker
 - Adlib OPL2 (optional)
 - SoundBlaster 16

For firmware, the BIOS/VGABIOS comes from seabios. Tiny386 also supports booting linux kernel directly, without traditional BIOS. The idea comes from JSLinux, and it uses a small stub code called linuxstart.

By default the directly booted kernel gets an 80x25 text console. With `vga=<mode>` in its command line (VESA mode number + 0x200, as with other boot loaders, e.g. `vga=0x303` for 800x600 with 256 colours, `0x314` with 16-bit colour, `0x305`/`0x317` for 1024x768), linuxstart sets that graphics mode and describes the linear frame buffer to the kernel, for its VESA frame buffer driver (`CONFIG_FB_VESA`, with `CONFIG_FRAMEBUFFER_CONSOLE` for a console). The mode has to fit the `[display]` size and `vga_mem_size`; otherwise text mode stays.

Tiny386 now supports i686, MMX, SSE (up to SSSE3) and amd64 (64bit long mode only). As these features are optional, they may be safely disregarded if not required.

## Demo
See [here](https://hchunhui.github.io/tiny386)

## Build

The project is designed with simplicity in mind. Just run `gcc -O3 *.c -o tiny386_headless -lm`, and you will get a "headless" executable without any optional features.

To build with display and sound:
- Linux (with rawdraw): Install `libslirp`, `libx11`, and `libasound2` first, then run `make`.
- Linux (with SDL): Install `libslirp` `SDL1.2` (or `sdl12-compat`) first, then run `make USE_SDL=y`.
- Windows: Install `mingw-w64` first, then run `make win32`.
- WebAssembly: Install `clang` first, then run `cd wasm; make`.

For details, please refer to `.github/workflows/build.yml` and `Makefile`.

`make USE_FAST=y` builds the CPU with a fast path (in `fast/`): common instructions run there, and the rest goes to the interpreter one instruction at a time. `make USE_FAST=pd` also decodes hot code once into blocks, which is faster but needs more memory. It's portable C (GCC/clang: computed goto) for little-endian hosts, as `i386.c`; on hosts without unaligned loads, build with `-DI386_ALIGNED_ONLY` as for `i386.c`. For the ESP32 build, compile `fast/fast.c` instead of `i386.c` with `-DUSE_FAST`.

Options (`CFLAGS`):
- `-DFAST_PD`: pre-decoding (`USE_FAST=pd`).
- `-DFAST_PD_SIZE=<bytes>`: memory for decoded blocks, default 2 MB. Pre-decoding also needs RAM/32 bytes (RAM/16 on 64-bit hosts) for its tables. If any of it can't be allocated, the fast path runs without pre-decoding.
- `-DFAST_VERIFY`: redo every instruction in the interpreter and compare (slow; for testing).

`make USE_FASTMEM=y` (Linux hosts on x86-64 or 32-bit ARM, e.g. ARMv7-A) lets the host MMU translate guest linear addresses for `i386.c` while paging is on, in the manner of Dolphin's fastmem. Guest RAM moves into a `memfd`, and guest pages are mapped into 4 GiB windows of host address space (read and write views, for the supervisor and for CPL 3) once the ordinary TLB path has allowed an access to them. After that, a memory operand costs one probing load instead of a TLB lookup. A probe that faults (page not mapped yet, not present, protected, or MMIO) is turned into a `false` by a `SIGSEGV` handler, and that access takes the old path. Guest page faults are raised exactly as before: same CR2 and error code, before the instruction has changed anything. The windows are flushed wherever the TLB is (CR0/CR3 writes, task switches, reset), lose one page on `invlpg`, and switch on CPL changes. With paging off nothing changes. `-bench` has paged rows (`pg ...`) that show the difference. Refilling a page costs a few microseconds, so phases that churn page tables (booting, `fork`) are no faster; a governor turns fastmem off for a while when refills come too often. Set `TINY386_FASTMEM=0` to switch it off at run time, `TINY386_FASTMEM=always` to disable the governor, and `TINY386_FASTMEM_STATS=1` for counters at exit. A 32-bit host has no room for 4 GiB windows, so there the windows cover only part of the guest's linear address space: 256 MiB from address 0 by default, or `TINY386_FASTMEM_WINDOW=base,size` (size up to 1G, e.g. `0xb0000000,512M` for a Linux guest's user stack and kernel). Accesses outside that part go the usual way. The default build and the ESP32 port are unaffected.

Pre-built binaries: [here](https://github.com/hchunhui/tiny386/releases)

## Usage

- Prepare an ini file
```ini
[pc]
; set path to BIOS and VGA BIOS
bios = bios.bin
vga_bios = vgabios.bin

; set memory size and VGA memory size
mem_size = 32M
vga_mem_size = 2M

; fda/fdb for floppy disks (optional)
fda = floppy.img

; hda/hdb/hdc/hdd for hard disks (optional)
; cda/cdb/cdc/cdd for CD-ROM disks (optional)
hda = win95.img
cdb = win95_cd.iso

; "fill_cmos" fixes "MS-DOS compatibility mode" in win9x, but it breaks winNT...
fill_cmos = 1

; force 8-dot mode (640 pixel wide in text mode) if set to 1
vga_force_8dm = 0

[display]
width = 720
height = 480

[cpu]
; gen = 3/4/5/6, for 386/486/586/686
gen = 3
; fpu = 0/1, to disable/enable x87
fpu = 0
```
- Run
```sh
./tiny386 config.ini
./tiny386 -kvm config.ini  # run with KVM (build with `make USE_CPUABS=y`)
```

For rawdraw and SDL port:
Press "Ctrl + ]" to grab/ungrab the keyboard and mouse. Press "Ctrl + [" to show/hide OSD (On Screen Display). In OSD mode, the floppy/CD-ROM disk can be changed on the fly.

## ESP32 port
Supported boards:

With ESP-IDF 5.2.x:
- JC3248W535 (ESP32-S3, 480x320)
- [Elecrow CrowPanel Advance 7.0" HMI](https://github.com/Elecrow-RD/CrowPanel-Advance-HMI-ESP32-AI-Display) (ESP32-S3, 800x480)

With ESP-IDF 6.0.x (experimental):
- JC4880P443 (ESP32-P4 Rev1.3 360MHz, 800x480)
- ClockworkPi PicoCalc with an ESP32-P4 in the Raspberry Pi Pico footprint
  (320x320) -- **work in progress**: display, keyboard, sound and microSD work,
  there is no mouse yet. Text mode is drawn 1:1 with a built-in 4x10 font,
  so use `vga_force_8dm = 1`. Files come from the microSD card in the ESP32-P4
  board's own slot (`esp/tiny386_picocalc.ini`), or, with no card inserted,
  from the `storage` flash partition, which `idf.py flash` fills from
  `esp/flash_data/` plus `bios.bin`/`vgabios.bin` in the repository root.
  Check the GPIO numbers in `esp/main/board_picocalc.h` against the ESP32-P4
  board or adapter you plug into the Pico socket.
  With `hda = /dev/mmcblk0` the guest uses the whole card as its disk instead
  of an image file, so a DOS running on another disk can read and write the
  card's own filesystem.
  `USE_USB_MSC` adds a boot mode (hold F1 at power-on) that exports the card to
  a host PC as a USB drive rather than starting the emulator; it is disabled
  because the board tested has a single USB socket wired to a UART bridge, so
  the ESP32-P4's own USB pins reach no host.

### Build and Flash
You can find the pre-built flash image `esp/flash_image_JC3248W535.bin` from [here](https://github.com/hchunhui/tiny386/releases).
The pre-built image can be flashed directly to offset 0.

Online flasher for esp chips: https://espressif.github.io/esptool-js

To build and flash manually:
```sh
scripts/build.sh patch_idf  # apply patches to ESP-IDF 5.2.x
#scripts/build.sh patch_idf_60  # apply patches to ESP-IDF 6.0.x
make prepare
cd esp
idf.py -DBOARD=jc3248w535 update-dependencies build  # or -DBOARD=elecrow7s3
idf.py flash
```

### Configure
All files should be put in a SD card with FAT/exFAT file system. The ini file should be `tiny386.ini` and put in the root directory.
Please refer to `esp/tiny386.ini`.

Alternative usage: `bios.bin` `vgabios.bin` `vmlinux.bin` and `linuxstart.bin` can be put in corresponding flash partition. Other files can be put in the `storage` flash partition. Please refer to `esp/partition.csv`.

### Keyboard/Mouse Input

- Forward over WIFI

`wifikbd` is used to forward keyboard/mouse events to the dev board over WIFI:
```
(ESP32-S3 board: listen on TCP port 9999) <--- WIFI ---> AP <--- WIFI/Wire ---> (PC: ./wifikbd esp_board_addr 9999)
```

- USB hid (experimental)

Add the following config:
```
[esp]
enable_usb = 1
```

Note: DO NOT enable USB and WIFI at the same time on ESP32-S3, due to insufficient memory.

More info, see [here](https://github.com/hchunhui/tiny386/pull/4).

## Troubleshooting

### "0 bytes of memory" during Windows 95 setup
Use "setup /im" to bypass memory check.

### "protection error" during Windows 95 startup
Use [patcher9x](https://github.com/JHRobotics/patcher9x).

### NE2000 doesn't work
Manually set the IRQ to 9(or 2).

### freeze during Windows NT4/2000/XP startup
Use `fill_cmos = 0` in the config ini file.

## License
The cpu emulator and the project as a whole are both licensed under the BSD-3-Clause license.

Adlib emulation is an optional part of the project, and it requires the library fmopl which is licensed under the LGPL.
Use `make USE_FMOPL=n` to build without adlib emulation.

SeaBIOS is distributed under the GNU LGPL-3 license.

Some parts ported from QEMU/TinyEMU are under the MIT license.
