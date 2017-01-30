/*	$OpenBSD$ */

/*
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
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/tree.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <err.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <imsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "log.h"
#include "netcfgd.h"

struct {
	pid_t			pid;
	int			fd;
	struct event		ev;
} kr_state;

void	kr_dispatch_msg(int, short, void *);
int	get_rtaddrs(int, struct sockaddr *, struct sockaddr **);
void	forward_v4proposal(struct rt_msghdr *, struct sockaddr **);
void	forward_v6proposal(struct rt_msghdr *, struct sockaddr **);

int
kr_init(void)
{
	int		opt = 0, rcvbuf, default_rcvbuf, rtfilter;
	socklen_t	optlen;

	if ((kr_state.fd = socket(AF_ROUTE,
	    SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, AF_INET)) == -1) {
		log_warn("kr_init: socket");
		return (-1);
	}

	/* not interested in my own messages */
	if (setsockopt(kr_state.fd, SOL_SOCKET, SO_USELOOPBACK,
	    &opt, sizeof(opt)) == -1)
		log_warn("kr_init: setsockopt");	/* not fatal */

	/* Only care about proposals. */
	rtfilter = ROUTE_FILTER(RTM_PROPOSAL);
	if (setsockopt(kr_state.fd, PF_ROUTE, ROUTE_MSGFILTER,
	    &rtfilter, sizeof(rtfilter)) == -1) {
		log_warn("setsockopt(ROUTE_MSGFILTER): %s", strerror(errno));
		return (-1);
	}

	/* grow receive buffer, don't wanna miss messages */
	optlen = sizeof(default_rcvbuf);
	if (getsockopt(kr_state.fd, SOL_SOCKET, SO_RCVBUF,
	    &default_rcvbuf, &optlen) == -1)
		log_warn("kr_init getsockopt SOL_SOCKET SO_RCVBUF");
	else
		for (rcvbuf = NETCFGD_MAX_RTSOCK_BUF;
		    rcvbuf > default_rcvbuf &&
		    setsockopt(kr_state.fd, SOL_SOCKET, SO_RCVBUF,
		    &rcvbuf, sizeof(rcvbuf)) == -1 && errno == ENOBUFS;
		    rcvbuf /= 2)
			;	/* nothing */

	kr_state.pid = getpid();

	event_set(&kr_state.ev, kr_state.fd, EV_READ | EV_PERSIST,
	    kr_dispatch_msg, NULL);
	event_add(&kr_state.ev, NULL);

	return (0);
}

/* ARGSUSED */
void
kr_dispatch_msg(int fd, short event, void *bula)
{
	char			 buf[NETCFGD_RT_BUF_SIZE];
	struct rt_msghdr	*rtm;
	struct sockaddr		*sa, *rti_info[RTAX_MAX];
	char			*next;
	ssize_t			 n;
	size_t			 len, offset;
	int			 v6;

	if ((n = read(kr_state.fd, &buf, sizeof(buf))) == -1) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		log_warn("dispatch_rtmsg: read error");
		event_loopexit(NULL);
		return;
	}

	if (n == 0) {
		log_warnx("routing socket closed");
		event_loopexit(NULL);
		return;
	}

	len = n;
	for (offset = 0; offset < len; offset += rtm->rtm_msglen) {
		next = buf + offset;
		rtm = (struct rt_msghdr *)next;
		if (len < offset + sizeof(u_short) ||
		    len < offset + rtm->rtm_msglen)
			fatalx("rtmsg_process: partial rtm in buffer");
		if (rtm->rtm_version != RTM_VERSION)
			continue;

		sa = (struct sockaddr *)(next + rtm->rtm_hdrlen);
		v6 = get_rtaddrs(rtm->rtm_addrs, sa, rti_info);

		switch (rtm->rtm_type) {
		case RTM_PROPOSAL:
			log_warnx("I see a %s RTM_PROPOSAL from %d!",
			    v6 ? "IPv6" : "IPv4", rtm->rtm_priority);
			if (v6)
				forward_v6proposal(rtm, rti_info);
			else
				forward_v4proposal(rtm, rti_info);
			break;
		default:
			/* ignore for now */
			break;
		}
	}

	return;
}

