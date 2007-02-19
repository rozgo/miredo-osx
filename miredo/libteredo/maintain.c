/*
 * maintain.c - Teredo client qualification & maintenance
 * $Id: maintain.c 1892 2006-12-31 11:13:50Z remi $
 *
 * See "Teredo: Tunneling IPv6 over UDP through NATs"
 * for more information
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

#include <string.h> /* memcmp() */
#include <assert.h>

#include <stdbool.h>
#include <inttypes.h>
#include <time.h> /* clock_gettime() */

#include <sys/types.h>
#include <unistd.h> /* sysconf() */
#include <netinet/in.h> /* struct in6_addr */
#include <netdb.h> /* getaddrinfo(), gai_strerror() */
#include <syslog.h>
#include <stdlib.h> /* malloc(), free() */
#include <errno.h> /* EINTR */
#include <pthread.h>
#include <arpa/nameser.h>
#include <resolv.h> /* res_init() */

#include "teredo.h"
#include "teredo-udp.h"
#include "packets.h"

#include "security.h"
#include "maintain.h"
#include "v4global.h" // is_ipv4_global_unicast()
#include "debug.h"

static inline void gettime (struct timespec *now)
{
#if (_POSIX_CLOCK_SELECTION - 0 >= 0) && (_POSIX_MONOTONIC_CLOCK - 0 >= 0)
	if (clock_gettime (CLOCK_MONOTONIC, now) == 0)
		return;
#else
# define pthread_condattr_setclock( a, c ) (((c) != CLOCK_REALTIME) ? EINVAL : 0)
# ifndef CLOCK_MONOTONIC
#  define CLOCK_MONOTONIC CLOCK_REALTIME
# endif
# warning Monotonic clock is needed for proper Teredo maintenance!
#endif
	clock_gettime (CLOCK_REALTIME, now);
}


struct teredo_maintenance
{
	pthread_t thread;
	pthread_mutex_t outer;
	pthread_mutex_t inner;
	pthread_cond_t received;
	pthread_cond_t processed;

	const teredo_packet *incoming;

	int fd;
	struct
	{
		teredo_state state;
		teredo_state_cb cb;
		void *opaque;
	} state;
	char *server;
	char *server2;
};


static int getipv4byname (const char *restrict name, uint32_t *restrict ipv4)
{
	struct addrinfo hints =
	{
		.ai_family = AF_INET,
		.ai_socktype = SOCK_DGRAM
	}, *res;
	int val;

	val = getaddrinfo (name, NULL, &hints, &res);
	if (val)
		return val;

	memcpy (ipv4, &((const struct sockaddr_in *)(res->ai_addr))->sin_addr, 4);
	freeaddrinfo (res);

	return 0;
}


/**
 * Resolve Teredo server addresses.
 *
 * @return 0 on success, or an error value as defined for getaddrinfo().
 */
static int resolveServerIP (const char *server, uint32_t *restrict ip,
                            const char *server2, uint32_t *restrict ip2)
{
	int val;

	/* Connectivity might have been reconfigured (DHCP...), and our DNS
	 * servers might no longer be those they were when the program started.
	 * As such, we call res_init() to re-read /etc/resolv.conf.
	 */
	res_init ();

	val = getipv4byname (server, ip);
	if (val)
		return val;

	if ((server2 == NULL) || getipv4byname (server2, ip2) || (ip2 == ip))
		/*
		 * NOTE:
		 * While not specified anywhere, Windows XP/2003 seems to always
		 * use the "next" IPv4 address as the secondary address.
		 * We use as default, or as a replacement in case of error.
		 */
		*ip2 = htonl (ntohl (*ip) + 1);

	return 0;
}


/**
 * Checks and parses a received Router Advertisement.
 *
 * @return true if successful.
 */
static bool
maintenance_recv (const teredo_packet *restrict packet, uint32_t server_ip,
                  uint8_t *restrict nonce, bool cone, uint16_t *restrict mtu,
                  union teredo_addr *restrict newaddr)
{
	assert (packet->auth_nonce != NULL);

	if (memcmp (packet->auth_nonce, nonce, 8))
		return false;

	/* TODO: fail instead of ignoring the packet? */
	if (packet->auth_conf_byte)
	{
		syslog (LOG_ERR, _("Authentication with server failed."));
		return false;
	}

