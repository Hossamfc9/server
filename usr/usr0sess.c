/*****************************************************************************

Copyright (c) 1996, 2009, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/******************************************************
Sessions

Created 6/25/1996 Heikki Tuuri
*******************************************************/

#include "usr0sess.h"

#ifdef UNIV_NONINL
#include "usr0sess.ic"
#endif

#include "trx0trx.h"

/*************************************************************************
Closes a session, freeing the memory occupied by it. */
static
void
sess_close(
/*=======*/
	sess_t*		sess);	/* in, own: session object */

/*************************************************************************
Opens a session. */
UNIV_INTERN
sess_t*
sess_open(void)
/*===========*/
					/* out, own: session object */
{
	sess_t*	sess;

	ut_ad(mutex_own(&kernel_mutex));

	sess = mem_alloc(sizeof(sess_t));

	sess->state = SESS_ACTIVE;

	sess->trx = trx_create(sess);

	UT_LIST_INIT(sess->graphs);

	return(sess);
}

/*************************************************************************
Closes a session, freeing the memory occupied by it. */
static
void
sess_close(
/*=======*/
	sess_t*	sess)	/* in, own: session object */
{
	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(sess->trx == NULL);

	mem_free(sess);
}

/*************************************************************************
Closes a session, freeing the memory occupied by it, if it is in a state
where it should be closed. */
UNIV_INTERN
ibool
sess_try_close(
/*===========*/
			/* out: TRUE if closed */
	sess_t*	sess)	/* in, own: session object */
{
	ut_ad(mutex_own(&kernel_mutex));

	if (UT_LIST_GET_LEN(sess->graphs) == 0) {
		sess_close(sess);

		return(TRUE);
	}

	return(FALSE);
}
