include ../common.mk

MODULES = $(shell ls -d */)
OBJS    = $(shell ls -d ./*/*.o ./*/*/*.o)
KERNEL_ELF = ../kernel-qemu
KERNEL_LD  = kernel.ld

.PHONY: build clean $(MODULES) 

$(MODULES):
	$(MAKE) build --directory=$@

$(KERNEL_ELF): $(MODULES) $(KERNEL_LD)
	$(LD) $(LDFLAGS) -T $(KERNEL_LD) -o $(KERNEL_ELF) $(OBJS)

build: $(MODULES) $(KERNEL_ELF)

clean:
	for d in $(MODULES); \
		do \
			$(MAKE) clean --directory=$$d; \
		done
	rm -f ${KERNEL_ELF}