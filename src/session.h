#ifndef ENUXDM_SESSION_H
#define ENUXDM_SESSION_H

#include "ipc.h"
#define ENUXDM_SESSION_FILE ENUXDM_SOCKET_DIR "/session.cred"

int enuxdm_session_write_cred_file(const char *path, const char *user, const char *pass);

int enuxdm_session_read_cred_file(const char *path, char *user, size_t user_sz, char *pass,
				  size_t pass_sz);

void enuxdm_xauth_merge_for_user(const char *display, const char *username);

#endif