	if (teredo_parse_ra (packet, newaddr, cone, mtu)
	/* TODO: try to work-around incorrect server IP */
	 || (newaddr->teredo.server_ip != server_ip))
		return false;

	/* Valid router advertisement received! */
	return true;
}


/**
 * Waits until the clock reaches deadline or a RS packet is received.
 * @return 0 if a packet was received, ETIMEDOUT if deadline was reached.
 */
static int wait_reply (teredo_maintenance *restrict m,
                       const struct timespec *restrict deadline)
{
	assert (m->incoming == NULL);

	/* Ignore EINTR */
	for (;;)
	{
		int val = pthread_cond_timedwait (&m->received, &m->inner, deadline);

		switch (val)
		{
			case 0:
				if (m->incoming == NULL) // spurious wakeup
					continue;
				/* fall through */
			case ETIMEDOUT:
				return val;
		}
	}
	return 0; // dead code
}


/**
 * Waits until the clock reaches deadline and ignore any RS packet received
 * in the mean time.
 */
static void wait_reply_ignore (teredo_maintenance *restrict m,
                               const struct timespec *restrict deadline)
{
	while (wait_reply (m, deadline) == 0)
	{
		m->incoming = NULL;
		pthread_cond_signal (&m->processed);
	}
}


/**
 * Make sure ts is in the future. If not, set it to the current time.
 * @return false if (*ts) was changed, true otherwise.
 */
static bool
checkTimeDrift (struct timespec *ts)
{
	struct timespec now;
	gettime (&now);

	if ((now.tv_sec > ts->tv_sec)
	 || ((now.tv_sec == ts->tv_sec) && (now.tv_nsec > ts->tv_nsec)))
	{
		/* process stopped, CPU starved, or (ACPI, APM, etc) suspend */
		syslog (LOG_WARNING, _("Too much time drift. Resynchronizing."));
		memcpy (ts, &now, sizeof (*ts));
		return false;
	}
	return true;
}


static void
cleanup_unlock (void *o)
{
	(void)pthread_mutex_unlock ((pthread_mutex_t *)o);
}


/*
 * NOTE:
 * We purposedly don't implement Teredo interval determination because
 * it makes NAT binding maintenance more brittle than it already is.
 * Interval determination is not required for compliance by the way.
 */
#define SERVER_PING_DELAY 30

/* TODO: allow modification of these values ? */
static unsigned QualificationTimeOut = 4; // seconds
static unsigned QualificationRetries = 3;

static unsigned RestartDelay = 100; // seconds

/**
 * Teredo client maintenance procedure
 */
