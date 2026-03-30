#include "auth.h"
#include "ipc.h"
#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ENUXDM_PREFIX
#define ENUXDM_PREFIX "/usr"
#endif

static volatile sig_atomic_t g_shutdown;
static int g_chld_pipe[2] = {-1, -1};
static pid_t g_xinit_pid;

struct enuxdm_config {
	char accent[32];
	char background[512];
	char logo[512];
	char font_family[128];
};

static struct enuxdm_config g_cfg;

static void load_config(void)
{
	strncpy(g_cfg.accent, "#b4b8c2", sizeof g_cfg.accent);
	g_cfg.background[0] = '\0';
	strncpy(g_cfg.logo, ENUXDM_PREFIX "/share/enuxdm/assets/enux-logo.svg", sizeof g_cfg.logo);
	strncpy(g_cfg.font_family, "Inter", sizeof g_cfg.font_family);

	FILE *f = fopen("/etc/enuxdm/enuxdm.conf", "r");
	if (!f)
		return;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;
		char *eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq++ = '\0';
		char *v = eq;
		size_t vl = strlen(v);
		if (vl && v[vl - 1] == '\n')
			v[--vl] = '\0';
		if (strcmp(p, "accent") == 0)
			strncpy(g_cfg.accent, v, sizeof g_cfg.accent - 1);
		else if (strcmp(p, "background") == 0)
			strncpy(g_cfg.background, v, sizeof g_cfg.background - 1);
		else if (strcmp(p, "logo") == 0)
			strncpy(g_cfg.logo, v, sizeof g_cfg.logo - 1);
		else if (strcmp(p, "font_family") == 0)
			strncpy(g_cfg.font_family, v, sizeof g_cfg.font_family - 1);
	}
	fclose(f);
}

static void on_signal(int sig)
{
	if (sig == SIGINT || sig == SIGTERM)
		g_shutdown = 1;
}

static void on_chld(int sig)
{
	(void)sig;
	if (g_chld_pipe[1] >= 0) {
		char b = 1;
		(void)write(g_chld_pipe[1], &b, 1);
	}
}

static int ensure_run_dir(void)
{
	if (mkdir(ENUXDM_SOCKET_DIR, 0700) != 0 && errno != EEXIST)
		return -1;
	if (chmod(ENUXDM_SOCKET_DIR, 0700) != 0)
		return -1;
	return 0;
}

