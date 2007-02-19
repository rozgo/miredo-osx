/*
 * server.c - Handling of a single Teredo datagram (server-side).
 * $Id: server.c 1808 2006-10-26 19:34:40Z remi $
 */

/***********************************************************************
 *  Copyright © 2004-2006 Rémi Denis-Courmont.                         *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license.         *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <gettext.h>

#include <stddef.h>
#include <string.h> /* memcpy(), memset() */
#include <inttypes.h>
#include <limits.h>

#include <stdbool.h>
#include <errno.h> // errno
#include <stdio.h> // snprintf()
#include <stdlib.h>

#include <sys/types.h>
#include <unistd.h> // close()
#include <sys/socket.h>
#include <netinet/in.h> // struct in6_addr
#include <netinet/ip6.h> // struct ip6_hdr
#include <arpa/inet.h> // inet_ntop()
#include <netinet/icmp6.h>
#include <fcntl.h>
#include <pthread.h>
#include <syslog.h>

#include "server.h"
#include "v4global.h"
#include "checksum.h"
#include "teredo.h"
#include <sys/uio.h>
#include "teredo-udp.h"
#include "debug.h"

static pthread_mutex_t raw_mutex = PTHREAD_MUTEX_INITIALIZER;
static int raw_fd; // raw IPv6 socket
static unsigned raw_users = 0;

struct teredo_server
{
	pthread_t t1, t2;

	int fd_primary, fd_secondary; // UDP/IPv4 sockets

	/* These are all in network byte order (including MTU!!) */
	uint32_t server_ip, prefix, advLinkMTU;
};

/**
 * Sends a Teredo-encapsulated Router Advertisement.
 */
static bool
SendRA (const teredo_server *restrict s, const struct teredo_packet *p,
        const struct in6_addr *dest_ip6, bool secondary)
{
	const uint8_t *nonce;
	union teredo_addr *addr;
	struct iovec iov[3];
	struct teredo_simple_auth auth;
	struct teredo_orig_ind orig;
	struct
	{
		struct ip6_hdr            ip6;
		struct nd_router_advert   ra;
		struct nd_opt_prefix_info pi;
		struct nd_opt_mtu         mtu;
	} ra;

	// Authentification header
	// TODO: support for secure qualification
	iov[0].iov_base = &auth;

	nonce = p->auth_nonce;
	if (nonce != NULL)
	{
		//memset (&auth, 0, sizeof (auth));
		auth.hdr.hdr.zero = 0;
		auth.hdr.hdr.code = teredo_auth_hdr;
		auth.hdr.id_len = auth.hdr.au_len = 0;
		memcpy (&auth.nonce, nonce, 8);
		auth.confirmation = 0;

		iov[0].iov_len = 13;
	}
	else
		iov[0].iov_len = 0;

	// Origin indication header
	//memset (&orig, 0, sizeof (orig));
	iov[1].iov_base = &orig;
	iov[1].iov_len = 8;
	orig.hdr.zero = 0;
	orig.hdr.code = teredo_orig_ind;
	orig.orig_port = ~p->source_port; // obfuscate
	orig.orig_addr = ~p->source_ipv4; // obfuscate

	// IPv6 header
	memset (&ra, 0, sizeof (ra));
	iov[2].iov_base = &ra;
	iov[2].iov_len = sizeof (ra);

	ra.ip6.ip6_flow = htonl (0x60000000);
	ra.ip6.ip6_plen = htons (sizeof (ra) - sizeof (ra.ip6));
	ra.ip6.ip6_nxt = IPPROTO_ICMPV6;
	ra.ip6.ip6_hlim = 255;

	addr = (union teredo_addr *)&ra.ip6.ip6_src;
	addr->teredo.prefix = htonl (0xfe800000);
	//addr->teredo.server_ip = 0;
	addr->teredo.flags = htons (TEREDO_FLAG_CONE);
	addr->teredo.client_port = htons (IPPORT_TEREDO);
	addr->teredo.client_ip = ~s->server_ip;

	memcpy (&ra.ip6.ip6_dst, dest_ip6, sizeof (ra.ip6.ip6_dst));

	// ICMPv6: Router Advertisement
	ra.ra.nd_ra_type = ND_ROUTER_ADVERT;
	//ra.ra.nd_ra_code = 0;
	//ra.ra.nd_ra_cksum = 0;
	//ra.ra.nd_ra_curhoplimit = 0;
	//ra.ra.nd_ra_flags_reserved = 0;
	//ra.ra.nd_ra_router_lifetime = 0;
	//ra.ra.nd_ra_reachable = 0;
	ra.ra.nd_ra_retransmit = htonl (2000);

	// ICMPv6 option: Prefix information
	ra.pi.nd_opt_pi_type = ND_OPT_PREFIX_INFORMATION;
	ra.pi.nd_opt_pi_len = sizeof (ra.pi) >> 3;
	ra.pi.nd_opt_pi_prefix_len = 64;
	ra.pi.nd_opt_pi_flags_reserved = ND_OPT_PI_FLAG_AUTO;
	ra.pi.nd_opt_pi_valid_time = 0xffffffff;
	ra.pi.nd_opt_pi_preferred_time = 0xffffffff;
	addr = (union teredo_addr *)&ra.pi.nd_opt_pi_prefix;
	addr->teredo.prefix = s->prefix;
	addr->teredo.server_ip = s->server_ip;
	//memset (addr->ip6.s6_addr + 8, 0, 8);

	// ICMPv6 option : MTU
	ra.mtu.nd_opt_mtu_type = ND_OPT_MTU;
	ra.mtu.nd_opt_mtu_len = sizeof (ra.mtu) >> 3;
	//ra.mtu.nd_opt_mtu_reserved = 0;
	ra.mtu.nd_opt_mtu_mtu = s->advLinkMTU;

	// ICMPv6 checksum computation
	ra.ra.nd_ra_cksum = icmp6_checksum (&ra.ip6, (struct icmp6_hdr *)&ra.ra);

	if (IN6_IS_TEREDO_ADDR_CONE (dest_ip6))
		secondary = !secondary;

	return teredo_sendv (secondary ? s->fd_secondary : s->fd_primary,
	                     iov, 3, p->source_ipv4, p->source_port) > 0;
}