#define	ROUNDUP(a)	\
    (((a) & (sizeof(long) - 1)) ? (1 + ((a) | (sizeof(long) - 1))) : (a))

int
get_rtaddrs(int addrs, struct sockaddr *sa, struct sockaddr **rti_info)
{
	int	i, v6 = 1;

	for (i = 0; i < RTAX_MAX; i++) {
		if (addrs & (1 << i)) {
			rti_info[i] = sa;
			if (sa->sa_family == AF_INET)
				v6 = 0;
			sa = (struct sockaddr *)((char *)(sa) +
			    ROUNDUP(sa->sa_len));
		} else
			rti_info[i] = NULL;
	}
	return (v6);
}

void
forward_v4proposal(struct rt_msghdr *rtm, struct sockaddr **rti_info)
{
	struct imsg_v4proposal	 proposal;
	struct sockaddr_in	*sa_in;

	memset(&proposal, 0, sizeof(proposal));

	proposal.addrs = rtm->rtm_addrs;
	proposal.inits = rtm->rtm_inits;
	proposal.flags = rtm->rtm_flags;
	proposal.xid = rtm->rtm_seq;
	proposal.index = rtm->rtm_index;
	proposal.source = rtm->rtm_priority;

	if (proposal.inits & RTV_MTU) {
		proposal.mtu = rtm->rtm_rmx.rmx_mtu;
	}

	if (rti_info[RTAX_STATIC] != NULL) {
		struct sockaddr_rtstatic *rtstatic;
		rtstatic = (struct sockaddr_rtstatic *)rti_info[RTAX_STATIC];
		memcpy(&proposal.rtstatic, rtstatic->sr_static,
		    sizeof(proposal.rtstatic));
	}
	if (rti_info[RTAX_SEARCH] != NULL) {
		struct sockaddr_rtsearch *rtsearch;
		rtsearch = (struct sockaddr_rtsearch *)rti_info[RTAX_SEARCH];
		if (rtsearch->sr_family == AF_INET6)
			proposal.rtsearch_encoded = 1;
		memcpy(&proposal.rtsearch, rtsearch->sr_search,
		    sizeof(proposal.rtsearch));
	}
	if (rti_info[RTAX_GATEWAY] != NULL) {
		sa_in = (struct sockaddr_in *)rti_info[RTAX_GATEWAY];
		memcpy(&proposal.gateway, &sa_in->sin_addr,
		    sizeof(proposal.gateway));
	}
	if (rti_info[RTAX_IFA] != NULL) {
		sa_in = (struct sockaddr_in *)rti_info[RTAX_IFA];
		memcpy(&proposal.ifa, &sa_in->sin_addr,
		    sizeof(proposal.ifa));
	}
	if (rti_info[RTAX_NETMASK] != NULL) {
		sa_in = (struct sockaddr_in *)rti_info[RTAX_NETMASK];
		memcpy(&proposal.netmask, &sa_in->sin_addr,
		    sizeof(proposal.netmask));
	}
	if (rti_info[RTAX_DNS1] != NULL) {
		sa_in = (struct sockaddr_in *)rti_info[RTAX_DNS1];
		memcpy(&proposal.dns1, &sa_in->sin_addr,
		    sizeof(proposal.dns1));
	}
	if (rti_info[RTAX_DNS2] != NULL) {
		sa_in = (struct sockaddr_in *)rti_info[RTAX_DNS2];
		memcpy(&proposal.dns2, &sa_in->sin_addr,
		    sizeof(proposal.dns2));
	}
	if (rti_info[RTAX_DNS3] != NULL) {
		sa_in = (struct sockaddr_in *)rti_info[RTAX_DNS3];
		memcpy(&proposal.dns3, &sa_in->sin_addr,
		    sizeof(proposal.dns3));
	}
	if (rti_info[RTAX_DNS4] != NULL) {
		sa_in = (struct sockaddr_in *)rti_info[RTAX_DNS4];
		memcpy(&proposal.dns4, &sa_in->sin_addr,
		    sizeof(proposal.dns4));
	}

	main_imsg_compose_engine(IMSG_SEND_V4PROPOSAL, 0, &proposal,
	    sizeof(proposal));
}

