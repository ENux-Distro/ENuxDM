#include "auth.h"
#include "session.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	const char *sf = getenv("ENUXDM_SESSION_FILE");
	if (!sf || !sf[0])
		sf = ENUXDM_SESSION_FILE;

	char user[ENUXDM_MAX_USER];
	char pass[ENUXDM_MAX_PASS];
	if (enuxdm_session_read_cred_file(sf, user, sizeof user, pass, sizeof pass) != 0)
		return 10;

	struct enuxdm_auth_cred cred;
	memset(&cred, 0, sizeof cred);
	strncpy(cred.user, user, sizeof cred.user - 1);
	strncpy(cred.pass, pass, sizeof cred.pass - 1);
	explicit_bzero(user, sizeof user);
	explicit_bzero(pass, sizeof pass);

	char *uname = strdup(cred.user);
	if (!uname) {
		enuxdm_auth_cred_clear(&cred);
		return 12;
	}

	const char *display = getenv("DISPLAY");
	if (!display || !display[0])
		display = ":0";

	enuxdm_xauth_merge_for_user(display, uname);

	pam_handle_t *pamh = NULL;
	int prc = enuxdm_pam_session_start(&cred, &pamh);
	enuxdm_auth_cred_clear(&cred);
	if (prc != 0 || !pamh) {
		free(uname);
		return 11;
	}

	struct passwd *pw = getpwnam(uname);
	const char *home = pw ? pw->pw_dir : NULL;
	const char *shell = pw ? pw->pw_shell : NULL;

	int x = enuxdm_pam_launch_user_session(pamh, uname, home, shell, display);
	free(uname);
	return x == 0 ? 0 : 13;
}
