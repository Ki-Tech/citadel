/*
 * $Id$
 *
 * Server-side module for Wiki rooms.  This will handle things like version control. 
 * 
 * Copyright (c) 2009 / released under the GNU General Public License v3
 */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <ctype.h>
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
#include <libcitadel.h>
#include "citadel.h"
#include "server.h"
#include "citserver.h"
#include "support.h"
#include "config.h"
#include "control.h"
#include "room_ops.h"
#include "user_ops.h"
#include "policy.h"
#include "database.h"
#include "msgbase.h"
#include "euidindex.h"
#include "ctdl_module.h"

/*
 * Before allowing a wiki page save to execute, we have to perform version control.
 * This involves fetching the old version of the page if it exists... FIXME finish this
 */
int wiki_upload_beforesave(struct CtdlMessage *msg) {
	struct CitContext *CCC = CC;
	long old_msgnum = (-1L);
	struct CtdlMessage *old_msg = NULL;
	int no_changes_were_made = 0;

	if (!CCC->logged_in) return(0);	/* Only do this if logged in. */

	/* Is this a room with a Wiki in it, don't run this hook. */
	if (CCC->room.QRdefaultview != VIEW_WIKI) {
		return(0);
	}

	/* If this isn't a MIME message, don't bother. */
	if (msg->cm_format_type != 4) return(0);

	/* If there's no EUID we can't do this. */
	if (msg->cm_fields['E'] == NULL) return(0);

	/* See if we can retrieve the previous version. */
	old_msgnum = locate_message_by_euid(msg->cm_fields['E'], &CCC->room);
	if (old_msgnum <= 0L) return(0);

	old_msg = CtdlFetchMessage(old_msgnum, 1);
	if (old_msg == NULL) return(0);

	if ((msg->cm_fields['M'] != NULL) && (old_msg->cm_fields['M'] != NULL)) {

		/* If no changes were made, don't bother saving it again */
		if (!strcmp(msg->cm_fields['M'], old_msg->cm_fields['M'])) {
			no_changes_were_made = 1;
		}

		/* FIXME here's where diffs should be generated
		 *
		FILE *fp;
		fp = fopen("/tmp/new.txt", "w");
		fwrite(msg->cm_fields['M'], strlen(msg->cm_fields['M']), 1, fp);
		fclose(fp);
		fp = fopen("/tmp/old.txt", "w");
		fwrite(old_msg->cm_fields['M'], strlen(old_msg->cm_fields['M']), 1, fp);
		fclose(fp);
		 *
		 */
	}

	CtdlFreeMessage(old_msg);
	return(no_changes_were_made);
}


/*
 * Module initialization
 */
CTDL_MODULE_INIT(wiki)
{
	if (!threading)
	{
		CtdlRegisterMessageHook(wiki_upload_beforesave, EVT_BEFORESAVE);
	}

	/* return our Subversion id for the Log */
	return "$Id$";
}