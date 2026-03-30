#include "auth.h"
#include "ipc.h"

#include <stdio.h>
#include <string.h>

static int extract_json_string_value(const char *json, const char *key, char *out, size_t out_sz)
{
	char pat[72];
	snprintf(pat, sizeof pat, "\"%s\"", key);
	const char *start = strstr(json, pat);
	if (!start)
		return -1;
	start += strlen(pat);
	while (*start == ' ' || *start == '\t')
		start++;
	if (*start != ':')
		return -1;
	start++;
	while (*start == ' ' || *start == '\t')
		start++;
	if (*start != '"')
		return -1;
	start++;
	size_t i = 0;
	while (*start && i + 1 < out_sz) {
		if (*start == '"')
			break;
		if (*start == '\\' && start[1]) {
			start++;
			switch (*start) {
			case 'n':
				out[i++] = '\n';
				break;
			case 'r':
				out[i++] = '\r';
				break;
			case 't':
				out[i++] = '\t';
				break;
			case '\\':
				out[i++] = '\\';
				break;
			case '"':
				out[i++] = '"';
				break;
			default:
				out[i++] = *start;
				break;
			}
			start++;
			continue;
		}
		out[i++] = *start++;
	}
	out[i] = '\0';
	return *start == '"' ? 0 : -1;
}

int enuxdm_ipc_parse_login(const char *json, size_t len, char *user, size_t user_sz, char *pass,
			   size_t pass_sz)
{
	char buf[ENUXDM_JSON_MAX];
	if (len >= sizeof buf)
		len = sizeof buf - 1;
	memcpy(buf, json, len);
	buf[len] = '\0';

	char u[ENUXDM_MAX_USER];
	char p[ENUXDM_MAX_PASS];
	if (extract_json_string_value(buf, "user", u, sizeof u) != 0)
		return -1;
	if (extract_json_string_value(buf, "password", p, sizeof p) != 0)
		return -1;

	if (strlen(u) >= user_sz || strlen(p) >= pass_sz)
		return -1;

	strncpy(user, u, user_sz - 1);
	user[user_sz - 1] = '\0';
	strncpy(pass, p, pass_sz - 1);
	pass[pass_sz - 1] = '\0';
	return 0;
}

int enuxdm_ipc_build_response(int ok, const char *error, char *out, size_t out_sz)
{
	(void)error;
	if (ok)
		return (int)snprintf(out, out_sz, "{\"ok\":true}\n");
	return (int)snprintf(out, out_sz, "{\"ok\":false,\"error\":\"Authentication failed.\"}\n");
}

int enuxdm_ipc_build_response_cooldown(char *out, size_t out_sz)
{
	return (int)snprintf(out, out_sz,
			     "{\"ok\":false,\"error\":\"Too many attempts. Wait before retrying.\",\"code\":\"cooldown\"}\n");
}
