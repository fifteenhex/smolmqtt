# smolmqtt

A single-header MQTT 3.1.1 client. One header, no dependencies. Builds
against libc or as a fully static binary with nolibc + nolibc-extensions.

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

## Test programs

- `smolmqtt_test [broker_ip]` -- subscribes to a unique topic, publishes
  to it and confirms the message comes back, plus a QoS 1 PUBACK check.
- `smolmqtt_pub <broker_ip> <topic> <message>`
- `smolmqtt_sub <broker_ip> <topic>`

```
make
./smolmqtt_test 192.168.3.2
```

## Static nolibc build

Pass `NOLIBCDIR` (your nolibc, e.g. `tools/include/nolibc` in the linux
source) and `NOLIBCEXTDIR` (a checkout of nolibc-extensions):

```
make NOLIBCDIR=/path/to/nolibc NOLIBCEXTDIR=/path/to/nolibc-extensions
```

builds `smolmqtt_{test,pub,sub}_nolibc` as fully static binaries.
