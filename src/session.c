#include "auth.h"
#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int enuxdm_session_write_cred_file(const char *path, const char *user, const char *pass)
{
	if (!path || !user || !pass)
		return -1;

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;

	size_t ulen = strlen(user) + 1;
	size_t plen = strlen(pass) + 1;
	ssize_t w1 = write(fd, user, ulen);
	ssize_t w2 = write(fd, pass, plen);
	close(fd);

	if (w1 != (ssize_t)ulen || w2 != (ssize_t)plen) {
		unlink(path);
		return -1;
	}
	chmod(path, 0600);
	return 0;
}

int enuxdm_session_read_cred_file(const char *path, char *user, size_t user_sz, char *pass,
				  size_t pass_sz)
{
	if (!path || !user || !pass)
		return -1;

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	char buf[ENUXDM_MAX_USER + ENUXDM_MAX_PASS + 4];
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	unlink(path);

	if (n <= 0)
		return -1;
	buf[n] = '\0';

	const char *u = buf;
	size_t ulen = strnlen(u, (size_t)n);
	if (ulen + 2 > (size_t)n || ulen >= user_sz)
		return -1;

	const char *p = u + ulen + 1;
	size_t rest = (size_t)n - ulen - 1;
	size_t plen = strnlen(p, rest);
	if (plen + 1 > rest || plen >= pass_sz)
		return -1;

	memcpy(user, u, ulen + 1);
	memcpy(pass, p, plen + 1);
	explicit_bzero(buf, sizeof buf);
	return 0;
}

void enuxdm_xauth_merge_for_user(const char *display, const char *username)
{
	if (!display || !username || display[0] == '\0')
		return;

	struct passwd *pw = getpwnam(username);
	if (!pw)
		return;

	char cmd[512];
	snprintf(cmd, sizeof cmd,
		 "xauth nlist %s 2>/dev/null | runuser -u %s -- env -i HOME=%s sh -c 'xauth nmerge -' 2>/dev/null",
		 display, username, pw->pw_dir);
	(void)system(cmd);
}
