/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        SMTP CHECK. Check an SMTP-server.
 *
 * Authors:     Jeremy Rumpf, <jrumpf@heavyload.net>
 *              Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>

#include "check_smtp.h"
#include "logger.h"
#include "ipwrapper.h"
#include "utils.h"
#include "parser.h"
#if !HAVE_DECL_SOCK_CLOEXEC
#include "old_socket.h"
#endif
#include "layer4.h"
#include "smtp.h"

static conn_opts_t* default_co;	/* Default conn_opts for SMTP_CHECK */
static conn_opts_t *sav_co;	/* Saved conn_opts while host{} block processed */

static int smtp_connect_thread(thread_t *);
static int smtp_final(thread_t *thread, int error, const char *format, ...)
	 __attribute__ ((format (printf, 3, 4)));

/*
 * Used as a callback from free_list() to free all
 * the list elements in smtp_checker->host before we
 * free smtp_checker itself.
 */
static void
smtp_free_host(void *data)
{
	FREE(data);
}

/* Used as a callback from the checker api, queue_checker(),
 * to free up a checker entry and all its associated data.
 */
static void
free_smtp_check(void *data)
{
	smtp_checker_t *smtp_checker = CHECKER_DATA(data);
	free_list(&smtp_checker->host);
	FREE(smtp_checker->helo_name);
	FREE(smtp_checker);
	FREE(data);
}

/*
 * Callback for whenever we've been requested to dump our
 * configuration.
 */
static void
dump_smtp_check(FILE *fp, void *data)
{
	checker_t *checker = data;
	smtp_checker_t *smtp_checker = checker->data;

	conf_write(fp, "   Keepalive method = SMTP_CHECK");
	conf_write(fp, "   helo = %s", smtp_checker->helo_name);
	dump_checker_opts(fp, checker);

	if (smtp_checker->host) {
		conf_write(fp, "   Host list");
		dump_list(fp, smtp_checker->host);
	}
}

static bool
smtp_check_compare(void *a, void *b)
{
	smtp_checker_t *old = CHECKER_DATA(a);
	smtp_checker_t *new = CHECKER_DATA(b);
	size_t n;
	conn_opts_t *h1, *h2;

	if (strcmp(old->helo_name, new->helo_name) != 0)
		return false;
	if (!compare_conn_opts(CHECKER_CO(a), CHECKER_CO(b)))
		return false;
	if (LIST_SIZE(old->host) != LIST_SIZE(new->host))
		return false;
	for (n = 0; n < LIST_SIZE(new->host); n++) {
		h1 = (conn_opts_t *)list_element(old->host, n);
		h2 = (conn_opts_t *)list_element(new->host, n);
		if (!compare_conn_opts(h1, h2)) {
			return false;
		}
	}

	return true;
}

/*
 * Callback for whenever an SMTP_CHECK keyword is encountered
 * in the config file.
 */
static void
smtp_check_handler(__attribute__((unused)) vector_t *strvec)
{
	smtp_checker_t *smtp_checker = (smtp_checker_t *)MALLOC(sizeof(smtp_checker_t));

	/* We keep a copy of the default settings for completing incomplete settings */
	default_co = (conn_opts_t*)MALLOC(sizeof(conn_opts_t));

	/* Have the checker queue code put our checker into the checkers_queue list. */
	queue_checker(free_smtp_check, dump_smtp_check, smtp_connect_thread,
		      smtp_check_compare, smtp_checker, default_co);

	/* Set an empty conn_opts for any connection configured */
	((checker_t *)checkers_queue->tail->data)->co = (conn_opts_t*)MALLOC(sizeof(conn_opts_t));

	/*
	 * Last, allocate the list that will hold all the per host
	 * configuration structures. We already have the "default host"
	 * in our checker->co.
	 * If there are additional "host" sections in the config, they will
	 * be used instead of the default, but all the uninitialized options
	 * of those hosts will be set to the default's values.
	 */
	smtp_checker->host = alloc_list(smtp_free_host, dump_connection_opts);
}

