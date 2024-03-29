/*
 * Health-checks functions.
 *
 * Copyright 2000-2009 Willy Tarreau <w@1wt.eu>
 * Copyright 2007-2009 Krzysztof Piotr Oledzki <ole@ans.pl>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <common/chunk.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/mini-clist.h>
#include <common/standard.h>
#include <common/time.h>

#include <types/global.h>

#ifdef USE_OPENSSL
#include <types/ssl_sock.h>
#include <proto/ssl_sock.h>
#endif /* USE_OPENSSL */

#include <proto/backend.h>
#include <proto/checks.h>
#include <proto/dumpstats.h>
#include <proto/fd.h>
#include <proto/log.h>
#include <proto/queue.h>
#include <proto/port_range.h>
#include <proto/proto_http.h>
#include <proto/proto_tcp.h>
#include <proto/protocol.h>
#include <proto/proxy.h>
#include <proto/raw_sock.h>
#include <proto/server.h>
#include <proto/session.h>
#include <proto/stream_interface.h>
#include <proto/task.h>

static int httpchk_expect(struct server *s, int done);
static int tcpcheck_get_step_id(struct server *);
static void tcpcheck_main(struct connection *);

static const struct check_status check_statuses[HCHK_STATUS_SIZE] = {
	[HCHK_STATUS_UNKNOWN]	= { CHK_RES_UNKNOWN,  "UNK",     "Unknown" },
	[HCHK_STATUS_INI]	= { CHK_RES_UNKNOWN,  "INI",     "Initializing" },
	[HCHK_STATUS_START]	= { /* SPECIAL STATUS*/ },

	[HCHK_STATUS_HANA]	= { CHK_RES_FAILED,   "HANA",    "Health analyze" },

	[HCHK_STATUS_SOCKERR]	= { CHK_RES_FAILED,   "SOCKERR", "Socket error" },

	[HCHK_STATUS_L4OK]	= { CHK_RES_PASSED,   "L4OK",    "Layer4 check passed" },
	[HCHK_STATUS_L4TOUT]	= { CHK_RES_FAILED,   "L4TOUT",  "Layer4 timeout" },
	[HCHK_STATUS_L4CON]	= { CHK_RES_FAILED,   "L4CON",   "Layer4 connection problem" },

	[HCHK_STATUS_L6OK]	= { CHK_RES_PASSED,   "L6OK",    "Layer6 check passed" },
	[HCHK_STATUS_L6TOUT]	= { CHK_RES_FAILED,   "L6TOUT",  "Layer6 timeout" },
	[HCHK_STATUS_L6RSP]	= { CHK_RES_FAILED,   "L6RSP",   "Layer6 invalid response" },

	[HCHK_STATUS_L7TOUT]	= { CHK_RES_FAILED,   "L7TOUT",  "Layer7 timeout" },
	[HCHK_STATUS_L7RSP]	= { CHK_RES_FAILED,   "L7RSP",   "Layer7 invalid response" },

	[HCHK_STATUS_L57DATA]	= { /* DUMMY STATUS */ },

	[HCHK_STATUS_L7OKD]	= { CHK_RES_PASSED,   "L7OK",    "Layer7 check passed" },
	[HCHK_STATUS_L7OKCD]	= { CHK_RES_CONDPASS, "L7OKC",   "Layer7 check conditionally passed" },
	[HCHK_STATUS_L7STS]	= { CHK_RES_FAILED,   "L7STS",   "Layer7 wrong status" },
};

static const struct analyze_status analyze_statuses[HANA_STATUS_SIZE] = {		/* 0: ignore, 1: error, 2: OK */
	[HANA_STATUS_UNKNOWN]		= { "Unknown",                         { 0, 0 }},

	[HANA_STATUS_L4_OK]		= { "L4 successful connection",        { 2, 0 }},
	[HANA_STATUS_L4_ERR]		= { "L4 unsuccessful connection",      { 1, 1 }},

	[HANA_STATUS_HTTP_OK]		= { "Correct http response",           { 0, 2 }},
	[HANA_STATUS_HTTP_STS]		= { "Wrong http response",             { 0, 1 }},
	[HANA_STATUS_HTTP_HDRRSP]	= { "Invalid http response (headers)", { 0, 1 }},
	[HANA_STATUS_HTTP_RSP]		= { "Invalid http response",           { 0, 1 }},

	[HANA_STATUS_HTTP_READ_ERROR]	= { "Read error (http)",               { 0, 1 }},
	[HANA_STATUS_HTTP_READ_TIMEOUT]	= { "Read timeout (http)",             { 0, 1 }},
	[HANA_STATUS_HTTP_BROKEN_PIPE]	= { "Close from server (http)",        { 0, 1 }},
};

/*
 * Convert check_status code to description
 */
const char *get_check_status_description(short check_status) {

	const char *desc;

	if (check_status < HCHK_STATUS_SIZE)
		desc = check_statuses[check_status].desc;
	else
		desc = NULL;

	if (desc && *desc)
		return desc;
	else
		return check_statuses[HCHK_STATUS_UNKNOWN].desc;
}

/*
 * Convert check_status code to short info
 */
const char *get_check_status_info(short check_status) {

	const char *info;

	if (check_status < HCHK_STATUS_SIZE)
		info = check_statuses[check_status].info;
	else
		info = NULL;

	if (info && *info)
		return info;
	else
		return check_statuses[HCHK_STATUS_UNKNOWN].info;
}

const char *get_analyze_status(short analyze_status) {

	const char *desc;

	if (analyze_status < HANA_STATUS_SIZE)
		desc = analyze_statuses[analyze_status].desc;
	else
		desc = NULL;

	if (desc && *desc)
		return desc;
	else
		return analyze_statuses[HANA_STATUS_UNKNOWN].desc;
}

static void server_status_printf(struct chunk *msg, struct server *s, struct check *check, int xferred) {
	if (s->track)
		chunk_appendf(msg, " via %s/%s",
			s->track->proxy->id, s->track->id);

	if (check) {
		chunk_appendf(msg, ", reason: %s", get_check_status_description(check->status));

		if (check->status >= HCHK_STATUS_L57DATA)
			chunk_appendf(msg, ", code: %d", check->code);

		if (*check->desc) {
			struct chunk src;

			chunk_appendf(msg, ", info: \"");

			chunk_initlen(&src, check->desc, 0, strlen(check->desc));
			chunk_asciiencode(msg, &src, '"');

			chunk_appendf(msg, "\"");
		}

		if (check->duration >= 0)
			chunk_appendf(msg, ", check duration: %ldms", check->duration);
	}

	if (xferred >= 0) {
		if (!(s->state & SRV_RUNNING))
			chunk_appendf(msg, ". %d active and %d backup servers left.%s"
				" %d sessions active, %d requeued, %d remaining in queue",
				s->proxy->srv_act, s->proxy->srv_bck,
				(s->proxy->srv_bck && !s->proxy->srv_act) ? " Running on backup." : "",
				s->cur_sess, xferred, s->nbpend);
		else 
			chunk_appendf(msg, ". %d active and %d backup servers online.%s"
				" %d sessions requeued, %d total in queue",
				s->proxy->srv_act, s->proxy->srv_bck,
				(s->proxy->srv_bck && !s->proxy->srv_act) ? " Running on backup." : "",
				xferred, s->nbpend);
	}
}

/*
 * Set check->status, update check->duration and fill check->result with
 * an adequate CHK_RES_* value.
 *
 * Show information in logs about failed health check if server is UP
 * or succeeded health checks if server is DOWN.
 */
static void set_server_check_status(struct check *check, short status, const char *desc)
{
	struct server *s = check->server;

	if (status == HCHK_STATUS_START) {
		check->result = CHK_RES_UNKNOWN;	/* no result yet */
		check->desc[0] = '\0';
		check->start = now;
		return;
	}

	if (!check->status)
		return;

	if (desc && *desc) {
		strncpy(check->desc, desc, HCHK_DESC_LEN-1);
		check->desc[HCHK_DESC_LEN-1] = '\0';
	} else
		check->desc[0] = '\0';

	check->status = status;
	if (check_statuses[status].result)
		check->result = check_statuses[status].result;

	if (status == HCHK_STATUS_HANA)
		check->duration = -1;
	else if (!tv_iszero(&check->start)) {
		/* set_server_check_status() may be called more than once */
		check->duration = tv_ms_elapsed(&check->start, &now);
		tv_zero(&check->start);
	}

	/* Failure to connect to the agent as a secondary check should not
	 * cause the server to be marked down. So only log status changes
	 * for HCHK_STATUS_* statuses */
	if ((check->state & CHK_ST_AGENT) && check->status < HCHK_STATUS_L7TOUT)
		return;

	if (s->proxy->options2 & PR_O2_LOGHCHKS &&
	(((check->health != 0) && (check->result == CHK_RES_FAILED)) ||
	    (((check->health != check->rise + check->fall - 1) ||
	      (!s->uweight && !(s->state & SRV_DRAIN)) ||
	      (s->uweight && (s->state & SRV_DRAIN))) &&
	     (check->result >= CHK_RES_PASSED)) ||
	    ((s->state & SRV_GOINGDOWN) && (check->result != CHK_RES_CONDPASS)) ||
	    (!(s->state & SRV_GOINGDOWN) && (check->result == CHK_RES_CONDPASS)))) {

		int health, rise, fall, state;

		chunk_reset(&trash);

		/* FIXME begin: calculate local version of the health/rise/fall/state */
		health = check->health;
		rise   = check->rise;
		fall   = check->fall;
		state  = s->state;

		switch (check->result) {
		case CHK_RES_FAILED:
			if (health > rise) {
				health--; /* still good */
			} else {
				if (health == rise)
					state &= ~(SRV_RUNNING | SRV_GOINGDOWN);

				health = 0;
			}
			break;

		case CHK_RES_PASSED:
		case CHK_RES_CONDPASS:
			if (health < rise + fall - 1) {
				health++; /* was bad, stays for a while */

				if (health == rise)
					state |= SRV_RUNNING;

				if (health >= rise)
					health = rise + fall - 1; /* OK now */
			}

			/* clear consecutive_errors if observing is enabled */
			if (s->onerror)
				s->consecutive_errors = 0;
			break;
		default:
			break;
		}

		chunk_appendf(&trash,
		             "Health check for %sserver %s/%s %s%s",
		             s->state & SRV_BACKUP ? "backup " : "",
		             s->proxy->id, s->id,
		             (check->result == CHK_RES_CONDPASS) ? "conditionally ":"",
		             (check->result >= CHK_RES_PASSED)   ? "succeeded":"failed");

		server_status_printf(&trash, s, check, -1);

		chunk_appendf(&trash, ", status: %d/%d %s",
		             (state & SRV_RUNNING) ? (health - rise + 1) : (health),
		             (state & SRV_RUNNING) ? (fall) : (rise),
			     (state & SRV_RUNNING)?(s->eweight?"UP":"DRAIN"):"DOWN");

		Warning("%s.\n", trash.str);
		send_log(s->proxy, LOG_NOTICE, "%s.\n", trash.str);
	}
}

