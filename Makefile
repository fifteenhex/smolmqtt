COPTS = -ggdb -std=c99 -Os

#	-Wall \
#	-Wextra \

HDR   := smolmqtt.h
PROGS := smolmqtt_test smolmqtt_pub smolmqtt_sub

all: $(PROGS)

$(PROGS): %: %.c $(HDR)
	$(CC) $(COPTS) -o $@ $<

.PHONY: clean
clean:
	rm -f $(PROGS)