static void
smtp_check_end_handler(void)
{
	smtp_checker_t *smtp_checker = CHECKER_GET();
	conn_opts_t *co = CHECKER_GET_CO();

	if (!smtp_checker->helo_name) {
		smtp_checker->helo_name = (char *)MALLOC(strlen(SMTP_DEFAULT_HELO) + 1);
		strcpy(smtp_checker->helo_name, SMTP_DEFAULT_HELO);
	}

	/* If any connection component has been configured, we want to add it to the host list */
	if (co->dst.ss_family != AF_UNSPEC ||
	    (co->dst.ss_family == AF_UNSPEC && ((struct sockaddr_in *)&co->dst)->sin_port) ||
	    co->bindto.ss_family != AF_UNSPEC ||
	    (co->bindto.ss_family == AF_UNSPEC && ((struct sockaddr_in *)&co->bindto)->sin_port) ||
	    co->bind_if[0] ||
#ifdef _WITH_SO_MARK_
	    co->fwmark ||
#endif
	    co->connection_to) {
		/* Set any necessary defaults */
		if (co->dst.ss_family == AF_UNSPEC) {
			if (((struct sockaddr_in *)&co->dst)->sin_port) {
				uint16_t saved_port = ((struct sockaddr_in *)&co->dst)->sin_port;
				co->dst = default_co->dst;
				checker_set_dst_port(&co->dst, saved_port);
			}
			else
				co->dst = default_co->dst;
		}
		if (!co->connection_to)
			co->connection_to = 5 * TIMER_HZ;

		if (!check_conn_opts(co)) {
			dequeue_new_checker();
			FREE(co);
		} else
			list_add(smtp_checker->host, co);
	}
	else
		FREE(co);
	CHECKER_GET_CO() = NULL;

	/* If there was no host{} section, add a single host to the list */
	if (LIST_ISEMPTY(smtp_checker->host)) {
		list_add(smtp_checker->host, default_co);
	} else
		FREE(default_co);
	default_co = NULL;
}

/* Callback for "host" keyword */
static void
smtp_host_handler(__attribute__((unused)) vector_t *strvec)
{
	checker_t *checker = CHECKER_GET_CURRENT();

	/* save the main conn_opts_t and set a new default for the host */
	sav_co = checker->co;
	checker->co = (conn_opts_t*)MALLOC(sizeof(conn_opts_t));
	memcpy(checker->co, default_co, sizeof(*default_co));

	log_message(LOG_INFO, "The SMTP_CHECK host block is deprecated. Please define additional checkers.");
}

static void
smtp_host_end_handler(void)
{
	checker_t *checker = CHECKER_GET_CURRENT();
	smtp_checker_t *smtp_checker = (smtp_checker_t *)checker->data;

	if (!check_conn_opts(checker->co))
		FREE(checker->co);
	else
		list_add(smtp_checker->host, checker->co);

	checker->co = sav_co;
}

/* "helo_name" keyword */
static void
smtp_helo_name_handler(vector_t *strvec)
{
	smtp_checker_t *smtp_checker = CHECKER_GET();
	if (smtp_checker->helo_name)
		FREE(smtp_checker->helo_name);
	smtp_checker->helo_name = CHECKER_VALUE_STRING(strvec);
}

/* Config callback installer */
void
install_smtp_check_keyword(void)
{
	/*
	 * Notify the config log parser that we need to be notified via
	 * callbacks when the following keywords are encountered in the
	 * keepalive.conf file.
	 */
	install_keyword("SMTP_CHECK", &smtp_check_handler);
	install_sublevel();
	install_keyword("helo_name", &smtp_helo_name_handler);

	install_checker_common_keywords(true);

	/*
	 * The host list feature is deprecated. It makes config fussy by
	 * adding another nesting level and is excessive since it is possible
	 * to attach multiple checkers to a RS.
	 * So these keywords below are kept for compatibility with users'
	 * existing configs.
	 */
	install_keyword("host", &smtp_host_handler);
	install_sublevel();
	install_checker_common_keywords(true);
	install_sublevel_end_handler(smtp_host_end_handler);
	install_sublevel_end();

	install_sublevel_end_handler(&smtp_check_end_handler);
	install_sublevel_end();
}