/* sends a log message when a backend goes down, and also sets last
 * change date.
 */
static void set_backend_down(struct proxy *be)
{
	be->last_change = now.tv_sec;
	be->down_trans++;

	Alert("%s '%s' has no server available!\n", proxy_type_str(be), be->id);
	send_log(be, LOG_EMERG, "%s %s has no server available!\n", proxy_type_str(be), be->id);
}

/* Redistribute pending connections when a server goes down. The number of
 * connections redistributed is returned.
 */
static int redistribute_pending(struct server *s)
{
	struct pendconn *pc, *pc_bck, *pc_end;
	int xferred = 0;

	FOREACH_ITEM_SAFE(pc, pc_bck, &s->pendconns, pc_end, struct pendconn *, list) {
		struct session *sess = pc->sess;
		if ((sess->be->options & (PR_O_REDISP|PR_O_PERSIST)) == PR_O_REDISP &&
		    !(sess->flags & SN_FORCE_PRST)) {
			/* The REDISP option was specified. We will ignore
			 * cookie and force to balance or use the dispatcher.
			 */

			/* it's left to the dispatcher to choose a server */
			sess->flags &= ~(SN_DIRECT | SN_ASSIGNED | SN_ADDR_SET);

			pendconn_free(pc);
			task_wakeup(sess->task, TASK_WOKEN_RES);
			xferred++;
		}
	}
	return xferred;
}

/* Check for pending connections at the backend, and assign some of them to
 * the server coming up. The server's weight is checked before being assigned
 * connections it may not be able to handle. The total number of transferred
 * connections is returned.
 */
static int check_for_pending(struct server *s)
{
	int xferred;

	if (!s->eweight)
		return 0;

	for (xferred = 0; !s->maxconn || xferred < srv_dynamic_maxconn(s); xferred++) {
		struct session *sess;
		struct pendconn *p;

		p = pendconn_from_px(s->proxy);
		if (!p)
			break;
		p->sess->target = &s->obj_type;
		sess = p->sess;
		pendconn_free(p);
		task_wakeup(sess->task, TASK_WOKEN_RES);
	}
	return xferred;
}

/* Shutdown all connections of a server. The caller must pass a termination
 * code in <why>, which must be one of SN_ERR_* indicating the reason for the
 * shutdown.
 */
static void shutdown_sessions(struct server *srv, int why)
{
	struct session *session, *session_bck;

	list_for_each_entry_safe(session, session_bck, &srv->actconns, by_srv)
		if (session->srv_conn == srv)
			session_shutdown(session, why);
}

/* Shutdown all connections of all backup servers of a proxy. The caller must
 * pass a termination code in <why>, which must be one of SN_ERR_* indicating
 * the reason for the shutdown.
 */
static void shutdown_backup_sessions(struct proxy *px, int why)
{
	struct server *srv;

	for (srv = px->srv; srv != NULL; srv = srv->next)
		if (srv->state & SRV_BACKUP)
			shutdown_sessions(srv, why);
}

/* Sets server <s> down, notifies by all available means, recounts the
 * remaining servers on the proxy and transfers queued sessions whenever
 * possible to other servers. It automatically recomputes the number of
 * servers, but not the map.
 */
void set_server_down(struct check *check)
{
	struct server *s = check->server;
	struct server *srv;
	int xferred;

	if (s->state & SRV_MAINTAIN) {
		check->health = check->rise;
	}

	if ((s->state & SRV_RUNNING && check->health == check->rise) || s->track) {
		int srv_was_paused = s->state & SRV_GOINGDOWN;
		int prev_srv_count = s->proxy->srv_bck + s->proxy->srv_act;

		s->last_change = now.tv_sec;
		s->state &= ~(SRV_RUNNING | SRV_GOINGDOWN);
		if (s->proxy->lbprm.set_server_status_down)
			s->proxy->lbprm.set_server_status_down(s);

		if (s->onmarkeddown & HANA_ONMARKEDDOWN_SHUTDOWNSESSIONS)
			shutdown_sessions(s, SN_ERR_DOWN);

		/* we might have sessions queued on this server and waiting for
		 * a connection. Those which are redispatchable will be queued
		 * to another server or to the proxy itself.
		 */
		xferred = redistribute_pending(s);

		chunk_reset(&trash);

		if (s->state & SRV_MAINTAIN) {
			chunk_appendf(&trash,
			             "%sServer %s/%s is DOWN for maintenance", s->state & SRV_BACKUP ? "Backup " : "",
			             s->proxy->id, s->id);
		} else {
			chunk_appendf(&trash,
			             "%sServer %s/%s is DOWN", s->state & SRV_BACKUP ? "Backup " : "",
			             s->proxy->id, s->id);

			server_status_printf(&trash, s,
			                     ((!s->track && !(s->proxy->options2 & PR_O2_LOGHCHKS)) ? check : 0),
			                     xferred);
		}
		Warning("%s.\n", trash.str);

		/* we don't send an alert if the server was previously paused */
		if (srv_was_paused)
			send_log(s->proxy, LOG_NOTICE, "%s.\n", trash.str);
		else
			send_log(s->proxy, LOG_ALERT, "%s.\n", trash.str);

		if (prev_srv_count && s->proxy->srv_bck == 0 && s->proxy->srv_act == 0)
			set_backend_down(s->proxy);

		s->counters.down_trans++;

		for (srv = s->trackers; srv; srv = srv->tracknext)
			if (!(srv->state & SRV_MAINTAIN))
				/* Only notify tracking servers that are not already in maintenance. */
				set_server_down(&srv->check);
	}

	check->health = 0; /* failure */
}

void set_server_up(struct check *check) {

	struct server *s = check->server;
	struct server *srv;
	int xferred;
	unsigned int old_state = s->state;

	if (s->state & SRV_MAINTAIN) {
		check->health = check->rise;
	}

	if (s->track ||
	    ((s->check.state & CHK_ST_ENABLED) && (s->check.health == s->check.rise) &&
	     (s->agent.health >= s->agent.rise || !(s->agent.state & CHK_ST_ENABLED))) ||
	    ((s->agent.state & CHK_ST_ENABLED) && (s->agent.health == s->agent.rise) &&
	     (s->check.health >= s->check.rise || !(s->check.state & CHK_ST_ENABLED))) ||
	    (!(s->agent.state & CHK_ST_ENABLED) && !(s->check.state & CHK_ST_ENABLED))) {
		if (s->proxy->srv_bck == 0 && s->proxy->srv_act == 0) {
			if (s->proxy->last_change < now.tv_sec)		// ignore negative times
				s->proxy->down_time += now.tv_sec - s->proxy->last_change;
			s->proxy->last_change = now.tv_sec;
		}

		if (s->last_change < now.tv_sec)			// ignore negative times
			s->down_time += now.tv_sec - s->last_change;

		s->last_change = now.tv_sec;
		s->state |= SRV_RUNNING;
		s->state &= ~SRV_MAINTAIN;
		s->check.state &= ~CHK_ST_PAUSED;

		if (s->slowstart > 0) {
			s->state |= SRV_WARMINGUP;
			task_schedule(s->warmup, tick_add(now_ms, MS_TO_TICKS(MAX(1000, s->slowstart / 20))));
		}

		server_recalc_eweight(s);

		/* If the server is set with "on-marked-up shutdown-backup-sessions",
		 * and it's not a backup server and its effective weight is > 0,
		 * then it can accept new connections, so we shut down all sessions
		 * on all backup servers.
		 */
		if ((s->onmarkedup & HANA_ONMARKEDUP_SHUTDOWNBACKUPSESSIONS) &&
		    !(s->state & SRV_BACKUP) && s->eweight)
			shutdown_backup_sessions(s->proxy, SN_ERR_UP);

		/* check if we can handle some connections queued at the proxy. We
		 * will take as many as we can handle.
		 */
		xferred = check_for_pending(s);

		chunk_reset(&trash);

		if (old_state & SRV_MAINTAIN) {
			chunk_appendf(&trash,
			             "%sServer %s/%s is UP (leaving maintenance)", s->state & SRV_BACKUP ? "Backup " : "",
			             s->proxy->id, s->id);
		} else {
			chunk_appendf(&trash,
			             "%sServer %s/%s is UP", s->state & SRV_BACKUP ? "Backup " : "",
			             s->proxy->id, s->id);

			server_status_printf(&trash, s,
			                     ((!s->track && !(s->proxy->options2 & PR_O2_LOGHCHKS)) ?  check : NULL),
			                     xferred);
		}

		Warning("%s.\n", trash.str);
		send_log(s->proxy, LOG_NOTICE, "%s.\n", trash.str);

		for (srv = s->trackers; srv; srv = srv->tracknext)
			if (!(srv->state & SRV_MAINTAIN))
				/* Only notify tracking servers if they're not in maintenance. */
				set_server_up(&srv->check);
	}

	if (check->health >= check->rise)
		check->health = check->rise + check->fall - 1; /* OK now */

}

static void set_server_disabled(struct check *check) {

	struct server *s = check->server;
	struct server *srv;
	int xferred;

	s->state |= SRV_GOINGDOWN;
	if (s->proxy->lbprm.set_server_status_down)
		s->proxy->lbprm.set_server_status_down(s);

	/* we might have sessions queued on this server and waiting for
	 * a connection. Those which are redispatchable will be queued
	 * to another server or to the proxy itself.
	 */
	xferred = redistribute_pending(s);

	chunk_reset(&trash);

	chunk_appendf(&trash,
	             "Load-balancing on %sServer %s/%s is disabled",
	             s->state & SRV_BACKUP ? "Backup " : "",
	             s->proxy->id, s->id);

	server_status_printf(&trash, s,
	                     ((!s->track && !(s->proxy->options2 & PR_O2_LOGHCHKS)) ? check : NULL),
	                     xferred);

	Warning("%s.\n", trash.str);
	send_log(s->proxy, LOG_NOTICE, "%s.\n", trash.str);

	if (!s->proxy->srv_bck && !s->proxy->srv_act)
		set_backend_down(s->proxy);

	for (srv = s->trackers; srv; srv = srv->tracknext)
		set_server_disabled(&srv->check);
}

