/*
 * $Id$
 *
 * This module supplies statistics about the activity levels of your Citadel
 * system.  We didn't bother writing a reporting module, because there is
 * already an excellent tool called MRTG (Multi Router Traffic Grapher) which
 * is available at http://www.mrtg.org that can fetch data using external
 * scripts.  This module supplies data in the format expected by MRTG.
 *
 */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <sys/types.h>

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

#include <sys/wait.h>
#include <string.h>
#include <limits.h>
#include "citadel.h"
#include "server.h"
#include "sysdep_decls.h"
#include "citserver.h"
#include "support.h"
#include "config.h"
#include "control.h"
#include "dynloader.h"
#include "room_ops.h"
#include "user_ops.h"
#include "policy.h"
#include "database.h"
#include "msgbase.h"
#include "tools.h"


/*
 * Other functions call this one to output data in MRTG format
 */
void mrtg_output(long value1, long value2) {
	time_t uptime_t;
	int uptime_days, uptime_hours, uptime_minutes;
	
	uptime_t = time(NULL) - server_startup_time;
	uptime_days = (int) (uptime_t / 86400L);
	uptime_hours = (int) ((uptime_t % 86400L) / 3600L);
	uptime_minutes = (int) ((uptime_t % 3600L) / 60L);

	cprintf("%d ok\n", LISTING_FOLLOWS);
	cprintf("%ld\n", value1);
	cprintf("%ld\n", value2);
	cprintf("%d days, %d hours, %d minutes\n",
		uptime_days, uptime_hours, uptime_minutes);
	cprintf("%s\n", config.c_humannode);
	cprintf("000\n");
}




/*
 * Tell us how many users are online
 */
void mrtg_users(void) {
	long connected_users = 0;
	long active_users = 0;
	
	struct CitContext *cptr;

        for (cptr = ContextList; cptr != NULL; cptr = cptr->next) {

		++connected_users;

		if ( (time(NULL) - (cptr->lastidle)) < 900L) {
			++active_users;
		}

	}

	mrtg_output(connected_users, active_users);
}


/*
 * Volume of messages submitted
 */
void mrtg_messages(void) {
	mrtg_output(CitControl.MMhighest, 0L);
}


/*
 * Fetch data for MRTG
 */
void cmd_mrtg(char *argbuf) {
	char which[SIZ];

	extract(which, argbuf, 0);

	if (!strcasecmp(which, "users")) {
		mrtg_users();
	}
	if (!strcasecmp(which, "messages")) {
		mrtg_messages();
	}
	else {
		cprintf("%d Unrecognized keyword '%s'\n",
			ERROR+ILLEGAL_VALUE, which);
	}
}


char *Dynamic_Module_Init(void)
{
        CtdlRegisterProtoHook(cmd_mrtg, "MRTG", "Supply stats to MRTG");
        return "$Id$";
}
