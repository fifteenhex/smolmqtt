# smolmqtt

A single-header MQTT 3.1.1 client on smolutils' helpers. Builds against
libc or as a fully static binary with nolibc + nolibc-extensions.

## What

`smolmqtt.h` implements the minimum of the protocol:

- CONNECT / CONNACK
- PUBLISH, QoS 0 and QoS 1 (waits for the PUBACK)
- SUBSCRIBE / SUBACK, with a callback for incoming messages
- PINGREQ (keepalive) and DISCONNECT

No auth, no TLS, no QoS 2. The broker address is an IPv4 dotted-quad, so
there is no DNS.

## Using it

```c
#include "smolmqtt.h"

struct smolmqtt m = { 0 };

smolmqtt_connect(&m, "192.168.3.2", 1883, "my-client-id");
smolmqtt_publish(&m, "some/topic", "hello", 5, 0, false);
smolmqtt_subscribe(&m, "some/topic", 0);
smolmqtt_poll(&m, my_callback, NULL);   /* blocks for one packet */
smolmqtt_disconnect(&m);
```

Everything is `static inline`; include it in one translation unit.

## Programs

- `smolmqtt_test [broker_ip]` -- subscribes to a unique topic, publishes
  to it and confirms the message comes back, plus a QoS 1 PUBACK check.
- `smolmqtt_pub <broker_ip> <topic> <message>`
- `smolmqtt_sub <broker_ip> <topic>`
- `mqtt2pty [-m ascii|data] [-l <link>] <broker_ip> <topic>` -- the other end
  of `serial2mqtt`: a pty on this machine that stands in for the shared port.
  Bytes written to it go to `<topic>/tx`, messages on `<topic>/rx` come out of
  it. It prints the pty's path (`/dev/pts/N`) and `-l` keeps a symlink at a
  fixed name, so a tool that wants a serial device can be pointed straight
  at it: `mqtt2pty -m data -l /tmp/clie 192.168.3.2 clielinux1 &` then
  `smallrom -d /tmp/clie ...`. The mode must match the far end's. Whatever
  the tool sets on the pty (a baud rate) is accepted and ignored; the real
  line runs at whatever `serial2mqtt` was started with.
- `file2mqtt [-r] [-s maxsize] <directory> <broker_ip> <topic>` -- serve a
  directory over MQTT: files can be pulled from it and pushed into it. `-r`
  makes it read-only, `-s` caps a push (default 32 MiB). Names are bare, with
  no slashes and no leading dot, so nothing outside the directory is
  reachable. A push is written to `<name>.part` and only renamed into place
  once its size and CRC match, so an interrupted transfer never replaces a
  good file with a broken one.
- `mqttfile [-t seconds] [-d usec] <broker_ip> <topic> list|get|put ...` --
  the other half: `list`, `get <remote> [local]`, `put <local> [remote]`. `-d`
  paces the chunks of a push, in microseconds (default 300).
- `serial2mqtt [-b baud] [-c 8N1] [-f none|dtr] [-m ascii|data] <serial>
  <broker_ip> <topic>` -- bridges a serial port to MQTT: bytes from the
  port go to `<topic>/rx`, messages on `<topic>/tx` go to the port. `data`
  mode base64-encodes each way so arbitrary binary survives. `-f dtr`
  honours the DTR/DSR handshake, for devices that hold the line low to say
  "stop sending".

## Building it

Both builds come from `smolcommon.mk`, a copy of smolutils' one; keep the two
in sync. The helpers are smolutils' too: its `inc/` does the option parsing,
the printing and the write loops, so a checkout of
[smolutils](https://github.com/fifteenhex/smolutils) has to be beside this
or named with `SMOLUTILSDIR`.

`make` builds all seven against the host's libc, into `<prog>.libc.elf`:

```
make SMOLUTILSDIR=/path/to/smolutils
./smolmqtt_test.libc.elf 192.168.3.2
```

`SMOL_ARCH` adds the static nolibc build. It needs a cross prefix, nolibc and
nolibc-extensions, plus `NOPIE=1` for a nolibc that cannot relocate itself:

```
make SMOL_ARCH=x86_64 CROSS_COMPILE=x86_64-linux-gnu- NOPIE=1 \
     SMOLUTILSDIR=/path/to/smolutils \
     NOLIBCDIR=/path/to/linux/tools/include/nolibc \
     NOLIBCEXTDIR=/path/to/nolibc-extensions
```

`SMOL_ARCH` is one of `x86_64`, `cortexa7`, `cortexa9`, `68000`, `68030`,
`68040`, `68060`, `sh4`. Output is `<prog>.<arch>.elf`, stripped, with the
symbols in `<prog>.<arch>.elf.dbg` beside it, so one tree holds several
targets. Add `UAPIDIR=/path/to/uapi/include` when the target's kernel headers
are not this machine's. `V=1` prints the command lines.

## DTR/DSR flow control

Linux termios only does RTS/CTS, so `-f dtr` is by hand: with a null modem
the far end's DTR arrives on our DSR, and `serial2mqtt` waits for it before
each few bytes. Off by default, since a device that never drives DTR would
stall every write until the timeout.

## Rootfs tarball

`rootfs.tarwak.json` lays the static binaries out under `/bin`. Adding `TARWAK`
to the static build above produces `smolmqtt.<arch>.tar`:

```
make SMOL_ARCH=x86_64 CROSS_COMPILE=x86_64-linux-gnu- NOPIE=1 \
     NOLIBCDIR=/path/to/linux/tools/include/nolibc \
     NOLIBCEXTDIR=/path/to/nolibc-extensions \
     TARWAK=/path/to/tarwak
```

[tarwak](https://github.com/fifteenhex/tarwak) builds the tarball.
