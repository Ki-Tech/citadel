/*
 * $Id$
 *
 * This file contains miscellaneous housekeeping tasks.
 *
 */

#ifdef DLL_EXPORT
#define IN_LIBCIT
#endif

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include "tools.h"
#include "citadel.h"
#include "server.h"
#include "serv_extensions.h"
#include "citserver.h"
#include "config.h"
#include "housekeeping.h"
#include "sysdep_decls.h"
#include "room_ops.h"
#include "database.h"




/*
 * Terminate idle sessions.  This function pounds through the session table
 * comparing the current time to each session's time-of-last-command.  If an
 * idle session is found it is terminated, then the search restarts at the
 * beginning because the pointer to our place in the list becomes invalid.
 */
void terminate_idle_sessions(void) {
	struct CitContext *ccptr;
	time_t now;
	int session_to_kill;
	int killed = 0;

	now = time(NULL);
	session_to_kill = 0;
	begin_critical_section(S_SESSION_TABLE);
	for (ccptr = ContextList; ccptr != NULL; ccptr = ccptr->next) {
		if (  (ccptr!=CC)
	   	&& (config.c_sleeping > 0)
	   	&& (now - (ccptr->lastcmd) > config.c_sleeping) ) {
			ccptr->kill_me = 1;
			++killed;
		}
	}
	end_critical_section(S_SESSION_TABLE);
	lprintf(9, "Terminated %d idle sessions\n", killed);
}



void check_sched_shutdown(void) {
	if ((ScheduledShutdown == 1) && (ContextList == NULL)) {
		lprintf(3, "Scheduled shutdown initiating.\n");
		time_to_die = 1;
	}
}



/*
 * Check (and fix) floor reference counts.  This doesn't need to be done
 * very often, since the counts should remain correct during normal operation.
 * NOTE: this function pair should ONLY be called during startup.  It is NOT
 * thread safe.
 */
void check_ref_counts_backend(struct ctdlroom *qrbuf, void *data) {
	struct floor flbuf;

	getfloor(&flbuf, qrbuf->QRfloor);
	++flbuf.f_ref_count;
	flbuf.f_flags = flbuf.f_flags | QR_INUSE;
	putfloor(&flbuf, qrbuf->QRfloor);
}

void check_ref_counts(void) {
	struct floor flbuf;
	int a;

	lprintf(7, "Checking floor reference counts\n");
	for (a=0; a<MAXFLOORS; ++a) {
		getfloor(&flbuf, a);
		flbuf.f_ref_count = 0;
		flbuf.f_flags = flbuf.f_flags & ~QR_INUSE;
		putfloor(&flbuf, a);
	}

	cdb_begin_transaction();
	ForEachRoom(check_ref_counts_backend, NULL);
	cdb_end_transaction();
}	

/*
 * This is the housekeeping loop.  Worker threads come through here after
 * processing client requests but before jumping back into the pool.  We
 * only allow housekeeping to execute once per minute, and we only allow one
 * instance to run at a time.
 */
void do_housekeeping(void) {
	static int housekeeping_in_progress = 0;
	static time_t last_timer = 0L;
	int do_housekeeping_now = 0;
	time_t now;

	/*
	 * We do it this way instead of wrapping the whole loop in an
	 * S_HOUSEKEEPING critical section because it eliminates the need to
	 * potentially have multiple concurrent mutexes in progress.
	 */
	begin_critical_section(S_HOUSEKEEPING);
	now = time(NULL);
	if ( (now - last_timer) > (time_t)60 ) {
		if (housekeeping_in_progress == 0) {
			do_housekeeping_now = 1;
			housekeeping_in_progress = 1;
			last_timer = time(NULL);
		}
	}
	end_critical_section(S_HOUSEKEEPING);

	if (do_housekeeping_now == 0) {
		return;
	}

	/*
	 * Ok, at this point we've made the decision to run the housekeeping
	 * loop.  Everything below this point is real work.
	 */

	cdb_check_handles();			/* suggested by Justin Case */
	PerformSessionHooks(EVT_TIMER);		/* Run any timer hooks */

	/*
	 * All done.
	 */
	housekeeping_in_progress = 0;
}
