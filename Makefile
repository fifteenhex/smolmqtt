PROGS := smolmqtt_test smolmqtt_pub smolmqtt_sub serial2mqtt file2mqtt mqttfile

HEADERS += smolmqtt.h filemqtt.h

LIBC_ELFS := $(addsuffix .libc.elf,$(PROGS))

# Ahead of the include so this is the default goal and not smolcommon.mk's
# first target
all: $(LIBC_ELFS)

# A copy of smolutils' smolcommon.mk; keep the two in sync. SMOL_ARCH picks the
# target and errors with what else it needs. NOPIE=1 for a nolibc that cannot
# relocate itself.
include smolcommon.mk

ifdef SMOL_ARCH
ELFS := $(addsuffix .$(SMOL_SUFFIX).elf,$(PROGS))
TARBALL := smolmqtt.$(SMOL_SUFFIX).tar

all: $(ELFS)

# A rootfs tarball, laid out by rootfs.tarwak.json.
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
