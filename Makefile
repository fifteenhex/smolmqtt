COPTS = -ggdb -std=c99 -Os

# -Wall and -Wextra are left off: nolibc-extensions defines a lot it does not
# use, so a static build buries anything of ours in -Wunused-function.

HDR   := smolmqtt.h filemqtt.h
PROGS := smolmqtt_test smolmqtt_pub smolmqtt_sub serial2mqtt file2mqtt mqttfile

all: $(PROGS)

# The usual libc build.
$(PROGS): %: %.c $(HDR)
	$(CC) $(COPTS) -o $@ $<

# Fully static nolibc build: pass NOLIBCDIR (your nolibc, e.g.
# tools/include/nolibc in the linux source) and NOLIBCEXTDIR (a checkout of
# nolibc-extensions, which carries the sockets nolibc lacks).
ifdef NOLIBCDIR
ifndef NOLIBCEXTDIR
$(warning Please also pass NOLIBCEXTDIR with the path to your nolibc-extensions checkout for static targets)
else
NOLIBC_PROGS := $(addsuffix _nolibc,$(PROGS))
all: $(NOLIBC_PROGS)

NOLIBC_INC = -include $(NOLIBCDIR)/nolibc.h \
	     -include $(NOLIBCEXTDIR)/include/nolibc-extensions.h

$(NOLIBC_PROGS): %_nolibc: %.c $(HDR)
	$(CC) -nostdlib $(NOLIBC_INC) $(COPTS) -static -o $@ $< -lgcc

# A rootfs tarball of the static binaries (installed as /bin/smolmqtt_*), built
# with tarwak. Pass TARWAK=<path to the tarwak binary>.
ifdef TARWAK
all: smolmqtt.tar

smolmqtt.tar: rootfs.tarwak.json $(NOLIBC_PROGS)
	$(TARWAK) -i rootfs.tarwak.json -o $@ -b ./ -p "%s_nolibc"
else
$(warning Pass TARWAK=<path to tarwak> to also build the smolmqtt.tar rootfs)
endif
endif
else
$(warning Pass NOLIBCDIR and NOLIBCEXTDIR to also build static nolibc binaries)
endif

.PHONY: clean
clean:
	rm -f $(PROGS) $(addsuffix _nolibc,$(PROGS)) smolmqtt.tar
