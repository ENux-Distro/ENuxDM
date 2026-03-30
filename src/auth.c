#include "auth.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static struct enuxdm_auth_cred *volatile g_active_cred;

#define ENV_KEEP_SLOTS 24
#define ENV_KEEP_LEN 768
static char env_keep[ENV_KEEP_SLOTS][ENV_KEEP_LEN];

/* Without pam_systemd in our PAM stack, /run/user/<uid> is often missing — D-Bus/XFCE need it. */
static int setup_xdg_runtime_dir(uid_t uid, gid_t gid, char *out, size_t outsiz)
{
	char path[128];
	snprintf(path, sizeof path, "/run/user/%u", (unsigned)uid);

	struct stat st;
	if (stat(path, &st) == 0) {
		if (!S_ISDIR(st.st_mode))
			goto fallback;
		(void)chmod(path, 0700);
		(void)chown(path, uid, gid);
		snprintf(out, outsiz, "%s", path);
		return 0;
	}

	if (mkdir(path, 0700) == -1 && errno != EEXIST)
		goto fallback;
	if (stat(path, &st) == -1 || !S_ISDIR(st.st_mode))
		goto fallback;
	(void)chmod(path, 0700);
	(void)chown(path, uid, gid);
	snprintf(out, outsiz, "%s", path);
	return 0;

fallback:
	snprintf(path, sizeof path, "/tmp/enuxdm-runtime-%u", (unsigned)uid);
	if (mkdir(path, 0700) == -1 && errno != EEXIST)
		return -1;
	if (stat(path, &st) == -1 || !S_ISDIR(st.st_mode))
		return -1;
	(void)chmod(path, 0700);
	(void)chown(path, uid, gid);
	snprintf(out, outsiz, "%s", path);
	return 0;
}

static int capture_locale_env(void)
{
	static const char *keys[] = {
		"LANG",		 "LANGUAGE",	  "LC_ALL",       "LC_CTYPE",   "LC_NUMERIC",
		"LC_TIME",	 "LC_COLLATE",	  "LC_MONETARY",  "LC_MESSAGES",
		"LC_PAPER",	 "LC_NAME",	  "LC_ADDRESS",   "LC_TELEPHONE",
		"LC_MEASUREMENT", "LC_IDENTIFICATION", "XDG_VTNR", "XDG_SEAT", NULL,
	};
	int n = 0;
	for (const char **k = keys; *k && n < ENV_KEEP_SLOTS; k++) {
		const char *v = getenv(*k);
		if (!v || !v[0])
			continue;
		int w = snprintf(env_keep[n], ENV_KEEP_LEN, "%s=%s", *k, v);
		if (w < 0 || w >= (int)ENV_KEEP_LEN)
			continue;
		n++;
	}
	return n;
}

static void replay_captured_env(int n)
{
	for (int i = 0; i < n; i++)
		putenv(env_keep[i]);
}

static const char *find_dbus_run_session(void)
{
	static const char *const cands[] = {"/usr/bin/dbus-run-session", "/bin/dbus-run-session", NULL};

	for (size_t i = 0; cands[i]; i++) {
		if (access(cands[i], X_OK) == 0)
			return cands[i];
	}
	return NULL;
}

static const char *find_startxfce4(void)
{
	static const char *const cands[] = {"/usr/bin/startxfce4", "/bin/startxfce4", NULL};

	for (size_t i = 0; cands[i]; i++) {
		if (access(cands[i], X_OK) == 0)
			return cands[i];
	}
	return "/usr/bin/startxfce4";
}

void enuxdm_auth_cred_clear(struct enuxdm_auth_cred *c)
{
	if (!c)
		return;
	explicit_bzero(c->user, sizeof(c->user));
	explicit_bzero(c->pass, sizeof(c->pass));
}

static int enuxdm_conv(int num_msg, const struct pam_message **msg,
		       struct pam_response **resp, void *appdata_ptr)
{
	(void)appdata_ptr;
	struct enuxdm_auth_cred *cred = (struct enuxdm_auth_cred *)g_active_cred;
	if (!cred || !resp)
		return PAM_CONV_ERR;

	struct pam_response *r = calloc((size_t)num_msg, sizeof *r);
	if (!r)
		return PAM_BUF_ERR;

	for (int i = 0; i < num_msg; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_ON: {
			r[i].resp = strdup(cred->user);
			if (!r[i].resp) {
				goto fail;
			}
			break;
		}
		case PAM_PROMPT_ECHO_OFF: {
			r[i].resp = strdup(cred->pass);
			if (!r[i].resp) {
				goto fail;
			}
			break;
		}
		case PAM_TEXT_INFO:
		case PAM_ERROR_MSG:
			r[i].resp = NULL;
			break;
		default:
			goto fail;
		}
	}
	*resp = r;
	return PAM_SUCCESS;

fail:
	for (int j = 0; j < num_msg; j++) {
		if (r[j].resp) {
			explicit_bzero(r[j].resp, strlen(r[j].resp));
			free(r[j].resp);
			r[j].resp = NULL;
		}
	}
	free(r);
	*resp = NULL;
	return PAM_CONV_ERR;
}

static int pam_begin(const struct enuxdm_auth_cred *cred, pam_handle_t **pamh_out)
{
	struct pam_conv conv = {.conv = enuxdm_conv, .appdata_ptr = NULL};
	g_active_cred = (struct enuxdm_auth_cred *)cred;
	int rc = pam_start(ENUXDM_PAM_SERVICE, cred->user, &conv, pamh_out);
	g_active_cred = NULL;
	if (rc != PAM_SUCCESS)
		return rc;

	const char *tty = ttyname(STDIN_FILENO);
	if (!tty || tty[0] == '\0')
		tty = "/dev/tty1";
	(void)pam_set_item(*pamh_out, PAM_TTY, tty);
	(void)pam_set_item(*pamh_out, PAM_RUSER, cred->user);
	(void)pam_set_item(*pamh_out, PAM_USER, cred->user);

	const char *xd = getenv("DISPLAY");
	if (!xd || !xd[0])
		xd = ":0";
	(void)pam_set_item(*pamh_out, PAM_XDISPLAY, xd);

	return PAM_SUCCESS;
}

