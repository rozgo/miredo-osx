/*
 * pselect.c - pselect() replacement
 * $Id: pselect.c 1237 2006-04-14 06:39:20Z remi $
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
#include <sys/types.h>
#include <sys/select.h>
#include <unistd.h>
#include <signal.h>

int pselect (int n, fd_set *rfds, fd_set *wfds, fd_set *efds,
             const struct timespec *ts, const sigset_t *sigmask)
{
	struct timeval tv = { 0, 0 }, *ptv;
	sigset_t oldset;
	int val;

	if (ts != NULL)
	{
		tv.tv_sec = ts->tv_sec;
		tv.tv_usec = (ts->tv_nsec + 999)/ 1000;
		ptv = &tv;
	}
	else
		ptv = NULL;

	/*
	 * This is obviously not atomic, and hence we have a tiny race
	 * condition, but nevertheless a genuine race condition.
	 */
	if (sigprocmask (SIG_SETMASK, sigmask, &oldset))
		return -1;

	val = select (n, rfds, wfds, efds, ptv);
	(void)sigprocmask (SIG_SETMASK, &oldset, NULL);
	return val;
}

