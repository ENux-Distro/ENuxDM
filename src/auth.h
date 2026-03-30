#ifndef ENUXDM_AUTH_H
#define ENUXDM_AUTH_H

#include <security/pam_appl.h>

#define ENUXDM_PAM_SERVICE "enuxdm"
#define ENUXDM_MAX_USER 256
#define ENUXDM_MAX_PASS 4096

struct enuxdm_auth_cred {
	char user[ENUXDM_MAX_USER];
	char pass[ENUXDM_MAX_PASS];
};

void enuxdm_auth_cred_clear(struct enuxdm_auth_cred *c);

int enuxdm_pam_verify_only(const struct enuxdm_auth_cred *cred);

int enuxdm_pam_session_start(const struct enuxdm_auth_cred *cred, pam_handle_t **pamh_out);
void enuxdm_pam_session_end(pam_handle_t *pamh);

int enuxdm_pam_launch_user_session(pam_handle_t *pamh, const char *username,
				   const char *home, const char *shell,
				   const char *display);

#endif
