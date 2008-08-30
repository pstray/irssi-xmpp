/*
 * $Id$
 *
 * Copyright (C) 2007 Colin DIDIER
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * XEP-0045: Multi-User Chat
 */

#include <stdlib.h>
#include <string.h>

#include "module.h"
#include "commands.h"
#include "settings.h"
#include "signals.h"

#include "rosters-tools.h"
#include "tools.h"
#include "disco.h"
#include "muc.h"
#include "muc-nicklist.h"

void send_join(MUC_REC *);

static void
topic(MUC_REC *channel, const char *topic, const char *nickname)
{
	if (channel->topic != NULL && topic != NULL
	    && strcmp(channel->topic, topic) == 0)
		return;
	g_free(channel->topic);
	channel->topic = (topic != NULL && *topic != '\0') ?
	    g_strdup(topic) : NULL;
	g_free(channel->topic_by);
	channel->topic_by = g_strdup(nickname);
	signal_emit("channel topic changed", 1, channel);
	if (channel->joined && nickname != NULL && *nickname != '\0')
		signal_emit("message topic", 5, channel->server, channel->name,
		    (channel->topic != NULL) ? channel->topic : "",
		    channel->topic_by, "");
	else {
		char *data = g_strconcat(" ", channel->name, " :",
		    (channel->topic != NULL) ? channel->topic : "", NULL);
		signal_emit("event 332", 2, channel->server, data);
		g_free(data);
	}
}

static void
nick_changed(MUC_REC *channel, const char *oldnick, const char *newnick)
{
	XMPP_NICK_REC *nick;

	if ((nick = xmpp_nicklist_find(channel, oldnick)) == NULL)
		return;
	xmpp_nicklist_rename(channel, nick, oldnick, newnick);
	if (channel->ownnick == NICK(nick))
		signal_emit("message xmpp channel own_nick", 3,
		    channel, nick, oldnick);
	else
		signal_emit("message xmpp channel nick", 3,
		    channel, nick, oldnick);
}

static void
own_join(MUC_REC *channel, const char *nickname,
    const char *full_jid, const char *affiliation, const char *role,
    gboolean forced)
{
	XMPP_NICK_REC *nick;

	if (channel->joined)
		return;
	if ((nick = xmpp_nicklist_find(channel, nickname)) != NULL)
		return;
	nick = xmpp_nicklist_insert(channel, nickname, full_jid);
	nicklist_set_own(CHANNEL(channel), NICK(nick));
	channel->chanop = channel->ownnick->op;
	xmpp_nicklist_set_modes(nick,
	    xmpp_nicklist_get_affiliation(affiliation),
	    xmpp_nicklist_get_role(role));
	channel->names_got = TRUE;
	channel->joined = TRUE;
	signal_emit("message join", 4, channel->server, channel->name,
	    nick->nick, nick->host);
	signal_emit("message xmpp channel mode", 4, channel,
	    nick->nick, nick->affiliation, nick->role);
	signal_emit("channel joined", 1, channel);
	signal_emit("channel sync", 1, channel);
	channel_send_autocommands(CHANNEL(channel));
	if (forced)
		nick_changed(channel, channel->nick, nick->nick);
	if (*channel->mode == '\0')
		disco_request(channel->server, channel->name);
}

static void
nick_join(MUC_REC *channel, const char *nickname, const char *full_jid,
    const char *affiliation, const char *role)
{
	XMPP_NICK_REC *nick;

	nick = xmpp_nicklist_insert(channel, nickname, full_jid);
	xmpp_nicklist_set_modes(nick,
	    xmpp_nicklist_get_affiliation(affiliation),
	    xmpp_nicklist_get_role(role));
	if (channel->names_got) {
		signal_emit("message join", 4, channel->server, channel->name,
		    nick->nick, nick->host);
		signal_emit("message xmpp channel mode", 4, channel,
		    nick->nick, nick->affiliation, nick->role);
	}
}

