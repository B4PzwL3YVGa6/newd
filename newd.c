/*	$OpenBSD$	*/

/*
 * Copyright (c) 2005 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2004 Esben Norby <norby@openbsd.org>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "newd.h"
#include "frontend.h"
#include "engine.h"
#include "control.h"

__dead void	usage(void);
__dead void	main_shutdown(void);

void	main_sig_handler(int, short, void *);

static pid_t	start_child(int, char *, int, int, int, char *);

void	main_dispatch_frontend(int, short, void *);
void	main_dispatch_engine(int, short, void *);

static int	main_imsg_send_ipc_sockets(struct imsgbuf *, struct imsgbuf *);
static int     main_imsg_send_config(struct newd_conf *);

int	main_reload(void);
int	main_sendboth(enum imsg_type, void *, u_int16_t);
void	main_showinfo_ctl(struct imsg *);

struct newd_conf	*main_conf;
struct imsgev		*iev_frontend;
struct imsgev		*iev_engine;
char			*conffile;

pid_t	 frontend_pid;
pid_t	 engine_pid;

void
main_sig_handler(int sig, short event, void *arg)
{
	/*
	 * Normal signal handler rules don't apply because libevent
	 * decouples for us.
	 */

	switch (sig) {
	case SIGTERM:
	case SIGINT:
		main_shutdown();
	case SIGHUP:
		if (main_reload() == -1)
			log_warnx("configuration reload failed");
		else
			log_debug("configuration reloaded");
		break;
	default:
		fatalx("unexpected signal");
	}
}

