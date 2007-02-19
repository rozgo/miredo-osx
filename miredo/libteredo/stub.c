/*
 * relay.c - Teredo relay core
 * $Id: relay.c 1699 2006-08-16 18:51:08Z remi $
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

#include <stdbool.h>
#include <assert.h>

#include <inttypes.h>
#include "tunnel.h"


/**
 * This is an alias for teredo_set_relay_mode.
 *
 * @param cone ignored for backward compatibility.
 *
 * @return 0.
 */
int teredo_set_cone_flag (teredo_tunnel *t, bool cone)
{
	(void)cone;
	return teredo_set_relay_mode (t);
}


/**
 * Does nothing (backward compatibility stub).
 */
void teredo_set_cone_ignore (teredo_tunnel *t, bool ignore)
{
	assert (t != NULL);
	(void)t;
	(void)ignore;
}
