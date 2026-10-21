# Test rig for the Cable Link of the mGBA libretro core. "make" builds the
# link test ROM (needs the Miyoo Mini toolchain) and the harness for this
# machine; "make arm" the harness for qemu-arm. run.sh runs a test.
TC      ?= /opt/miyoomini-toolchain/usr/bin/arm-linux
ROMFLAGS = -march=armv4t -marm -mfloat-abi=soft -ffreestanding -nostdlib -O2 -fno-pic
INC      = -I../../src/platform/libretro

all: dualtest

linktest.gba: crt0.S linktest.c rom.ld
	$(TC)-gcc $(ROMFLAGS) -c crt0.S -o crt0.o
	$(TC)-gcc $(ROMFLAGS) -c linktest.c -o linktest.o
	$(TC)-ld -T rom.ld crt0.o linktest.o -o linktest.elf --no-warn-mismatch
	$(TC)-objcopy -O binary linktest.elf $@
	truncate -s 2M $@
	rm -f crt0.o linktest.o linktest.elf

dualtest: dualtest.c
	gcc -O2 -Wall $(INC) -o $@ $< -ldl -lpthread

arm: dualtest.c
	$(TC)-gcc -O2 -Wall $(INC) -o dualtest-arm $< -ldl -lpthread

clean:
	rm -f dualtest dualtest-arm crt0.o linktest.o linktest.elf
