# Cross-builds an x86-64 bare-metal image with clang + lld (works on an arm64 host).
CC      := clang
LD      := ld.lld
OBJCOPY := llvm-objcopy
TARGET  := --target=x86_64-unknown-none-elf
CFLAGS  := $(TARGET) -O2 -std=c11 -Wall -Wextra -ffreestanding -fno-pic -fno-pie \
           -mno-red-zone -mgeneral-regs-only -fno-stack-protector -fno-builtin \
           -fno-asynchronous-unwind-tables -mcmodel=small
ASFLAGS := $(TARGET) -fno-pic
B       := build
OBJS    := $(B)/boot.o $(B)/entry.o $(B)/guest16.o $(B)/main.o

all: $(B)/minivisor.img

$(B):
	mkdir -p $@
$(B)/%.o: %.c vmx.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: %.S | $(B)
	$(CC) $(ASFLAGS) -c $< -o $@
$(B)/minivisor.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld -nostdlib -static -o $@ $(OBJS)
# Flat disk image: 2 cyl x 16 heads x 63 spt = 2016 sectors (geometry in bochsrc).
$(B)/minivisor.img: $(B)/minivisor.elf
	$(OBJCOPY) -O binary $< $@
	truncate -s $$((2016 * 512)) $@

run: $(B)/minivisor.img
	./run.sh

clean:
	rm -rf $(B)

.PHONY: all run clean