static void set_server_enabled(struct check *check) {

	struct server *s = check->server;
	struct server *srv;
	int xferred;

	s->state &= ~SRV_GOINGDOWN;
	if (s->proxy->lbprm.set_server_status_up)
		s->proxy->lbprm.set_server_status_up(s);

	/* check if we can handle some connections queued at the proxy. We
	 * will take as many as we can handle.
	 */
	xferred = check_for_pending(s);

	chunk_reset(&trash);

	chunk_appendf(&trash,
	             "Load-balancing on %sServer %s/%s is enabled again",
	             s->state & SRV_BACKUP ? "Backup " : "",
	             s->proxy->id, s->id);

	server_status_printf(&trash, s,
	                     ((!s->track && !(s->proxy->options2 & PR_O2_LOGHCHKS)) ? check : NULL),
	                     xferred);

	Warning("%s.\n", trash.str);
	send_log(s->proxy, LOG_NOTICE, "%s.\n", trash.str);

	for (srv = s->trackers; srv; srv = srv->tracknext)
		set_server_enabled(&srv->check);
}

static void check_failed(struct check *check)
{
	struct server *s = check->server;

	/* The agent secondary check should only cause a server to be marked
	 * as down if check->status is HCHK_STATUS_L7STS, which indicates
	 * that the agent returned "fail", "stopped" or "down".
	 * The implication here is that failure to connect to the agent
	 * as a secondary check should not cause the server to be marked
	 * down. */
	if ((check->state & CHK_ST_AGENT) && check->status != HCHK_STATUS_L7STS)
		return;

	if (check->health > check->rise) {
		check->health--; /* still good */
		s->counters.failed_checks++;
	}
	else
		set_server_down(check);
}

/* note: use health_adjust() only, which first checks that the observe mode is
 * enabled.
 */
void __health_adjust(struct server *s, short status)
{
	int failed;
	int expire;

	if (s->observe >= HANA_OBS_SIZE)
		return;

	if (status >= HANA_STATUS_SIZE || !analyze_statuses[status].desc)
		return;

	switch (analyze_statuses[status].lr[s->observe - 1]) {
		case 1:
			failed = 1;
			break;

		case 2:
			failed = 0;
			break;

		default:
			return;
	}

	if (!failed) {
		/* good: clear consecutive_errors */
		s->consecutive_errors = 0;
		return;
	}

	s->consecutive_errors++;

	if (s->consecutive_errors < s->consecutive_errors_limit)
		return;

	chunk_printf(&trash, "Detected %d consecutive errors, last one was: %s",
	             s->consecutive_errors, get_analyze_status(status));

	switch (s->onerror) {
		case HANA_ONERR_FASTINTER:
		/* force fastinter - nothing to do here as all modes force it */
			break;

		case HANA_ONERR_SUDDTH:
		/* simulate a pre-fatal failed health check */
			if (s->check.health > s->check.rise)
				s->check.health = s->check.rise + 1;

			/* no break - fall through */

		case HANA_ONERR_FAILCHK:
		/* simulate a failed health check */
			set_server_check_status(&s->check, HCHK_STATUS_HANA, trash.str);
			check_failed(&s->check);

			break;

		case HANA_ONERR_MARKDWN:
		/* mark server down */
			s->check.health = s->check.rise;
			set_server_check_status(&s->check, HCHK_STATUS_HANA, trash.str);
			set_server_down(&s->check);

			break;

		default:
			/* write a warning? */
			break;
	}

	s->consecutive_errors = 0;
	s->counters.failed_hana++;

	if (s->check.fastinter) {
		expire = tick_add(now_ms, MS_TO_TICKS(s->check.fastinter));
		if (s->check.task->expire > expire) {
			s->check.task->expire = expire;
			/* requeue check task with new expire */
			task_queue(s->check.task);
		}
	}
}

static int httpchk_build_status_header(struct server *s, char *buffer, int size)
{
	int sv_state;
	int ratio;
	int hlen = 0;
	const char *srv_hlt_st[7] = { "DOWN", "DOWN %d/%d",
				      "UP %d/%d", "UP",
				      "NOLB %d/%d", "NOLB",
				      "no check" };

	memcpy(buffer + hlen, "X-Haproxy-Server-State: ", 24);
	hlen += 24;

	if (!(s->check.state & CHK_ST_ENABLED))
		sv_state = 6;
	else if (s->state & SRV_RUNNING) {
		if (s->check.health == s->check.rise + s->check.fall - 1)
			sv_state = 3; /* UP */
		else
			sv_state = 2; /* going down */

		if (s->state & SRV_GOINGDOWN)
			sv_state += 2;
	} else {
		if (s->check.health)
			sv_state = 1; /* going up */
		else
			sv_state = 0; /* DOWN */
	}

	hlen += snprintf(buffer + hlen, size - hlen,
			     srv_hlt_st[sv_state],
			     (s->state & SRV_RUNNING) ? (s->check.health - s->check.rise + 1) : (s->check.health),
			     (s->state & SRV_RUNNING) ? (s->check.fall) : (s->check.rise));

	hlen += snprintf(buffer + hlen,  size - hlen, "; name=%s/%s; node=%s; weight=%d/%d; scur=%d/%d; qcur=%d",
			     s->proxy->id, s->id,
			     global.node,
			     (s->eweight * s->proxy->lbprm.wmult + s->proxy->lbprm.wdiv - 1) / s->proxy->lbprm.wdiv,
			     (s->proxy->lbprm.tot_weight * s->proxy->lbprm.wmult + s->proxy->lbprm.wdiv - 1) / s->proxy->lbprm.wdiv,
			     s->cur_sess, s->proxy->beconn - s->proxy->nbpend,
			     s->nbpend);

	if ((s->state & SRV_WARMINGUP) &&
	    now.tv_sec < s->last_change + s->slowstart &&
	    now.tv_sec >= s->last_change) {
		ratio = MAX(1, 100 * (now.tv_sec - s->last_change) / s->slowstart);
		hlen += snprintf(buffer + hlen, size - hlen, "; throttle=%d%%", ratio);
	}

	buffer[hlen++] = '\r';
	buffer[hlen++] = '\n';

	return hlen;
}

/* Check the connection. If an error has already been reported or the socket is
 * closed, keep errno intact as it is supposed to contain the valid error code.
 * If no error is reported, check the socket's error queue using getsockopt().
 * Warning, this must be done only once when returning from poll, and never
 * after an I/O error was attempted, otherwise the error queue might contain
 * inconsistent errors. If an error is detected, the CO_FL_ERROR is set on the
 * socket. Returns non-zero if an error was reported, zero if everything is
 * clean (including a properly closed socket).
 */
static int retrieve_errno_from_socket(struct connection *conn)
{
	int skerr;
	socklen_t lskerr = sizeof(skerr);

	if (conn->flags & CO_FL_ERROR && ((errno && errno != EAGAIN) || !conn->ctrl))
		return 1;

	if (!conn_ctrl_ready(conn))
		return 0;

	if (getsockopt(conn->t.sock.fd, SOL_SOCKET, SO_ERROR, &skerr, &lskerr) == 0)
		errno = skerr;

	if (errno == EAGAIN)
		errno = 0;

	if (!errno) {
		/* we could not retrieve an error, that does not mean there is
		 * none. Just don't change anything and only report the prior
		 * error if any.
		 */
		if (conn->flags & CO_FL_ERROR)
			return 1;
		else
			return 0;
	}

	conn->flags |= CO_FL_ERROR | CO_FL_SOCK_WR_SH | CO_FL_SOCK_RD_SH;
	return 1;
}

/* Try to collect as much information as possible on the connection status,
 * and adjust the server status accordingly. It may make use of <errno_bck>
 * if non-null when the caller is absolutely certain of its validity (eg:
 * checked just after a syscall). If the caller doesn't have a valid errno,
 * it can pass zero, and retrieve_errno_from_socket() will be called to try
 * to extract errno from the socket. If no error is reported, it will consider
 * the <expired> flag. This is intended to be used when a connection error was
 * reported in conn->flags or when a timeout was reported in <expired>. The
 * function takes care of not updating a server status which was already set.
 * All situations where at least one of <expired> or CO_FL_ERROR are set
 * produce a status.
 */
static void chk_report_conn_err(struct connection *conn, int errno_bck, int expired)
{
	struct check *check = conn->owner;
	const char *err_msg;
	struct chunk *chk;

	if (check->result != CHK_RES_UNKNOWN)
		return;

	errno = errno_bck;
	if (!errno || errno == EAGAIN)
		retrieve_errno_from_socket(conn);

	if (!(conn->flags & CO_FL_ERROR) && !expired)
		return;

	/* we'll try to build a meaningful error message depending on the
	 * context of the error possibly present in conn->err_code, and the
	 * socket error possibly collected above. This is useful to know the
	 * exact step of the L6 layer (eg: SSL handshake).
	 */
	chk = get_trash_chunk();

	if (check->type == PR_O2_TCPCHK_CHK) {
		chunk_printf(chk, " at step %d of tcp-check", tcpcheck_get_step_id(check->server));
		/* we were looking for a string */
		if (check->current_step && check->current_step->action == TCPCHK_ACT_CONNECT) {
			chunk_appendf(chk, " (connect)");
		}
		else if (check->current_step && check->current_step->action == TCPCHK_ACT_EXPECT) {
			if (check->current_step->string)
				chunk_appendf(chk, " (string '%s')", check->current_step->string);
			else if (check->current_step->expect_regex)
				chunk_appendf(chk, " (expect regex)");
		}
		else if (check->current_step && check->current_step->action == TCPCHK_ACT_SEND) {
			chunk_appendf(chk, " (send)");
		}
	}

	if (conn->err_code) {
		if (errno && errno != EAGAIN)
			chunk_printf(&trash, "%s (%s)%s", conn_err_code_str(conn), strerror(errno), chk->str);
		else
			chunk_printf(&trash, "%s%s", conn_err_code_str(conn), chk->str);
		err_msg = trash.str;
	}
	else {
		if (errno && errno != EAGAIN) {
			chunk_printf(&trash, "%s%s", strerror(errno), chk->str);
			err_msg = trash.str;
		}
		else {
			err_msg = chk->str;
		}
	}

	if ((conn->flags & (CO_FL_CONNECTED|CO_FL_WAIT_L4_CONN)) == CO_FL_WAIT_L4_CONN) {
		/* L4 not established (yet) */
		if (conn->flags & CO_FL_ERROR)
			set_server_check_status(check, HCHK_STATUS_L4CON, err_msg);
		else if (expired)
			set_server_check_status(check, HCHK_STATUS_L4TOUT, err_msg);
	}
	else if ((conn->flags & (CO_FL_CONNECTED|CO_FL_WAIT_L6_CONN)) == CO_FL_WAIT_L6_CONN) {
		/* L6 not established (yet) */
		if (conn->flags & CO_FL_ERROR)
			set_server_check_status(check, HCHK_STATUS_L6RSP, err_msg);
		else if (expired)
			set_server_check_status(check, HCHK_STATUS_L6TOUT, err_msg);
	}
	else if (conn->flags & CO_FL_ERROR) {
		/* I/O error after connection was established and before we could diagnose */
		set_server_check_status(check, HCHK_STATUS_SOCKERR, err_msg);
	}
	else if (expired) {
		/* connection established but expired check */
		if (check->type == PR_O2_SSL3_CHK)
			set_server_check_status(check, HCHK_STATUS_L6TOUT, err_msg);
		else	/* HTTP, SMTP, ... */
			set_server_check_status(check, HCHK_STATUS_L7TOUT, err_msg);
	}

	return;
}

