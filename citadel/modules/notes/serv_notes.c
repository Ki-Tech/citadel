/*
 * $Id$
 *
 * Handles functions related to yellow sticky notes.
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
#include <libcitadel.h>
#include "citadel.h"
#include "server.h"
#include "citserver.h"
#include "support.h"
#include "config.h"
#include "room_ops.h"
#include "user_ops.h"
#include "policy.h"
#include "database.h"
#include "msgbase.h"

#include "ctdl_module.h"


/*
 * Callback function for serv_notes_beforesave() hunts for a vNote in the MIME structure
 */
void notes_extract_vnote(char *name, char *filename, char *partnum, char *disp,
		   void *content, char *cbtype, char *cbcharset, size_t length,
		   char *encoding, char *cbid, void *cbuserdata)
{
	struct vnote **v = (struct vnote **) cbuserdata;

	if (!strcasecmp(cbtype, "text/vnote")) {

		CtdlLogPrintf(CTDL_DEBUG, "Part %s contains a vNote!  Loading...\n", partnum);
		if (*v != NULL) {
			vnote_free(*v);
		}
		*v = vnote_new_from_str(content);
	}
}


/*
 * Before-save hook searches for two different types of notes (legacy Kolab/Aethera notes
 * and modern vNote format notes) and does its best to learn the subject (summary)
 * and EUID (uid) of the note for Citadel's own nefarious purposes.
 */
int serv_notes_beforesave(struct CtdlMessage *msg)
{
	char *p;
	int a, i;
	char uuid[512];
	struct vnote *v = NULL;

	/* First determine if this room has the "notes" view set */

	if (CC->room.QRdefaultview != VIEW_NOTES) {
		return(0);			/* not notes; do nothing */
	}

	/* It must be an RFC822 message! */
	if (msg->cm_format_type != 4) {
		return(0);	/* You tried to save a non-RFC822 message! */
	}
	
	/*
	 * If we are in a "notes" view room, and the client has sent an RFC822
	 * message containing an X-KOrg-Note-Id: field (Aethera does this, as
	 * do some Kolab clients) then set both the Subject and the Exclusive ID
	 * of the message to that.  It's going to be a UUID so we want to replace
	 * any existing message containing that UUID.
	 */
	strcpy(uuid, "");
	p = msg->cm_fields['M'];
	a = strlen(p);
	while (--a > 0) {
		if (!strncasecmp(p, "X-KOrg-Note-Id: ", 16)) {	/* Found it */
			safestrncpy(uuid, p + 16, sizeof(uuid));
			for (i = 0; uuid[i]; ++i) {
				if ( (uuid[i] == '\r') || (uuid[i] == '\n') ) {
					uuid[i] = 0;
					break;
				}
			}

			CtdlLogPrintf(9, "UUID of note is: %s\n", uuid);
			if (!IsEmptyStr(uuid)) {

				if (msg->cm_fields['E'] != NULL) {
					free(msg->cm_fields['E']);
				}
				msg->cm_fields['E'] = strdup(uuid);

				if (msg->cm_fields['U'] != NULL) {
					free(msg->cm_fields['U']);
				}
				msg->cm_fields['U'] = strdup(uuid);
			}
		}
		p++;
	}

	/* Modern clients are using vNote format.  Check for one... */

	mime_parser(msg->cm_fields['M'],
		NULL,
		*notes_extract_vnote,
		NULL, NULL,
		&v,		/* user data ptr - put the vnote here */
		0
	);

	if (v == NULL) return(0);	/* no vNotes were found in this message */

	/* Set the message EUID to the vNote UID */

	if (v->uid) if (!IsEmptyStr(v->uid)) {
		CtdlLogPrintf(9, "UID of vNote is: %s\n", v->uid);
		if (msg->cm_fields['E'] != NULL) {
			free(msg->cm_fields['E']);
		}
		msg->cm_fields['E'] = strdup(v->uid);
	}

	/* Set the message Subject to the vNote Summary */

	if (v->summary) if (!IsEmptyStr(v->summary)) {
		if (msg->cm_fields['U'] != NULL) {
			free(msg->cm_fields['U']);
		}
		msg->cm_fields['U'] = strdup(v->summary);
		if (strlen(msg->cm_fields['U']) > 72) {
			strcpy(&msg->cm_fields['U'][68], "...");
		}
	}

	vnote_free(v);
	
	return(0);
}


CTDL_MODULE_INIT(notes)
{
	if (!threading)
	{
		CtdlRegisterMessageHook(serv_notes_beforesave, EVT_BEFORESAVE);
	}
	
	/* return our Subversion id for the Log */
	return "$Id$";
}