/*
 * Final handler. Determines if we need a retry or not.
 * Also has to make a decision if we need to bring the resulting
 * service down in case of error.
 */
static int
smtp_final(thread_t *thread, int error, const char *format, ...)
{
	checker_t *checker = THREAD_ARG(thread);
	smtp_checker_t *smtp_checker = CHECKER_ARG(checker);
	char error_buff[512];
	char smtp_buff[542];
	va_list varg_list;
	bool checker_was_up;
	bool rs_was_alive;

	/* Error or no error we should always have to close the socket */
	close(thread->u.fd);

	/* If we're here, an attempt HAS been made already for the current host */
	checker->retry_it++;

	if (error) {
		/* Always syslog the error when the real server is up */
		if (checker->is_up) {
			if (format != NULL) {
				/* prepend format with the "SMTP_CHECK " string */
				strncpy(error_buff, "SMTP_CHECK ", sizeof(error_buff) - 1);
				strncat(error_buff, format, sizeof(error_buff) - 11 - 1);

				va_start(varg_list, format);
				vlog_message(LOG_INFO, error_buff, varg_list);
				va_end(varg_list);
			} else {
				log_message(LOG_INFO, "SMTP_CHECK Unknown error");
			}
		}

		/*
		 * If we still have retries left, try this host again by
		 * scheduling the main thread to check it again after the
		 * configured backoff delay. Otherwise down the RS.
		 */
		if (checker->retry_it < checker->retry) {
			thread_add_timer(thread->master, smtp_connect_thread, checker,
					 checker->delay_before_retry);
			return 0;
		}

		/*
		 * No more retries, pull the real server from the virtual server.
		 * Only smtp_alert if it wasn't previously down. It should
		 * be noted that smtp_alert makes a copy of the string arguments, so
		 * we don't have to keep them statically allocated.
		 */
		if (checker->is_up || !checker->has_run) {
			checker_was_up = checker->is_up;
			rs_was_alive = checker->rs->alive;
			update_svr_checker_state(DOWN, checker);
			if (checker->rs->smtp_alert && checker_was_up &&
			    (rs_was_alive != checker->rs->alive || !global_data->no_checker_emails)) {
				if (format != NULL) {
					snprintf(error_buff, sizeof(error_buff), "=> CHECK failed on service : %s <=", format);
					va_start(varg_list, format);
					vsnprintf(smtp_buff, sizeof(smtp_buff), error_buff, varg_list);
					va_end(varg_list);
				} else
					strncpy(smtp_buff, "=> CHECK failed on service <=", sizeof(smtp_buff));

				smtp_buff[sizeof(smtp_buff) - 1] = '\0';
				smtp_alert(SMTP_MSG_RS, checker, NULL, smtp_buff);
			}
		}

		/* Reset everything back to the first host in the list */
		checker->retry_it = 0;
		smtp_checker->host_ctr = 0;

		/* Reschedule the main thread using the configured delay loop */;
		thread_add_timer(thread->master, smtp_connect_thread, checker, checker->delay_loop);

		return 0;
	}

	/*
	 * Ok this host was successful, increment to the next host in the list
	 * and reset the retry_it counter. We'll then reschedule the main thread again.
	 * If host_ptr exceeds the end of the list, http_main_thread will
	 * take note and bring up the real server as well as inject the delay_loop.
	 */
	checker->retry_it = 0;
	smtp_checker->host_ctr++;

	thread_add_timer(thread->master, smtp_connect_thread, checker, 1);
	return 0;
}

/*
 * Zeros out the rx/tx buffer
 */
static void
smtp_clear_buff(thread_t *thread)
{
	checker_t *checker = THREAD_ARG(thread);
	smtp_checker_t *smtp_checker = CHECKER_ARG(checker);
	memset(smtp_checker->buff, 0, SMTP_BUFF_MAX);
	smtp_checker->buff_ctr = 0;
}

/*
 * One thing to note here is we do a very cheap check for a newline.
 * We could receive two lines (with two newline characters) in a
 * single packet, but we don't care. We are only looking at the
 * SMTP response codes at the beginning anyway.
 */
