PROGS := smolmqtt_test smolmqtt_pub smolmqtt_sub serial2mqtt mqtt2pty file2mqtt mqttfile gpio2mqtt

# The helpers come from smolutils (config.h and inc/*.h). Pass SMOLUTILSDIR
# with a checkout; a sibling directory is the default.
SMOLUTILSDIR ?= ../smolutils
ifeq ($(wildcard $(SMOLUTILSDIR)/config.h),)
$(warning Pass SMOLUTILSDIR=<path to a smolutils checkout>; $(SMOLUTILSDIR)/config.h is not there)
endif

# smolcommon.mk has real targets of its own (the formatting ones), so say
# which goal is the default before including it.
.DEFAULT_GOAL := all

HEADERS += smolmqtt.h filemqtt.h base64.h
HEADERS += $(SMOLUTILSDIR)/config.h $(wildcard $(SMOLUTILSDIR)/inc/*.h)

# Both builds come out of smolcommon.mk, which is a copy of smolutils' one --
# keep the two in sync. SMOL_ARCH picks a target and it will say what else that
# wants: NOLIBCDIR, NOLIBCEXTDIR and CROSS_COMPILE. Upstream nolibc cannot self
# relocate yet, so pass NOPIE=1 with those.
include smolcommon.mk

# Both compilers have to find smolutils' inc/; appended after the include so
# the libc side's defaults stay as smolcommon.mk has them. smolutils is
# -isystem on the libc side: its helpers are written against nolibc and have
# never been built against a libc, so what -Wall -Wextra say about them there
# is not ours to clean up.
COPTS += -I$(SMOLUTILSDIR)
LIBC_COPTS += -isystem $(SMOLUTILSDIR)

# nolibc's sh entry point reaches _start_c with bsr, whose displacement is
# 12 bits, and LTO is free to lay the two out further apart than that:
#
#   Error: displacement to defined symbol _start_c overflows 12-bit field
#
# serial2mqtt is big enough for it to happen, so sh builds without LTO until
# nolibc reaches _start_c some other way.
ifeq ($(SMOL_ARCH),sh4)
COPTS := $(filter-out -flto,$(COPTS))
endif

LIBC_ELFS := $(addsuffix .libc.elf,$(PROGS))

all: $(LIBC_ELFS)

ifdef SMOL_ARCH
ELFS := $(addsuffix .$(SMOL_SUFFIX).elf,$(PROGS))
TARBALL := smolmqtt.$(SMOL_SUFFIX).tar

all: $(ELFS)

# A rootfs tarball, laid out by rootfs.tarwak.json. Pass TARWAK=<path to the
# tarwak binary>.
ifdef TARWAK
all: $(TARBALL)

$(TARBALL): rootfs.tarwak.json $(ELFS)
	$(MSG) TARWAK $@
	$(Q)$(TARWAK) -i $< -o $@ -b ./ -p "%s.$(SMOL_SUFFIX).elf"
endif
endif

.PHONY: all clean
clean:
	$(Q)rm -f .buildmode.* *.elf *.elf.dbg *.tar
