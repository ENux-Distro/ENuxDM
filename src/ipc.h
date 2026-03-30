#ifndef ENUXDM_IPC_H
#define ENUXDM_IPC_H

#include <stddef.h>
#include <stdint.h>

#define ENUXDM_SOCKET_DIR "/run/enuxdm"
#define ENUXDM_AUTH_SOCK ENUXDM_SOCKET_DIR "/auth.sock"

#define ENUXDM_JSON_MAX 16384

enum enuxdm_ipc_err {
	ENUXDM_IPC_OK = 0,
	ENUXDM_IPC_BAD = 1,
	ENUXDM_IPC_DENIED = 2,
	ENUXDM_IPC_COOLDOWN = 3,
};

int enuxdm_ipc_parse_login(const char *json, size_t len, char *user, size_t user_sz,
			   char *pass, size_t pass_sz);

int enuxdm_ipc_build_response(int ok, const char *error, char *out, size_t out_sz);

int enuxdm_ipc_build_response_cooldown(char *out, size_t out_sz);

#endif