static void
nick_mode(MUC_REC *channel, XMPP_NICK_REC *nick, const char *affiliation_str,
    const char *role_str)
{
	int affiliation, role;

	affiliation = xmpp_nicklist_get_affiliation(affiliation_str);
	role = xmpp_nicklist_get_role(role_str);
	if (xmpp_nicklist_modes_changed(nick, affiliation, role)) {
		xmpp_nicklist_set_modes(nick, affiliation, role);
		signal_emit("message xmpp channel mode", 4, channel,
		    nick->nick, affiliation, role);
	}
}

static void
own_event(MUC_REC *channel, const char *nickname, const char *full_jid, 
    const char *affiliation, const char *role, gboolean forced)
{
	XMPP_NICK_REC *nick;

	if ((nick = xmpp_nicklist_find(channel, nickname)) == NULL)
		own_join(channel, nickname, full_jid, affiliation, role,
		    forced);
	else
		nick_mode(channel, nick, affiliation, role);
}

static void
nick_event(MUC_REC *channel, const char *nickname, const char *full_jid,
    const char *affiliation, const char *role)
{
	XMPP_NICK_REC *nick;

	if ((nick = xmpp_nicklist_find(channel, nickname)) == NULL)
		nick_join(channel, nickname, full_jid, affiliation, role);
	else 
		nick_mode(channel, nick, affiliation, role);
}

static void
nick_part(MUC_REC *channel, const char *nickname, const char *reason)
{
	XMPP_NICK_REC *nick;

	if ((nick = xmpp_nicklist_find(channel, nickname)) == NULL)
		return;
	signal_emit("message part", 5, channel->server, channel->name,
	    nick->nick, nick->host, reason);
	if (channel->ownnick == NICK(nick)) {
		channel->left = TRUE;
		channel_destroy(CHANNEL(channel));
	} else
		nicklist_remove(CHANNEL(channel), NICK(nick));
}

static void
nick_presence(MUC_REC *channel, const char *nickname, const char *show_str,
    const char *status)
{
	XMPP_NICK_REC *nick;
	int show;

	if ((nick = xmpp_nicklist_find(channel, nickname)) == NULL)
		return;
	show = xmpp_get_show(show_str);
	if (xmpp_presence_changed(show, nick->show, status, nick->status,
	    0, 0)) {
		xmpp_nicklist_set_presence(nick, show, status);
		if (channel->joined && channel->ownnick != NICK(nick)) {
			/* TODO show event */
		}
	}
}

static void
nick_kicked(MUC_REC *channel, const char *nickname, const char *actor,
    const char *reason)
{
	XMPP_NICK_REC *nick;

	if ((nick = xmpp_nicklist_find(channel, nickname)) == NULL)
		return;
	signal_emit("message kick", 6, channel->server, channel->name,
	    nick->nick, (actor != NULL) ? actor : channel->name, nick->host,
	    reason);
	if (channel->ownnick == NICK(nick)) {
		channel->kicked = TRUE;
		channel_destroy(CHANNEL(channel));
	} else
		nicklist_remove(CHANNEL(channel), NICK(nick));
}

static void
error_message(MUC_REC *channel, const char *code)
{
	switch (atoi(code)) {
	case 401:
		signal_emit("xmpp channel error", 2, channel, "not allowed");
		break;
	}
}

static void
error_join(MUC_REC *channel, const char *code)
{
	char *altnick;
	int error;

	error = atoi(code);
	/* TODO: emit display signal */
	/* rejoin with alternate nick */
	if (error == MUC_ERROR_USE_RESERVED_ROOM_NICK
	    || error == MUC_ERROR_NICK_IN_USE) {
		altnick = (char *)settings_get_str("alternate_nick");
		if (altnick != NULL && *altnick != '\0'
		    && strcmp(channel->nick, altnick) != 0) {
			g_free(channel->nick);
			channel->nick = g_strdup(altnick);
		} else {
			altnick = g_strdup_printf("%s_", channel->nick);
			g_free(channel->nick);
			channel->nick = altnick;
		}
		send_join(channel);
		return;
	}
	channel_destroy(CHANNEL(channel));
}