/*
 * This function is used only for server health-checks. It handles
 * the connection acknowledgement. If the proxy requires L7 health-checks,
 * it sends the request. In other cases, it calls set_server_check_status()
 * to set check->status, check->duration and check->result.
 */
static void event_srv_chk_w(struct connection *conn)
{
	struct check *check = conn->owner;
	struct server *s = check->server;
	struct task *t = check->task;

	if (unlikely(check->result == CHK_RES_FAILED))
		goto out_wakeup;

	if (conn->flags & CO_FL_HANDSHAKE)
		return;

	if (retrieve_errno_from_socket(conn)) {
		chk_report_conn_err(conn, errno, 0);
		__conn_data_stop_both(conn);
		goto out_wakeup;
	}

	if (conn->flags & (CO_FL_SOCK_WR_SH | CO_FL_DATA_WR_SH)) {
		/* if the output is closed, we can't do anything */
		conn->flags |= CO_FL_ERROR;
		chk_report_conn_err(conn, 0, 0);
		goto out_wakeup;
	}

	/* here, we know that the connection is established. That's enough for
	 * a pure TCP check.
	 */
	if (!check->type)
		goto out_wakeup;

	if (check->type == PR_O2_TCPCHK_CHK) {
		tcpcheck_main(conn);
		return;
	}

	if (check->bo->o) {
		conn->xprt->snd_buf(conn, check->bo, 0);
		if (conn->flags & CO_FL_ERROR) {
			chk_report_conn_err(conn, errno, 0);
			__conn_data_stop_both(conn);
			goto out_wakeup;
		}
		if (check->bo->o)
			return;
	}

	/* full request sent, we allow up to <timeout.check> if nonzero for a response */
	if (s->proxy->timeout.check) {
		t->expire = tick_add_ifset(now_ms, s->proxy->timeout.check);
		task_queue(t);
	}
	goto out_nowake;

 out_wakeup:
	task_wakeup(t, TASK_WOKEN_IO);
 out_nowake:
	__conn_data_stop_send(conn);   /* nothing more to write */
}

/*
 * This function is used only for server health-checks. It handles the server's
 * reply to an HTTP request, SSL HELLO or MySQL client Auth. It calls
 * set_server_check_status() to update check->status, check->duration
 * and check->result.

 * The set_server_check_status function is called with HCHK_STATUS_L7OKD if
 * an HTTP server replies HTTP 2xx or 3xx (valid responses), if an SMTP server
 * returns 2xx, HCHK_STATUS_L6OK if an SSL server returns at least 5 bytes in
 * response to an SSL HELLO (the principle is that this is enough to
 * distinguish between an SSL server and a pure TCP relay). All other cases will
 * call it with a proper error status like HCHK_STATUS_L7STS, HCHK_STATUS_L6RSP,
 * etc.
 */
