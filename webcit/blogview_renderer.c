/* 
 * Blog view renderer module for WebCit
 *
 * Copyright (c) 1996-2010 by the citadel.org team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "webcit.h"
#include "webserver.h"
#include "groupdav.h"

/*
 * Data which gets passed around between the various functions in this module
 *
 */
struct blogview {
	long *msgs;		/* Array of msgnums for messages we are displaying */
	int num_msgs;		/* Number of msgnums stored in 'msgs' */
	int alloc_msgs;		/* Currently allocated size of array */
};


/*
 * Entry point for message read operations.
 */
int blogview_GetParamsGetServerCall(SharedMessageStatus *Stat, 
				   void **ViewSpecific, 
				   long oper, 
				   char *cmd, 
				   long len)
{
	struct blogview *BLOG = malloc(sizeof(struct blogview));
	memset(BLOG, 0, sizeof(struct blogview));
	*ViewSpecific = BLOG;

	Stat->startmsg = (-1);					/* not used here */
	Stat->sortit = 1;					/* not used here */
	Stat->num_displayed = DEFAULT_MAXMSGS;			/* not used here */
	if (Stat->maxmsgs == 0) Stat->maxmsgs = DEFAULT_MAXMSGS;
	
	/* perform a "read all" call to fetch the message list -- we'll cut it down later */
	rlid[2].cmd(cmd, len);
	
	return 200;
}


/*
 * This function is called for every message in the list.
 */
int blogview_LoadMsgFromServer(SharedMessageStatus *Stat, 
			      void **ViewSpecific, 
			      message_summary* Msg, 
			      int is_new, 
			      int i)
{
	struct blogview *BLOG = (struct blogview *) *ViewSpecific;

	if (BLOG->alloc_msgs == 0) {
		BLOG->alloc_msgs = 1000;
		BLOG->msgs = malloc(BLOG->alloc_msgs * sizeof(long));
		memset(BLOG->msgs, 0, (BLOG->alloc_msgs * sizeof(long)) );
	}

	/* Check our buffer size */
	if (BLOG->num_msgs >= BLOG->alloc_msgs) {
		BLOG->alloc_msgs *= 2;
		BLOG->msgs = realloc(BLOG->msgs, (BLOG->alloc_msgs * sizeof(long)));
		memset(&BLOG->msgs[BLOG->num_msgs], 0, ((BLOG->alloc_msgs - BLOG->num_msgs) * sizeof(long)) );
	}

	BLOG->msgs[BLOG->num_msgs++] = Msg->msgnum;

	return 200;
}


int blogview_sortfunc(const void *s1, const void *s2) {
	long l1;
	long l2;

	l1 = *(long *)(s1);
	l2 = *(long *)(s2);

	if (l1 > l2) return(+1);
	if (l1 < l2) return(-1);
	return(0);
}


int blogview_RenderView_or_Tail(SharedMessageStatus *Stat, 
			       void **ViewSpecific, 
			       long oper)
{
	struct blogview *BLOG = (struct blogview *) *ViewSpecific;
	int i;
	const StrBuf *Mime;

	wc_printf("<div class=\"fix_scrollbar_bug\">");

	if (Stat->nummsgs > 0) {
		lprintf(9, "sorting %d messages\n", BLOG->num_msgs);
		qsort(BLOG->msgs, (size_t)(BLOG->num_msgs), sizeof(long), blogview_sortfunc);
	}

	for (i=0; (i<BLOG->num_msgs); ++i) {
		if (BLOG->msgs[i] > 0L) {
			read_message(WC->WBuf, HKEY("view_message"), BLOG->msgs[i], NULL, &Mime);
		}
	}

	wc_printf("</div>\n");
	return(0);
}


int blogview_Cleanup(void **ViewSpecific)
{
	struct blogview *BLOG = (struct blogview *) *ViewSpecific;

	if (BLOG->alloc_msgs != 0) {
		free(BLOG->msgs);
	}
	free(BLOG);

	wDumpContent(1);
	return 0;
}


void 
InitModule_BLOGVIEWRENDERERS
(void)
{
	RegisterReadLoopHandlerset(
		VIEW_BLOG,
		blogview_GetParamsGetServerCall,
		NULL,
		NULL, 
		blogview_LoadMsgFromServer,
		blogview_RenderView_or_Tail,
		blogview_Cleanup
	);
}