static void
error_presence(MUC_REC *channel, const char *code, const char *nick)
{
	int error;

	error = atoi(code);
	switch (atoi(code)) {
	case 409:
		signal_emit("message xmpp channel nick in use", 2, channel,
			channel, nick);
		break;
	}
}

static void
available(MUC_REC *channel, const char *from, LmMessage *lmsg)
{
	LmMessageNode *node;
	const char *item_affiliation, *item_role, *nick;
	char *item_jid, *item_nick, *status;
	gboolean own, forced, created;

	item_affiliation = item_role = status = NULL;
	item_jid = item_nick = NULL;
	own = forced = created = FALSE;
	/* <x xmlns='http://jabber.org/protocol/muc#user'> */
	node = lm_find_node(lmsg->node, "x", "xmlns", XMLNS_MUC_USER);
	node = node != NULL ? lm_message_node_get_child(node, "item") : NULL;
	if (node != NULL) {
		/* <item affiliation='item_affiliation'
		 *     role='item_role'
		 *     nick='item_nick'/> */
		item_affiliation =
		    lm_message_node_get_attribute(node, "affiliation");
		item_role =
		    lm_message_node_get_attribute(node, "role");
		item_jid = xmpp_recode_in(
		    lm_message_node_get_attribute(node, "jid"));
		item_nick = xmpp_recode_in(
		    lm_message_node_get_attribute(node, "nick"));
		/* <status code='110'/> */
		own = lm_find_node(node, "status", "code", "110") != NULL;
		/* <status code='210'/> */
		forced = lm_find_node(node, "status", "code", "210") != NULL;
		/* <status code='201'/> */
		created = lm_find_node(node, "status", "code", "201") != NULL;
	}
	nick = item_nick != NULL ? item_nick : from;
	if (created) {
		/* TODO send disco
		 * show event IRCTXT_CHANNEL_CREATED */
	}
	if (own || strcmp(nick, channel->nick) == 0)
		own_event(channel, nick, item_jid, item_affiliation, item_role,
		    forced);
	else 
		nick_event(channel, nick, item_jid, item_affiliation, item_role);
	/* <status>text</status> */
	node = lm_message_node_get_child(lmsg->node, "status");
	if (node != NULL)
		status = xmpp_recode_in(node->value);
	/* <show>show</show> */
	node = lm_message_node_get_child(lmsg->node, "show");
	nick_presence(channel, nick, node != NULL ? node->value : NULL, status);
	g_free(item_jid);
	g_free(item_nick);
	g_free(status);
}

static void
unavailable(MUC_REC *channel, const char *nick, LmMessage *lmsg)
{
	LmMessageNode *node, *child;
	const char *status_code;
	char *reason, *actor, *item_nick, *status;

	status_code = NULL;
	reason = actor = item_nick = status = NULL;
	/* <x xmlns='http://jabber.org/protocol/muc#user'> */
	node = lm_find_node(lmsg->node, "x", "xmlns", XMLNS_MUC_USER);
	if (node != NULL) {
		/* <status code='status_code'/> */
		child = lm_message_node_get_child(node, "status");
		if (child != NULL)
			status_code =
			    lm_message_node_get_attribute(child, "code");
		/* <item nick='item_nick'> */
		node = lm_message_node_get_child(node, "item");
		if (node != NULL) {
			item_nick = xmpp_recode_in(
			    lm_message_node_get_attribute(node, "nick"));
			/* <reason>reason</reason> */
			child = lm_message_node_get_child(node, "reason");
			if (child != NULL)
				reason = xmpp_recode_in(child->value);
			/* <actor jid='actor'/> */
			child = lm_message_node_get_child(node, "actor");
			if (child != NULL)
				actor = xmpp_recode_in(
				    lm_message_node_get_attribute(child, "jid"));
		}
	}
	if (status_code != NULL) {
		switch (atoi(status_code)) {
		case 303: /* <status code='303'/> */
			signal_emit("xmpp channel nick", 5, channel, nick,
			    item_nick);
			break;
		case 307: /* kick: <status code='307'/> */
			nick_kicked(channel, nick, actor, reason);
			break;
		case 301: /* ban: <status code='301'/> */
			nick_kicked(channel, nick, actor, reason);
			break;
		}
	} else {
		/* <status>text</status> */
		node = lm_message_node_get_child(lmsg->node, "status");
		if (node != NULL)
			status = xmpp_recode_in(node->value);
		nick_part(channel, nick, status);
		g_free(status);
	}
	g_free(item_nick);
	g_free(reason);
	g_free(actor);
}

