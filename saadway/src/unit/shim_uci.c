/* shim_uci.c — unit-test shim for the libuci surface (T042).
 *
 * The four units under test never require REAL persistence behavior; the
 * shim FAILS CLOSED: every libuci call logs and returns an error, like a
 * missing/failing backing store would on-device, so units' error paths
 * are validated. Set SW_UNIT_STRICT_SHIM=1 to escalate any shim hit into
 * an abort (for debugging an out-of-scope path).
 * Declarations come from the repo syntax stub libuci.h.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include <libuci.h>
#include <stdlib.h>

static int hit(const char *what)
{
	static int logged;

	fprintf(stderr, "unit-shim: libuci '%s' reached (fail-closed)%s\n",
		what, logged ? "" : " — set SW_UNIT_STRICT_SHIM=1 to abort");
	logged = 1;
	if (getenv("SW_UNIT_STRICT_SHIM")) {
		assert(!!"libuci path entered in unit test");
	}
	return -1;
}

struct uci_context *uci_alloc_context(void)
{
	(void)hit("uci_alloc_context");
	return NULL;
}

void uci_free_context(struct uci_context *ctx)
{
	(void)ctx;
}

int uci_load(struct uci_context *ctx, const char *name,
	     struct uci_package **pkg)
{
	(void)ctx; (void)name; (void)pkg;
	return hit("uci_load");
}

int uci_unload(struct uci_context *ctx, struct uci_package *pkg)
{
	(void)ctx; (void)pkg;
	return -1;
}

int uci_commit(struct uci_context *ctx, struct uci_package **pkg,
	       bool overwrite)
{
	(void)ctx; (void)pkg; (void)overwrite;
	return hit("uci_commit");
}

int uci_save(struct uci_context *ctx, struct uci_package *pkg)
{
	(void)ctx; (void)pkg;
	return hit("uci_save");
}

int uci_set(struct uci_context *ctx, struct uci_ptr *ptr)
{
	(void)ctx; (void)ptr;
	return hit("uci_set");
}

int uci_delete(struct uci_context *ctx, struct uci_ptr *ptr)
{
	(void)ctx; (void)ptr;
	return hit("uci_delete");
}

int uci_lookup_ptr(struct uci_context *ctx, struct uci_ptr *ptr, char *str,
		   bool extended)
{
	(void)ctx; (void)ptr; (void)str; (void)extended;
	return hit("uci_lookup_ptr");
}

const char *uci_lookup_option_string(struct uci_context *ctx,
				     struct uci_section *s, const char *name)
{
	(void)ctx; (void)s; (void)name;
	(void)hit("uci_lookup_option_string");
	return NULL;
}