static void event_srv_chk_r(struct connection *conn)
{
	struct check *check = conn->owner;
	struct server *s = check->server;
	struct task *t = check->task;
	char *desc;
	int done;
	unsigned short msglen;

	if (unlikely(check->result == CHK_RES_FAILED))
		goto out_wakeup;

	if (conn->flags & CO_FL_HANDSHAKE)
		return;

	if (check->type == PR_O2_TCPCHK_CHK) {
		tcpcheck_main(conn);
		return;
	}

	/* Warning! Linux returns EAGAIN on SO_ERROR if data are still available
	 * but the connection was closed on the remote end. Fortunately, recv still
	 * works correctly and we don't need to do the getsockopt() on linux.
	 */

	/* Set buffer to point to the end of the data already read, and check
	 * that there is free space remaining. If the buffer is full, proceed
	 * with running the checks without attempting another socket read.
	 */

	done = 0;

	conn->xprt->rcv_buf(conn, check->bi, check->bi->size);
	if (conn->flags & (CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_DATA_RD_SH)) {
		done = 1;
		if ((conn->flags & CO_FL_ERROR) && !check->bi->i) {
			/* Report network errors only if we got no other data. Otherwise
			 * we'll let the upper layers decide whether the response is OK
			 * or not. It is very common that an RST sent by the server is
			 * reported as an error just after the last data chunk.
			 */
			chk_report_conn_err(conn, errno, 0);
			goto out_wakeup;
		}
	}


	/* Intermediate or complete response received.
	 * Terminate string in check->bi->data buffer.
	 */
	if (check->bi->i < check->bi->size)
		check->bi->data[check->bi->i] = '\0';
	else {
		check->bi->data[check->bi->i - 1] = '\0';
		done = 1; /* buffer full, don't wait for more data */
	}

	/* Run the checks... */
	switch (check->type) {
	case PR_O2_HTTP_CHK:
		if (!done && check->bi->i < strlen("HTTP/1.0 000\r"))
			goto wait_more_data;

		/* Check if the server speaks HTTP 1.X */
		if ((check->bi->i < strlen("HTTP/1.0 000\r")) ||
		    (memcmp(check->bi->data, "HTTP/1.", 7) != 0 ||
		    (*(check->bi->data + 12) != ' ' && *(check->bi->data + 12) != '\r')) ||
		    !isdigit((unsigned char) *(check->bi->data + 9)) || !isdigit((unsigned char) *(check->bi->data + 10)) ||
		    !isdigit((unsigned char) *(check->bi->data + 11))) {
			cut_crlf(check->bi->data);
			set_server_check_status(check, HCHK_STATUS_L7RSP, check->bi->data);

			goto out_wakeup;
		}

		check->code = str2uic(check->bi->data + 9);
		desc = ltrim(check->bi->data + 12, ' ');
		
		if ((s->proxy->options & PR_O_DISABLE404) &&
			 (s->state & SRV_RUNNING) && (check->code == 404)) {
			/* 404 may be accepted as "stopping" only if the server was up */
			cut_crlf(desc);
			set_server_check_status(check, HCHK_STATUS_L7OKCD, desc);
		}
		else if (s->proxy->options2 & PR_O2_EXP_TYPE) {
			/* Run content verification check... We know we have at least 13 chars */
			if (!httpchk_expect(s, done))
				goto wait_more_data;
		}
		/* check the reply : HTTP/1.X 2xx and 3xx are OK */
		else if (*(check->bi->data + 9) == '2' || *(check->bi->data + 9) == '3') {
			cut_crlf(desc);
			set_server_check_status(check,  HCHK_STATUS_L7OKD, desc);
		}
		else {
			cut_crlf(desc);
			set_server_check_status(check, HCHK_STATUS_L7STS, desc);
		}
		break;

	case PR_O2_SSL3_CHK:
		if (!done && check->bi->i < 5)
			goto wait_more_data;

		/* Check for SSLv3 alert or handshake */
		if ((check->bi->i >= 5) && (*check->bi->data == 0x15 || *check->bi->data == 0x16))
			set_server_check_status(check, HCHK_STATUS_L6OK, NULL);
		else
			set_server_check_status(check, HCHK_STATUS_L6RSP, NULL);
		break;

	case PR_O2_SMTP_CHK:
		if (!done && check->bi->i < strlen("000\r"))
			goto wait_more_data;

		/* Check if the server speaks SMTP */
		if ((check->bi->i < strlen("000\r")) ||
		    (*(check->bi->data + 3) != ' ' && *(check->bi->data + 3) != '\r') ||
		    !isdigit((unsigned char) *check->bi->data) || !isdigit((unsigned char) *(check->bi->data + 1)) ||
		    !isdigit((unsigned char) *(check->bi->data + 2))) {
			cut_crlf(check->bi->data);
			set_server_check_status(check, HCHK_STATUS_L7RSP, check->bi->data);

			goto out_wakeup;
		}

		check->code = str2uic(check->bi->data);

		desc = ltrim(check->bi->data + 3, ' ');
		cut_crlf(desc);

		/* Check for SMTP code 2xx (should be 250) */
		if (*check->bi->data == '2')
			set_server_check_status(check, HCHK_STATUS_L7OKD, desc);
		else
			set_server_check_status(check, HCHK_STATUS_L7STS, desc);
		break;

	case PR_O2_LB_AGENT_CHK: {
		short status = HCHK_STATUS_L7RSP;
		const char *desc = "Unknown feedback string";
		const char *down_cmd = NULL;
		int disabled;
		char *p;

		/* get a complete line first */
		p = check->bi->data;
		while (*p && *p != '\n' && *p != '\r')
			p++;

		if (!*p) {
			if (!done)
				goto wait_more_data;

			/* at least inform the admin that the agent is mis-behaving */
			set_server_check_status(check, check->status, "Ignoring incomplete line from agent");
			break;
		}
		*p = 0;

		/*
		 * The agent may have been disabled after a check was
		 * initialised.  If so, ignore weight changes and drain
		 * settings from the agent.  Note that the setting is
		 * always present in the state of the agent the server,
		 * regardless of if the agent is being run as a primary or
		 * secondary check. That is, regardless of if the check
		 * parameter of this function is the agent or check field
		 * of the server.
		 */
		disabled = !(check->server->agent.state & CHK_ST_ENABLED);

		if (strchr(check->bi->data, '%')) {
			if (disabled)
				break;
			desc = server_parse_weight_change_request(s, check->bi->data);
			if (!desc) {
				status = HCHK_STATUS_L7OKD;
				desc = check->bi->data;
			}
		} else if (!strcasecmp(check->bi->data, "drain")) {
			if (disabled)
				break;
			desc = server_parse_weight_change_request(s, "0%");
			if (!desc) {
				desc = "drain";
				status = HCHK_STATUS_L7OKD;
			}
		} else if (!strncasecmp(check->bi->data, "down", strlen("down"))) {
			down_cmd = "down";
		} else if (!strncasecmp(check->bi->data, "stopped", strlen("stopped"))) {
			down_cmd = "stopped";
		} else if (!strncasecmp(check->bi->data, "fail", strlen("fail"))) {
			down_cmd = "fail";
		}

		if (down_cmd) {
			const char *end = check->bi->data + strlen(down_cmd);
			/*
			 * The command keyword must terminated the string or
			 * be followed by a blank.
			 */
			if (end[0] == '\0' || end[0] == ' ' || end[0] == '\t') {
				status = HCHK_STATUS_L7STS;
				desc = check->bi->data;
			}
		}

		set_server_check_status(check, status, desc);
		set_server_drain_state(check->server);
		break;
	}

	case PR_O2_PGSQL_CHK:
		if (!done && check->bi->i < 9)
			goto wait_more_data;

		if (check->bi->data[0] == 'R') {
			set_server_check_status(check, HCHK_STATUS_L7OKD, "PostgreSQL server is ok");
		}
		else {
			if ((check->bi->data[0] == 'E') && (check->bi->data[5]!=0) && (check->bi->data[6]!=0))
				desc = &check->bi->data[6];
			else
				desc = "PostgreSQL unknown error";

			set_server_check_status(check, HCHK_STATUS_L7STS, desc);
		}
		break;

	case PR_O2_REDIS_CHK:
		if (!done && check->bi->i < 7)
			goto wait_more_data;

		if (strcmp(check->bi->data, "+PONG\r\n") == 0) {
			set_server_check_status(check, HCHK_STATUS_L7OKD, "Redis server is ok");
		}
		else {
			set_server_check_status(check, HCHK_STATUS_L7STS, check->bi->data);
		}
		break;

	case PR_O2_MYSQL_CHK:
		if (!done && check->bi->i < 5)
			goto wait_more_data;

		if (s->proxy->check_len == 0) { // old mode
			if (*(check->bi->data + 4) != '\xff') {
				/* We set the MySQL Version in description for information purpose
				 * FIXME : it can be cool to use MySQL Version for other purpose,
				 * like mark as down old MySQL server.
				 */
				if (check->bi->i > 51) {
					desc = ltrim(check->bi->data + 5, ' ');
					set_server_check_status(check, HCHK_STATUS_L7OKD, desc);
				}
				else {
					if (!done)
						goto wait_more_data;
					/* it seems we have a OK packet but without a valid length,
					 * it must be a protocol error
					 */
					set_server_check_status(check, HCHK_STATUS_L7RSP, check->bi->data);
				}
			}
			else {
				/* An error message is attached in the Error packet */
				desc = ltrim(check->bi->data + 7, ' ');
				set_server_check_status(check, HCHK_STATUS_L7STS, desc);
			}
		} else {
			unsigned int first_packet_len = ((unsigned int) *check->bi->data) +
			                                (((unsigned int) *(check->bi->data + 1)) << 8) +
			                                (((unsigned int) *(check->bi->data + 2)) << 16);

			if (check->bi->i == first_packet_len + 4) {
				/* MySQL Error packet always begin with field_count = 0xff */
				if (*(check->bi->data + 4) != '\xff') {
					/* We have only one MySQL packet and it is a Handshake Initialization packet
					* but we need to have a second packet to know if it is alright
					*/
					if (!done && check->bi->i < first_packet_len + 5)
						goto wait_more_data;
				}
				else {
					/* We have only one packet and it is an Error packet,
					* an error message is attached, so we can display it
					*/
					desc = &check->bi->data[7];
					//Warning("onlyoneERR: %s\n", desc);
					set_server_check_status(check, HCHK_STATUS_L7STS, desc);
				}
			} else if (check->bi->i > first_packet_len + 4) {
				unsigned int second_packet_len = ((unsigned int) *(check->bi->data + first_packet_len + 4)) +
				                                 (((unsigned int) *(check->bi->data + first_packet_len + 5)) << 8) +
				                                 (((unsigned int) *(check->bi->data + first_packet_len + 6)) << 16);

				if (check->bi->i == first_packet_len + 4 + second_packet_len + 4 ) {
					/* We have 2 packets and that's good */
					/* Check if the second packet is a MySQL Error packet or not */
					if (*(check->bi->data + first_packet_len + 8) != '\xff') {
						/* No error packet */
						/* We set the MySQL Version in description for information purpose */
						desc = &check->bi->data[5];
						//Warning("2packetOK: %s\n", desc);
						set_server_check_status(check, HCHK_STATUS_L7OKD, desc);
					}
					else {
						/* An error message is attached in the Error packet
						* so we can display it ! :)
						*/
						desc = &check->bi->data[first_packet_len+11];
						//Warning("2packetERR: %s\n", desc);
						set_server_check_status(check, HCHK_STATUS_L7STS, desc);
					}
				}
			}
			else {
				if (!done)
					goto wait_more_data;
				/* it seems we have a Handshake Initialization packet but without a valid length,
				 * it must be a protocol error
				 */
				desc = &check->bi->data[5];
				//Warning("protoerr: %s\n", desc);
				set_server_check_status(check, HCHK_STATUS_L7RSP, desc);
			}
		}
		break;

	case PR_O2_LDAP_CHK:
		if (!done && check->bi->i < 14)
			goto wait_more_data;

		/* Check if the server speaks LDAP (ASN.1/BER)
		 * http://en.wikipedia.org/wiki/Basic_Encoding_Rules
		 * http://tools.ietf.org/html/rfc4511
		 */

		/* http://tools.ietf.org/html/rfc4511#section-4.1.1
		 *   LDAPMessage: 0x30: SEQUENCE
		 */
		if ((check->bi->i < 14) || (*(check->bi->data) != '\x30')) {
			set_server_check_status(check, HCHK_STATUS_L7RSP, "Not LDAPv3 protocol");
		}
		else {
			 /* size of LDAPMessage */
			msglen = (*(check->bi->data + 1) & 0x80) ? (*(check->bi->data + 1) & 0x7f) : 0;

			/* http://tools.ietf.org/html/rfc4511#section-4.2.2
			 *   messageID: 0x02 0x01 0x01: INTEGER 1
			 *   protocolOp: 0x61: bindResponse
			 */
			if ((msglen > 2) ||
			    (memcmp(check->bi->data + 2 + msglen, "\x02\x01\x01\x61", 4) != 0)) {
				set_server_check_status(check, HCHK_STATUS_L7RSP, "Not LDAPv3 protocol");

				goto out_wakeup;
			}

			/* size of bindResponse */
			msglen += (*(check->bi->data + msglen + 6) & 0x80) ? (*(check->bi->data + msglen + 6) & 0x7f) : 0;

			/* http://tools.ietf.org/html/rfc4511#section-4.1.9
			 *   ldapResult: 0x0a 0x01: ENUMERATION
			 */
			if ((msglen > 4) ||
			    (memcmp(check->bi->data + 7 + msglen, "\x0a\x01", 2) != 0)) {
				set_server_check_status(check, HCHK_STATUS_L7RSP, "Not LDAPv3 protocol");

				goto out_wakeup;
			}

			/* http://tools.ietf.org/html/rfc4511#section-4.1.9
			 *   resultCode
			 */
			check->code = *(check->bi->data + msglen + 9);
			if (check->code) {
				set_server_check_status(check, HCHK_STATUS_L7STS, "See RFC: http://tools.ietf.org/html/rfc4511#section-4.1.9");
			} else {
				set_server_check_status(check, HCHK_STATUS_L7OKD, "Success");
			}
		}
		break;

	default:
		/* for other checks (eg: pure TCP), delegate to the main task */
		break;
	} /* switch */

 out_wakeup:
	/* collect possible new errors */
	if (conn->flags & CO_FL_ERROR)
		chk_report_conn_err(conn, 0, 0);

	/* Reset the check buffer... */
	*check->bi->data = '\0';
	check->bi->i = 0;

	/* Close the connection... We absolutely want to perform a hard close
	 * and reset the connection if some data are pending, otherwise we end
	 * up with many TIME_WAITs and eat all the source port range quickly.
	 * To avoid sending RSTs all the time, we first try to drain pending
	 * data.
	 */
	if (conn->xprt && conn->xprt->shutw)
		conn->xprt->shutw(conn, 0);

	/* OK, let's not stay here forever */
	if (check->result == CHK_RES_FAILED)
		conn->flags |= CO_FL_ERROR;

	__conn_data_stop_both(conn);
	task_wakeup(t, TASK_WOKEN_IO);
	return;

 wait_more_data:
	__conn_data_want_recv(conn);
}

/*
 * This function is used only for server health-checks. It handles connection
 * status updates including errors. If necessary, it wakes the check task up.
 * It always returns 0.
 */
static int wake_srv_chk(struct connection *conn)
{
	struct check *check = conn->owner;

	if (unlikely(conn->flags & CO_FL_ERROR)) {
		/* We may get error reports bypassing the I/O handlers, typically
		 * the case when sending a pure TCP check which fails, then the I/O
		 * handlers above are not called. This is completely handled by the
		 * main processing task so let's simply wake it up. If we get here,
		 * we expect errno to still be valid.
		 */
		chk_report_conn_err(conn, errno, 0);

		__conn_data_stop_both(conn);
		task_wakeup(check->task, TASK_WOKEN_IO);
	}
	else if (!(conn->flags & (CO_FL_DATA_RD_ENA|CO_FL_DATA_WR_ENA|CO_FL_HANDSHAKE))) {
		/* we may get here if only a connection probe was required : we
		 * don't have any data to send nor anything expected in response,
		 * so the completion of the connection establishment is enough.
		 */
		task_wakeup(check->task, TASK_WOKEN_IO);
	}

	if (check->result != CHK_RES_UNKNOWN) {
		/* We're here because nobody wants to handle the error, so we
		 * sure want to abort the hard way.
		 */
		conn_drain(conn);
		conn_force_close(conn);
	}
	return 0;
}

