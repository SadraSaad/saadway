# host unit-test Makefile (T042) — see README.md for prerequisites
#
# Prefers real installed libraries (libcmocka/libjson-c); when absent, it
# links the semantic-shim implementations from the syntax-check stub pool
# (DEV-ONLY, never shipped) so the suite still runs on a dev box:
#   $(SWSTUBS)/shim_impl/cmocka_impl.c   cmocka-compatible runner macros
#   $(SWSTUBS)/shim_impl/json_impl.c     json-c-compatible codec subset
#   $(SWSTUBS)/shim_impl/mbedtls_impl.c  SHA-256 reference implementation
# Declare-only stub headers (uci.h, libubox/*, libubus.h, mbedtls) come
# from the same pool; the uci shim fails closed (shim_uci.c below).

CC     ?= cc
SWSTUBS ?= /tmp/opencode/swstubs
INCS   := -I.. -I$(SWSTUBS)
WARN   := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
CFLAGS ?= -std=c11 -D_GNU_SOURCE -O0 -g $(WARN)

# prefer real libraries; shim in only those that are missing
USE_REAL_CMOCKA := $(shell [ -e /usr/include/cmocka.h ] || \
	[ -e /usr/include/cmocka/cmocka.h ] && echo y)
USE_REAL_JSONC  := $(shell [ -e /usr/include/json-c/json.h ] && echo y)
USE_REAL_MBEDTLS := $(shell [ -e /usr/include/mbedtls/sha256.h ] && echo y)

LIBS := $(if $(USE_REAL_CMOCKA),-lcmocka,) $(if $(USE_REAL_JSONC),-ljson-c,) \
	$(if $(USE_REAL_MBEDTLS),-lmbedtls -lmbedcrypto,) -lcrypt

LOCAL_SRC += $(if $(USE_REAL_CMOCKA),,$(SWSTUBS)/shim_impl/cmocka_impl.c)
LOCAL_SRC += $(if $(USE_REAL_JSONC),,$(SWSTUBS)/shim_impl/json_impl.c)
LOCAL_SRC += $(if $(USE_REAL_MBEDTLS),,$(SWSTUBS)/shim_impl/mbedtls_impl.c)

SRC    := $(patsubst %,../%.c,uci-state log steering policy failover \
                              backup tunnel-openvpn tunnel-ikev2 dns)
TESTS  := test_policy_render test_steer_order test_failover test_backup_codec \
          test_name_rules

TEST_DEPS := test-stubs.c shim_uci.c $(SRC:.c=.o) $(LOCAL_SRC)

%.o: %.c
	$(CC) $(CFLAGS) $(INCS) -c -o $@ $<

all: $(TESTS)

$(TESTS): %: %.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(INCS) -o $@ $@.c test-stubs.c shim_uci.c \
		$(SRC:.c=.o) $(LOCAL_SRC) $(LIBS)

check: all
	@for t in $(TESTS); do \
		echo "== $$t"; \
		./$$t || { echo "FAILED: $$t"; exit 1; }; \
	done
	@echo "ALL UNIT TESTS PASSED"

clean:
	$(RM) -f $(TESTS) *.o

.PHONY: all check clean
