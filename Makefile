obj-m := fki.o

KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ODIR := $(MDIR)/out/$(if $(VER),$(VER),unknown)

DEPS := KallRecon Type_info HooKern
include $(MDIR)/mk/deps.mk

fki-y := src/core/fki_main.o \
	 src/core/fki_mem.o \
	 src/core/fki_cpu.o \
	 src/core/fki_io.o \
	 src/core/fki_hostcall.o \
	 src/hook/fki_hook.o \
	 src/tool/sym_reg.o \
	 $(DEPS_OBJS_ALL)

ccflags-y += -std=gnu11
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-strict-prototypes
ccflags-y += -I$(src)/include
ccflags-y += -I$(src)/src/core
ccflags-y += -I$(src)/src/hook
ccflags-y += -I$(src)/src/tool
ccflags-y += $(addprefix -I$(src)/,$(DEPS_INCS_ALL))

$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))
$(info -- ODIR: $(ODIR))

all:
	mkdir -p $(ODIR)
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) srcroot=$(MDIR) modules
clean:
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) srcroot=$(MDIR) clean

$(obj)/%.o: $(src)/%.c $(recordmcount_source) FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)

$(obj)/%.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)