struct data_cb check_conn_cb = {
	.recv = event_srv_chk_r,
	.send = event_srv_chk_w,
	.wake = wake_srv_chk,
};

/*
 * updates the server's weight during a warmup stage. Once the final weight is
 * reached, the task automatically stops. Note that any server status change
 * must have updated s->last_change accordingly.
 */
static struct task *server_warmup(struct task *t)
{
	struct server *s = t->context;

	/* by default, plan on stopping the task */
	t->expire = TICK_ETERNITY;
	if ((s->state & (SRV_RUNNING|SRV_WARMINGUP|SRV_MAINTAIN)) != (SRV_RUNNING|SRV_WARMINGUP))
		return t;

	server_recalc_eweight(s);

	/* probably that we can refill this server with a bit more connections */
	check_for_pending(s);

	/* get back there in 1 second or 1/20th of the slowstart interval,
	 * whichever is greater, resulting in small 5% steps.
	 */
	if (s->state & SRV_WARMINGUP)
		t->expire = tick_add(now_ms, MS_TO_TICKS(MAX(1000, s->slowstart / 20)));
	return t;
}

/*
 * manages a server health-check. Returns
 * the time the task accepts to wait, or TIME_ETERNITY for infinity.
 */
static struct task *process_chk(struct task *t)
{
	struct check *check = t->context;
	struct server *s = check->server;
	struct connection *conn = check->conn;
	int rv;
	int ret;
	int expired = tick_is_expired(t->expire, now_ms);

	if (!(check->state & CHK_ST_INPROGRESS)) {
		/* no check currently running */
		if (!expired) /* woke up too early */
			return t;

		/* we don't send any health-checks when the proxy is
		 * stopped, the server should not be checked or the check
		 * is disabled.
		 */
		if (((check->state & (CHK_ST_ENABLED | CHK_ST_PAUSED)) != CHK_ST_ENABLED) ||
		    s->proxy->state == PR_STSTOPPED)
			goto reschedule;

		/* we'll initiate a new check */
		set_server_check_status(check, HCHK_STATUS_START, NULL);

		check->state |= CHK_ST_INPROGRESS;
		check->bi->p = check->bi->data;
		check->bi->i = 0;
		check->bo->p = check->bo->data;
		check->bo->o = 0;

		/* tcpcheck send/expect initialisation */
		if (check->type == PR_O2_TCPCHK_CHK)
			check->current_step = NULL;

		/* prepare the check buffer.
		 * This should not be used if check is the secondary agent check
		 * of a server as s->proxy->check_req will relate to the
		 * configuration of the primary check. Similarly, tcp-check uses
		 * its own strings.
		 */
		if (check->type && check->type != PR_O2_TCPCHK_CHK && !(check->state & CHK_ST_AGENT)) {
			bo_putblk(check->bo, s->proxy->check_req, s->proxy->check_len);

			/* we want to check if this host replies to HTTP or SSLv3 requests
			 * so we'll send the request, and won't wake the checker up now.
			 */
			if ((check->type) == PR_O2_SSL3_CHK) {
				/* SSL requires that we put Unix time in the request */
				int gmt_time = htonl(date.tv_sec);
				memcpy(check->bo->data + 11, &gmt_time, 4);
			}
			else if ((check->type) == PR_O2_HTTP_CHK) {
				if (s->proxy->options2 & PR_O2_CHK_SNDST)
					bo_putblk(check->bo, trash.str, httpchk_build_status_header(s, trash.str, trash.size));
				bo_putstr(check->bo, "\r\n");
				*check->bo->p = '\0'; /* to make gdb output easier to read */
			}
		}

		/* prepare a new connection */
		conn_init(conn);
		conn_prepare(conn, s->check_common.proto, s->check_common.xprt);
		conn_attach(conn, check, &check_conn_cb);
		conn->target = &s->obj_type;

		/* no client address */
		clear_addr(&conn->addr.from);

		if (is_addr(&s->check_common.addr))
			/* we'll connect to the check addr specified on the server */
			conn->addr.to = s->check_common.addr;
		else
			/* we'll connect to the addr on the server */
			conn->addr.to = s->addr;

		if (check->port) {
			set_host_port(&conn->addr.to, check->port);
		}

		if (check->type == PR_O2_TCPCHK_CHK) {
			struct tcpcheck_rule *r = (struct tcpcheck_rule *) s->proxy->tcpcheck_rules.n;
			/* if first step is a 'connect', then tcpcheck_main must run it */
			if (r->action == TCPCHK_ACT_CONNECT) {
				tcpcheck_main(conn);
				return t;
			}
		}


		/* It can return one of :
		 *  - SN_ERR_NONE if everything's OK
		 *  - SN_ERR_SRVTO if there are no more servers
		 *  - SN_ERR_SRVCL if the connection was refused by the server
		 *  - SN_ERR_PRXCOND if the connection has been limited by the proxy (maxconn)
		 *  - SN_ERR_RESOURCE if a system resource is lacking (eg: fd limits, ports, ...)
		 *  - SN_ERR_INTERNAL for any other purely internal errors
		 * Additionnally, in the case of SN_ERR_RESOURCE, an emergency log will be emitted.
		 * Note that we try to prevent the network stack from sending the ACK during the
		 * connect() when a pure TCP check is used (without PROXY protocol).
		 */
		ret = SN_ERR_INTERNAL;
		if (s->check_common.proto->connect)
			ret = s->check_common.proto->connect(conn, check->type, (check->type) ? 0 : 2);
		conn->flags |= CO_FL_WAKE_DATA;
		if (s->check.send_proxy) {
			conn->send_proxy_ofs = 1;
			conn->flags |= CO_FL_SEND_PROXY;
		}

		switch (ret) {
		case SN_ERR_NONE:
			/* we allow up to min(inter, timeout.connect) for a connection
			 * to establish but only when timeout.check is set
			 * as it may be to short for a full check otherwise
			 */
			t->expire = tick_add(now_ms, MS_TO_TICKS(check->inter));

			if (s->proxy->timeout.check && s->proxy->timeout.connect) {
				int t_con = tick_add(now_ms, s->proxy->timeout.connect);
				t->expire = tick_first(t->expire, t_con);
			}

			if (check->type)
				conn_data_want_recv(conn);   /* prepare for reading a possible reply */

			goto reschedule;

		case SN_ERR_SRVTO: /* ETIMEDOUT */
		case SN_ERR_SRVCL: /* ECONNREFUSED, ENETUNREACH, ... */
			conn->flags |= CO_FL_ERROR;
			chk_report_conn_err(conn, errno, 0);
			break;
		case SN_ERR_PRXCOND:
		case SN_ERR_RESOURCE:
		case SN_ERR_INTERNAL:
			conn->flags |= CO_FL_ERROR;
			chk_report_conn_err(conn, 0, 0);
			break;
		}

		/* here, we have seen a synchronous error, no fd was allocated */

		check->state &= ~CHK_ST_INPROGRESS;
		check_failed(check);

		/* we allow up to min(inter, timeout.connect) for a connection
		 * to establish but only when timeout.check is set
		 * as it may be to short for a full check otherwise
		 */
		while (tick_is_expired(t->expire, now_ms)) {
			int t_con;

			t_con = tick_add(t->expire, s->proxy->timeout.connect);
			t->expire = tick_add(t->expire, MS_TO_TICKS(check->inter));

			if (s->proxy->timeout.check)
				t->expire = tick_first(t->expire, t_con);
		}
	}
	else {
		/* there was a test running.
		 * First, let's check whether there was an uncaught error,
		 * which can happen on connect timeout or error.
		 */
		if (s->check.result == CHK_RES_UNKNOWN) {
			/* good connection is enough for pure TCP check */
			if ((conn->flags & CO_FL_CONNECTED) && !check->type) {
				if (check->use_ssl)
					set_server_check_status(check, HCHK_STATUS_L6OK, NULL);
				else
					set_server_check_status(check, HCHK_STATUS_L4OK, NULL);
			}
			else if ((conn->flags & CO_FL_ERROR) || expired) {
				chk_report_conn_err(conn, 0, expired);
			}
			else
				goto out_wait; /* timeout not reached, wait again */
		}

		/* check complete or aborted */
		if (conn->xprt) {
			/* The check was aborted and the connection was not yet closed.
			 * This can happen upon timeout, or when an external event such
			 * as a failed response coupled with "observe layer7" caused the
			 * server state to be suddenly changed.
			 */
			conn_drain(conn);
			conn_force_close(conn);
		}

		if (check->result == CHK_RES_FAILED)  /* a failure or timeout detected */
			check_failed(check);
		else {  /* check was OK */
			/* we may have to add/remove this server from the LB group */
			if ((s->state & SRV_RUNNING) && (s->proxy->options & PR_O_DISABLE404)) {
				if ((s->state & SRV_GOINGDOWN) && (check->result != CHK_RES_CONDPASS))
					set_server_enabled(check);
				else if (!(s->state & SRV_GOINGDOWN) && (check->result == CHK_RES_CONDPASS))
					set_server_disabled(check);
			}

			if (!(s->state & SRV_MAINTAIN) &&
			    check->health < check->rise + check->fall - 1) {
				check->health++; /* was bad, stays for a while */
				set_server_up(check);
			}
		}
		check->state &= ~CHK_ST_INPROGRESS;

		rv = 0;
		if (global.spread_checks > 0) {
			rv = srv_getinter(check) * global.spread_checks / 100;
			rv -= (int) (2 * rv * (rand() / (RAND_MAX + 1.0)));
		}
		t->expire = tick_add(now_ms, MS_TO_TICKS(srv_getinter(check) + rv));
	}

 reschedule:
	while (tick_is_expired(t->expire, now_ms))
		t->expire = tick_add(t->expire, MS_TO_TICKS(check->inter));
 out_wait:
	return t;
}