static int
smtp_get_line_cb(thread_t *thread)
{
	checker_t *checker = THREAD_ARG(thread);
	smtp_checker_t *smtp_checker = CHECKER_ARG(checker);
	conn_opts_t *smtp_host = smtp_checker->host_ptr;
	unsigned x;
	ssize_t r;

	/* Handle read timeout */
	if (thread->type == THREAD_READ_TIMEOUT) {
		smtp_final(thread, 1, "Read timeout from server %s"
				    , FMT_SMTP_RS(smtp_host));
		return 0;
	}

	/* wrap the buffer, if full, by clearing it */
	if (smtp_checker->buff_ctr > SMTP_BUFF_MAX) {
		log_message(LOG_INFO, "SMTP_CHECK Buffer overflow reading from server %s. "
				      "Increase SMTP_BUFF_MAX in smtp_check.h"
				    , FMT_SMTP_RS(smtp_host));
		smtp_clear_buff(thread);
	}

	/* read the data */
	r = read(thread->u.fd, smtp_checker->buff + smtp_checker->buff_ctr,
		 SMTP_BUFF_MAX - smtp_checker->buff_ctr);

	if (r == -1 && (errno == EAGAIN || errno == EINTR)) {
		thread_add_read(thread->master, smtp_get_line_cb, checker,
				thread->u.fd, smtp_host->connection_to);
		return 0;
	} else if (r > 0)
		smtp_checker->buff_ctr += (size_t)r;

	/* check if we have a newline, if so, callback */
	for (x = 0; x < SMTP_BUFF_MAX; x++) {
		if (smtp_checker->buff[x] == '\n') {
			smtp_checker->buff[SMTP_BUFF_MAX - 1] = '\0';

			DBG("SMTP_CHECK %s < %s"
			    , FMT_SMTP_RS(smtp_host)
			    , smtp_checker->buff);

			(smtp_checker->buff_cb)(thread);

			return 0;
		}
	}

	/*
	 * If the connection was closed or there was
	 * some sort of error, notify smtp_final()
	 */
	if (r <= 0) {
		smtp_final(thread, 1, "Read failure from server %s"
				     , FMT_SMTP_RS(smtp_host));
		return 0;
	}

	/*
	 * Last case, we haven't read enough data yet
	 * to pull a newline. Schedule ourselves for
	 * another round.
	 */
	thread_add_read(thread->master, smtp_get_line_cb, checker,
			thread->u.fd, smtp_host->connection_to);
	return 0;
}

/*
 * Ok a caller has asked us to asyncronously schedule a single line
 * to be received from the server. They have also passed us a call back
 * function that we'll call once we have the newline. If something bad
 * happens, the caller assumes we'll pass the error off to smtp_final(),
 * which will either down the real server or schedule a retry. The
 * function smtp_get_line_cb is what does the dirty work since the
 * sceduler can only accept a single *thread argument.
 */
static void
smtp_get_line(thread_t *thread, int (*callback) (thread_t *))
{
	checker_t *checker = THREAD_ARG(thread);
	smtp_checker_t *smtp_checker = CHECKER_ARG(checker);
	conn_opts_t *smtp_host = smtp_checker->host_ptr;

	/* clear the buffer */
	smtp_clear_buff(thread);

	/* set the callback */
	smtp_checker->buff_cb = callback;

	/* schedule the I/O with our helper function  */
	thread_add_read(thread->master, smtp_get_line_cb, checker,
		thread->u.fd, smtp_host->connection_to);
	thread_del_write(thread);
	return;
}

/*
 * The scheduler function that puts the data out on the wire.
 * All our data will fit into one packet, so we only check if
 * the current write would block or not. If it wants to block,
 * we'll return to the scheduler and try again later.
 */
