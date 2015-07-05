/*
 * FST Manager: hostapd/wpa_supplicant control interface wrapper: CLI based
 * implementation
 *
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define FST_MGR_COMPONENT "CTRL"
#include "fst_manager.h"

#include "utils/common.h"
#include "common/defs.h"
#include "utils/eloop.h"

#include "common/wpa_ctrl.h"
#include "fst/fst_ctrl_defs.h"
#include "fst_ctrl.h"

static struct wpa_ctrl *ctrl_evt;
static struct wpa_ctrl *ctrl_cmd;
static unsigned int     ctrl_ping_interval;

static fst_notification_cb_func global_ntfy_cb = NULL;
static void *global_ntfy_cb_ctx = NULL;

#define defstrcmp(s, d) strncmp((s), (d), sizeof(d) - 1)

static char *get_pval(char *str, const char *pdesc)
{
	char *p = strchr(str, '=');
	if (p) {
		*p++ = '\0';
		/* strip leading whitespaces */
		while (*p == ' ' || *p == '\t')
			++p;
	}
	else
		fst_mgr_printf(MSG_ERROR,
			"bad %s parameter string reported \'%s\'",
			pdesc, str);
	return p;
}

/* parsers */
static int parse_list(char *p, char *res[], size_t size)
{
	int ret = 0;

	for (;;) {
		while (isspace(*p))
			p++;
		if (!*p)
			return ret;

		if ((size_t)ret == size)
			return -1;

		res[ret++] = p;
		while (*p && !isspace(*p))
			p++;
		if (isspace(*p))
			*p++ = '\0';
	}

	return ret;
}

struct parse_list_proc_ctx {
	size_t item_size;
	int (*item_parser) (char *str, void *data);
	void **output;
	void *current;
};

static int parse_list_proc(char *buf, void *data)
{
	char *list[256];
	struct parse_list_proc_ctx *c = data;
	int ret, i;

	ret = parse_list(buf, list, ARRAY_SIZE(list));
	if (!ret) {
		fst_mgr_printf(MSG_DEBUG, "str list is empty");
		return 0;
	} else if (ret < 0) {
		fst_mgr_printf(MSG_ERROR, "str list overflow");
		return -1;
	}

	if (!c->current &&
	    !(*c->output = c->current = malloc(c->item_size * ret))) {
		fst_mgr_printf(MSG_ERROR, "cannot allocate mem for %u items", ret);
		return -1;
	}

	for (i = 0; i < ret; i++) {
		c->item_parser(list[i], c->current);
		c->current = (u8*)c->current + c->item_size;
	}

	return ret;
}

/* notifications */
static int peer_event_parser(char *str, void *data)
{
	union fst_event_extra *ev = data;

	if (!defstrcmp(str, FST_CEP_PNAME_CONNECTED))
		ev->peer_state.connected = 1;
	else if (!defstrcmp(str, FST_CEP_PNAME_DISCONNECTED))
		ev->peer_state.connected = 0;
	else {
		char *p = get_pval(str, "Peer Event");
		if (!p)
			return -1;

		if (!defstrcmp(str, FST_CEP_PNAME_IFNAME))
			strncpy(ev->peer_state.ifname, p,
				sizeof(ev->peer_state.ifname));
		else if (!defstrcmp(str, FST_CEP_PNAME_ADDR)) {
			if (hwaddr_aton(p, ev->peer_state.addr))
				fst_mgr_printf(MSG_ERROR,
					"bad peer address string \'%s\'", p);
		}
	}

	return 0;
}

struct session_event_data {
	enum fst_event_type event_type;
	uint32_t session_id;
	union fst_event_extra extra;
};

static int session_event_parser(char *str, void *data)
{
	struct session_event_data *ev = data;
	char *p = get_pval(str, "Session Event");

	if (!p)
		return -1;

	if (!defstrcmp(p, FST_CTRL_PVAL_NONE))
		;
	else if (!defstrcmp(str, FST_CES_PNAME_EVT_TYPE))
		ev->event_type = fst_session_event_num(p);
	else if (!defstrcmp(str, FST_CES_PNAME_SESSION_ID))
		ev->session_id = strtoul(p, NULL, 0);
	else if (!defstrcmp(str, FST_CES_PNAME_OLD_STATE))
		ev->extra.session_state.old_state = fst_session_state_num(p);
	else if (!defstrcmp(str, FST_CES_PNAME_NEW_STATE))
		ev->extra.session_state.new_state = fst_session_state_num(p);
	else if (!defstrcmp(str, FST_CES_PNAME_REASON))
		ev->extra.session_state.extra.to_initial.reason =
					fst_reason_num(p);
	else if (!defstrcmp(str, FST_CES_PNAME_REJECT_CODE))
		ev->extra.session_state.extra.to_initial.reject_code =
			strtoul(p, NULL, 0);
	else if (!defstrcmp(str, FST_CES_PNAME_INITIATOR))
		ev->extra.session_state.extra.to_initial.initiator =
			defstrcmp(p, FST_CS_PVAL_INITIATOR_LOCAL) ?
				FST_INITIATOR_REMOTE : FST_INITIATOR_LOCAL;

	return 0;
}