static inline LIBTEREDO_NORETURN
void maintenance_thread (teredo_maintenance *m)
{
	struct timespec deadline = { 0, 0 };
	teredo_state *c_state = &m->state.state;
	uint32_t server_ip = 0, server_ip2 = 0;
	unsigned count = 0;
	enum
	{
		QUALIFIED,
		PROBE_RESTRICT,
		PROBE_SYMMETRIC
	} state = PROBE_RESTRICT;
	enum
	{
		TERR_NONE,
		TERR_BLACKHOLE,
		TERR_SYMM
	} last_error = TERR_NONE;

	pthread_mutex_lock (&m->inner);

	/*
	 * Qualification/maintenance procedure
	 */
	pthread_cleanup_push (cleanup_unlock, &m->inner);
	for (;;)
	{
		assert (c_state->up == (state == QUALIFIED));

		/* Resolve server IPv4 addresses */
		while (server_ip == 0)
		{
			/* FIXME: mutex kept while resolving - very bad */
			int val = resolveServerIP (m->server, &server_ip,
			                           m->server2, &server_ip2);

			gettime (&deadline);

			if (val)
			{
				/* DNS resolution failed */
				syslog (LOG_ERR,
				        _("Cannot resolve Teredo server address \"%s\": %s"),
				        m->server, gai_strerror (val));

				/* wait some time before next resolution attempt */
				deadline.tv_sec += RestartDelay;
				wait_reply_ignore (m, &deadline);
			}
			else
			{
				/* DNS resolution succeeded */
				if (!is_ipv4_global_unicast (server_ip)
				|| !is_ipv4_global_unicast (server_ip2))
					syslog (LOG_WARNING, _("Server has a non global IPv4 address. "
					                       "It will most likely not work."));
	
				/* Tells Teredo client about the new server's IP */
				assert (!c_state->up);
				c_state->addr.teredo.server_ip = server_ip;
				m->state.cb (c_state, m->state.opaque);
			}
		}

		/* SEND ROUTER SOLICATION */
		do
			deadline.tv_sec += QualificationTimeOut;
		while (!checkTimeDrift (&deadline));

		uint8_t nonce[8];
		uint32_t dst = (state == PROBE_SYMMETRIC) ? server_ip2 : server_ip;
		teredo_get_nonce (deadline.tv_sec, dst, htons (IPPORT_TEREDO), nonce);

		if (state == PROBE_RESTRICT)
		{
			/*
			 * Send a cone probe with an incorrect nonce - so that we won't
			 * process any would-be reply. This helps detects some symmetric
			 * NATs that would otherwise be seen as restricted NATs.
			 */
			uint8_t bad_nonce[8];
			for (unsigned i = 0; i < sizeof (nonce); i++)
				bad_nonce[i] = nonce[i] ^ 0x96;

			teredo_send_rs (m->fd, server_ip, bad_nonce, true);
		}

		teredo_send_rs (m->fd, dst, nonce, false);

		int val = 0;
		union teredo_addr newaddr;
		uint16_t mtu = 1280;

		/* RECEIVE ROUTER ADVERTISEMENT */
		for (;;)
		{
			val = wait_reply (m, &deadline);
			if (val)
				break; // time out

			/* check received packet */
			bool ok;
			ok = maintenance_recv (m->incoming, server_ip,
			                       nonce, false, &mtu, &newaddr);
			m->incoming = NULL;

			pthread_cond_signal (&m->processed);
			if (ok)
				break;
		}

		unsigned delay = 0;

		/* UPDATE FINITE STATE MACHINE */
		if (val /* == ETIMEDOUT */)
		{
			/* no response */
			count++;

			if (count >= QualificationRetries)
			{
				if (state == QUALIFIED)
				{
					syslog (LOG_NOTICE, _("Lost Teredo connectivity"));
					c_state->up = false;
					m->state.cb (c_state, m->state.opaque);
					server_ip = 0;
				}

				count = 0;
				/* No response from server */
				if (last_error != TERR_BLACKHOLE)
				{
					syslog (LOG_INFO, _("No reply from Teredo server"));
					last_error = TERR_BLACKHOLE;
				}
				/* Wait some time before retrying */
				state = PROBE_RESTRICT;
				delay = RestartDelay;
			}
		}
		else
		/* RA received and parsed succesfully */
		if (state == PROBE_RESTRICT)
		{
			state = PROBE_SYMMETRIC;
			memcpy (&c_state->addr, &newaddr, sizeof (c_state->addr));
			gettime (&deadline);
		}
		else
		{
			count = 0;

			if ((c_state->addr.teredo.client_port != newaddr.teredo.client_port)
			 || (c_state->addr.teredo.client_ip != newaddr.teredo.client_ip))
			{
				if (state == PROBE_SYMMETRIC)
				{
					// Symmetric NAT failure
					if (last_error != TERR_SYMM)
					{
						last_error = TERR_SYMM;
						syslog (LOG_ERR,
						        _("Unsupported symmetric NAT detected."));
					}
					delay = RestartDelay; // Wait some time before retry
				}
				else
				{
					// External mapping changed! Reset everything.
					c_state->up = false;
					m->state.cb (c_state, m->state.opaque);
					gettime (&deadline);
				}

				state = PROBE_RESTRICT;
			}
			else
			{
				if (state != QUALIFIED)
				{
					syslog (LOG_INFO, _("Qualified (NAT type: %s)"),
					        _("restricted"));
					state = QUALIFIED;
				}

				if ((!c_state->up)
				 || memcmp (&c_state->addr, &newaddr, sizeof (c_state->addr))
				 || (c_state->mtu != mtu))
				{
					memcpy (&c_state->addr, &newaddr, sizeof (c_state->addr));
					c_state->mtu = mtu;
					c_state->up = true;

					syslog (LOG_NOTICE, _("New Teredo address/MTU"));
					m->state.cb (c_state, m->state.opaque);
				}

				/* Success: schedule next NAT binding maintenance */
				last_error = TERR_NONE;
				delay = SERVER_PING_DELAY;
			}
		}

		/* WAIT UNTIL NEXT SOLICITATION */
		/* TODO: watch for new interface events
		 * (netlink on Linux, PF_ROUTE on BSD) */
		if (delay)
		{
			deadline.tv_sec -= QualificationTimeOut;
			deadline.tv_sec += delay;
			wait_reply_ignore (m, &deadline);
		}
	}
	/* dead code */
	pthread_cleanup_pop (1);
}


