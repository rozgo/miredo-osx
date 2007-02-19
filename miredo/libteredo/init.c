/*
 * init.c
 * $Id: init.c 1831 2006-12-12 17:06:40Z remi $
 */

/***********************************************************************
 *  Copyright © 2005-2006 Rémi Denis-Courmont.                         *
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

#include <stdbool.h>
#include <gettext.h>

#include <assert.h>

#include <inttypes.h>
#include "security.h"
#include "tunnel.h"
#include "clock.h"

/**
 * Initializes libteredo. That function must be called before any other
 * libteredo functions. It can safely be called multiple times and is
 * thread-safe. If the process is to be chrooted(), it should be called
 * before chroot().
 *
 * @param use_client true if libteredo is to be used in client-mode
 *
 * @return 0 on success, -1 on failure.
 * -1 is also returned when use_client is true while libteredo was
 *  compiled without client support.
 */
int teredo_startup (bool use_client)
{
	(void)bindtextdomain (PACKAGE_NAME, LOCALEDIR);

#ifdef MIREDO_TEREDO_CLIENT
	(void)use_client;
#else
	if (use_client)
		return -1;
#endif

	if (teredo_init_HMAC () == 0)
	{
		if (teredo_clock_create () == 0)
			return 0;
		teredo_deinit_HMAC ();
	}
	return -1;
}


/**
 * Releases resources allocated with teredo_startup().
 * Should be called as many times as teredo_startup() was called.
 * Thread-safe.
 *
 * @param use_client true if the matching teredo_preinit call
 * had the use_client parameter set.
 */
void teredo_cleanup (bool use_client)
{
	(void)use_client;
#ifndef MIREDO_TEREDO_CLIENT
	assert (!use_client);
#endif

	teredo_clock_destroy ();
	teredo_deinit_HMAC ();
}