static MUC_REC *
get_muc(XMPP_SERVER_REC *server, const char *data)
{
	MUC_REC *channel;
	char *str;

	str = muc_extract_channel(data);
	channel = muc_find(server, str);
	g_free(str);
	return channel;
}

static void
sig_recv_message(XMPP_SERVER_REC *server, LmMessage *lmsg, const int type,
    const char *id, const char *from, const char *to)
{
	MUC_REC *channel;
	LmMessageNode *node;
	char *nick, *str;
	gboolean action, own;

	if ((channel = get_muc(server, from)) == NULL)
		return;
	nick = muc_extract_nick(from);
	switch (type) {
	case LM_MESSAGE_SUB_TYPE_ERROR:
		node = lm_message_node_get_child(lmsg->node, "error");
		if (node == NULL)
			goto out;
		/* TODO: extract error type and name -> XMLNS_STANZAS */
		error_message(channel,
		    lm_message_node_get_attribute(node, "code"));
		break;
	case LM_MESSAGE_SUB_TYPE_NOT_SET:
		/* TODO: invite */
		break;
	case LM_MESSAGE_SUB_TYPE_GROUPCHAT:
		node = lm_message_node_get_child(lmsg->node, "subject");
		if (node != NULL) {
			str = xmpp_recode_in(node->value);
			topic(channel, str, nick);
			g_free(str);
		}
		node = lm_message_node_get_child(lmsg->node, "body");
		if (node != NULL && nick != NULL) {
			str = xmpp_recode_in(node->value);
			own = strcmp(nick, channel->nick) == 0;
			action = g_ascii_strncasecmp(str, "/me ", 4) == 0;
			if (action && own)
				signal_emit("message xmpp own_action", 4,
				    server, str+4, channel->name,
				    GINT_TO_POINTER(SEND_TARGET_CHANNEL));
			else if (action)
				signal_emit("message xmpp action", 5,
				    server, str+4, nick, channel->name,
				    GINT_TO_POINTER(SEND_TARGET_CHANNEL));
			else if (own)
				signal_emit("message xmpp own_public", 3,
				    server, str, channel->name);
			else
				signal_emit("message public", 5,
				    server, str, nick, "", channel->name);
			g_free(str);
		}
		break;
	}
out:
	g_free(nick);
}

static void
sig_recv_presence(XMPP_SERVER_REC *server, LmMessage *lmsg, const int type,
    const char *id, const char *from, const char *to)
{
	MUC_REC *channel;
	LmMessageNode *node;
	char *nick;

	if ((channel = get_muc(server, from)) == NULL)
		return;
	nick = muc_extract_nick(from);
	switch (type) {
	case LM_MESSAGE_SUB_TYPE_ERROR:
		node = lm_message_node_get_child(lmsg->node, "error");
		if (node == NULL)
			goto out;
		/* TODO: extract error type and name -> XMLNS_STANZAS */
		if (!channel->joined)
			error_join(channel,
			    lm_message_node_get_attribute(node, "code"));
		else
			error_presence(channel,
			    lm_message_node_get_attribute(node, "code"), nick);
		break;
	case LM_MESSAGE_SUB_TYPE_AVAILABLE:
		available(channel, nick, lmsg);
		break;
	case LM_MESSAGE_SUB_TYPE_UNAVAILABLE:
		unavailable(channel, nick, lmsg);
		break;
	}

out:
	g_free(nick);
}

void
muc_events_init(void)
{
	signal_add("xmpp recv message", sig_recv_message);
	signal_add("xmpp recv presence", sig_recv_presence);
}

void
muc_events_deinit(void)
{
	signal_remove("xmpp recv message", sig_recv_message);
	signal_remove("xmpp recv presence", sig_recv_presence);
}
