/*
 * citserver's main() function lives here.
 *
 * $Id$
 */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <syslog.h>

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

#include <limits.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/un.h>
#include <string.h>
#include <pwd.h>
#include <errno.h>
#include <stdarg.h>
#include <grp.h>
#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif
#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif
#include "citadel.h"
#include "server.h"
#include "serv_extensions.h"
#include "sysdep_decls.h"
#include "citserver.h"
#include "support.h"
#include "config.h"
#include "database.h"
#include "housekeeping.h"
#include "tools.h"

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifndef HAVE_SNPRINTF
#include "snprintf.h"
#endif

int running_as_daemon = 0;

/*
 * Here's where it all begins.
 */
int main(int argc, char **argv)
{
	char facility[32];
	int a, i;			/* General-purpose variables */
	struct passwd *pw;
	int drop_root_perms = 1;
	size_t size;

	/* initialize the master context */
	InitializeMasterCC();

	/* set default syslog facility */
	syslog_facility = LOG_DAEMON;

	/* parse command-line arguments */
	for (a=1; a<argc; ++a) {

		if (!strncmp(argv[a], "-l", 2)) {
			safestrncpy(facility, argv[a], sizeof(facility));
			syslog_facility = SyslogFacility(facility);
		}

		/* run in the background if -d was specified */
		else if (!strcmp(argv[a], "-d")) {
			running_as_daemon = 1;
		}

		/* -x specifies the desired logging level */
		else if (!strncmp(argv[a], "-x", 2)) {
			verbosity = atoi(&argv[a][2]);
		}

		else if (!strncmp(argv[a], "-h", 2)) {
			safestrncpy(bbs_home_directory, &argv[a][2],
				    sizeof bbs_home_directory);
			home_specified = 1;
		}

		else if (!strncmp(argv[a], "-t", 2)) {
			freopen(&argv[a][2], "w", stderr);
		}

		else if (!strncmp(argv[a], "-f", 2)) {
			do_defrag = 1;
		}

		/* -r tells the server not to drop root permissions. don't use
		 * this unless you know what you're doing. this should be
		 * removed in the next release if it proves unnecessary. */
		else if (!strcmp(argv[a], "-r"))
			drop_root_perms = 0;

		/* any other parameter makes it crash and burn */
		else {
			lprintf(CTDL_EMERG,	"citserver: usage: "
					"citserver "
					"[-lLogFacility] "
					"[-d] [-f]"
					" [-tTraceFile]"
					" [-xLogLevel] [-hHomeDir]\n");
			exit(1);
		}

	}

	/* daemonize, if we were asked to */
	if (running_as_daemon) { start_daemon(0); drop_root_perms = 1; }

	/* initialize the syslog facility */
	if (running_as_daemon) openlog("Citadel", LOG_NDELAY, syslog_facility);
	else openlog("Citadel", LOG_PERROR|LOG_NDELAY, syslog_facility);
	setlogmask(LOG_UPTO(verbosity));
	
	/* Tell 'em who's in da house */
	lprintf(CTDL_NOTICE, "\n");
	lprintf(CTDL_NOTICE, "\n");
	lprintf(CTDL_NOTICE,
		"*** Citadel server engine v%d.%02d ***\n",
		(REV_LEVEL/100), (REV_LEVEL%100));
	lprintf(CTDL_NOTICE,
		"Copyright (C) 1987-2005 by the Citadel development team.\n");
	lprintf(CTDL_NOTICE,
		"This program is distributed under the terms of the GNU "
		"General Public License.\n");
	lprintf(CTDL_NOTICE, "\n");
	lprintf(CTDL_DEBUG, "Called as: %s\n", argv[0]);

	/* Load site-specific parameters, and set the ipgm secret */
	lprintf(CTDL_INFO, "Loading citadel.config\n");
	get_config();
	config.c_ipgm_secret = rand();
	put_config();

	/* Initialize... */
	init_sysdep();

	/*
	 * Do non system dependent startup functions.
	 */
	master_startup();

	/*
	 * Bind the server to a Unix-domain socket.
	 */
	CtdlRegisterServiceHook(0,
				"citadel.socket",
				citproto_begin_session,
				do_command_loop,
				do_async_loop);

	/*
	 * Bind the server to our favorite TCP port (usually 504).
	 */
	CtdlRegisterServiceHook(config.c_port_number,
				NULL,
				citproto_begin_session,
				do_command_loop,
				do_async_loop);

	/*
	 * Load any server-side extensions available here.
	 */
	lprintf(CTDL_INFO, "Initializing server extensions\n");
	size = strlen(bbs_home_directory) + 9;
	initialize_server_extensions();

	/*
	 * Now that we've bound the sockets, change to the BBS user id and its
	 * corresponding group ids
	 */
	if (drop_root_perms) {
		if ((pw = getpwuid(BBSUID)) == NULL)
			lprintf(CTDL_CRIT, "WARNING: getpwuid(%ld): %s\n"
				   "Group IDs will be incorrect.\n", (long)BBSUID,
				strerror(errno));
		else {
			initgroups(pw->pw_name, pw->pw_gid);
			if (setgid(pw->pw_gid))
				lprintf(CTDL_CRIT, "setgid(%ld): %s\n", (long)pw->pw_gid,
					strerror(errno));
		}
		lprintf(CTDL_INFO, "Changing uid to %ld\n", (long)BBSUID);
		if (setuid(BBSUID) != 0) {
			lprintf(CTDL_CRIT, "setuid() failed: %s\n", strerror(errno));
		}
#if defined (HAVE_SYS_PRCTL_H) && defined (PR_SET_DUMPABLE)
		prctl(PR_SET_DUMPABLE, 1);
#endif
	}

	/* We want to check for idle sessions once per minute */
	CtdlRegisterSessionHook(terminate_idle_sessions, EVT_TIMER);

	/*
	 * Now create a bunch of worker threads.
	 */
	lprintf(CTDL_DEBUG, "Starting %d worker threads\n", config.c_min_workers-1);
	begin_critical_section(S_WORKER_LIST);
	for (i=0; i<(config.c_min_workers-1); ++i) {
		create_worker();
	}
	end_critical_section(S_WORKER_LIST);

	/* Now this thread can become a worker as well. */
	worker_thread(NULL);

	/* Server is exiting. Wait for workers to shutdown. */
	lprintf(CTDL_INFO, "Server is shutting down.\n");
	master_cleanup(0);
	return(0);
}