static int
smtp_put_line_cb(thread_t *thread)
{
	checker_t *checker = THREAD_ARG(thread);
	smtp_checker_t *smtp_checker = CHECKER_ARG(checker);
	conn_opts_t *smtp_host = smtp_checker->host_ptr;
	ssize_t w;


	/* Handle read timeout */
	if (thread->type == THREAD_WRITE_TIMEOUT) {
		smtp_final(thread, 1, "Write timeout to server %s"
				     , FMT_SMTP_RS(smtp_host));
		return 0;
	}

	/* write the data */
	w = write(thread->u.fd, smtp_checker->buff, smtp_checker->buff_ctr);

	if (w == -1 && (errno == EAGAIN || errno == EINTR)) {
		thread_add_write(thread->master, smtp_put_line_cb, checker,
				 thread->u.fd, smtp_host->connection_to);
		return 0;
	}

	DBG("SMTP_CHECK %s > %s"
	    , FMT_SMTP_RS(smtp_host)
	    , smtp_checker->buff);

	/*
	 * If the connection was closed or there was
	 * some sort of error, notify smtp_final()
	 */
	if (w <= 0) {
		smtp_final(thread, 1, "Write failure to server %s"
				     , FMT_SMTP_RS(smtp_host));
		return 0;
	}

	/* Execute the callback */
	(smtp_checker->buff_cb)(thread);
	return 0;
}

/*
 * This is the same as smtp_get_line() except that we're sending a
 * line of data instead of receiving one.
 */
static void
smtp_put_line(thread_t *thread, int (*callback) (thread_t *))
{
	checker_t *checker = THREAD_ARG(thread);
	smtp_checker_t *smtp_checker = CHECKER_ARG(checker);
	conn_opts_t *smtp_host = smtp_checker->host_ptr;

	smtp_checker->buff[SMTP_BUFF_MAX - 1] = '\0';
	smtp_checker->buff_ctr = strlen(smtp_checker->buff);

	/* set the callback */
	smtp_checker->buff_cb = callback;

	/* schedule the I/O with our helper function  */
	thread_add_write(thread->master, smtp_put_line_cb, checker,
			 thread->u.fd, smtp_host->connection_to);
	thread_del_read(thread);
	return;
}

/*
 * Ok, our goal here is to snag the status code out of the
 * buffer and return it as an integer. If it's not legible,
 * return -1.
 */
static int
smtp_get_status(thread_t *thread)
{
	checker_t *checker = THREAD_ARG(thread);
	smtp_checker_t *smtp_checker = CHECKER_ARG(checker);
	char *buff = smtp_checker->buff;
	int status;
	char *endptr;

	status = strtoul(buff, &endptr, 10);
	if (endptr - buff != 3 ||
	    (*endptr && *endptr != ' '))
		return -1;

	return status;
}

/*
 * We have a connected socket and are ready to begin
 * the conversation. This function schedules itself to
 * be called via callbacks and tracking state in
 * smtp_checker->state. Upon first calling, smtp_checker->state
 * should be set to SMTP_START.
 */
