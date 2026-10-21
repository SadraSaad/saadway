/* creds.c — the standalone dashboard's own login credential (Phase 23 / T076)
 *
 * The dashboard's login identity lives in the platform's restricted store
 * (/etc/shadow, 0600) and is referenced — never copied — from
 * /etc/config/rpcd as `option password '$p$<user>'` (upstream resolves the
 * suffix with getspnam() and compares with crypt(3)).
 *
 * This module therefore never computes, stores or logs a hash: it validates
 * the administrator's input and relays the new secret ONCE over a pipe to
 * the root-only provisioning helper
 * (/usr/libexec/saadway/provision set-credentials <user>), which drives the
 * platform's own passwd (the plaintext goes to the child's STDIN, never to
 * argv) and then updates the login mapping and reloads rpcd. The daemon
 * keeps no credential store — it holds the secret only for the duration of
 * the call, and the reply never echoes it (FR-033).
 *
 * The method is granted to a dedicated, root-only ACL group
 * (saadway-credentials); the dashboard's own session must never be able to
 * re-key its login (FR-034).
 */

#define _GNU_SOURCE
#include "saadway.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#define SW_CREDS_USER_MAX    32
#define SW_CREDS_PASS_MAX    128
#define SW_CREDS_TIMEOUT_MS  15000
#define SW_CREDS_HELPER      "/usr/libexec/saadway/provision"

static struct {
	struct ubus_context      *ctx;
	struct ubus_request_data  dreq;
	bool                      active;
	pid_t                     pid;
	struct uloop_timeout      to;
} g_creds;

/* ------------------------------------------------------------------ */
/* validation (FR-031)                                                  */
/* ------------------------------------------------------------------ */

/* The login name ends up in a world-readable UCI option, so it is not a
 * secret — but it must be a safe account name (it becomes a /etc/passwd
 * row). Restricted to the portable set [A-Za-z0-9._-]. */
static bool creds_username_ok(const char *u)
{
	size_t n, i;

	if (!u || !*u)
		return false;
	n = strlen(u);
	if (n > SW_CREDS_USER_MAX)
		return false;
	for (i = 0; i < n; i++) {
		char ch = u[i];

		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
		      ch == '-'))
			return false;
	}
	return true;
}

static bool creds_password_ok(const char *p)
{
	size_t n;

	if (!p || !*p)
		return false;
	n = strlen(p);
	return n <= SW_CREDS_PASS_MAX;
}

/* the validation is exported so the ubus handler can reject the request
 * BEFORE the reply is deferred (an invalid input must never reach the
 * helper) */
bool sw_creds_validate(const char *username, const char *password)
{
	return creds_username_ok(username) && creds_password_ok(password);
}

/* ------------------------------------------------------------------ */
/* completion                                                           */
/* ------------------------------------------------------------------ */

static void creds_finish(int status)
{
	struct blob_buf b;

	if (!g_creds.active)
		return;
	g_creds.active = false;
	g_creds.pid = 0;
	uloop_timeout_cancel(&g_creds.to);

	if (status == UBUS_STATUS_OK) {
		blob_buf_init(&b, 0);
		blobmsg_add_bool(&b, "applied", true);
		ubus_send_reply(g_creds.ctx, &g_creds.dreq, b.head);
		blob_buf_free(&b);
		ubus_complete_deferred_request(g_creds.ctx, &g_creds.dreq,
					       UBUS_STATUS_OK);
		return;
	}
	/* the failure detail never contains the secret */
	ubus_complete_deferred_request(g_creds.ctx, &g_creds.dreq, status);
}

/* the reaper (main.c) reports the child's exit */
void sw_creds_child_exited(pid_t pid, int status)
{
	if (!g_creds.active || g_creds.pid != pid)
		return;

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		sw_info("dashboard credential applied");
		creds_finish(UBUS_STATUS_OK);
		return;
	}
	sw_warn("dashboard credential apply failed (child status %d)",
		WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	creds_finish(UBUS_STATUS_UNKNOWN_ERROR);
}

/* bounded wait: a stuck helper must not hold the request forever */
static void creds_timeout(struct uloop_timeout *t)
{
	(void)t;
	if (!g_creds.active)
		return;
	sw_warn("dashboard credential apply timed out");
	if (g_creds.pid > 0)
		kill(g_creds.pid, SIGTERM);
	creds_finish(UBUS_STATUS_UNKNOWN_ERROR);
}

/* ------------------------------------------------------------------ */
/* entry point                                                          */
/* ------------------------------------------------------------------ */

bool sw_creds_busy(void)
{
	return g_creds.active;
}

/* sw_creds_start() defers the request and spawns the helper.
 * Returns 0 when the work is under way (the reply is deferred and will be
 * completed by the reaper), -2/-3 when the input was rejected or another
 * change is running (NOT deferred — the caller replies), and -1 when the
 * spawn failed AFTER the deferral (the request is already completed here). */
int sw_creds_start(struct ubus_context *ctx, struct ubus_request_data *req,
		   const char *username, const char *password)
{
	int fds[2];
	pid_t pid;
	ssize_t n;

	/* defensive re-check (the handler validates first) */
	if (!sw_creds_validate(username, password))
		return -2;
	if (g_creds.active)
		return -3;

	/* defer BEFORE starting the work (the T061 ordering): the deferred
	 * reply can then never race ahead of the deferral itself. */
	ubus_defer_request(ctx, req, &g_creds.dreq);
	g_creds.ctx = ctx;
	g_creds.active = true;
	g_creds.pid = 0;

	if (pipe(fds) != 0) {
		sw_err("credential apply: pipe failed");
		ubus_complete_deferred_request(ctx, &g_creds.dreq,
					       UBUS_STATUS_UNKNOWN_ERROR);
		g_creds.active = false;
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		sw_err("credential apply: fork failed");
		ubus_complete_deferred_request(ctx, &g_creds.dreq,
					       UBUS_STATUS_UNKNOWN_ERROR);
		g_creds.active = false;
		return -1;
	}
	if (pid == 0) {
		/* child: the password arrives on stdin, never on argv */
		close(fds[1]);
		if (dup2(fds[0], STDIN_FILENO) < 0)
			_exit(127);
		close(fds[0]);
		execl(SW_CREDS_HELPER, "provision", "set-credentials",
		      username, (char *)NULL);
		_exit(127);
	}

	close(fds[0]);
	n = write(fds[1], password, strlen(password));
	if (n > 0)
		(void)write(fds[1], "\n", 1);
	close(fds[1]);

	g_creds.pid = pid;
	sw_child_track(pid, SW_CHILD_CREDS, NULL);
	uloop_timeout_set(&g_creds.to, SW_CREDS_TIMEOUT_MS);
	sw_info("dashboard credential apply started");
	return 0;
}