/**
 * Forwards a Teredo packet to a client
 */
static bool
teredo_forward_udp (int fd, const struct teredo_packet *packet,
                       bool insert_orig)
{
	struct teredo_orig_ind orig;
	struct iovec iov[2];
	uint32_t dest_ipv4;
	uint16_t dest_port;

	/* extract the IPv4 destination directly from the Teredo IPv6 destination
	   within the IPv6 header */
	memcpy (&dest_ipv4, packet->ip6 + 24 + 12, 4);
	dest_ipv4 = ~dest_ipv4;

	if (!is_ipv4_global_unicast (dest_ipv4))
		return 0; // ignore invalid client IP

	memcpy (&dest_port, packet->ip6 + 24 + 10, 2);
	dest_port = ~dest_port;

	// Origin indication header
	// if the Teredo server's address is ours
	// NOTE: I wonder in which legitimate case insert_orig might be
	// false... but the spec implies it could
	iov[0].iov_base = &orig;
	if (insert_orig)
	{
		iov[0].iov_len = sizeof (orig);
		orig.hdr.zero = 0;
		orig.hdr.code = teredo_orig_ind;
		orig.orig_port = ~packet->source_port; // obfuscate
		orig.orig_addr = ~packet->source_ipv4; // obfuscate
	}
	else
		iov[0].iov_len = 0;

	iov[1].iov_base = packet->ip6;
	iov[1].iov_len = packet->ip6_len;

	return teredo_sendv (fd, iov, 2, dest_ipv4, dest_port) > 0;
}


/**
 * Sends an IPv6 packet of *payload* length <plen> with a raw IPv6 socket.
 */
static bool
teredo_send_ipv6 (const void *p, size_t len)
{
	struct sockaddr_in6 dst;
	int tries, res;

	memset (&dst, 0, sizeof (dst));
	dst.sin6_family = AF_INET6;
#ifdef HAVE_SA_LEN
	dst.sin6_len = sizeof (dst);
#endif
	memcpy (&dst.sin6_addr, &((const struct ip6_hdr *)p)->ip6_dst,
	        sizeof (dst.sin6_addr));

	for (tries = 0; tries < 10; tries++)
	{
		res = sendto (raw_fd, p, len, 0,
		              (struct sockaddr *)&dst, sizeof (dst));
		if (res != -1)
			return res == (int)len;

		switch (errno)
		{
			case ENETUNREACH: /* ICMPv6 unreach no route */
			case EACCES: /* ICMPv6 unreach administravely prohibited */
			case EHOSTUNREACH: /* ICMPv6 unreach addres unreachable */
				               /* ICMPv6 time exceeded */
			case ECONNREFUSED: /* ICMPv6 unreach port unreachable */
			case EMSGSIZE: /* ICMPv6 packet too big */
#ifdef EPROTO
			case EPROTO: /* ICMPv6 param prob (and other errors) */
#endif
				break;

			default:
				return false;
		}
	}

	return false;
}


