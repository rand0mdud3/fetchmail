/*
 * interface.c -- implements fetchmail 'interface' and 'monitor' commands
 *
 * This module was implemented by George M. Sipe <gsipe@mindspring.com>
 * or <gsipe@acm.org> and is:
 *
 *	Copyright (c) 1996,1997 by George M. Sipe - ALL RIGHTS RESERVED
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 2, or (at your option) any later version.
 */

#if defined(linux) && !defined(INET6)

#include "config.h"
#include <stdio.h>
#include <string.h>
#if defined(STDC_HEADERS)
#include <stdlib.h>
#endif
#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#endif
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include "config.h"
#include "fetchmail.h"
#include "i18n.h"

typedef struct {
	struct in_addr addr, dstaddr, netmask;
	int rx_packets, tx_packets;
} ifinfo_t;

struct interface_pair_s {
	struct in_addr interface_address;
	struct in_addr interface_mask;
} *interface_pair;

static char *netdevfmt;

void interface_init(void)
/* figure out which /roc/dev/net format to use */
{
    FILE *fp = fopen("/proc/sys/kernel/osrelease", "r");

    /* pre-linux-2.2 format -- transmit packet count in 8th field */
    netdevfmt = "%d %d %*d %*d %*d %d %*d %d %*d %*d %*d %*d %d";

    if (!fp)
	return;
    else
    {
	int major, minor;

	if (fscanf(fp, "%d.%d.%*d", &major, &minor) != 2)
	    return;

	if (major >= 2 && minor >= 2)
	    /* Linux 2.2 -- transmit packet count in 10th field */
	    netdevfmt = "%d %d %*d %*d %*d %d %*d %*d %*d %*d %d %*d %d";
    }
}

static int _get_ifinfo_(int socket_fd, FILE *stats_file, const char *ifname,
		ifinfo_t *ifinfo)
/* get active network interface information - return non-zero upon success */
{
	int namelen = strlen(ifname);
	struct ifreq request;
	char *cp, buffer[256];
	int found = 0;
	int counts[4];

	/* initialize result */
	memset((char *) ifinfo, 0, sizeof(ifinfo_t));

	/* get the packet I/O counts */
	while (fgets(buffer, sizeof(buffer) - 1, stats_file)) {
		for (cp = buffer; *cp && *cp == ' '; ++cp);
		if (!strncmp(cp, ifname, namelen) &&
				cp[namelen] == ':') {
			cp += namelen + 1;
			if (sscanf(cp, netdevfmt,
				   counts, counts+1, counts+2, 
				   counts+3,&found)>4) { /* found = dummy */
			        /* newer kernel with byte counts */
			        ifinfo->rx_packets=counts[1];
			        ifinfo->tx_packets=counts[3];
			} else {
			        /* older kernel, no byte counts */
			        ifinfo->rx_packets=counts[0];
			        ifinfo->tx_packets=counts[2];
			}
                        found = 1;
		}
	}
        if (!found) return (FALSE);

	/* see if the interface is up */
	strcpy(request.ifr_name, ifname);
	if (ioctl(socket_fd, SIOCGIFFLAGS, &request) < 0)
		return(FALSE);
	if (!(request.ifr_flags & IFF_RUNNING))
		return(FALSE);

	/* get the IP address */
	strcpy(request.ifr_name, ifname);
	if (ioctl(socket_fd, SIOCGIFADDR, &request) < 0)
		return(FALSE);
	ifinfo->addr = ((struct sockaddr_in *) (&request.ifr_addr))->sin_addr;

	/* get the PPP destination IP address */
	strcpy(request.ifr_name, ifname);
	if (ioctl(socket_fd, SIOCGIFDSTADDR, &request) >= 0)
		ifinfo->dstaddr = ((struct sockaddr_in *)
					(&request.ifr_dstaddr))->sin_addr;

	/* get the netmask */
	strcpy(request.ifr_name, ifname);
	if (ioctl(socket_fd, SIOCGIFNETMASK, &request) >= 0) {
          ifinfo->netmask = ((struct sockaddr_in *)
                             (&request.ifr_netmask))->sin_addr;
          return (TRUE);
        }

	return(FALSE);
}

static int get_ifinfo(const char *ifname, ifinfo_t *ifinfo)
{
	int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	FILE *stats_file = fopen("/proc/net/dev", "r");
	int result;

	if (socket_fd < 0 || !stats_file)
		result = FALSE;
	else
	{
	    char	*sp = strchr(ifname, '/');

	    if (sp)
		*sp = '\0';
	    result = _get_ifinfo_(socket_fd, stats_file, ifname, ifinfo);
	    if (sp)
		*sp = '/';
	}
	if (socket_fd >= 0)
		close(socket_fd);
	if (stats_file)
		fclose(stats_file);
	return(result);
}

