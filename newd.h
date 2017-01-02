/*	$OpenBSD$	*/

/*
 * Copyright (c) YYYY YOUR NAME HERE <user@your.dom.ain>
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

#define CONF_FILE		"/etc/newd.conf"
#define	NEWD_SOCKET		"/var/run/newd.sock"
#define NEWD_USER		"_newd"

#define NEWD_MAXTEXT		256
#define NEWD_MAXGROUPNAME	16

static const char * const log_procnames[] = {
	"main",
	"frontend",
	"engine"
};

struct imsgev {
	struct imsgbuf		 ibuf;
	void			(*handler)(int, short, void *);
	struct event		 ev;
	void			*data;
	short			 events;
};

enum imsg_type {
	IMSG_NONE,
	IMSG_CTL_RELOAD,
	IMSG_CTL_LOG_VERBOSE,
	IMSG_RECONF_CONF,
	IMSG_RECONF_END
};

enum {
	PROC_MAIN,
	PROC_ENGINE,
	PROC_FRONTEND
} newd_process;

struct group {
	LIST_ENTRY(group)	 entry;
	char		name[NEWD_MAXGROUPNAME];
	int		yesno, group_yesno;
	int		integer, group_integer;
	int		v4_bits, group_v4_bits;
	int		v6_bits, group_v6_bits;
	struct in_addr	v4address, group_v4address;
	struct in6_addr	v6address, group_v6address;
	char		text[NEWD_MAXTEXT];
	char		group_text[NEWD_MAXTEXT];
};

struct newd_conf {
	char		*csock;
	u_int32_t	opts;
#define OPT_VERBOSE	0x00000001
#define OPT_VERBOSE2	0x00000002
#define OPT_NOACTION	0x00000004
	int		yesno, global_yesno;
	int		integer, global_integer;
	int		v4_bits, global_v4_bits;
	int		v6_bits, global_v6_bits;
	struct in_addr	v4address, global_v4address;
	struct in6_addr	v6address, global_v6address;
	char		text[NEWD_MAXTEXT];
	char		global_text[NEWD_MAXTEXT];
	LIST_HEAD(, group)	group_list;
};

/* newd.c */
void	main_imsg_compose_frontend(int, pid_t, void *, u_int16_t);
void	main_imsg_compose_engine(int, pid_t, void *, u_int16_t);
void	merge_config(struct newd_conf *, struct newd_conf *);
void	imsg_event_add(struct imsgev *);
int	imsg_compose_event(struct imsgev *, u_int16_t, u_int32_t, pid_t,
	    int, void *, u_int16_t);

/* printconf.c */
void	print_config(struct newd_conf *);

/* parse.y */
struct newd_conf	*parse_config(char *, int);
int			 cmdline_symset(char *);