int enuxdm_pam_verify_only(const struct enuxdm_auth_cred *cred)
{
	if (!cred || cred->user[0] == '\0')
		return PAM_AUTH_ERR;

	pam_handle_t *pamh = NULL;
	int rc = pam_begin(cred, &pamh);
	if (rc != PAM_SUCCESS)
		return rc;

	g_active_cred = (struct enuxdm_auth_cred *)cred;
	rc = pam_authenticate(pamh, 0);
	g_active_cred = NULL;
	if (rc != PAM_SUCCESS) {
		pam_end(pamh, rc);
		return rc;
	}

	g_active_cred = (struct enuxdm_auth_cred *)cred;
	rc = pam_acct_mgmt(pamh, 0);
	g_active_cred = NULL;
	pam_end(pamh, rc);
	return rc;
}

int enuxdm_pam_session_start(const struct enuxdm_auth_cred *cred, pam_handle_t **pamh_out)
{
	if (!cred || !pamh_out || cred->user[0] == '\0')
		return PAM_PERM_DENIED;

	pam_handle_t *pamh = NULL;
	int rc = pam_begin(cred, &pamh);
	if (rc != PAM_SUCCESS)
		return rc;

	g_active_cred = (struct enuxdm_auth_cred *)cred;
	rc = pam_authenticate(pamh, 0);
	g_active_cred = NULL;
	if (rc != PAM_SUCCESS)
		goto err;

	g_active_cred = (struct enuxdm_auth_cred *)cred;
	rc = pam_acct_mgmt(pamh, 0);
	g_active_cred = NULL;
	if (rc != PAM_SUCCESS)
		goto err;

	g_active_cred = (struct enuxdm_auth_cred *)cred;
	rc = pam_setcred(pamh, PAM_ESTABLISH_CRED);
	g_active_cred = NULL;
	if (rc != PAM_SUCCESS)
		goto err;

	g_active_cred = (struct enuxdm_auth_cred *)cred;
	rc = pam_open_session(pamh, 0);
	g_active_cred = NULL;
	if (rc != PAM_SUCCESS) {
		pam_setcred(pamh, PAM_DELETE_CRED);
		goto err;
	}

	*pamh_out = pamh;
	return PAM_SUCCESS;

err:
	pam_end(pamh, rc);
	*pamh_out = NULL;
	return rc;
}

void enuxdm_pam_session_end(pam_handle_t *pamh)
{
	if (!pamh)
		return;
	pam_close_session(pamh, 0);
	pam_setcred(pamh, PAM_DELETE_CRED);
	pam_end(pamh, PAM_SUCCESS);
}

int enuxdm_pam_launch_user_session(pam_handle_t *pamh, const char *username,
				   const char *home, const char *shell,
				   const char *display)
{
	if (!pamh || !username)
		return -1;

	struct passwd *pw = getpwnam(username);
	if (!pw)
		return -1;

	const char *h = (home && home[0]) ? home : pw->pw_dir;
	const char *sh = (shell && shell[0]) ? shell : pw->pw_shell;
	if (!sh || !sh[0])
		sh = "/bin/sh";

	pid_t pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		if (setsid() < 0)
			_exit(111);

		if (chdir(h) != 0)
			_exit(112);

		char xdg_rt[256];
		if (setup_xdg_runtime_dir(pw->pw_uid, pw->pw_gid, xdg_rt, sizeof xdg_rt) != 0)
			_exit(117);

		if (initgroups(username, pw->pw_gid) != 0 && pw->pw_uid != 0)
			_exit(113);

		if (setgid(pw->pw_gid) != 0)
			_exit(114);

		if (setuid(pw->pw_uid) != 0)
			_exit(115);

		int nenv = capture_locale_env();
		clearenv();
		replay_captured_env(nenv);

		setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
		setenv("HOME", h, 1);
		setenv("USER", username, 1);
		setenv("LOGNAME", username, 1);
		setenv("SHELL", sh, 1);
		if (display && display[0])
			setenv("DISPLAY", display, 1);
		setenv("XDG_RUNTIME_DIR", xdg_rt, 1);
		setenv("XDG_SESSION_CLASS", "user", 1);
		setenv("XDG_SESSION_TYPE", "x11", 1);
		setenv("XDG_CURRENT_DESKTOP", "XFCE", 1);
		/* Helps systemd-logind / polkit (restart, suspend, …). Preserved from DM if present. */
		if (!getenv("XDG_VTNR"))
			setenv("XDG_VTNR", "1", 1);
		if (!getenv("XDG_SEAT"))
			setenv("XDG_SEAT", "seat0", 1);
		setenv("DESKTOP_SESSION", "xfce", 1);

		char xauth[1024];
		snprintf(xauth, sizeof xauth, "%s/.Xauthority", h);
		setenv("XAUTHORITY", xauth, 1);

		const char *xfce = find_startxfce4();
		const char *drs = find_dbus_run_session();
		if (drs) {
			char *argv_d[] = {(char *)drs, "--", (char *)xfce, NULL};
			execve(drs, argv_d, environ);
		}

		char *argv[] = {(char *)xfce, NULL};
		execve(xfce, argv, environ);
		_exit(116);
	}

	int st = 0;
	if (waitpid(pid, &st, 0) < 0)
		return -1;

	enuxdm_pam_session_end(pamh);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