static const struct in6_addr in6addr_allrouters =
	{ { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x2 } } };

/**
 * Checks and handles an Teredo-encapsulated packet.
 * Thread-safety note: prefix and advLinkMTU might be changed by another
 * thread.
 * @return -1 in case of I/O error, -2 if the packet was discarded,
 * 1 if it was processed as a qualification probe,
 * 2 if it was processed as a request for direct IPv6 connectivity check,
 * 3 if it was forwarded over UDP/IPv4 (hole punching).
 */
static int
teredo_process_packet (const teredo_server *s, bool sec)
{
	const uint8_t *ptr;
	struct teredo_packet packet;
	size_t ip6len;
	struct ip6_hdr ip6;
	uint32_t myprefix;

	if (teredo_wait_recv (sec ? s->fd_secondary : s->fd_primary, &packet))
		return -1;

	/*
	 * Cases 1 to 3 are all about discarding packets,
	 * so their actual order do not matter.
	 * Similarly, cases 4 to 6 are all about processing the packet.
	 */
	// Teredo server case number 3
	if (!is_ipv4_global_unicast (packet.source_ipv4))
		return -2;

	// Check IPv6 packet (Teredo server case number 1)
	ptr = packet.ip6;
	ip6len = packet.ip6_len;

	if (ip6len < sizeof (ip6))
		return -2; // too small

	memcpy (&ip6, ptr, sizeof (ip6));
	ip6len -= sizeof (ip6);
	ptr += sizeof (ip6);

	if (((ip6.ip6_vfc >> 4) != 6)
	 || (ntohs (ip6.ip6_plen) != ip6len))
		return -2; // not an IPv6 packet

	// NOTE: ptr is not aligned => read single bytes only

	// Teredo server case number 2
	if (((ip6.ip6_nxt != IPPROTO_NONE) || (ip6len > 0)) // neither a bubble...
	 && (ip6.ip6_nxt != IPPROTO_ICMPV6)) // nor an ICMPv6 message
		return -2; // packet not allowed through server

	// Teredo server case number 3 was done above...

	// Teredo server case number 4
	if (IN6_IS_ADDR_LINKLOCAL (&ip6.ip6_src)
	 && IN6_ARE_ADDR_EQUAL (&in6addr_allrouters, &ip6.ip6_dst)
	 && (ip6.ip6_nxt == IPPROTO_ICMPV6)
	 && (ip6len >= sizeof (struct nd_router_solicit))
	 && (((const struct icmp6_hdr *)ptr)->icmp6_type == ND_ROUTER_SOLICIT))
		// sends a Router Advertisement
		return SendRA (s, &packet, &ip6.ip6_src, sec) ? 1 : -1;

	/* Secondary address is only meant for Router Solicitation */
	if (sec)
		return -2;

	/* Security fix: Prevent infinite UDP packet loop */
	if ((packet.source_ipv4 == s->server_ip)
	 && (packet.source_port == htons (3544)))
		return -2;

	myprefix = s->prefix;

	if (IN6_TEREDO_PREFIX (&ip6.ip6_src) == myprefix)
	{
		/** Source address is Teredo **/
		// Teredo server case number 5 (accept), otherwise 7 (discard)
		if (!IN6_MATCHES_TEREDO_CLIENT (&ip6.ip6_src, packet.source_ipv4,
		                                packet.source_port))
			return -2;

		/** Packet accepted for processing **/
		/*
		 * NOTE: Theoretically, we "should" accept ICMPv6 toward the
		 * server's own local-link address or the ip6-allrouters
		 * multicast address. In practice, it never happens.
		 */

		// Ensures destination is of global scope (ie 2000::/3)
		if ((ip6.ip6_dst.s6_addr[0] & 0xe0) != 0x20)
			return -2; // must not be forwarded over IPv6

		if (IN6_TEREDO_PREFIX (&ip6.ip6_dst) != myprefix)
			return teredo_send_ipv6 (packet.ip6, packet.ip6_len) ? 2 : -1;
	}
	else
	{
		/** Source address is NOT Teredo **/
		// Teredo server case number 6 (accept), otherwise 7 (discard)
		if ((IN6_TEREDO_PREFIX (&ip6.ip6_dst) != myprefix)
		 || (IN6_TEREDO_SERVER (&ip6.ip6_dst) != s->server_ip))
			return -2;

		/** Packet accepted for processing **/
	}

	/*
	 * While an explicit breakage of RFC 4380, we purposedly drop ICMPv6
	 * packets here, as these can have a large payload, and we don't want
	 * to be an open UDP relay. A conformant Teredo client/relay will never
	 * try to relay such a packet through a Teredo server anyway. Moreover,
	 * Microsoft Teredo servers also drop these packets.
	 *
	 * We still allow small ICMPv6 packets, as these could be used within
	 * a custom planned extension of the protocol. On the one hand, 128 bytes
	 * is big enough to transmit the IPv6 header, a 512-bits(!) hash, and 24
	 * of data. Currently, Miredo only uses a 128-bits hash. On the other
	 * hand, this is too small to hold application-layer packets such as
	 * G711 VoIPv6 packets (220 bytes, if I remember correctly).
	 */
	if ((ip6.ip6_nxt != IPPROTO_NONE) && (ntohs (ip6.ip6_plen) > 88))
		return -2;

	// Forwards packet over Teredo (destination is a Teredo IPv6 address)
	return teredo_forward_udp (s->fd_primary, &packet,
		IN6_TEREDO_SERVER (&ip6.ip6_dst) == s->server_ip) ? 3 : -1;
}