static int
smtp_engine_thread(thread_t *thread)
{
	checker_t *checker = THREAD_ARG(thread);
	smtp_checker_t *smtp_checker = CHECKER_ARG(checker);
	conn_opts_t *smtp_host = smtp_checker->host_ptr;

	switch (smtp_checker->state) {

		/* First step, schedule to receive the greeting banner */
		case SMTP_START:
			/*
			 * Ok, if smtp_get_line schedules us back, we will
			 * have data to analyze. Otherwise, smtp_get_line
			 * will defer directly to smtp_final.
			 */
			smtp_checker->state = SMTP_HAVE_BANNER;
			smtp_get_line(thread, smtp_engine_thread);
			return 0;
			break;

		/* Second step, analyze banner, send HELO */
		case SMTP_HAVE_BANNER:
			/* Check for "220 some.mailserver.com" in the greeting */
			if (smtp_get_status(thread) != 220) {
				smtp_final(thread, 1, "Bad greeting banner from server %s"
						     , FMT_SMTP_RS(smtp_host));
			} else {
				/*
				 * Schedule to send the HELO, smtp_put_line will
				 * defer directly to smtp_final on error.
				 */
				smtp_checker->state = SMTP_SENT_HELO;
				snprintf(smtp_checker->buff, SMTP_BUFF_MAX, "HELO %s\r\n",
					 smtp_checker->helo_name);
				smtp_put_line(thread, smtp_engine_thread);
			}
			break;

		/* Third step, schedule to read the HELO response */
		case SMTP_SENT_HELO:
			smtp_checker->state = SMTP_RECV_HELO;
			smtp_get_line(thread, smtp_engine_thread);
			break;

		/* Fourth step, analyze HELO return, send QUIT */
		case SMTP_RECV_HELO:
			/* Check for "250 Please to meet you..." */
			if (smtp_get_status(thread) != 250) {
				smtp_final(thread, 1, "Bad HELO response from server %s"
						     , FMT_SMTP_RS(smtp_host));
			} else {
				smtp_checker->state = SMTP_SENT_QUIT;
				snprintf(smtp_checker->buff, SMTP_BUFF_MAX, "QUIT\r\n");
				smtp_put_line(thread, smtp_engine_thread);
			}
			break;

		/* Fifth step, schedule to receive QUIT confirmation */
		case SMTP_SENT_QUIT:
			smtp_checker->state = SMTP_RECV_QUIT;
			smtp_get_line(thread, smtp_engine_thread);
			break;

		/* Sixth step, wrap up success to smtp_final */
		case SMTP_RECV_QUIT:
			smtp_final(thread, 0, NULL);
			break;

		default:
			/* We shouldn't be here */
			smtp_final(thread, 1, "Unknown smtp engine state encountered");
			break;
	}

	return 0;
}

/*
 * Second step in the process. Here we'll see if the connection
 * to the host we're checking was successful or not.
 */
static int
smtp_check_thread(thread_t *thread)
{
	checker_t *checker = THREAD_ARG(thread);
	smtp_checker_t *smtp_checker = CHECKER_ARG(checker);
	conn_opts_t *smtp_host = smtp_checker->host_ptr;
	int status;

	status = tcp_socket_state(thread, smtp_check_thread);
	switch (status) {
		case connect_error:
			smtp_final(thread, 1, "Error connecting to server %s"
					     , FMT_SMTP_RS(smtp_host));
			break;

		case connect_timeout:
			smtp_final(thread, 1, "Connection timeout to server %s"
					     , FMT_SMTP_RS(smtp_host));
			break;

		case connect_success:
			DBG("SMTP_CHECK Remote SMTP server %s connected"
			    , FMT_SMTP_RS(smtp_host));

			/* Enter the engine at SMTP_START */
			smtp_checker->state = SMTP_START;
			smtp_engine_thread(thread);
			break;

		default:
			/* we shouldn't be here */
			smtp_final(thread, 1, "Unknown connection error to server %s"
					     , FMT_SMTP_RS(smtp_host));
			break;
	}

	return 0;
}

/*
 * This is the main thread, where all the action starts.
 * When the check daemon comes up, it goes down the checkers_queue
 * and launches a thread for each checker that got registered.
 * This is the callback/event function for that initial thread.
 *
 * It should be noted that we ARE responsible for sceduling
 * ourselves to run again. It doesn't have to be right here,
 * but eventually has to happen.
 */
