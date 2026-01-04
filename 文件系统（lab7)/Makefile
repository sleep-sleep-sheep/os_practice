include common.mk

KERN = kernel
KERNEL_ELF = kernel-qemu
CPUNUM = 2
# 修正：FS_IMG 改为有效磁盘镜像文件名
FS_IMG = fs.img
# 新增：磁盘镜像大小配置
FS_SIZE = 10M

.PHONY: clean $(KERN)

# 新增：构建磁盘镜像 fs.img
$(FS_IMG):
	@echo "Creating disk image $(FS_IMG) (size: $(FS_SIZE))..."
	dd if=/dev/zero of=$(FS_IMG) bs=$(FS_SIZE) count=1 status=none
	@echo "Disk image $(FS_IMG) created successfully."

$(KERN):
	$(MAKE) build --directory=$@

# QEMU相关配置
QEMU     =  qemu-system-riscv64
QEMUOPTS =  -machine virt -bios none -kernel $(KERNEL_ELF) 
QEMUOPTS += -m 128M -smp $(CPUNUM) -nographic
QEMUOPTS += -drive file=$(FS_IMG),if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
# 调试
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)

build: $(KERN)

# qemu运行（依赖磁盘镜像）
qemu: $(KERN) $(FS_IMG)
	$(QEMU) $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

# 调试模式（同步依赖磁盘镜像）
qemu-gdb: $(KERN) $(FS_IMG) .gdbinit
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

clean:
	$(MAKE) --directory=$(KERN) clean
	rm -f $(KERNEL_ELF) .gdbinit
	# 清理磁盘镜像
	rm -f $(FS_IMG)