#ifndef HAVE_INET_ATON
/*
 * Note: This is not a true replacement for inet_aton(), as it won't
 * do the right thing on "255.255.255.255" (which translates to -1 on
 * most machines).  Fortunately this code will be used only if you're
 * on an older Linux that lacks a real implementation.
 */
#ifdef HAVE_NETINET_IN_SYSTM_H
# include <sys/types.h>
# include <netinet/in_systm.h>
#endif

#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <string.h>

static int inet_aton(const char *cp, struct in_addr *inp) {
    long addr;

    addr = inet_addr(cp);
    if (addr == ((long) -1)) return 0;

    memcpy(inp, &addr, sizeof(addr));
    return 1;
}
#endif /* HAVE_INET_ATON */

void interface_parse(char *buf, struct hostdata *hp)
/* parse 'interface' specification */
{
	char *cp1, *cp2;

	hp->interface = xstrdup(buf);

	/* find and isolate just the IP address */
	if (!(cp1 = strchr(buf, '/')))
		(void) report(stderr, PS_SYNTAX, _("missing IP interface address"));
	*cp1++ = '\000';

	/* find and isolate just the netmask */
	if (!(cp2 = strchr(cp1, '/')))
		cp2 = "255.255.255.255";
	else
		*cp2++ = '\000';

	/* convert IP address and netmask */
	hp->interface_pair = (struct interface_pair_s *)xmalloc(sizeof(struct interface_pair_s));
	if (!inet_aton(cp1, &hp->interface_pair->interface_address))
		(void) report(stderr, PS_SYNTAX, _("invalid IP interface address"));
	if (!inet_aton(cp2, &hp->interface_pair->interface_mask))
		(void) report(stderr, PS_SYNTAX, _("invalid IP interface mask"));
	/* apply the mask now to the IP address (range) required */
	hp->interface_pair->interface_address.s_addr &=
		hp->interface_pair->interface_mask.s_addr;

	/* restore original interface string (for configuration dumper) */
	*--cp1 = '/';
	return;
}

void interface_note_activity(struct hostdata *hp)
/* save interface I/O counts */
{
	ifinfo_t ifinfo;
	struct query *ctl;

	/* if not monitoring link, all done */
	if (!hp->monitor)
		return;

	/* get the current I/O stats for the monitored link */
	if (get_ifinfo(hp->monitor, &ifinfo))
		/* update this and preceeding host entries using the link
		   (they were already set during this pass but the I/O
		   count has now changed and they need to be re-updated)
		*/
		for (ctl = querylist; ctl; ctl = ctl->next) {
			if (ctl->server.monitor && !strcmp(hp->monitor, ctl->server.monitor))
				ctl->server.monitor_io =
					ifinfo.rx_packets + ifinfo.tx_packets;
			/* do NOT update host entries following this one */
			if (&ctl->server == hp)
				break;
		}

#ifdef	ACTIVITY_DEBUG
	(void) report(stdout, 0, _("activity on %s -noted- as %d"), 
		hp->monitor, hp->monitor_io);
#endif
}

int interface_approve(struct hostdata *hp)
/* return TRUE if OK to poll, FALSE otherwise */
{
	ifinfo_t ifinfo;

	/* check interface IP address (range), if specified */
	if (hp->interface) {
		/* get interface info */
		if (!get_ifinfo(hp->interface, &ifinfo)) {
			(void) report(stdout, 0, _("skipping poll of %s, %s down"),
				hp->pollname, hp->interface);
			return(FALSE);
		}
		/* check the IP address (range) */
		if ((ifinfo.addr.s_addr &
				hp->interface_pair->interface_mask.s_addr) !=
				hp->interface_pair->interface_address.s_addr) {
			(void) report(stdout, 0,
				_("skipping poll of %s, %s IP address excluded"),
				hp->pollname, hp->interface);
			return(FALSE);
		}
	}

	/* if not monitoring link, all done */
	if (!hp->monitor)
		return(TRUE);

#ifdef	ACTIVITY_DEBUG
	(void) report(stdout, 0, _("activity on %s checked as %d"), 
		hp->monitor, hp->monitor_io);
#endif
	/* if monitoring, check link for activity if it is up */
	if (get_ifinfo(hp->monitor, &ifinfo) &&
			hp->monitor_io == ifinfo.rx_packets +
				ifinfo.tx_packets) {
		(void) report(stdout, 0, _("skipping poll of %s, %s inactive"),
			hp->pollname, hp->monitor);
		return(FALSE);
	}

#ifdef ACTIVITY_DEBUG
       error(0, 0, _("activity on %s was %d, is %d"),
             hp->monitor, hp->monitor_io,
             ifinfo.rx_packets + ifinfo.tx_packets);
#endif

	return(TRUE);
}
#endif /* defined(linux) && !defined(INET6) */