int teredo_server_check (char *errmsg, size_t len)
{
	int fd = socket (AF_INET6, SOCK_RAW, IPPROTO_RAW);

	if (fd >= 0)
	{
		close (fd);
		return 0;
	}

	snprintf (errmsg, len, _("Raw IPv6 socket not working: %s"),
	          strerror (errno));
	return -1;
}


static LIBTEREDO_NORETURN void *thread_primary (void *data)
{
	for (;;)
	{
		pthread_testcancel ();
		teredo_process_packet ((teredo_server *)data, false);
	}
}


static LIBTEREDO_NORETURN void *thread_secondary (void *data)
{
	for (;;)
	{
		pthread_testcancel ();
		teredo_process_packet ((teredo_server *)data, true);
	}
}


/**
 * Creates a Teredo server handler. You should then drop your
 * privileges and call teredo_server_start().
 *
 * @note Only one thread should use a given server handle at a time 
 *
 * @param ip1 server primary IPv4 address (network byte order),
 * @param ip2 server secondary IPv4 address (network byte order).
 *
 * @return NULL on error.
 */
teredo_server *teredo_server_create (uint32_t ip1, uint32_t ip2)
{
	(void)bindtextdomain (PACKAGE_NAME, LOCALEDIR);

	/* Initializes shared raw IPv6 socket */
	pthread_mutex_lock (&raw_mutex);
	if (raw_users == UINT_MAX) /* integer overflow */
	{
		/* piece of code that will probably never ever be executed */
		pthread_mutex_unlock (&raw_mutex);
		return NULL;
	}
	if (raw_users++ == 0)
	{
		raw_fd = socket (AF_INET6, SOCK_RAW, IPPROTO_RAW);
		if (raw_fd != -1)
		{
			int flags = fcntl (raw_fd, F_GETFL, 0);
			//shutdown (fd, SHUT_RD); -- won't work
			fcntl (raw_fd, F_SETFL,
			       O_NONBLOCK | ((flags != -1) ? flags : 0));
			fcntl (raw_fd, F_SETFD, FD_CLOEXEC);
		}
	}
	pthread_mutex_unlock (&raw_mutex);

	if (raw_fd == -1)
	{
		syslog (LOG_ERR, _("Raw IPv6 socket not working: %s"),
		        strerror (errno));
		return NULL;
	}

	/* Initializes exclusive UDP/IPv4 sockets */
	if (!is_ipv4_global_unicast (ip1) || !is_ipv4_global_unicast (ip2))
	{
		syslog (LOG_ERR, _("Teredo server UDP socket error: "
		        "Server IPv4 addresses must be global unicast."));
		return NULL;
	}

	teredo_server *s = malloc (sizeof (*s));

	if (s != NULL)
	{
		int fd;

		memset (s, 0, sizeof (s));
		s->server_ip = ip1;
		s->prefix = htonl (TEREDO_PREFIX);
		s->advLinkMTU = htonl (1280);

		fd = s->fd_primary = teredo_socket (ip1, htons (IPPORT_TEREDO));
		if (fd != -1)
		{
			fd = s->fd_secondary = teredo_socket (ip2, htons (IPPORT_TEREDO));
			if (fd != -1)
				return s;
			else
			{
				char str[INET_ADDRSTRLEN];

				inet_ntop (AF_INET, &ip2, str, sizeof (str));
				syslog (LOG_ERR, _("Error (%s): %s\n"), str,
				        strerror (errno));
			}

			teredo_close (s->fd_primary);
		}
		else
		{
			char str[INET_ADDRSTRLEN];

			inet_ntop (AF_INET, &ip1, str, sizeof (str));
			syslog (LOG_ERR, _("Error (%s): %s\n"), str,
			        strerror (errno));
		}

		free (s);
	}
	return NULL;
}