static int start_check_task(struct check *check, int mininter,
			    int nbcheck, int srvpos)
{
	struct task *t;
	/* task for the check */
	if ((t = task_new()) == NULL) {
		Alert("Starting [%s:%s] check: out of memory.\n",
		      check->server->proxy->id, check->server->id);
		return 0;
	}

	check->task = t;
	t->process = process_chk;
	t->context = check;

	if (mininter < srv_getinter(check))
		mininter = srv_getinter(check);

	if (global.max_spread_checks && mininter > global.max_spread_checks)
		mininter = global.max_spread_checks;

	/* check this every ms */
	t->expire = tick_add(now_ms, MS_TO_TICKS(mininter * srvpos / nbcheck));
	check->start = now;
	task_queue(t);

	return 1;
}

/*
 * Start health-check.
 * Returns 0 if OK, -1 if error, and prints the error in this case.
 */
int start_checks() {

	struct proxy *px;
	struct server *s;
	struct task *t;
	int nbcheck=0, mininter=0, srvpos=0;

	/* 1- count the checkers to run simultaneously.
	 * We also determine the minimum interval among all of those which
	 * have an interval larger than SRV_CHK_INTER_THRES. This interval
	 * will be used to spread their start-up date. Those which have
	 * a shorter interval will start independently and will not dictate
	 * too short an interval for all others.
	 */
	for (px = proxy; px; px = px->next) {
		for (s = px->srv; s; s = s->next) {
			if (s->slowstart) {
				if ((t = task_new()) == NULL) {
					Alert("Starting [%s:%s] check: out of memory.\n", px->id, s->id);
					return -1;
				}
				/* We need a warmup task that will be called when the server
				 * state switches from down to up.
				 */
				s->warmup = t;
				t->process = server_warmup;
				t->context = s;
				t->expire = TICK_ETERNITY;
			}

			if (s->check.state & CHK_ST_CONFIGURED) {
				nbcheck++;
				if ((srv_getinter(&s->check) >= SRV_CHK_INTER_THRES) &&
				    (!mininter || mininter > srv_getinter(&s->check)))
					mininter = srv_getinter(&s->check);
			}

			if (s->agent.state & CHK_ST_CONFIGURED) {
				nbcheck++;
				if ((srv_getinter(&s->agent) >= SRV_CHK_INTER_THRES) &&
				    (!mininter || mininter > srv_getinter(&s->agent)))
					mininter = srv_getinter(&s->agent);
			}
		}
	}

	if (!nbcheck)
		return 0;

	srand((unsigned)time(NULL));

	/*
	 * 2- start them as far as possible from each others. For this, we will
	 * start them after their interval set to the min interval divided by
	 * the number of servers, weighted by the server's position in the list.
	 */
	for (px = proxy; px; px = px->next) {
		for (s = px->srv; s; s = s->next) {
			/* A task for the main check */
			if (s->check.state & CHK_ST_CONFIGURED) {
				if (!start_check_task(&s->check, mininter, nbcheck, srvpos))
					return -1;
				srvpos++;
			}

			/* A task for a auxiliary agent check */
			if (s->agent.state & CHK_ST_CONFIGURED) {
				if (!start_check_task(&s->agent, mininter, nbcheck, srvpos)) {
					return -1;
				}
				srvpos++;
			}
		}
	}
	return 0;
}

/*
 * Perform content verification check on data in s->check.buffer buffer.
 * The buffer MUST be terminated by a null byte before calling this function.
 * Sets server status appropriately. The caller is responsible for ensuring
 * that the buffer contains at least 13 characters. If <done> is zero, we may
 * return 0 to indicate that data is required to decide of a match.
 */
static int httpchk_expect(struct server *s, int done)
{
	static char status_msg[] = "HTTP status check returned code <000>";
	char status_code[] = "000";
	char *contentptr;
	int crlf;
	int ret;

	switch (s->proxy->options2 & PR_O2_EXP_TYPE) {
	case PR_O2_EXP_STS:
	case PR_O2_EXP_RSTS:
		memcpy(status_code, s->check.bi->data + 9, 3);
		memcpy(status_msg + strlen(status_msg) - 4, s->check.bi->data + 9, 3);

		if ((s->proxy->options2 & PR_O2_EXP_TYPE) == PR_O2_EXP_STS)
			ret = strncmp(s->proxy->expect_str, status_code, 3) == 0;
		else
			ret = regexec(s->proxy->expect_regex, status_code, MAX_MATCH, pmatch, 0) == 0;

		/* we necessarily have the response, so there are no partial failures */
		if (s->proxy->options2 & PR_O2_EXP_INV)
			ret = !ret;

		set_server_check_status(&s->check, ret ? HCHK_STATUS_L7OKD : HCHK_STATUS_L7STS, status_msg);
		break;

	case PR_O2_EXP_STR:
	case PR_O2_EXP_RSTR:
		/* very simple response parser: ignore CR and only count consecutive LFs,
		 * stop with contentptr pointing to first char after the double CRLF or
		 * to '\0' if crlf < 2.
		 */
		crlf = 0;
		for (contentptr = s->check.bi->data; *contentptr; contentptr++) {
			if (crlf >= 2)
				break;
			if (*contentptr == '\r')
				continue;
			else if (*contentptr == '\n')
				crlf++;
			else
				crlf = 0;
		}

		/* Check that response contains a body... */
		if (crlf < 2) {
			if (!done)
				return 0;

			set_server_check_status(&s->check, HCHK_STATUS_L7RSP,
						"HTTP content check could not find a response body");
			return 1;
		}

		/* Check that response body is not empty... */
		if (*contentptr == '\0') {
			if (!done)
				return 0;

			set_server_check_status(&s->check, HCHK_STATUS_L7RSP,
						"HTTP content check found empty response body");
			return 1;
		}

		/* Check the response content against the supplied string
		 * or regex... */
		if ((s->proxy->options2 & PR_O2_EXP_TYPE) == PR_O2_EXP_STR)
			ret = strstr(contentptr, s->proxy->expect_str) != NULL;
		else
			ret = regexec(s->proxy->expect_regex, contentptr, MAX_MATCH, pmatch, 0) == 0;

		/* if we don't match, we may need to wait more */
		if (!ret && !done)
			return 0;

		if (ret) {
			/* content matched */
			if (s->proxy->options2 & PR_O2_EXP_INV)
				set_server_check_status(&s->check, HCHK_STATUS_L7RSP,
							"HTTP check matched unwanted content");
			else
				set_server_check_status(&s->check, HCHK_STATUS_L7OKD,
							"HTTP content check matched");
		}
		else {
			if (s->proxy->options2 & PR_O2_EXP_INV)
				set_server_check_status(&s->check, HCHK_STATUS_L7OKD,
							"HTTP check did not match unwanted content");
			else
				set_server_check_status(&s->check, HCHK_STATUS_L7RSP,
							"HTTP content check did not match");
		}
		break;
	}
	return 1;
}

/*
 * return the id of a step in a send/expect session
 */
static int tcpcheck_get_step_id(struct server *s)
{
	struct tcpcheck_rule *cur = NULL, *next = NULL;
	int i = 0;

	cur = s->check.last_started_step;

	/* no step => first step */
	if (cur == NULL)
		return 1;

	/* increment i until current step */
	list_for_each_entry(next, &s->proxy->tcpcheck_rules, list) {
		if (next->list.p == &cur->list)
			break;
		++i;
	}

	return i;
}