static LIBTEREDO_NORETURN void *do_maintenance (void *opaque)
{
	maintenance_thread ((teredo_maintenance *)opaque);
}

/**
 * Creates and starts a Teredo client maintenance procedure thread.
 *
 * @return NULL on error.
 */
teredo_maintenance *
teredo_maintenance_start (int fd, teredo_state_cb cb, void *opaque,
                          const char *s1, const char *s2)
{
	teredo_maintenance *m = (teredo_maintenance *)malloc (sizeof (*m));

	if (m == NULL)
		return NULL;

	memset (m, 0, sizeof (*m));
	m->fd = fd;
	m->state.cb = cb;
	m->state.opaque = opaque;
	m->server = strdup (s1);

	if (m->server == NULL)
	{
		free (m);
		return NULL;
	}

	m->server2 = (s2 != NULL) ? strdup (s2) : NULL;
	if ((s2 != NULL) != (m->server2 != NULL))
		goto error;
	else
	{
		pthread_condattr_t attr;

		pthread_condattr_init (&attr);
		(void)pthread_condattr_setclock (&attr, CLOCK_MONOTONIC);
		/* EINVAL: CLOCK_MONOTONIC unknown */

		pthread_cond_init (&m->received, &attr);
		pthread_condattr_destroy (&attr);
	}

	pthread_cond_init (&m->processed, NULL);
	pthread_mutex_init (&m->outer, NULL);
	pthread_mutex_init (&m->inner, NULL);

	int err = pthread_create (&m->thread, NULL, do_maintenance, m);
	if (err == 0)
		return m;

	syslog (LOG_ALERT, _("Error (%s): %s\n"), "pthread_create",
	        strerror (err));

	pthread_cond_destroy (&m->processed);
	pthread_cond_destroy (&m->received);
	pthread_mutex_destroy (&m->outer);
	pthread_mutex_destroy (&m->inner);

error:
	if (m->server2 != NULL)
		free (m->server2);
	free (m->server);
	free (m);
	return NULL;
}

/**
 * Stops and destroys a maintenance thread created by
 * teredo_maintenance_start()
 */
void teredo_maintenance_stop (teredo_maintenance *m)
{
	pthread_cancel (m->thread);
	pthread_join (m->thread, NULL);

	pthread_cond_destroy (&m->processed);
	pthread_cond_destroy (&m->received);
	pthread_mutex_destroy (&m->inner);
	pthread_mutex_destroy (&m->outer);

	if (m->server2 != NULL)
		free (m->server2);
	free (m->server);
	free (m);
}


/**
 * Passes a Teredo packet to a maintenance thread for processing.
 * Thread-safe, not async-cancel safe.
 *
 * @return 0 if processed, -1 if not a valid router solicitation.
 */
int teredo_maintenance_process (teredo_maintenance *restrict m,
                                const teredo_packet *restrict packet)
{
	assert (m != NULL);
	assert (packet != NULL);

	/*
	 * We don't accept router advertisement without nonce.
	 * It is far too easy to spoof such packets.
	 */
	if ((packet->source_port != htons (IPPORT_TEREDO))
	    /* TODO: check for primary or secondary server address */
	 || (packet->auth_nonce == NULL)
	 || memcmp (packet->ip6 + 24, &teredo_restrict, 16))
		return -1;

	pthread_mutex_lock (&m->outer);
	pthread_mutex_lock (&m->inner);

	m->incoming = packet;
	pthread_cond_signal (&m->received);

	/* Waits for maintenance thread to process packet... */
	do
		pthread_cond_wait (&m->processed, &m->inner);
	while (m->incoming != NULL);

	pthread_mutex_unlock (&m->inner);
	pthread_mutex_unlock (&m->outer);

	return 0;
}