static int make_auth_socket(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	unlink(ENUXDM_AUTH_SOCK);
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, ENUXDM_AUTH_SOCK, sizeof addr.sun_path - 1);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		close(fd);
		return -1;
	}
	chmod(ENUXDM_AUTH_SOCK, 0600);
	if (listen(fd, 8) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static const char *pick_x_server(void)
{
	static const char *const candidates[] = {"/usr/bin/Xorg", "/usr/bin/X", NULL};

	for (size_t i = 0; candidates[i]; i++) {
		if (access(candidates[i], X_OK) == 0)
			return candidates[i];
	}
	return "Xorg";
}

/* If Xorg died but .X0-lock remains, the next xinit fails with "Server already active for display 0". */
static int cleanup_stale_x0_lock(void)
{
	const char *path = "/tmp/.X0-lock";
	FILE *fp = fopen(path, "r");
	if (!fp)
		return 0;

	int xpid = -1;
	if (fscanf(fp, "%d", &xpid) != 1 || xpid < 1) {
		fclose(fp);
		return 0;
	}
	fclose(fp);

	if (kill((pid_t)xpid, 0) == -1 && errno == ESRCH) {
		if (unlink(path) == 0)
			return 1;
	}
	return 0;
}

static void spawn_xinit(void)
{
	if (g_xinit_pid > 0)
		return;

	if (cleanup_stale_x0_lock())
		usleep(300000);

	g_xinit_pid = fork();
	if (g_xinit_pid < 0) {
		g_xinit_pid = 0;
		return;
	}

	if (g_xinit_pid == 0) {
		setenv("ENUXDM_PREFIX", ENUXDM_PREFIX, 1);
		setenv("ENUXDM_AUTH_SOCKET", ENUXDM_AUTH_SOCK, 1);
		setenv("ENUXDM_SESSION_FILE", ENUXDM_SESSION_FILE, 1);
		setenv("ENUXDM_ACCENT", g_cfg.accent, 1);
		if (g_cfg.background[0])
			setenv("ENUXDM_BACKGROUND", g_cfg.background, 1);
		setenv("ENUXDM_LOGO", g_cfg.logo, 1);
		setenv("ENUXDM_FONT_FAMILY", g_cfg.font_family, 1);
		/* Same GTK rendering hints as xinitrc (belt and suspenders). */
		setenv("GDK_BACKEND", "x11", 1);
		setenv("GSK_RENDERER", "cairo", 1);
		setenv("XDG_VTNR", "1", 1);
		setenv("XDG_SEAT", "seat0", 1);

		const char *xinitrc = ENUXDM_PREFIX "/lib/enuxdm/xinitrc";
		const char *xsrv = pick_x_server();
		execlp("xinit", "xinit", xinitrc, "--", xsrv, ":0", "vt1", "-nolisten", "tcp",
		       (char *)NULL);
		_exit(127);
	}
}

static void terminate_xinit(void)
{
	if (g_xinit_pid > 0) {
		kill(g_xinit_pid, SIGTERM);
		(void)waitpid(g_xinit_pid, NULL, 0);
		g_xinit_pid = 0;
	}
}

static void reap_xinit(void)
{
	if (g_xinit_pid <= 0)
		return;
	int st;
	pid_t p = waitpid(g_xinit_pid, &st, WNOHANG);
	if (p == g_xinit_pid)
		g_xinit_pid = 0;
}

static int handle_client(int cfd, int *fail_streak, time_t *cooldown_until)
{
	char buf[ENUXDM_JSON_MAX];
	size_t total = 0;
	while (total < sizeof buf - 1) {
		ssize_t n = recv(cfd, buf + total, sizeof buf - 1 - total, 0);
		if (n <= 0)
			break;
		total += (size_t)n;
		buf[total] = '\0';
		if (memchr(buf, '\n', total))
			break;
	}
	if (total == 0)
		return 0;

	time_t now = time(NULL);
	if (now < *cooldown_until) {
		char out[256];
		enuxdm_ipc_build_response_cooldown(out, sizeof out);
		(void)send(cfd, out, strlen(out), MSG_NOSIGNAL);
		return 0;
	}

	char user[ENUXDM_MAX_USER];
	char pass[ENUXDM_MAX_PASS];
	if (enuxdm_ipc_parse_login(buf, total, user, sizeof user, pass, sizeof pass) != 0) {
		char out[256];
		enuxdm_ipc_build_response(0, NULL, out, sizeof out);
		(void)send(cfd, out, strlen(out), MSG_NOSIGNAL);
		return 0;
	}

	struct enuxdm_auth_cred cred;
	memset(&cred, 0, sizeof cred);
	strncpy(cred.user, user, sizeof cred.user - 1);
	strncpy(cred.pass, pass, sizeof cred.pass - 1);

	int prc = enuxdm_pam_verify_only(&cred);
	if (prc != PAM_SUCCESS) {
		syslog(LOG_NOTICE, "PAM verify failed for \"%s\": %s", user,
		       pam_strerror(NULL, prc));
		enuxdm_auth_cred_clear(&cred);
		explicit_bzero(user, sizeof user);
		explicit_bzero(pass, sizeof pass);
		(*fail_streak)++;
		if (*fail_streak >= 3)
			*cooldown_until = now + 30;
		char out[256];
		enuxdm_ipc_build_response(0, NULL, out, sizeof out);
		(void)send(cfd, out, strlen(out), MSG_NOSIGNAL);
		return 0;
	}

	if (enuxdm_session_write_cred_file(ENUXDM_SESSION_FILE, cred.user, cred.pass) != 0) {
		enuxdm_auth_cred_clear(&cred);
		explicit_bzero(user, sizeof user);
		explicit_bzero(pass, sizeof pass);
		char out[256];
		enuxdm_ipc_build_response(0, NULL, out, sizeof out);
		(void)send(cfd, out, strlen(out), MSG_NOSIGNAL);
		return 0;
	}

	enuxdm_auth_cred_clear(&cred);
	explicit_bzero(user, sizeof user);
	explicit_bzero(pass, sizeof pass);

	*fail_streak = 0;
	*cooldown_until = 0;

	char out[256];
	enuxdm_ipc_build_response(1, NULL, out, sizeof out);
	(void)send(cfd, out, strlen(out), MSG_NOSIGNAL);
	return 0;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	load_config();

	openlog("enuxdm", LOG_PID, LOG_AUTHPRIV);

	if (ensure_run_dir() != 0) {
		fprintf(stderr, "enuxdm: cannot create %s\n", ENUXDM_SOCKET_DIR);
		closelog();
		return 1;
	}

	if (pipe2(g_chld_pipe, O_CLOEXEC) != 0) {
		closelog();
		return 1;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	struct sigaction sa_ch;
	memset(&sa_ch, 0, sizeof sa_ch);
	sa_ch.sa_handler = on_chld;
	sigaction(SIGCHLD, &sa_ch, NULL);

	int listen_fd = make_auth_socket();
	if (listen_fd < 0) {
		fprintf(stderr, "enuxdm: cannot bind auth socket\n");
		closelog();
		return 1;
	}

	int fail_streak = 0;
	time_t cooldown_until = 0;

	while (!g_shutdown) {
		spawn_xinit();

		struct pollfd pf[2];
		pf[0].fd = listen_fd;
		pf[0].events = POLLIN;
		pf[1].fd = g_chld_pipe[0];
		pf[1].events = POLLIN;

		int pr = poll(pf, 2, 500);
		if (pr < 0) {
			if (errno == EINTR) {
				reap_xinit();
				continue;
			}
			break;
		}

		if (pf[1].revents & POLLIN) {
			char drain[16];
			(void)read(g_chld_pipe[0], drain, sizeof drain);
			reap_xinit();
		}

		if (g_shutdown)
			break;

		if (pf[0].revents & POLLIN) {
			int cfd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
			if (cfd >= 0) {
				handle_client(cfd, &fail_streak, &cooldown_until);
				shutdown(cfd, SHUT_RDWR);
				close(cfd);
			}
		} else {
			reap_xinit();
		}
	}

	terminate_xinit();
	close(listen_fd);
	unlink(ENUXDM_AUTH_SOCK);
	if (g_chld_pipe[0] >= 0)
		close(g_chld_pipe[0]);
	if (g_chld_pipe[1] >= 0)
		close(g_chld_pipe[1]);
	closelog();
	return 0;
}