static Boolean fst_ctrl_notify(char *buf, size_t len)
{
	struct parse_list_proc_ctx ctx;
	char *p = buf;

	memset(&ctx, 0, sizeof(ctx));

	fst_mgr_printf(MSG_DEBUG, "recv: %s", buf);

	if (*p == '<' && (p = strchr(p, '>')))
		p++;

	if (!strncmp(p, WPA_EVENT_TERMINATING, sizeof(WPA_EVENT_TERMINATING) - 1)) {
		fst_mgr_printf(MSG_ERROR, WPA_EVENT_TERMINATING
				"received: terminating");
		return TRUE;
	}

	if (!global_ntfy_cb)
		return FALSE;

	if (!strncmp(p, FST_CTRL_EVENT_PEER " ", sizeof(FST_CTRL_EVENT_PEER))) {
		union fst_event_extra evt_data = { };

		ctx.item_parser = peer_event_parser;
		ctx.current = &evt_data;

		parse_list_proc(p + sizeof(FST_CTRL_EVENT_PEER), &ctx);

		global_ntfy_cb(global_ntfy_cb_ctx, FST_INVALID_SESSION_ID,
			       EVENT_PEER_STATE_CHANGED, &evt_data);
	} else if (!strncmp(p, FST_CTRL_EVENT_SESSION " ",
			    sizeof(FST_CTRL_EVENT_SESSION))) {
		struct session_event_data evt_data;

		memset(&evt_data, 0, sizeof(evt_data));

		ctx.item_parser = session_event_parser;
		ctx.current = &evt_data;

		parse_list_proc(p + sizeof(FST_CTRL_EVENT_SESSION), &ctx);

		global_ntfy_cb(global_ntfy_cb_ctx, evt_data.session_id,
			       evt_data.event_type, &evt_data.extra);
	}

	return FALSE;
}

int fst_set_notify_cb(fst_notification_cb_func ntfy_cb, void *ntfy_cb_ctx)
{
	global_ntfy_cb = ntfy_cb;
	global_ntfy_cb_ctx = ntfy_cb_ctx;
	return 0;
}

/* commands */

static int do_hostap_command(const char* cmd, size_t cmd_len,
	char* resp, size_t* resp_len)
{
	int ret;

	fst_mgr_printf(MSG_DEBUG, "send: %s", cmd);

	ret = wpa_ctrl_request(ctrl_cmd, cmd, cmd_len, resp, resp_len, NULL);
	if (ret < 0) {
		fst_mgr_printf(MSG_ERROR, "command '%s' %s.", cmd,
		    ret == -2 ? "timed out" : "failed");
		return ret;
	}
	resp[*resp_len] = '\0';
	fst_mgr_printf(MSG_DEBUG, "recv (len %zu): %s", *resp_len, resp);

	return 0;
}

static int do_command_ex(int (*res_proc) (char *, void *), void *res_data,
		      const char *prefix, const char *fmt, ...)
{
	char buf[1024] = {0};
	char cmd[256];
	size_t buf_len = sizeof(buf) - 1;
	int ret = 0;
	va_list ap;

	if (prefix)
		ret = snprintf(cmd, sizeof(cmd), "%s", prefix);
	va_start(ap, fmt);
	ret += vsnprintf(cmd + ret, sizeof(cmd) - ret, fmt, ap);
	va_end(ap);

	ret = do_hostap_command(cmd, ret, buf, &buf_len);
	if (ret < 0) {
		return ret;
	}

	if (buf_len == 0)
		return 0;

	if (res_proc)
		return res_proc(buf, res_data);

	return strncmp(buf, "OK", 2) ? -1 : 0;
}

