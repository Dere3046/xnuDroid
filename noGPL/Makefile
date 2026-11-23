obj-m := xnu.o

KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ODIR := $(MDIR)/out/$(if $(VER),$(VER),unknown)
FKI_SYMVERS ?= $(MDIR)/../out/$(if $(VER),$(VER),unknown)/Module.symvers

xnu-y := src/xnu_main.o \
	 src/xnu_boot.o \
	 src/xnu_fault.o \
	 src/xnu_upcall.o \
	 src/xnu_mon.o \
	 src/macho.o \
	 src/adt.o \
	 src/world.o

ccflags-y += -std=gnu11
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-strict-prototypes
ccflags-y += -I$(src)/src
ccflags-y += -I$(src)/../include

$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))
$(info -- ODIR: $(ODIR))

all:
	mkdir -p $(ODIR)
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) srcroot=$(MDIR) KBUILD_EXTRA_SYMBOLS=$(FKI_SYMVERS) modules
clean:
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) srcroot=$(MDIR) clean

$(obj)/%.o: $(src)/%.c $(recordmcount_source) FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)

$(obj)/%.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)
