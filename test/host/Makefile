# Host-side unit tests (G1 gate, coding plan 5): build with any C99 compiler.
#   make -C test/host        (or: cc -Icomponents/c6_proto ... directly)
CC      ?= gcc
CFLAGS  ?= -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter
INC     := -I../../../components/c6_proto -I../../../components/c6_ota

TESTS   := test_proto test_sha512 test_ed25519 test_bundle
RUN_TARGETS := $(addprefix run-,$(TESTS))

all: $(TESTS)

test_proto: test_proto.c ../../components/c6_proto/proto_frames.c
	$(CC) $(CFLAGS) $(INC) -o $@ $^

test_sha512: test_sha512.c ../../components/c6_ota/sha512.c
	$(CC) $(CFLAGS) $(INC) -o $@ $^

test_ed25519: test_ed25519.c ../../components/c6_ota/ed25519v.c ../../components/c6_ota/sha512.c
	$(CC) $(CFLAGS) $(INC) -o $@ $^

test_bundle: test_bundle.c ../../components/c6_ota/bundle.c ../../components/c6_ota/sha512.c ../../components/c6_ota/ed25519v.c ../../components/c6_proto/proto_frames.c
	$(CC) $(CFLAGS) $(INC) -o $@ $^ $(LIBS)

run-: ;@true
run-%: %
	./$<

check: $(TESTS) $(RUN_TARGETS)

clean:
	rm -f $(TESTS)

.PHONY: all check clean