void
forward_v6proposal(struct rt_msghdr *rtm, struct sockaddr **rti_info)
{
	struct imsg_v4proposal	 proposal;
	struct sockaddr_in6	*sa_in6;

	memset(&proposal, 0, sizeof(proposal));

	proposal.addrs = rtm->rtm_addrs;
	proposal.inits = rtm->rtm_inits;
	proposal.flags = rtm->rtm_flags;
	proposal.xid = rtm->rtm_seq;
	proposal.index = rtm->rtm_index;
	proposal.source = rtm->rtm_priority;

	if (proposal.inits & RTV_MTU) {
		proposal.mtu = rtm->rtm_rmx.rmx_mtu;
	}

	if (rti_info[RTAX_STATIC] != NULL) {
		struct sockaddr_rtstatic *rtstatic;
		rtstatic = (struct sockaddr_rtstatic *)rti_info[RTAX_STATIC];
		memcpy(&proposal.rtstatic, rtstatic->sr_static,
		    sizeof(proposal.rtstatic));
	}
	if (rti_info[RTAX_SEARCH] != NULL) {
		struct sockaddr_rtsearch *rtsearch;
		rtsearch = (struct sockaddr_rtsearch *)rti_info[RTAX_SEARCH];
		if (rtsearch->sr_family == AF_INET6)
			proposal.rtsearch_encoded = 1;
		memcpy(&proposal.rtsearch, rtsearch->sr_search,
		    sizeof(proposal.rtsearch));
	}
	if (rti_info[RTAX_GATEWAY] != NULL) {
		sa_in6 = (struct sockaddr_in6 *)rti_info[RTAX_GATEWAY];
		memcpy(&proposal.gateway, &sa_in6->sin6_addr,
		    sizeof(proposal.gateway));
	}
	if (rti_info[RTAX_IFA] != NULL) {
		sa_in6 = (struct sockaddr_in6 *)rti_info[RTAX_IFA];
		memcpy(&proposal.ifa, &sa_in6->sin6_addr,
		    sizeof(proposal.ifa));
	}
	if (rti_info[RTAX_NETMASK] != NULL) {
		sa_in6 = (struct sockaddr_in6 *)rti_info[RTAX_NETMASK];
		memcpy(&proposal.netmask, &sa_in6->sin6_addr,
		    sizeof(proposal.netmask));
	}
	if (rti_info[RTAX_DNS1] != NULL) {
		sa_in6 = (struct sockaddr_in6 *)rti_info[RTAX_DNS1];
		memcpy(&proposal.dns1, &sa_in6->sin6_addr,
		    sizeof(proposal.dns1));
	}
	if (rti_info[RTAX_DNS2] != NULL) {
		sa_in6 = (struct sockaddr_in6 *)rti_info[RTAX_DNS2];
		memcpy(&proposal.dns2, &sa_in6->sin6_addr,
		    sizeof(proposal.dns2));
	}
	if (rti_info[RTAX_DNS3] != NULL) {
		sa_in6 = (struct sockaddr_in6 *)rti_info[RTAX_DNS3];
		memcpy(&proposal.dns3, &sa_in6->sin6_addr,
		    sizeof(proposal.dns3));
	}
	if (rti_info[RTAX_DNS4] != NULL) {
		sa_in6 = (struct sockaddr_in6 *)rti_info[RTAX_DNS4];
		memcpy(&proposal.dns4, &sa_in6->sin6_addr,
		    sizeof(proposal.dns4));
	}

	main_imsg_compose_engine(IMSG_SEND_V6PROPOSAL, 0, &proposal,
	    sizeof(proposal));
}