#define do_command(proc, data, fmt, ...) \
	do_command_ex(proc, data, "FST-MANAGER ", fmt, ##__VA_ARGS__)


static int session_info_parser(char *str, void *data)
{
	struct fst_session_info *si = data;
	char *p = get_pval(str, "Session Info");

	if (!p)
		return -1;

	if (!defstrcmp(p, FST_CTRL_PVAL_NONE))
		;
	else if (!defstrcmp(str, FST_CSG_PNAME_OLD_PEER_ADDR)) {
		if (hwaddr_aton(p, si->old_peer_addr))
			fst_mgr_printf(MSG_ERROR,
				"bad own address string \'%s\'", p);
	} else if (!defstrcmp(str, FST_CSG_PNAME_NEW_PEER_ADDR)) {
		if (hwaddr_aton(p, si->new_peer_addr))
			fst_mgr_printf(MSG_ERROR,
				"bad peer address string \'%s\'", p);
	} else if (!defstrcmp(str, FST_CSG_PNAME_NEW_IFNAME)) {
		strncpy(si->new_ifname, p, sizeof(si->new_ifname));
	} else if (!defstrcmp(str, FST_CSG_PNAME_OLD_IFNAME)) {
		strncpy(si->old_ifname, p, sizeof(si->old_ifname));
	} else if (!defstrcmp(str, FST_CSG_PNAME_LLT)) {
		si->llt = strtoul(p, NULL, 0);
	} else if (!defstrcmp(str, FST_CSG_PNAME_STATE)) {
		si->state = fst_session_state_num(p);
	} else {
		fst_mgr_printf(MSG_ERROR, "unknown session parameter \'%s\'", str);
	}

	return 0;
}

int fst_session_get_info(u32 session_id, struct fst_session_info *si)
{
	struct parse_list_proc_ctx ctx;
	int ret;

	ctx.item_size = 0;
	ctx.item_parser = session_info_parser;
	ctx.current = si;

	si->session_id = session_id;

	ret = do_command(parse_list_proc, &ctx, FST_CMD_SESSION_GET " %u",
			 session_id);

	return ret >= 0 ? 0 : -1;
}

int fst_session_respond(u32 session_id, const char *response_status)
{
	return do_command(NULL, NULL, FST_CMD_SESSION_RESPOND " %u %s",
			  session_id, response_status);
}

int fst_session_set(u32 session_id, const char *pname, const char *pval)
{
	return do_command(NULL, NULL, FST_CMD_SESSION_SET " %u %s=%s",
			  session_id, pname, pval);
}

static int session_id_parser(char *str, void *data)
{
	uint32_t *session_id = data;
	char *end;

	*session_id = strtoul(str, &end, 0);

	return *session_id == (uint32_t) - 1 ||
	    (*end && !isspace(*end)) ? -1 : 0;
}

int fst_session_add(const char *group_id, uint32_t * session_id)
{
	return do_command(session_id_parser, session_id,
			  FST_CMD_SESSION_ADD " %s", group_id);
}

int fst_session_remove(uint32_t session_id)
{
	return do_command(NULL, NULL, FST_CMD_SESSION_REMOVE " %u", session_id);
}

int fst_session_initiate(u32 session_id)
{
	return do_command(NULL, NULL, FST_CMD_SESSION_INITIATE " %u",
			  session_id);
}

int fst_session_transfer(u32 session_id)
{
	return do_command(NULL, NULL, FST_CMD_SESSION_TRANSFER " %u",
			  session_id);
}

int fst_session_teardown(u32 session_id)
{
	return do_command(NULL, NULL, FST_CMD_SESSION_TEARDOWN " %u",
			  session_id);
}

int fst_get_sessions(const struct fst_group_info *group, uint32_t ** sessions)
{
	struct parse_list_proc_ctx ctx;

	ctx.item_size = sizeof(*sessions[0]);
	ctx.item_parser = session_id_parser;
	ctx.output = (void *)sessions;
	ctx.current = NULL;

	return do_command(parse_list_proc, &ctx, FST_CMD_LIST_SESSIONS " %s", group->id);
}

static int iface_peer_parser(char *str, void *data)
{
	uint8_t *addr = data;

	if (hwaddr_aton(str, addr))
		fst_mgr_printf(MSG_ERROR, "bad peer address string \'%s\'", str);

	return 0;
}

int fst_get_iface_peers(const struct fst_group_info *group, struct fst_iface_info *iface, uint8_t **peers)
{
	struct parse_list_proc_ctx ctx;

	ctx.item_size = ETH_ALEN;
	ctx.item_parser = iface_peer_parser;
	ctx.output = (void *)peers;
	ctx.current = NULL;

	return do_command(parse_list_proc, &ctx, FST_CMD_IFACE_PEERS " %s %s",
			  group->id, iface->name);
}

static int parse_peer_mbies(char *buf, void *data)
{
	char **mbies = data;

	*mbies = NULL;

	if (!strncmp(buf, "FAIL", 4))
		return -EINVAL;

	*mbies = os_strdup(buf);
	if (!*mbies)
		return -ENOMEM;

	return os_strlen(*mbies);
}

int fst_get_peer_mbies(const struct fst_group_info *group,
	struct fst_iface_info *iface, uint8_t *peer, char **mbies)
{
	return do_command(parse_peer_mbies, mbies, FST_CMD_GET_PEER_MBIES " %s "
			  MACSTR, iface->name, MAC2STR(peer));
}

static int iface_parser(char *str, void *data)
{
	struct fst_iface_info *ii = data;
	char *p, *e;
	const char delims[] = "|";

	p = strtok(str, delims);
	if (!p) {
		fst_mgr_printf(MSG_ERROR, "bad iface name reported \'%s\'", str);
		return -1;
	}
	strncpy(ii->name, p, sizeof(ii->name));

	p = strtok(NULL, delims);
	if (!p) {
		fst_mgr_printf(MSG_ERROR, "bad iface address reported \'%s\'", str);
		return -1;
	}
	if (hwaddr_aton(p, ii->addr)) {
		fst_mgr_printf(MSG_ERROR, "bad addr %s: invalid addr string",
			   p);
		return -1;
	}

	p = strtok(NULL, delims);
	if (!p) {
		fst_mgr_printf(MSG_ERROR, "bad iface priority reported \'%s\'", str);
		return -1;
	}
	ii->priority = strtoul(p, &e, 0);
	if (*e != '\0') {
		fst_mgr_printf(MSG_ERROR, "bad iface string reported \'%s\'", p);
		return -1;
	}

	p = strtok(NULL, delims);
	if (!p) {
		fst_mgr_printf(MSG_ERROR, "bad iface llt reported \'%s\'", str);
		return -1;
	}
	ii->llt = strtoul(p, &e, 0);
	if (*e != '\0') {
		fst_mgr_printf(MSG_ERROR, "bad iface llt reported \'%s\'", p);
		return -1;
	}

	return 0;
}

int fst_get_group_ifaces(const struct fst_group_info *group,
			 struct fst_iface_info **ifaces)
{
	struct parse_list_proc_ctx ctx;

	ctx.item_size = sizeof(*ifaces[0]);
	ctx.item_parser = iface_parser;
	ctx.output = (void *)ifaces;
	ctx.current = NULL;

	return do_command(parse_list_proc, &ctx, FST_CMD_LIST_IFACES " %s",
			  group->id);
}

int fst_detach_iface(const struct fst_iface_info *iface)
{
	return do_command_ex(NULL, NULL, "FST-DETACH", " %s", iface->name);
}

int fst_attach_iface(const struct fst_group_info *group,
	const struct fst_iface_info *iface)
{
	return do_command_ex(NULL, NULL, "FST-ATTACH",
		" %s %s llt=%d priority=%d", iface->name, group->id,
		iface->llt, iface->priority);
}

static int group_id_parser(char *str, void *data)
{
	strncpy(((struct fst_group_info *)data)->id, str,
		sizeof(((struct fst_group_info *) data)->id));
	return 0;
}

int fst_get_groups(struct fst_group_info **groups)
{
	struct parse_list_proc_ctx ctx;

	ctx.item_size = sizeof(*groups[0]);
	ctx.item_parser = group_id_parser;
	ctx.output = (void *)groups;
	ctx.current = NULL;

	return do_command(parse_list_proc, &ctx, FST_CMD_LIST_GROUPS);
}

Boolean fst_is_supplicant(void)
{
	char buf[4096];
	size_t buf_len = sizeof(buf) - 1;
	char *cmd = "INTERFACE_LIST";
	int ret = 0;

	ret = do_hostap_command(cmd, os_strlen(cmd), buf, &buf_len);
	if (ret < 0) {
		fst_mgr_printf(MSG_ERROR, "Cannot execute command %s", cmd);
		return FALSE;
	}
	if ((buf_len < 3) || strncmp(buf, "BAD", 3))
		return TRUE;
	else
		return FALSE; 
}

int fst_add_iface(const struct fst_iface_info *iface)
{
	int res;
	if (fst_is_supplicant()) {
		res = do_command_ex(NULL, NULL, "INTERFACE_ADD",
			" %s", iface->name);
	}
	else {
		/* TODO: add interface in hostpad */
		fst_mgr_printf(MSG_ERROR, "hostapd add iface not yet supported");
		res = -1;
	}
	return res;
}

int fst_del_iface(const struct fst_iface_info *iface)
{
	int res;
	if (fst_is_supplicant()) {
		res = do_command_ex(NULL, NULL, "INTERFACE_REMOVE", " %s", iface->name);
	}
	else {
		fst_mgr_printf(MSG_ERROR, "hostapd remove iface not yet supported");
		res = -1;
	}
	return res;
}

int fst_dup_connection(const struct fst_iface_info *iface,
	const char *master, const u8 *addr)
{
	char buf[4096];
	char cmd[256];
	size_t buf_len = sizeof(buf) - 1;
	int ret, netid=-1;

	if ( fst_is_supplicant()) {
		ret = snprintf(cmd, sizeof(cmd), "IFNAME=%s ADD_NETWORK", iface->name);
		ret = do_hostap_command(cmd, ret, buf, &buf_len);
		if (ret < 0) {
			return ret;
		}

		if (!strncmp(buf, "FAIL", 4)) {
			fst_mgr_printf(MSG_ERROR, "Failed add_network for %s: %s",
				iface->name, buf);
			return -1;
		}
		netid = atoi(buf);
		fst_mgr_printf(MSG_DEBUG, "Added network %d for %s", netid, iface->name);

		buf_len = sizeof(buf) - 1;
		ret = snprintf(cmd, sizeof(cmd), "IFNAME=%s SET_NETWORK %d bssid " MACSTR,
			       iface->name, netid, MAC2STR(addr));
		ret = do_hostap_command(cmd, ret, buf, &buf_len);
		if (ret < 0) {
			goto error_set;
		}
		if (strncmp(buf, "OK", 2)) {
			fst_mgr_printf(MSG_ERROR, "Set bssid for %d failed",
				netid);
			goto error_set;
		}

		buf_len = sizeof(buf) - 1;
		ret = snprintf(cmd, sizeof(cmd), "IFNAME=%s SET_NETWORK %d key_mgmt NONE",
		       iface->name, netid);

		ret = do_hostap_command(cmd, ret, buf, &buf_len);
		if (ret < 0)
			goto error_set;

		if (strncmp(buf, "OK", 2)) {
			fst_mgr_printf(MSG_ERROR, "Set key_mgmt for %d failed",
				netid);
			goto error_set;
		}

		buf_len = sizeof(buf) - 1;
		ret = snprintf(cmd, sizeof(cmd), "IFNAME=%s SELECT_NETWORK %d",
		       iface->name, netid);
		ret = do_hostap_command(cmd, ret, buf, &buf_len);
		if (ret < 0)
			goto error_set;

		if (strncmp(buf, "OK", 2)) {
			fst_mgr_printf(MSG_ERROR, "Select network for %d failed",
				netid);
			goto error_set;
		}
		ret = 0;
	}
	else {
		/* TODO: add c in hostpad */
		fst_mgr_printf(MSG_ERROR, "hostapd add iface not yet supported");
		ret = -1;
	}
	return ret;
error_set:
	if (netid  != -1) {
		fst_disconnect_iface(iface);

	}
	return ret;
}

int fst_disconnect_iface(const struct fst_iface_info *iface)
{
	if ( fst_is_supplicant()) {
		return(do_command_ex(NULL, NULL,
		   "IFNAME=", "%s REMOVE_NETWORK ALL", iface->name));
	}
	else {
		fst_mgr_printf(MSG_ERROR, "hostapd disconnect not yet supported");
		return -1;
	}
}

void fst_free(void *p)
{
	free(p);
}

/* wpa ctrl */
static void fst_ctrl_receiver(int sock, void *eloop_ctx, void *sock_ctx)
{
	char buf[4096];
	struct wpa_ctrl *ctrl = sock_ctx;
	size_t len;

	while (wpa_ctrl_pending(ctrl) > 0) {
		len = sizeof(buf) - 1;
		if (wpa_ctrl_recv(ctrl, buf, &len)) {
			fst_mgr_printf(MSG_ERROR, "wpa_ctrl_recv");
			break;
		}
		buf[len] = '\0';
		if (fst_ctrl_notify(buf, len)) {
			fst_mgr_printf(MSG_ERROR, "fst_ctrl_notify");
			goto cli_disconnected;
		}
	}

	if (wpa_ctrl_pending(ctrl) < 0) {
		fst_mgr_printf(MSG_ERROR, "wpa_ctrl_pending: connection lost: "
			"terminating");
		goto cli_disconnected;
	}

	return;

cli_disconnected:
	eloop_terminate();
}

static struct wpa_ctrl *try_to_open_wpa_ctrl(const char *name)
{
	WPA_ASSERT(name != NULL);

	if (*name) {
#ifndef ANDROID
		struct stat stat_buf;
		if (stat(name, &stat_buf) < 0) {
			fst_mgr_printf(MSG_ERROR, "cannot stat \'%s\'", name);
			return NULL;
		}
#endif

		return wpa_ctrl_open(name);
	}

	return NULL;
}

static void fst_ping(void *eloop_ctx, void *timeout_ctx)
{
	char buf[4096];
	size_t buf_len = sizeof(buf) - 1;
	int ret;

	fst_mgr_printf(MSG_EXCESSIVE, "send: PING");

	ret = wpa_ctrl_request(ctrl_cmd, "PING", 4, buf, &buf_len, NULL);
	if (ret < 0) {
		fst_mgr_printf(MSG_ERROR, "PING %s",
		    ret == -2 ? "timed out" : "failed");
		goto error_ping;
	}
	buf[buf_len] = '\0';

	if (os_strncmp(buf, "PONG", 4)) {
		fst_mgr_printf(MSG_ERROR, "Wrong PING response: %s", buf);
		goto error_pong;
	}

	eloop_register_timeout(ctrl_ping_interval, 0, fst_ping, NULL, NULL);

	return;

error_ping:
error_pong:
	eloop_terminate();
}

Boolean fst_ctrl_create(const char *ctrl_iface, unsigned int ping_interval)
{
	WPA_ASSERT(ctrl_iface != NULL);
	WPA_ASSERT(ctrl_evt == NULL);
	WPA_ASSERT(ctrl_cmd == NULL);

	ctrl_evt = try_to_open_wpa_ctrl(ctrl_iface);
	if (!ctrl_evt) {
		fst_mgr_printf(MSG_ERROR, "cannot open control iface (evt): %s",
			ctrl_iface);
		goto error_open_evt;
	}

	if (wpa_ctrl_attach(ctrl_evt)) {
		fst_mgr_printf(MSG_ERROR, "cannot attach control iface: %s",
			ctrl_iface);
		goto error_attach_evt;
	}

	ctrl_cmd = try_to_open_wpa_ctrl(ctrl_iface);
	if (!ctrl_cmd) {
		fst_mgr_printf(MSG_ERROR, "cannot open control iface (cmd): %s",
			ctrl_iface);
		goto error_open_cmd;
	}

	if (eloop_register_read_sock(wpa_ctrl_get_fd(ctrl_evt),
		fst_ctrl_receiver, NULL, ctrl_evt)) {
		fst_mgr_printf(MSG_ERROR, "eloop_register_read_sock");
		goto error_eloop_register_read_sock;
	}

	ctrl_ping_interval = ping_interval;
	if (ctrl_ping_interval)
		eloop_register_timeout(ctrl_ping_interval, 0, fst_ping, NULL, NULL);

	fst_mgr_printf(MSG_INFO, "connected to control iface: %s",
			ctrl_iface);

	return TRUE;

error_eloop_register_read_sock:
	wpa_ctrl_close(ctrl_cmd);
	ctrl_cmd = NULL;
error_open_cmd:
	wpa_ctrl_detach(ctrl_evt);
error_attach_evt:
	wpa_ctrl_close(ctrl_evt);
	ctrl_evt = NULL;
error_open_evt:
	return FALSE;
}

void fst_ctrl_free(void)
{
	WPA_ASSERT(ctrl_evt != NULL);
	WPA_ASSERT(ctrl_cmd != NULL);
	eloop_unregister_read_sock(wpa_ctrl_get_fd(ctrl_evt));
	eloop_cancel_timeout(fst_ping, NULL, NULL);
	wpa_ctrl_close(ctrl_cmd);
	ctrl_cmd = NULL;
	wpa_ctrl_detach(ctrl_evt);
	wpa_ctrl_close(ctrl_evt);
	ctrl_evt = NULL;
}
