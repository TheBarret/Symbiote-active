# Nuke built-in rules and delete the target of a failed recipe.
.SUFFIXES:
.DELETE_ON_ERROR:

IMAGE_NAME := symbiote-x86_64

# Extensions to build into the image (see kernel/src/ext/). Override like:
#   make EXTENSIONS="hello"       headless image, serial console only
# disabled: forth bf btool cpu vfs explorer
EXTENSIONS ?= fbcon system memory env envtool

# Extra QEMU flags. -serial stdio sends the serial console to your terminal.
QEMUFLAGS := -m 256M -serial stdio

override QEMU_MACHINE := -M q35
override QEMU_UEFI := -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on

.PHONY: all
all: $(IMAGE_NAME).iso

# Boot in UEFI mode (needs OVMF, downloaded on first use).
.PHONY: run
run: edk2-ovmf-bins $(IMAGE_NAME).iso
	qemu-system-x86_64 $(QEMU_MACHINE) $(QEMU_UEFI) -cdrom $(IMAGE_NAME).iso $(QEMUFLAGS)

# Boot in legacy BIOS mode (no download needed; the quickest way to iterate).
.PHONY: run-bios
run-bios: $(IMAGE_NAME).iso
	qemu-system-x86_64 $(QEMU_MACHINE) -cdrom $(IMAGE_NAME).iso -boot d $(QEMUFLAGS)

# No window at all: the serial console is your terminal. Ctrl-A X quits.
.PHONY: run-serial
run-serial: $(IMAGE_NAME).iso
	qemu-system-x86_64 $(QEMU_MACHINE) -cdrom $(IMAGE_NAME).iso -boot d $(QEMUFLAGS) -display none

.PHONY: size
size:
	$(MAKE) -C kernel size EXTENSIONS="$(EXTENSIONS)"

.INTERMEDIATE: edk2-ovmf-bins.tar.gz
edk2-ovmf-bins.tar.gz:
	curl -fL -o $@ https://github.com/osdev0/edk2-ovmf-stable-bins/releases/latest/download/edk2-ovmf-bins.tar.gz

edk2-ovmf-bins: edk2-ovmf-bins.tar.gz
	rm -rf edk2-ovmf-bins
	gunzip < edk2-ovmf-bins.tar.gz | tar -xf -

.INTERMEDIATE: limine-binary.tar.gz
limine-binary.tar.gz:
	curl -fL -o $@ https://github.com/Limine-Bootloader/Limine/releases/latest/download/limine-binary.tar.gz

limine-binary/limine: limine-binary.tar.gz
	rm -rf limine-binary
	gunzip < limine-binary.tar.gz | tar -xf -
	$(MAKE) -C limine-binary

kernel/.deps-obtained:
	./kernel/get-deps

# Always re-enter the kernel makefile; it knows what is out of date.
.PHONY: kernel
kernel: kernel/.deps-obtained
	$(MAKE) -C kernel EXTENSIONS="$(EXTENSIONS)"

# Include files in ISO block:
# Add: cp boot/your.file iso_root/boot/your.file
# (dont forget to update limine.conf for each entry)

$(IMAGE_NAME).iso: limine-binary/limine kernel limine.conf
	rm -rf iso_root
	mkdir -p iso_root/boot/limine iso_root/EFI/BOOT
	cp kernel/bin-x86_64/symbiote iso_root/boot/
	cp limine.conf iso_root/boot/limine/
	cp limine-binary/limine-bios.sys limine-binary/limine-bios-cd.bin \
	   limine-binary/limine-uefi-cd.bin iso_root/boot/limine/
	cp limine-binary/BOOTX64.EFI limine-binary/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
	    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
	    -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    iso_root -o $(IMAGE_NAME).iso
	./limine-binary/limine bios-install $(IMAGE_NAME).iso
	rm -rf iso_root

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso

.PHONY: distclean
distclean:
	$(MAKE) -C kernel distclean
	rm -rf iso_root *.iso limine-binary limine-binary.tar.gz edk2-ovmf-bins edk2-ovmf-bins.tar.gz