static void tcpcheck_main(struct connection *conn)
{
	char *contentptr;
	unsigned int contentlen;
	struct list *head = NULL;
	struct tcpcheck_rule *cur = NULL;
	int done = 0, ret = 0;

	struct check *check = conn->owner;
	struct server *s = check->server;
	struct task *t = check->task;

	/*
	 * don't do anything until the connection is established but if we're running
	 * first step which must be a connect
	 */
	if (check->current_step && (!(conn->flags & CO_FL_CONNECTED))) {
		/* update expire time, should be done by process_chk */
		/* we allow up to min(inter, timeout.connect) for a connection
		 * to establish but only when timeout.check is set
		 * as it may be to short for a full check otherwise
		 */
		while (tick_is_expired(t->expire, now_ms)) {
			int t_con;

			t_con = tick_add(t->expire, s->proxy->timeout.connect);
			t->expire = tick_add(t->expire, MS_TO_TICKS(check->inter));

			if (s->proxy->timeout.check)
				t->expire = tick_first(t->expire, t_con);
		}
		return;
	}

	/* here, we know that the connection is established */
	if (check->result != CHK_RES_UNKNOWN)
		goto out_end_tcpcheck;

	/* head is be the first element of the double chained list */
	head = &s->proxy->tcpcheck_rules;

	/* no step means first step
	 * initialisation */
	if (check->current_step == NULL) {
		check->bo->p = check->bo->data;
		check->bo->o = 0;
		check->bi->p = check->bi->data;
		check->bi->i = 0;
		cur = check->current_step = LIST_ELEM(head->n, struct tcpcheck_rule *, list);
		t->expire = tick_add(now_ms, MS_TO_TICKS(check->inter));
		if (s->proxy->timeout.check)
			t->expire = tick_add_ifset(now_ms, s->proxy->timeout.check);
	}
	/* keep on processing step */
	else {
		cur = check->current_step;
	}

	if (conn->flags & CO_FL_HANDSHAKE)
		return;

	/* It's only the rules which will enable send/recv */
	__conn_data_stop_both(conn);

	while (1) {
		/* we have to try to flush the output buffer before reading, at the end,
		 * or if we're about to send a string that does not fit in the remaining space.
		 */
		if (check->bo->o &&
		    (&cur->list == head ||
		     check->current_step->action != TCPCHK_ACT_SEND ||
		     check->current_step->string_len >= buffer_total_space(check->bo))) {

			if (conn->xprt->snd_buf(conn, check->bo, 0) <= 0) {
				if (conn->flags & CO_FL_ERROR) {
					chk_report_conn_err(conn, errno, 0);
					__conn_data_stop_both(conn);
					goto out_end_tcpcheck;
				}
				goto out_need_io;
			}
		}

		/* did we reach the end ? If so, let's check that everything was sent */
		if (&cur->list == head) {
			if (check->bo->o)
				goto out_need_io;
			break;
		}

		if (check->current_step->action == TCPCHK_ACT_CONNECT) {
			struct protocol *proto;
			struct xprt_ops *xprt;

			/* mark the step as started */
			check->last_started_step = check->current_step;
			/* first, shut existing connection */
			conn_force_close(conn);

			/* prepare new connection */
			/* initialization */
			conn_init(conn);
			conn_attach(conn, check, &check_conn_cb);
			conn->target = &s->obj_type;

			/* no client address */
			clear_addr(&conn->addr.from);

			if (is_addr(&s->check_common.addr))
				/* we'll connect to the check addr specified on the server */
				conn->addr.to = s->check_common.addr;
			else
				/* we'll connect to the addr on the server */
				conn->addr.to = s->addr;

			/* protocol */
			proto = protocol_by_family(conn->addr.to.ss_family);

			/* port */
			if (check->current_step->port)
				set_host_port(&conn->addr.to, check->current_step->port);
			else if (check->port)
				set_host_port(&conn->addr.to, check->port);

#ifdef USE_OPENSSL
			if (check->current_step->conn_opts & TCPCHK_OPT_SSL) {
				xprt = &ssl_sock;
			}
			else {
				xprt = &raw_sock;
			}
#else  /* USE_OPENSSL */
			xprt = &raw_sock;
#endif /* USE_OPENSSL */
			conn_prepare(conn, proto, xprt);

			ret = SN_ERR_INTERNAL;
			if (proto->connect)
				ret = proto->connect(conn, check->type, (check->type) ? 0 : 2);
			conn->flags |= CO_FL_WAKE_DATA;
			if (check->current_step->conn_opts & TCPCHK_OPT_SEND_PROXY) {
				conn->send_proxy_ofs = 1;
				conn->flags |= CO_FL_SEND_PROXY;
			}

			/* It can return one of :
			 *  - SN_ERR_NONE if everything's OK
			 *  - SN_ERR_SRVTO if there are no more servers
			 *  - SN_ERR_SRVCL if the connection was refused by the server
			 *  - SN_ERR_PRXCOND if the connection has been limited by the proxy (maxconn)
			 *  - SN_ERR_RESOURCE if a system resource is lacking (eg: fd limits, ports, ...)
			 *  - SN_ERR_INTERNAL for any other purely internal errors
			 * Additionnally, in the case of SN_ERR_RESOURCE, an emergency log will be emitted.
			 * Note that we try to prevent the network stack from sending the ACK during the
			 * connect() when a pure TCP check is used (without PROXY protocol).
			 */
			switch (ret) {
			case SN_ERR_NONE:
				/* we allow up to min(inter, timeout.connect) for a connection
				 * to establish but only when timeout.check is set
				 * as it may be to short for a full check otherwise
				 */
				t->expire = tick_add(now_ms, MS_TO_TICKS(check->inter));

				if (s->proxy->timeout.check && s->proxy->timeout.connect) {
					int t_con = tick_add(now_ms, s->proxy->timeout.connect);
					t->expire = tick_first(t->expire, t_con);
				}
				break;
			case SN_ERR_SRVTO: /* ETIMEDOUT */
			case SN_ERR_SRVCL: /* ECONNREFUSED, ENETUNREACH, ... */
				chunk_printf(&trash, "TCPCHK error establishing connection at step %d: %s",
						tcpcheck_get_step_id(s), strerror(errno));
				set_server_check_status(check, HCHK_STATUS_L4CON, trash.str);
				goto out_end_tcpcheck;
			case SN_ERR_PRXCOND:
			case SN_ERR_RESOURCE:
			case SN_ERR_INTERNAL:
				chunk_printf(&trash, "TCPCHK error establishing connection at step %d",
						tcpcheck_get_step_id(s));
				set_server_check_status(check, HCHK_STATUS_SOCKERR, trash.str);
				goto out_end_tcpcheck;
			}

			/* allow next rule */
			cur = (struct tcpcheck_rule *)cur->list.n;
			check->current_step = cur;

			/* don't do anything until the connection is established */
			if (!(conn->flags & CO_FL_CONNECTED)) {
				/* update expire time, should be done by process_chk */
				/* we allow up to min(inter, timeout.connect) for a connection
				 * to establish but only when timeout.check is set
				 * as it may be to short for a full check otherwise
				 */
				while (tick_is_expired(t->expire, now_ms)) {
					int t_con;

					t_con = tick_add(t->expire, s->proxy->timeout.connect);
					t->expire = tick_add(t->expire, MS_TO_TICKS(check->inter));

					if (s->proxy->timeout.check)
						t->expire = tick_first(t->expire, t_con);
				}
				return;
			}

		} /* end 'connect' */
		else if (check->current_step->action == TCPCHK_ACT_SEND) {
			/* mark the step as started */
			check->last_started_step = check->current_step;

			/* reset the read buffer */
			if (*check->bi->data != '\0') {
				*check->bi->data = '\0';
				check->bi->i = 0;
			}

			if (conn->flags & (CO_FL_SOCK_WR_SH | CO_FL_DATA_WR_SH)) {
				conn->flags |= CO_FL_ERROR;
				chk_report_conn_err(conn, 0, 0);
				goto out_end_tcpcheck;
			}

			if (check->current_step->string_len >= check->bo->size) {
				chunk_printf(&trash, "tcp-check send : string too large (%d) for buffer size (%d) at step %d",
					     check->current_step->string_len, check->bo->size,
					     tcpcheck_get_step_id(s));
				set_server_check_status(check, HCHK_STATUS_L7RSP, trash.str);
				goto out_end_tcpcheck;
			}

			/* do not try to send if there is no space */
			if (check->current_step->string_len >= buffer_total_space(check->bo))
				continue;

			bo_putblk(check->bo, check->current_step->string, check->current_step->string_len);
			*check->bo->p = '\0'; /* to make gdb output easier to read */

			/* go to next rule and try to send */
			cur = (struct tcpcheck_rule *)cur->list.n;
			check->current_step = cur;
		} /* end 'send' */
		else if (check->current_step->action == TCPCHK_ACT_EXPECT) {
			if (unlikely(check->result == CHK_RES_FAILED))
				goto out_end_tcpcheck;

			if (conn->xprt->rcv_buf(conn, check->bi, check->bi->size) <= 0) {
				if (conn->flags & (CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_DATA_RD_SH)) {
					done = 1;
					if ((conn->flags & CO_FL_ERROR) && !check->bi->i) {
						/* Report network errors only if we got no other data. Otherwise
						 * we'll let the upper layers decide whether the response is OK
						 * or not. It is very common that an RST sent by the server is
						 * reported as an error just after the last data chunk.
						 */
						chk_report_conn_err(conn, errno, 0);
						goto out_end_tcpcheck;
					}
				}
				else
					goto out_need_io;
			}

			/* mark the step as started */
			check->last_started_step = check->current_step;


			/* Intermediate or complete response received.
			 * Terminate string in check->bi->data buffer.
			 */
			if (check->bi->i < check->bi->size) {
				check->bi->data[check->bi->i] = '\0';
			}
			else {
				check->bi->data[check->bi->i - 1] = '\0';
				done = 1; /* buffer full, don't wait for more data */
			}

			contentptr = check->bi->data;
			contentlen = check->bi->i;

			/* Check that response body is not empty... */
			if (*contentptr == '\0') {
				if (!done)
					continue;

				/* empty response */
				chunk_printf(&trash, "TCPCHK got an empty response at step %d",
						tcpcheck_get_step_id(s));
				set_server_check_status(check, HCHK_STATUS_L7RSP, trash.str);

				goto out_end_tcpcheck;
			}

			if (!done && (cur->string != NULL) && (check->bi->i < cur->string_len) )
				continue; /* try to read more */

		tcpcheck_expect:
			if (cur->string != NULL)
				ret = my_memmem(contentptr, contentlen, cur->string, cur->string_len) != NULL;
			else if (cur->expect_regex != NULL)
				ret = regexec(cur->expect_regex, contentptr, MAX_MATCH, pmatch, 0) == 0;

			if (!ret && !done)
				continue; /* try to read more */

			/* matched */
			if (ret) {
				/* matched but we did not want to => ERROR */
				if (cur->inverse) {
					/* we were looking for a string */
					if (cur->string != NULL) {
						chunk_printf(&trash, "TCPCHK matched unwanted content '%s' at step %d",
								cur->string, tcpcheck_get_step_id(s));
					}
					else {
					/* we were looking for a regex */
						chunk_printf(&trash, "TCPCHK matched unwanted content (regex) at step %d",
								tcpcheck_get_step_id(s));
					}
					set_server_check_status(check, HCHK_STATUS_L7RSP, trash.str);
					goto out_end_tcpcheck;
				}
				/* matched and was supposed to => OK, next step */
				else {
					cur = (struct tcpcheck_rule*)cur->list.n;
					check->current_step = cur;
					if (check->current_step->action == TCPCHK_ACT_EXPECT)
						goto tcpcheck_expect;
					__conn_data_stop_recv(conn);
				}
			}
			else {
			/* not matched */
				/* not matched and was not supposed to => OK, next step */
				if (cur->inverse) {
					cur = (struct tcpcheck_rule*)cur->list.n;
					check->current_step = cur;
					if (check->current_step->action == TCPCHK_ACT_EXPECT)
						goto tcpcheck_expect;
					__conn_data_stop_recv(conn);
				}
				/* not matched but was supposed to => ERROR */
				else {
					/* we were looking for a string */
					if (cur->string != NULL) {
						chunk_printf(&trash, "TCPCHK did not match content '%s' at step %d",
								cur->string, tcpcheck_get_step_id(s));
					}
					else {
					/* we were looking for a regex */
						chunk_printf(&trash, "TCPCHK did not match content (regex) at step %d",
								tcpcheck_get_step_id(s));
					}
					set_server_check_status(check, HCHK_STATUS_L7RSP, trash.str);
					goto out_end_tcpcheck;
				}
			}
		} /* end expect */
	} /* end loop over double chained step list */

	set_server_check_status(check, HCHK_STATUS_L7OKD, "(tcp-check)");
	goto out_end_tcpcheck;

 out_need_io:
	if (check->bo->o)
		__conn_data_want_send(conn);

	if (check->current_step->action == TCPCHK_ACT_EXPECT)
		__conn_data_want_recv(conn);
	return;

 out_end_tcpcheck:
	/* collect possible new errors */
	if (conn->flags & CO_FL_ERROR)
		chk_report_conn_err(conn, 0, 0);

	/* cleanup before leaving */
	check->current_step = NULL;

	if (check->result == CHK_RES_FAILED)
		conn->flags |= CO_FL_ERROR;

	__conn_data_stop_both(conn);

	return;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