/**
 * Changes the Teredo prefix to be advertised by a Teredo server.
 * If not set, the internal default will be used.
 *
 * @note The default Teredo prefix is expected to change in a future
 * version of this library, when IANA assigns a permanent Teredo prefix.
 *
 * @param s server handler as returned from teredo_server_create(),
 * @param prefix 32-bits IPv6 address prefix (network byte order).
 *
 * @return 0 on success, -1 if the prefix is not acceptable.
 */
int teredo_server_set_prefix (teredo_server *s, uint32_t prefix)
{
	if (is_valid_teredo_prefix (prefix))
	{
		s->prefix = prefix;
		return 0;
	}
	return -1;
}


/**
 * Returns the Teredo prefix currently advertised by the server (in network
 * byte order).
 *
 * @param s server handler as returned from teredo_server_create(),
 */
uint32_t teredo_server_get_prefix (const teredo_server *s)
{
	return s->prefix;
}

/**
 * Changes the link MTU advertised by the Teredo server.
 * If not set, the internal default will be used (currently 1280 bytes).
 *
 * @param s server handler as returned from teredo_server_create(),
 * @param prefix MTU (in bytes) (host byte order).
 *
 * @return 0 on success, -1 if the MTU is not acceptable.
 */
int teredo_server_set_MTU (teredo_server *s, uint16_t mtu)
{
	if (mtu < 1280)
		return -1;

	s->advLinkMTU = htonl (mtu);
	return 0;
}


/**
 * Returns the link MTU currently advertised by the server in host byte order.
 *
 * @param s server handler as returned from teredo_server_create(),
 */
uint16_t teredo_server_get_MTU (const teredo_server *s)
{
	return ntohl (s->advLinkMTU);
}


/**
 * Starts a Teredo server processing.
 *
 * @param s server handler as returned from teredo_server_create(),
 *
 * @return 0 on success, -1 on error.
 */
int teredo_server_start (teredo_server *s)
{
	if (pthread_create (&s->t1, NULL, thread_primary, s) == 0)
	{
		if (pthread_create (&s->t2, NULL, thread_secondary, s) == 0)
			return 0;
		pthread_cancel (s->t1);
		pthread_join (s->t1, NULL);
	}

	return -1;
}


/**
 * Stops a Teredo server. Behavior is not defined if it was not started first.
 *
 * @param s server handler as returned from teredo_server_create(),
 */
void teredo_server_stop (teredo_server *s)
{
	pthread_cancel (s->t1);
	pthread_cancel (s->t2);
	pthread_join (s->t1, NULL);
	pthread_join (s->t2, NULL);
}


/**
 * Destroys a Teredo server handle. Behavior is not defined if the associated
 * server is currently running - you must stop it with teredo_server_stop()
 * first, if it is running.
 *
 * @param s server handler as returned from teredo_server_create(),
 */
void teredo_server_destroy (teredo_server *s)
{
	teredo_close (s->fd_primary);
	teredo_close (s->fd_secondary);
	free (s);

	pthread_mutex_lock (&raw_mutex);
	if (--raw_users == 0)
		close (raw_fd);
	pthread_mutex_unlock (&raw_mutex);
}