static int
smtp_connect_thread(thread_t *thread)
{
	checker_t *checker = THREAD_ARG(thread);
	smtp_checker_t *smtp_checker = CHECKER_ARG(checker);
	conn_opts_t *smtp_host;
	enum connect_result status;
	int sd;
	bool checker_was_up;
	bool rs_was_alive;

	/* Let's review our data structures.
	 *
	 * Thread is the structure used by the sceduler
	 * for sceduling many types of events. thread->arg in this
	 * case points to a checker structure. The checker
	 * structure holds data about the vs and rs configurations
	 * as well as the delay loop, etc. Each real server
	 * defined in the keepalived.conf will more than likely have
	 * a checker structure assigned to it. Each checker structure
	 * has a data element that is meant to hold per checker
	 * configurations. So thread->arg(checker)->data points to
	 * a smtp_checker structure. In the smtp_checker structure
	 * we hold global configuration data for the smtp check.
	 * Smtp_checker has a list of per host (smtp_host) configuration
	 * data in smtp_checker->host.
	 *
	 * So this whole thing looks like this:
	 * thread->arg(checker)->data(smtp_checker)->host(smtp_host)
	 *
	 * To make life simple, we'll break the structures out so
	 * that "checker" always points to the current checker structure,
	 * "smtp_checker" points to the current smtp_checker structure,
	 * and "smtp_host" points to the current smtp_host structure.
	 */

	/*
	 * If we're disabled, we'll do nothing at all.
	 * But we still have to register ourselves again so
	 * we don't fall of the face of the earth.
	 */
	if (!checker->enabled) {
		thread_add_timer(thread->master, smtp_connect_thread, checker,
				 checker->delay_loop);
		return 0;
	}

	/*
	 * Set the internal host pointer to the host that we'll be
	 * working on. If it's NULL, we've successfully tested all hosts.
	 * We'll bring the service up (if it's not already), reset the host list,
	 * and insert the delay loop. When we get scheduled again the host list
	 * will be reset and we will continue on checking them one by one.
	 */
	if ((smtp_checker->host_ptr = list_element(smtp_checker->host, smtp_checker->host_ctr)) == NULL) {
		if (!checker->is_up || !checker->has_run) {
			log_message(LOG_INFO, "Remote SMTP server %s succeed on service."
					    , FMT_CHK(checker));

			checker_was_up = checker->is_up;
			rs_was_alive = checker->rs->alive;
			update_svr_checker_state(UP, checker);
			if (checker->rs->smtp_alert && !checker_was_up &&
			    (rs_was_alive != checker->rs->alive || !global_data->no_checker_emails))
				smtp_alert(SMTP_MSG_RS, checker, NULL,
					   "=> CHECK succeed on service <=");
		}

		checker->retry_it = 0;
		smtp_checker->host_ctr = 0;
		smtp_checker->host_ptr = list_element(smtp_checker->host, 0);

		thread_add_timer(thread->master, smtp_connect_thread, checker, checker->delay_loop);
		return 0;
	}

	smtp_host = smtp_checker->host_ptr;

	/* Create the socket, failing here should be an oddity */
	if ((sd = socket(smtp_host->dst.ss_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP)) == -1) {
		log_message(LOG_INFO, "SMTP_CHECK connection failed to create socket. Rescheduling.");
		thread_add_timer(thread->master, smtp_connect_thread, checker,
				 checker->delay_loop);
		return 0;
	}

#if !HAVE_DECL_SOCK_NONBLOCK
	if (set_sock_flags(sd, F_SETFL, O_NONBLOCK))
		log_message(LOG_INFO, "Unable to set NONBLOCK on smtp socket - %s (%d)", strerror(errno), errno);
#endif

#if !HAVE_DECL_SOCK_CLOEXEC
	if (set_sock_flags(sd, F_SETFD, FD_CLOEXEC))
		log_message(LOG_INFO, "Unable to set CLOEXEC on smtp socket - %s (%d)", strerror(errno), errno);
#endif

	status = tcp_bind_connect(sd, smtp_host);

	/* handle tcp connection status & register callback the next setp in the process */
	if(tcp_connection_state(sd, status, thread, smtp_check_thread, smtp_host->connection_to)) {
		close(sd);
		log_message(LOG_INFO, "SMTP_CHECK socket bind failed. Rescheduling.");
		thread_add_timer(thread->master, smtp_connect_thread, checker,
			checker->delay_loop);
	}

	return 0;
}

#ifdef _TIMER_DEBUG_
void
print_check_smtp_addresses(void)
{
	log_message(LOG_INFO, "Address of dump_smtp_check() is 0x%p", dump_smtp_check);
	log_message(LOG_INFO, "Address of smtp_check_thread() is 0x%p", smtp_check_thread);
	log_message(LOG_INFO, "Address of smtp_connect_thread() is 0x%p", smtp_connect_thread);
	log_message(LOG_INFO, "Address of smtp_get_line_cb() is 0x%p", smtp_get_line_cb);
	log_message(LOG_INFO, "Address of smtp_put_line_cb() is 0x%p", smtp_put_line_cb);
}
#endif