__dead void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-dnv] [-f file] [-s socket]\n",
	    __progname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct event	 ev_sigint, ev_sigterm, ev_sighup;
	int		 ch, opts = 0;
	int		 debug = 0, engine_flag = 0, frontend_flag = 0;
	char		*sockname;
	char		*saved_argv0;
	int		 pipe_main2frontend[2];
	int		 pipe_main2engine[2];

	conffile = CONF_FILE;
	sockname = NEWD_SOCKET;

	log_init(1, LOG_DAEMON);	/* Log to stderr until daemonized. */
	log_verbose(1);

	saved_argv0 = argv[0];
	if (saved_argv0 == NULL)
		saved_argv0 = "newd";

	while ((ch = getopt(argc, argv, "dEFf:ns:v")) != -1) {
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'E':
			engine_flag = 1;
			break;
		case 'F':
			frontend_flag = 1;
			break;
		case 'f':
			conffile = optarg;
			break;
		case 'n':
			opts |= OPT_NOACTION;
			break;
		case 's':
			sockname = optarg;
			break;
		case 'v':
			if (opts & OPT_VERBOSE)
				opts |= OPT_VERBOSE2;
			opts |= OPT_VERBOSE;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;
	if (argc > 0 || (engine_flag && frontend_flag))
		usage();

	if (engine_flag)
		engine(debug, opts & OPT_VERBOSE);
	else if (frontend_flag)
		frontend(debug, opts & OPT_VERBOSE, sockname);

	/* parse config file */
	if ((main_conf = parse_config(conffile, opts)) == NULL) {
		exit(1);
	}

	if (main_conf->opts & OPT_NOACTION) {
		if (main_conf->opts & OPT_VERBOSE)
			print_config(main_conf);
		else
			fprintf(stderr, "configuration OK\n");
		exit(0);
	}

	/* Check for root privileges. */
	if (geteuid())
		errx(1, "need root privileges");

	/* Check for assigned daemon user */
	if (getpwnam(NEWD_USER) == NULL)
		errx(1, "unknown user %s", NEWD_USER);

	log_init(debug, LOG_DAEMON);
	log_verbose(main_conf->opts & OPT_VERBOSE);

	if (!debug)
		daemon(1, 0);

	log_info("startup");

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
	    PF_UNSPEC, pipe_main2frontend) == -1)
		fatal("main2frontend socketpair");
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
	    PF_UNSPEC, pipe_main2engine) == -1)
		fatal("main2engine socketpair");

	/* Start children. */
	engine_pid = start_child(PROC_ENGINE, saved_argv0, pipe_main2engine[1],
	    debug, opts & OPT_VERBOSE, NULL);
	frontend_pid = start_child(PROC_FRONTEND, saved_argv0,
	    pipe_main2frontend[1], debug, opts & OPT_VERBOSE, sockname);

	newd_process = PROC_MAIN;
	setproctitle(log_procnames[newd_process]);
	log_procinit(log_procnames[newd_process]);

	event_init();

	/* Setup signal handler. */
	signal_set(&ev_sigint, SIGINT, main_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, main_sig_handler, NULL);
	signal_set(&ev_sighup, SIGHUP, main_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal_add(&ev_sighup, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* Setup pipes to children. */

	if ((iev_frontend = malloc(sizeof(struct imsgev))) == NULL ||
	    (iev_engine = malloc(sizeof(struct imsgev))) == NULL)
		fatal(NULL);
	imsg_init(&iev_frontend->ibuf, pipe_main2frontend[0]);
	iev_frontend->handler = main_dispatch_frontend;
	imsg_init(&iev_engine->ibuf, pipe_main2engine[0]);
	iev_engine->handler = main_dispatch_engine;

	/* Setup event handlers for pipes to engine & frontend. */
	iev_frontend->events = EV_READ;
	event_set(&iev_frontend->ev, iev_frontend->ibuf.fd,
	    iev_frontend->events, iev_frontend->handler, iev_frontend);
	event_add(&iev_frontend->ev, NULL);

	iev_engine->events = EV_READ;
	event_set(&iev_engine->ev, iev_engine->ibuf.fd, iev_engine->events,
	    iev_engine->handler, iev_engine);
	event_add(&iev_engine->ev, NULL);

	if (main_imsg_send_ipc_sockets(&iev_frontend->ibuf, &iev_engine->ibuf))
		fatal("could not establish imsg links");
	main_imsg_send_config(main_conf);

	if (pledge("stdio sendfd", NULL) == -1)
		fatal("pledge");

	event_dispatch();

	main_shutdown();
	return (0);
}

__dead void
main_shutdown(void)
{
	pid_t	 pid;
	int	 status;

	/* Close pipes. */
	msgbuf_clear(&iev_frontend->ibuf.w);
	close(iev_frontend->ibuf.fd);
	msgbuf_clear(&iev_engine->ibuf.w);
	close(iev_engine->ibuf.fd);

	control_cleanup(main_conf->csock);
	config_clear(main_conf);

	log_debug("waiting for children to terminate");
	do {
		pid = wait(&status);
		if (pid == -1) {
			if (errno != EINTR && errno != ECHILD)
				fatal("wait");
		} else if (WIFSIGNALED(status))
			log_warnx("%s terminated; signal %d",
			    (pid == engine_pid) ? "engine" :
			    "frontend", WTERMSIG(status));
	} while (pid != -1 || (pid == -1 && errno == EINTR));

	free(iev_frontend);
	free(iev_engine);
	free(main_conf);

	log_info("terminating");
	exit(0);
}

static pid_t
start_child(int p, char *argv0, int fd, int debug, int verbose, char *sockname)
{
	char	*argv[7];
	int	 argc = 0;
	pid_t	 pid;

	switch (pid = fork()) {
	case -1:
		fatal("cannot fork");
	case 0:
		break;
	default:
		close(fd);
		return (pid);
	}

	if (dup2(fd, 3) == -1)
		fatal("cannot setup imsg fd");

	argv[argc++] = argv0;
	switch (p) {
	case PROC_MAIN:
		fatalx("Can not start main process");
	case PROC_ENGINE:
		argv[argc++] = "-E";
		break;
	case PROC_FRONTEND:
		argv[argc++] = "-F";
		break;
	}
	if (debug)
		argv[argc++] = "-d";
	if (verbose)
		argv[argc++] = "-v";
	if (sockname) {
		argv[argc++] = "-s";
		argv[argc++] = sockname;
	}
	argv[argc++] = NULL;

	execvp(argv0, argv);
	fatal("execvp");
}

void
main_dispatch_frontend(int fd, short event, void *bula)
{
	struct imsgev		*iev = bula;
	struct imsgbuf		*ibuf;
	struct imsg		 imsg;
	ssize_t			 n;
	int			 shut = 0, verbose;

	ibuf = &iev->ibuf;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("imsg_get");
		if (n == 0)	/* No more messages. */
			break;

		switch (imsg.hdr.type) {
		case IMSG_CTL_RELOAD:
			if (main_reload() == -1)
				log_warnx("configuration reload failed");
			else
				log_warnx("configuration reloaded");
			break;
		case IMSG_CTL_LOG_VERBOSE:
			/* Already checked by frontend. */
			memcpy(&verbose, imsg.data, sizeof(verbose));
			log_verbose(verbose);
			break;
		case IMSG_CTL_SHOW_MAIN_INFO:
			main_showinfo_ctl(&imsg);
			break;
		default:
			log_debug("main_dispatch_frontend: error handling "
			    "imsg %d", imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	if (!shut)
		imsg_event_add(iev);
	else {
		/* This pipe is dead. Remove its event handler */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

void
main_dispatch_engine(int fd, short event, void *bula)
{
	struct imsgev	*iev = bula;
	struct imsgbuf  *ibuf;
	struct imsg	 imsg;
	ssize_t		 n;
	int		 shut = 0;

	ibuf = &iev->ibuf;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("imsg_get");
		if (n == 0)	/* No more messages. */
			break;

		switch (imsg.hdr.type) {
		default:
			log_debug("main_dispatch_engine: error handling "
			    "imsg %d", imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	if (!shut)
		imsg_event_add(iev);
	else {
		/* This pipe is dead. Remove its event handler. */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

void
main_imsg_compose_frontend(int type, pid_t pid, void *data, u_int16_t datalen)
{
	if (iev_frontend)
		imsg_compose_event(iev_frontend, type, 0, pid, -1, data,
		    datalen);
}

void
main_imsg_compose_engine(int type, pid_t pid, void *data, u_int16_t datalen)
{
	if (iev_engine)
		imsg_compose_event(iev_engine, type, 0, pid, -1, data,
		    datalen);
}

void
imsg_event_add(struct imsgev *iev)
{
	iev->events = EV_READ;
	if (iev->ibuf.w.queued)
		iev->events |= EV_WRITE;

	event_del(&iev->ev);
	event_set(&iev->ev, iev->ibuf.fd, iev->events, iev->handler, iev);
	event_add(&iev->ev, NULL);
}

int
imsg_compose_event(struct imsgev *iev, u_int16_t type, u_int32_t peerid,
    pid_t pid, int fd, void *data, u_int16_t datalen)
{
	int	ret;

	if ((ret = imsg_compose(&iev->ibuf, type, peerid, pid, fd, data,
	    datalen)) != -1)
		imsg_event_add(iev);

	return (ret);
}

static int
main_imsg_send_ipc_sockets(struct imsgbuf *frontend_buf,
    struct imsgbuf *engine_buf)
{
	int pipe_frontend2engine[2];

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
	    PF_UNSPEC, pipe_frontend2engine) == -1)
		return (-1);

	if (imsg_compose(frontend_buf, IMSG_SOCKET_IPC, 0, 0,
	    pipe_frontend2engine[0], NULL, 0) == -1)
		return (-1);
	if (imsg_compose(engine_buf, IMSG_SOCKET_IPC, 0, 0,
	    pipe_frontend2engine[1], NULL, 0) == -1)
		return (-1);

	return (0);
}

int
main_reload(void)
{
	struct newd_conf *xconf;

	if ((xconf = parse_config(conffile, main_conf->opts)) == NULL)
		return (-1);

	if (main_imsg_send_config(xconf) == -1)
		return (-1);

	merge_config(main_conf, xconf);

	return (0);
}

int
main_imsg_send_config(struct newd_conf *xconf)
{
	struct group	 *g;

	/* Send fixed part of config to children. */
	if (main_sendboth(IMSG_RECONF_CONF, xconf, sizeof(*xconf)) == -1)
		return (-1);

	/* Send the group list to children. */
	LIST_FOREACH(g, &xconf->group_list, entry) {
		if (main_sendboth(IMSG_RECONF_GROUP, g, sizeof(*g)) == -1)
			return (-1);
	}

	/* Tell children the revised config is now complete. */
	if (main_sendboth(IMSG_RECONF_END, NULL, 0) == -1)
		return (-1);

	return (0);
}

int
main_sendboth(enum imsg_type type, void *buf, u_int16_t len)
{
	if (imsg_compose_event(iev_frontend, type, 0, 0, -1, buf, len) == -1)
		return (-1);
	if (imsg_compose_event(iev_engine, type, 0, 0, -1, buf, len) == -1)
		return (-1);
	return (0);
}

void
main_showinfo_ctl(struct imsg *imsg)
{
	struct ctl_main_info cmi;
	size_t n;

	switch (imsg->hdr.type) {
	case IMSG_CTL_SHOW_MAIN_INFO:
		memset(cmi.text, 0, sizeof(cmi.text));
		n = strlcpy(cmi.text, "I'm a little teapot.",
		    sizeof(cmi.text));
		if (n >= sizeof(cmi.text))
			log_debug("main_showinfo_ctl: I was cut off!");
		main_imsg_compose_frontend(IMSG_CTL_SHOW_MAIN_INFO,
		    imsg->hdr.pid, &cmi, sizeof(cmi));
		memset(cmi.text, 0, sizeof(cmi.text));
		n = strlcpy(cmi.text, "Full of sencha.",
		    sizeof(cmi.text));
		if (n >= sizeof(cmi.text))
			log_debug("main_showinfo_ctl: I was cut off!");
		main_imsg_compose_frontend(IMSG_CTL_SHOW_MAIN_INFO,
		    imsg->hdr.pid, &cmi, sizeof(cmi));
		main_imsg_compose_frontend(IMSG_CTL_END, imsg->hdr.pid, NULL,
		    0);
		break;
	default:
		log_debug("main_showinfo_ctl: error handling imsg");
		break;
	}
}

void
merge_config(struct newd_conf *conf, struct newd_conf *xconf)
{
	struct group	*g;

	conf->opts = xconf->opts;
	conf->yesno = xconf->yesno;
	conf->integer = xconf->integer;
	memcpy(conf->global_text, xconf->global_text,
	    sizeof(conf->global_text));

	/* Remove & discard existing groups. */
	while ((g = LIST_FIRST(&conf->group_list)) != NULL) {
		LIST_REMOVE(g, entry);
		free(g);
	}

	/* Add new groups. */
	while ((g = LIST_FIRST(&xconf->group_list)) != NULL) {
		LIST_REMOVE(g, entry);
		LIST_INSERT_HEAD(&conf->group_list, g, entry);
	}

	free(xconf);
}

struct newd_conf *
config_new_empty(void)
{
	struct newd_conf	*xconf;

	xconf = calloc(1, sizeof(*xconf));
	if (xconf == NULL)
		fatal(NULL);

	LIST_INIT(&xconf->group_list);

	return (xconf);
}

void
config_clear(struct newd_conf *conf)
{
	struct newd_conf	*xconf;

	/* Merge current config with an empty config. */
	xconf = config_new_empty();
	merge_config(conf, xconf);

	free(conf);
}
