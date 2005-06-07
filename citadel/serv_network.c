/*
 * $Id$ 
 *
 * This module handles shared rooms, inter-Citadel mail, and outbound
 * mailing list processing.
 *
 * Copyright (C) 2000-2002 by Art Cancro and others.
 * This code is released under the terms of the GNU General Public License.
 *
 * ** NOTE **   A word on the S_NETCONFIGS semaphore:
 * This is a fairly high-level type of critical section.  It ensures that no
 * two threads work on the netconfigs files at the same time.  Since we do
 * so many things inside these, here are the rules:
 *  1. begin_critical_section(S_NETCONFIGS) *before* begin_ any others.
 *  2. Do *not* perform any I/O with the client during these sections.
 *
 */

/*
 * Duration of time (in seconds) after which pending list subscribe/unsubscribe
 * requests that have not been confirmed will be deleted.
 */
#define EXP	259200	/* three days */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
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
#include "serv_extensions.h"
#include "room_ops.h"
#include "user_ops.h"
#include "policy.h"
#include "database.h"
#include "msgbase.h"
#include "tools.h"
#include "internet_addressing.h"
#include "serv_network.h"
#include "clientsocket.h"
#include "file_ops.h"

#ifndef HAVE_SNPRINTF
#include "snprintf.h"
#endif

/* Nonzero while we are doing outbound network processing */
static int doing_queue = 0;

/*
 * When we do network processing, it's accomplished in two passes; one to
 * gather a list of rooms and one to actually do them.  It's ok that rplist
 * is global; this process *only* runs as part of the housekeeping loop and
 * therefore only one will run at a time.
 */
struct RoomProcList *rplist = NULL;

/*
 * We build a map of network nodes during processing.
 */
struct NetMap *the_netmap = NULL;
int netmap_changed = 0;
char *working_ignetcfg = NULL;

/*
 * Load or refresh the Citadel network (IGnet) configuration for this node.
 */
void load_working_ignetcfg(void) {
	char *cfg;
	char *oldcfg;

	cfg = CtdlGetSysConfig(IGNETCFG);
	if (cfg == NULL) {
		cfg = strdup("");
	}

	oldcfg = working_ignetcfg;
	working_ignetcfg = cfg;
	if (oldcfg != NULL) {
		free(oldcfg);
	}
}





/*
 * Keep track of what messages to reject
 */
struct FilterList *load_filter_list(void) {
	char *serialized_list = NULL;
	int i;
	char buf[SIZ];
	struct FilterList *newlist = NULL;
	struct FilterList *nptr;

	serialized_list = CtdlGetSysConfig(FILTERLIST);
	if (serialized_list == NULL) return(NULL); /* if null, no entries */

	/* Use the string tokenizer to grab one line at a time */
	for (i=0; i<num_tokens(serialized_list, '\n'); ++i) {
		extract_token(buf, serialized_list, i, '\n', sizeof buf);
		nptr = (struct FilterList *) malloc(sizeof(struct FilterList));
		extract_token(nptr->fl_user, buf, 0, '|', sizeof nptr->fl_user);
		striplt(nptr->fl_user);
		extract_token(nptr->fl_room, buf, 1, '|', sizeof nptr->fl_room);
		striplt(nptr->fl_room);
		extract_token(nptr->fl_node, buf, 2, '|', sizeof nptr->fl_node);
		striplt(nptr->fl_node);

		/* Cowardly refuse to add an any/any/any entry that would
		 * end up filtering every single message.
		 */
		if (strlen(nptr->fl_user) + strlen(nptr->fl_room)
		   + strlen(nptr->fl_node) == 0) {
			free(nptr);
		}
		else {
			nptr->next = newlist;
			newlist = nptr;
		}
	}

	free(serialized_list);
	return newlist;
}


void free_filter_list(struct FilterList *fl) {
	if (fl == NULL) return;
	free_filter_list(fl->next);
	free(fl);
}



/*
 * Check the use table.  This is a list of messages which have recently
 * arrived on the system.  It is maintained and queried to prevent the same
 * message from being entered into the database multiple times if it happens
 * to arrive multiple times by accident.
 */
int network_usetable(struct CtdlMessage *msg) {

	char msgid[SIZ];
	struct cdbdata *cdbut;
	struct UseTable ut;

	/* Bail out if we can't generate a message ID */
	if (msg == NULL) {
		return(0);
	}
	if (msg->cm_fields['I'] == NULL) {
		return(0);
	}
	if (strlen(msg->cm_fields['I']) == 0) {
		return(0);
	}

	/* Generate the message ID */
	strcpy(msgid, msg->cm_fields['I']);
	if (haschar(msgid, '@') == 0) {
		strcat(msgid, "@");
		if (msg->cm_fields['N'] != NULL) {
			strcat(msgid, msg->cm_fields['N']);
		}
		else {
			return(0);
		}
	}

	cdbut = cdb_fetch(CDB_USETABLE, msgid, strlen(msgid));
	if (cdbut != NULL) {
		cdb_free(cdbut);
		return(1);
	}

	/* If we got to this point, it's unique: add it. */
	strcpy(ut.ut_msgid, msgid);
	ut.ut_timestamp = time(NULL);
	cdb_store(CDB_USETABLE, msgid, strlen(msgid),
		&ut, sizeof(struct UseTable) );
	return(0);
}


/* 
 * Read the network map from its configuration file into memory.
 */
void read_network_map(void) {
	char *serialized_map = NULL;
	int i;
	char buf[SIZ];
	struct NetMap *nmptr;

	serialized_map = CtdlGetSysConfig(IGNETMAP);
	if (serialized_map == NULL) return;	/* if null, no entries */

	/* Use the string tokenizer to grab one line at a time */
	for (i=0; i<num_tokens(serialized_map, '\n'); ++i) {
		extract_token(buf, serialized_map, i, '\n', sizeof buf);
		nmptr = (struct NetMap *) malloc(sizeof(struct NetMap));
		extract_token(nmptr->nodename, buf, 0, '|', sizeof nmptr->nodename);
		nmptr->lastcontact = extract_long(buf, 1);
		extract_token(nmptr->nexthop, buf, 2, '|', sizeof nmptr->nexthop);
		nmptr->next = the_netmap;
		the_netmap = nmptr;
	}

	free(serialized_map);
	netmap_changed = 0;
}


/*
 * Write the network map from memory back to the configuration file.
 */
void write_network_map(void) {
	char *serialized_map = NULL;
	struct NetMap *nmptr;


	if (netmap_changed) {
		serialized_map = strdup("");
	
		if (the_netmap != NULL) {
			for (nmptr = the_netmap; nmptr != NULL; nmptr = nmptr->next) {
				serialized_map = realloc(serialized_map,
							(strlen(serialized_map)+SIZ) );
				if (strlen(nmptr->nodename) > 0) {
					snprintf(&serialized_map[strlen(serialized_map)],
						SIZ,
						"%s|%ld|%s\n",
						nmptr->nodename,
						(long)nmptr->lastcontact,
						nmptr->nexthop);
				}
			}
		}

		CtdlPutSysConfig(IGNETMAP, serialized_map);
		free(serialized_map);
	}

	/* Now free the list */
	while (the_netmap != NULL) {
		nmptr = the_netmap->next;
		free(the_netmap);
		the_netmap = nmptr;
	}
	netmap_changed = 0;
}



/* 
 * Check the network map and determine whether the supplied node name is
 * valid.  If it is not a neighbor node, supply the name of a neighbor node
 * which is the next hop.  If it *is* a neighbor node, we also fill in the
 * shared secret.
 */
int is_valid_node(char *nexthop, char *secret, char *node) {
	int i;
	char linebuf[SIZ];
	char buf[SIZ];
	int retval;
	struct NetMap *nmptr;

	if (node == NULL) {
		return(-1);
	}

	/*
	 * First try the neighbor nodes
	 */
	if (working_ignetcfg == NULL) {
		lprintf(CTDL_ERR, "working_ignetcfg is NULL!\n");
		if (nexthop != NULL) {
			strcpy(nexthop, "");
		}
		return(-1);
	}

	retval = (-1);
	if (nexthop != NULL) {
		strcpy(nexthop, "");
	}

	/* Use the string tokenizer to grab one line at a time */
	for (i=0; i<num_tokens(working_ignetcfg, '\n'); ++i) {
		extract_token(linebuf, working_ignetcfg, i, '\n', sizeof linebuf);
		extract_token(buf, linebuf, 0, '|', sizeof buf);
		if (!strcasecmp(buf, node)) {
			if (nexthop != NULL) {
				strcpy(nexthop, "");
			}
			if (secret != NULL) {
				extract_token(secret, linebuf, 1, '|', 256);
			}
			retval = 0;
		}
	}

	if (retval == 0) {
		return(retval);		/* yup, it's a direct neighbor */
	}

	/*	
	 * If we get to this point we have to see if we know the next hop
	 */
	if (the_netmap != NULL) {
		for (nmptr = the_netmap; nmptr != NULL; nmptr = nmptr->next) {
			if (!strcasecmp(nmptr->nodename, node)) {
				if (nexthop != NULL) {
					strcpy(nexthop, nmptr->nexthop);
				}
				return(0);
			}
		}
	}

	/*
	 * If we get to this point, the supplied node name is bogus.
	 */
	lprintf(CTDL_ERR, "Invalid node name <%s>\n", node);
	return(-1);
}





void cmd_gnet(char *argbuf) {
	char filename[SIZ];
	char buf[SIZ];
	FILE *fp;

	if (CtdlAccessCheck(ac_room_aide)) return;
	assoc_file_name(filename, sizeof filename, &CC->room, "netconfigs");
	cprintf("%d Network settings for room #%ld <%s>\n",
		LISTING_FOLLOWS,
		CC->room.QRnumber, CC->room.QRname);

	fp = fopen(filename, "r");
	if (fp != NULL) {
		while (fgets(buf, sizeof buf, fp) != NULL) {
			buf[strlen(buf)-1] = 0;
			cprintf("%s\n", buf);
		}
		fclose(fp);
	}

	cprintf("000\n");
}


void cmd_snet(char *argbuf) {
	char tempfilename[SIZ];
	char filename[SIZ];
	char buf[SIZ];
	FILE *fp;

	unbuffer_output();

	if (CtdlAccessCheck(ac_room_aide)) return;
	safestrncpy(tempfilename, tmpnam(NULL), sizeof tempfilename);
	assoc_file_name(filename, sizeof filename, &CC->room, "netconfigs");

	fp = fopen(tempfilename, "w");
	if (fp == NULL) {
		cprintf("%d Cannot open %s: %s\n",
			ERROR + INTERNAL_ERROR,
			tempfilename,
			strerror(errno));
	}

	cprintf("%d %s\n", SEND_LISTING, tempfilename);
	while (client_getln(buf, sizeof buf), strcmp(buf, "000")) {
		fprintf(fp, "%s\n", buf);
	}
	fclose(fp);

	/* Now copy the temp file to its permanent location
	 * (We use /bin/mv instead of link() because they may be on
	 * different filesystems)
	 */
	unlink(filename);
	snprintf(buf, sizeof buf, "/bin/mv %s %s", tempfilename, filename);
	begin_critical_section(S_NETCONFIGS);
	system(buf);
	end_critical_section(S_NETCONFIGS);
}


/*
 * Spools out one message from the list.
 */
void network_spool_msg(long msgnum, void *userdata) {
	struct SpoolControl *sc;
	int err;
	int i;
	char *newpath = NULL;
	char *instr = NULL;
	size_t instr_len = SIZ;
	struct CtdlMessage *msg = NULL;
	struct CtdlMessage *imsg;
	struct namelist *nptr;
	struct maplist *mptr;
	struct ser_ret sermsg;
	FILE *fp;
	char filename[SIZ];
	char buf[SIZ];
	int bang = 0;
	int send = 1;
	int delete_after_send = 0;	/* Set to 1 to delete after spooling */
	long list_msgnum = 0L;
	int ok_to_participate = 0;
	char *end_of_headers = NULL;

	sc = (struct SpoolControl *)userdata;

	/*
	 * Process mailing list recipients
	 */
	instr_len = SIZ;
	if (sc->listrecps != NULL) {
	
		/* First, copy it to the spoolout room */
		err = CtdlSaveMsgPointerInRoom(SMTP_SPOOLOUT_ROOM, msgnum, 0);
		if (err != 0) return;

		/* 
		 * Figure out how big a buffer we need to allocate
	 	 */
		for (nptr = sc->listrecps; nptr != NULL; nptr = nptr->next) {
			instr_len = instr_len + strlen(nptr->name);
		}
	
		/*
	 	 * allocate...
	 	 */
		lprintf(CTDL_DEBUG, "Generating delivery instructions\n");
		instr = malloc(instr_len);
		if (instr == NULL) {
			lprintf(CTDL_EMERG,
				"Cannot allocate %ld bytes for instr...\n",
				(long)instr_len);
			abort();
		}
		snprintf(instr, instr_len,
			"Content-type: %s\n\nmsgid|%ld\nsubmitted|%ld\n"
			"bounceto|postmaster@%s\n" ,
			SPOOLMIME, msgnum, (long)time(NULL), config.c_fqdn );
	
		/* Generate delivery instructions for each recipient */
		for (nptr = sc->listrecps; nptr != NULL; nptr = nptr->next) {
			size_t tmp = strlen(instr);
			snprintf(&instr[tmp], instr_len - tmp,
				 "remote|%s|0||\n", nptr->name);
		}
	
		/*
	 	 * Generate a message from the instructions
	 	 */
       		imsg = malloc(sizeof(struct CtdlMessage));
		memset(imsg, 0, sizeof(struct CtdlMessage));
		imsg->cm_magic = CTDLMESSAGE_MAGIC;
		imsg->cm_anon_type = MES_NORMAL;
		imsg->cm_format_type = FMT_RFC822;
		imsg->cm_fields['A'] = strdup("Citadel");
		imsg->cm_fields['M'] = instr;
	
		/* Save delivery instructions in spoolout room */
		CtdlSubmitMsg(imsg, NULL, SMTP_SPOOLOUT_ROOM);
		CtdlFreeMessage(imsg);
	}

	/*
	 * Process digest recipients
	 */
	if ((sc->digestrecps != NULL) && (sc->digestfp != NULL)) {
		msg = CtdlFetchMessage(msgnum, 1);
		if (msg != NULL) {
			fprintf(sc->digestfp,	" -----------------------------------"
						"------------------------------------"
						"-------\n");
			fprintf(sc->digestfp, "From: ");
			if (msg->cm_fields['A'] != NULL) {
				fprintf(sc->digestfp, "%s ", msg->cm_fields['A']);
			}
			if (msg->cm_fields['F'] != NULL) {
				fprintf(sc->digestfp, "<%s> ", msg->cm_fields['F']);
			}
			else if (msg->cm_fields['N'] != NULL) {
				fprintf(sc->digestfp, "@%s ", msg->cm_fields['N']);
			}
			fprintf(sc->digestfp, "\n");
			if (msg->cm_fields['U'] != NULL) {
				fprintf(sc->digestfp, "Subject: %s\n", msg->cm_fields['U']);
			}

			CC->redirect_buffer = malloc(SIZ);
			CC->redirect_len = 0;
			CC->redirect_alloc = SIZ;

			safestrncpy(CC->preferred_formats, "text/plain", sizeof CC->preferred_formats);
			CtdlOutputPreLoadedMsg(msg, 0L, MT_MIME, HEADERS_NONE, 0, 0);

			end_of_headers = bmstrstr(CC->redirect_buffer, "\n\r\n", strncmp);
			if (end_of_headers == NULL) {
				end_of_headers = bmstrstr(CC->redirect_buffer, "\n\n", strncmp);
			}
			if (end_of_headers == NULL) {
				end_of_headers = CC->redirect_buffer;
			}
			striplt(end_of_headers);
			fprintf(sc->digestfp, "\n%s\n", end_of_headers);

			free(CC->redirect_buffer);
			CC->redirect_buffer = NULL;
			CC->redirect_len = 0;
			CC->redirect_alloc = 0;

			sc->num_msgs_spooled += 1;
			free(msg);
		}
	}

	/*
	 * Process client-side list participations for this room
	 */
	instr_len = SIZ;
	if (sc->participates != NULL) {
		msg = CtdlFetchMessage(msgnum, 1);
		if (msg != NULL) {

			/* Only send messages which originated on our own Citadel
			 * network, otherwise we'll end up sending the remote
			 * mailing list's messages back to it, which is rude...
			 */
			ok_to_participate = 0;
			if (msg->cm_fields['N'] != NULL) {
				if (!strcasecmp(msg->cm_fields['N'], config.c_nodename)) {
					ok_to_participate = 1;
				}
				if (is_valid_node(NULL, NULL, msg->cm_fields['N']) == 0) {
					ok_to_participate = 1;
				}
			}
			if (ok_to_participate) {
				if (msg->cm_fields['F'] != NULL) {
					free(msg->cm_fields['F']);
				}
				msg->cm_fields['F'] = malloc(SIZ);
				/* Replace the Internet email address of the actual
			 	* author with the email address of the room itself,
			 	* so the remote listserv doesn't reject us.
			 	* FIXME ... I want to be able to pick any address
			 	*/
				snprintf(msg->cm_fields['F'], SIZ,
					"room_%s@%s", CC->room.QRname,
					config.c_fqdn);
				for (i=0; i<strlen(msg->cm_fields['F']); ++i) {
					if (isspace(msg->cm_fields['F'][i])) {
						msg->cm_fields['F'][i] = '_';
					}
				}

				/* Now save it and generate delivery instructions */
				list_msgnum = CtdlSubmitMsg(msg, NULL, SMTP_SPOOLOUT_ROOM);

				/* 
				 * Figure out how big a buffer we need to allocate
			 	 */
				for (nptr = sc->participates; nptr != NULL; nptr = nptr->next) {
					instr_len = instr_len + strlen(nptr->name);
				}
			
				/*
			 	 * allocate...
	 	 		 */
				instr = malloc(instr_len);
				if (instr == NULL) {
					lprintf(CTDL_EMERG,
						"Cannot allocate %ld bytes for instr...\n",
						(long)instr_len);
					abort();
				}
				snprintf(instr, instr_len,
					"Content-type: %s\n\nmsgid|%ld\nsubmitted|%ld\n"
					"bounceto|postmaster@%s\n" ,
					SPOOLMIME, list_msgnum, (long)time(NULL), config.c_fqdn );
			
				/* Generate delivery instructions for each recipient */
				for (nptr = sc->participates; nptr != NULL; nptr = nptr->next) {
					size_t tmp = strlen(instr);
					snprintf(&instr[tmp], instr_len - tmp,
						 "remote|%s|0||\n", nptr->name);
				}
			
				/*
			 	 * Generate a message from the instructions
	 			 */
     		  		imsg = malloc(sizeof(struct CtdlMessage));
				memset(imsg, 0, sizeof(struct CtdlMessage));
				imsg->cm_magic = CTDLMESSAGE_MAGIC;
				imsg->cm_anon_type = MES_NORMAL;
				imsg->cm_format_type = FMT_RFC822;
				imsg->cm_fields['A'] = strdup("Citadel");
				imsg->cm_fields['M'] = instr;
			
				/* Save delivery instructions in spoolout room */
				CtdlSubmitMsg(imsg, NULL, SMTP_SPOOLOUT_ROOM);
				CtdlFreeMessage(imsg);
			}
			CtdlFreeMessage(msg);
		}
	}
	
	/*
	 * Process IGnet push shares
	 */
	if (sc->ignet_push_shares != NULL) {
	
		msg = CtdlFetchMessage(msgnum, 1);
		if (msg != NULL) {
			size_t newpath_len;

			/* Prepend our node name to the Path field whenever
			 * sending a message to another IGnet node
			 */
			if (msg->cm_fields['P'] == NULL) {
				msg->cm_fields['P'] = strdup("username");
			}
			newpath_len = strlen(msg->cm_fields['P']) +
				 strlen(config.c_nodename) + 2;
			newpath = malloc(newpath_len);
			snprintf(newpath, newpath_len, "%s!%s",
				 config.c_nodename, msg->cm_fields['P']);
			free(msg->cm_fields['P']);
			msg->cm_fields['P'] = newpath;

			/*
			 * Determine if this message is set to be deleted
			 * after sending out on the network
			 */
			if (msg->cm_fields['S'] != NULL) {
				if (!strcasecmp(msg->cm_fields['S'],
				   "CANCEL")) {
					delete_after_send = 1;
				}
			}

			/* Now send it to every node */
			for (mptr = sc->ignet_push_shares; mptr != NULL;
			    mptr = mptr->next) {

				send = 1;

				/* Check for valid node name */
				if (is_valid_node(NULL, NULL, mptr->remote_nodename) != 0) {
					lprintf(CTDL_ERR, "Invalid node <%s>\n",
						mptr->remote_nodename);
					send = 0;
				}

				/* Check for split horizon */
				lprintf(CTDL_DEBUG, "Path is %s\n", msg->cm_fields['P']);
				bang = num_tokens(msg->cm_fields['P'], '!');
				if (bang > 1) for (i=0; i<(bang-1); ++i) {
					extract_token(buf, msg->cm_fields['P'],
						i, '!', sizeof buf);
					if (!strcasecmp(buf, mptr->remote_nodename)) {
						send = 0;
					}
				}

				/* Send the message */
				if (send == 1) {

					/*
					 * Force the message to appear in the correct room
					 * on the far end by setting the C field correctly
					 */
					if (msg->cm_fields['C'] != NULL) {
						free(msg->cm_fields['C']);
					}
					if (strlen(mptr->remote_roomname) > 0) {
						msg->cm_fields['C'] = strdup(mptr->remote_roomname);
					}
					else {
						msg->cm_fields['C'] = strdup(CC->room.QRname);
					}

					/* serialize it for transmission */
					serialize_message(&sermsg, msg);

					/* write it to the spool file */
					snprintf(filename, sizeof filename,
						"./network/spoolout/%s",
						mptr->remote_nodename);
					fp = fopen(filename, "ab");
					if (fp != NULL) {
						fwrite(sermsg.ser,
							sermsg.len, 1, fp);
						fclose(fp);
					}

					/* free the serialized version */
					free(sermsg.ser);
				}
			}
			CtdlFreeMessage(msg);
		}
	}

	/* update lastsent */
	sc->lastsent = msgnum;

	/* Delete this message if delete-after-send is set */
	if (delete_after_send) {
		CtdlDeleteMessages(CC->room.QRname, msgnum, "");
	}

}
	

/*
 * Deliver digest messages
 */
void network_deliver_digest(struct SpoolControl *sc) {
	char buf[SIZ];
	int i;
	struct CtdlMessage *msg;
	long msglen;
	long msgnum;
	char *instr = NULL;
	size_t instr_len = SIZ;
	struct CtdlMessage *imsg;
	struct namelist *nptr;

	if (sc->num_msgs_spooled < 1) {
		fclose(sc->digestfp);
		sc->digestfp = NULL;
		return;
	}

	msg = malloc(sizeof(struct CtdlMessage));
	memset(msg, 0, sizeof(struct CtdlMessage));
	msg->cm_magic = CTDLMESSAGE_MAGIC;
	msg->cm_format_type = FMT_RFC822;
	msg->cm_anon_type = MES_NORMAL;

	sprintf(buf, "%ld", time(NULL));
	msg->cm_fields['T'] = strdup(buf);
	msg->cm_fields['A'] = strdup(CC->room.QRname);
	msg->cm_fields['U'] = strdup(CC->room.QRname);
	sprintf(buf, "room_%s@%s", CC->room.QRname, config.c_fqdn);
	for (i=0; i<strlen(buf); ++i) {
		if (isspace(buf[i])) buf[i]='_';
		buf[i] = tolower(buf[i]);
	}
	msg->cm_fields['F'] = strdup(buf);

	fseek(sc->digestfp, 0L, SEEK_END);
	msglen = ftell(sc->digestfp);

	msg->cm_fields['M'] = malloc(msglen + 1);
	fseek(sc->digestfp, 0L, SEEK_SET);
	fread(msg->cm_fields['M'], (size_t)msglen, 1, sc->digestfp);
	msg->cm_fields['M'][msglen] = 0;

	fclose(sc->digestfp);
	sc->digestfp = NULL;

	msgnum = CtdlSubmitMsg(msg, NULL, SMTP_SPOOLOUT_ROOM);
	CtdlFreeMessage(msg);

	/* Now generate the delivery instructions */

	/* 
	 * Figure out how big a buffer we need to allocate
	 */
	for (nptr = sc->digestrecps; nptr != NULL; nptr = nptr->next) {
		instr_len = instr_len + strlen(nptr->name);
	}
	
	/*
 	 * allocate...
 	 */
	lprintf(CTDL_DEBUG, "Generating delivery instructions\n");
	instr = malloc(instr_len);
	if (instr == NULL) {
		lprintf(CTDL_EMERG, "Cannot allocate %ld bytes for instr...\n",
			(long)instr_len);
		abort();
	}
	snprintf(instr, instr_len,
		"Content-type: %s\n\nmsgid|%ld\nsubmitted|%ld\n"
		"bounceto|postmaster@%s\n" ,
		SPOOLMIME, msgnum, (long)time(NULL), config.c_fqdn );

	/* Generate delivery instructions for each recipient */
	for (nptr = sc->digestrecps; nptr != NULL; nptr = nptr->next) {
		size_t tmp = strlen(instr);
		snprintf(&instr[tmp], instr_len - tmp,
			 "remote|%s|0||\n", nptr->name);
	}

	/*
 	 * Generate a message from the instructions
 	 */
  	imsg = malloc(sizeof(struct CtdlMessage));
	memset(imsg, 0, sizeof(struct CtdlMessage));
	imsg->cm_magic = CTDLMESSAGE_MAGIC;
	imsg->cm_anon_type = MES_NORMAL;
	imsg->cm_format_type = FMT_RFC822;
	imsg->cm_fields['A'] = strdup("Citadel");
	imsg->cm_fields['M'] = instr;

	/* Save delivery instructions in spoolout room */
	CtdlSubmitMsg(imsg, NULL, SMTP_SPOOLOUT_ROOM);
	CtdlFreeMessage(imsg);
}


/*
 * Batch up and send all outbound traffic from the current room
 */
void network_spoolout_room(char *room_to_spool) {
	char filename[SIZ];
	char buf[SIZ];
	char instr[SIZ];
	char nodename[256];
	char roomname[ROOMNAMELEN];
	char nexthop[256];
	FILE *fp;
	struct SpoolControl sc;
	struct namelist *nptr = NULL;
	struct maplist *mptr = NULL;
	size_t miscsize = 0;
	size_t linesize = 0;
	int skipthisline = 0;
	int i;

	if (getroom(&CC->room, room_to_spool) != 0) {
		lprintf(CTDL_CRIT, "ERROR: cannot load <%s>\n", room_to_spool);
		return;
	}

	memset(&sc, 0, sizeof(struct SpoolControl));
	assoc_file_name(filename, sizeof filename, &CC->room, "netconfigs");

	begin_critical_section(S_NETCONFIGS);

	fp = fopen(filename, "r");
	if (fp == NULL) {
		end_critical_section(S_NETCONFIGS);
		return;
	}

	lprintf(CTDL_INFO, "Networking started for <%s>\n", CC->room.QRname);

	while (fgets(buf, sizeof buf, fp) != NULL) {
		buf[strlen(buf)-1] = 0;

		extract_token(instr, buf, 0, '|', sizeof instr);
		if (!strcasecmp(instr, "lastsent")) {
			sc.lastsent = extract_long(buf, 1);
		}
		else if (!strcasecmp(instr, "listrecp")) {
			nptr = (struct namelist *)
				malloc(sizeof(struct namelist));
			nptr->next = sc.listrecps;
			extract_token(nptr->name, buf, 1, '|', sizeof nptr->name);
			sc.listrecps = nptr;
		}
		else if (!strcasecmp(instr, "participate")) {
			nptr = (struct namelist *)
				malloc(sizeof(struct namelist));
			nptr->next = sc.participates;
			extract_token(nptr->name, buf, 1, '|', sizeof nptr->name);
			sc.participates = nptr;
		}
		else if (!strcasecmp(instr, "digestrecp")) {
			nptr = (struct namelist *)
				malloc(sizeof(struct namelist));
			nptr->next = sc.digestrecps;
			extract_token(nptr->name, buf, 1, '|', sizeof nptr->name);
			sc.digestrecps = nptr;
		}
		else if (!strcasecmp(instr, "ignet_push_share")) {
			/* by checking each node's validity, we automatically
			 * purge nodes which do not exist from room network
			 * configurations at this time.
			 */
			extract_token(nodename, buf, 1, '|', sizeof nodename);
			extract_token(roomname, buf, 2, '|', sizeof roomname);
			strcpy(nexthop, "xxx");
			if (is_valid_node(nexthop, NULL, nodename) == 0) {
				if (strlen(nexthop) == 0) {
					mptr = (struct maplist *)
						malloc(sizeof(struct maplist));
					mptr->next = sc.ignet_push_shares;
					strcpy(mptr->remote_nodename, nodename);
					strcpy(mptr->remote_roomname, roomname);
					sc.ignet_push_shares = mptr;
				}
			}
		}
		else {
			/* Preserve 'other' lines ... *unless* they happen to
			 * be subscribe/unsubscribe pendings with expired
			 * timestamps.
			 */
			skipthisline = 0;
			if (!strncasecmp(buf, "subpending|", 11)) {
				if (time(NULL) - extract_long(buf, 4) > EXP) {
					skipthisline = 1;
				}
			}
			if (!strncasecmp(buf, "unsubpending|", 13)) {
				if (time(NULL) - extract_long(buf, 3) > EXP) {
					skipthisline = 1;
				}
			}

			if (skipthisline == 0) {
				linesize = strlen(buf);
				sc.misc = realloc(sc.misc,
					(miscsize + linesize + 2) );
				sprintf(&sc.misc[miscsize], "%s\n", buf);
				miscsize = miscsize + linesize + 1;
			}
		}


	}
	fclose(fp);

	/* If there are digest recipients, we have to build a digest */
	if (sc.digestrecps != NULL) {
		sc.digestfp = tmpfile();
		fprintf(sc.digestfp, "Content-type: text/plain\n\n");
	}

	/* Do something useful */
	CtdlForEachMessage(MSGS_GT, sc.lastsent, NULL, NULL,
		network_spool_msg, &sc);

	/* If we wrote a digest, deliver it and then close it */
	snprintf(buf, sizeof buf, "room_%s@%s",
		CC->room.QRname, config.c_fqdn);
	for (i=0; i<strlen(buf); ++i) {
		buf[i] = tolower(buf[i]);
		if (isspace(buf[i])) buf[i] = '_';
	}
	if (sc.digestfp != NULL) {
		fprintf(sc.digestfp,	" -----------------------------------"
					"------------------------------------"
					"-------\n"
					"You are subscribed to the '%s' "
					"list.\n"
					"To post to the list: %s\n",
					CC->room.QRname, buf
		);
		network_deliver_digest(&sc);	/* deliver and close */
	}

	/* Now rewrite the config file */
	fp = fopen(filename, "w");
	if (fp == NULL) {
		lprintf(CTDL_CRIT, "ERROR: cannot open %s: %s\n",
			filename, strerror(errno));
	}
	else {
		fprintf(fp, "lastsent|%ld\n", sc.lastsent);

		/* Write out the listrecps while freeing from memory at the
		 * same time.  Am I clever or what?  :)
		 */
		while (sc.listrecps != NULL) {
			fprintf(fp, "listrecp|%s\n", sc.listrecps->name);
			nptr = sc.listrecps->next;
			free(sc.listrecps);
			sc.listrecps = nptr;
		}
		/* Do the same for digestrecps */
		while (sc.digestrecps != NULL) {
			fprintf(fp, "digestrecp|%s\n", sc.digestrecps->name);
			nptr = sc.digestrecps->next;
			free(sc.digestrecps);
			sc.digestrecps = nptr;
		}
		/* Do the same for participates */
		while (sc.participates != NULL) {
			fprintf(fp, "participate|%s\n", sc.participates->name);
			nptr = sc.participates->next;
			free(sc.participates);
			sc.participates = nptr;
		}
		while (sc.ignet_push_shares != NULL) {
			/* by checking each node's validity, we automatically
			 * purge nodes which do not exist from room network
			 * configurations at this time.
			 */
			if (is_valid_node(NULL, NULL, sc.ignet_push_shares->remote_nodename) == 0) {
			}
			fprintf(fp, "ignet_push_share|%s",
				sc.ignet_push_shares->remote_nodename);
			if (strlen(sc.ignet_push_shares->remote_roomname) > 0) {
				fprintf(fp, "|%s", sc.ignet_push_shares->remote_roomname);
			}
			fprintf(fp, "\n");
			mptr = sc.ignet_push_shares->next;
			free(sc.ignet_push_shares);
			sc.ignet_push_shares = mptr;
		}
		if (sc.misc != NULL) {
			fwrite(sc.misc, strlen(sc.misc), 1, fp);
		}
		free(sc.misc);

		fclose(fp);
	}
	end_critical_section(S_NETCONFIGS);
}



/*
 * Send the *entire* contents of the current room to one specific network node,
 * ignoring anything we know about which messages have already undergone
 * network processing.  This can be used to bring a new node into sync.
 */
int network_sync_to(char *target_node) {
	struct SpoolControl sc;
	int num_spooled = 0;
	int found_node = 0;
	char buf[256];
	char sc_type[256];
	char sc_node[256];
	char sc_room[256];
	char filename[256];
	FILE *fp;

	/* Grab the configuration line we're looking for */
	assoc_file_name(filename, sizeof filename, &CC->room, "netconfigs");
	begin_critical_section(S_NETCONFIGS);
	fp = fopen(filename, "r");
	if (fp == NULL) {
		end_critical_section(S_NETCONFIGS);
		return(-1);
	}
	while (fgets(buf, sizeof buf, fp) != NULL) {
		buf[strlen(buf)-1] = 0;
		extract_token(sc_type, buf, 0, '|', sizeof sc_type);
		extract_token(sc_node, buf, 1, '|', sizeof sc_node);
		extract_token(sc_room, buf, 2, '|', sizeof sc_room);
		if ( (!strcasecmp(sc_type, "ignet_push_share"))
		   && (!strcasecmp(sc_node, target_node)) ) {
			found_node = 1;
			
			/* Concise syntax because we don't need a full linked-list */
			memset(&sc, 0, sizeof(struct SpoolControl));
			sc.ignet_push_shares = (struct maplist *)
				malloc(sizeof(struct maplist));
			sc.ignet_push_shares->next = NULL;
			safestrncpy(sc.ignet_push_shares->remote_nodename,
				sc_node,
				sizeof sc.ignet_push_shares->remote_nodename);
			safestrncpy(sc.ignet_push_shares->remote_roomname,
				sc_room,
				sizeof sc.ignet_push_shares->remote_roomname);
		}
	}
	fclose(fp);
	end_critical_section(S_NETCONFIGS);

	if (!found_node) return(-1);

	/* Send ALL messages */
	num_spooled = CtdlForEachMessage(MSGS_ALL, 0L, NULL, NULL,
		network_spool_msg, &sc);

	/* Concise cleanup because we know there's only one node in the sc */
	free(sc.ignet_push_shares);

	lprintf(CTDL_NOTICE, "Synchronized %d messages to <%s>\n",
		num_spooled, target_node);
	return(num_spooled);
}


/*
 * Implements the NSYN command
 */
void cmd_nsyn(char *argbuf) {
	int num_spooled;
	char target_node[256];

	if (CtdlAccessCheck(ac_aide)) return;

	extract_token(target_node, argbuf, 0, '|', sizeof target_node);
	num_spooled = network_sync_to(target_node);
	if (num_spooled >= 0) {
		cprintf("%d Spooled %d messages.\n", CIT_OK, num_spooled);
	}
	else {
		cprintf("%d No such room/node share exists.\n",
			ERROR + ROOM_NOT_FOUND);
	}
}



/*
 * Batch up and send all outbound traffic from the current room
 */
void network_queue_room(struct ctdlroom *qrbuf, void *data) {
	struct RoomProcList *ptr;

	ptr = (struct RoomProcList *) malloc(sizeof (struct RoomProcList));
	if (ptr == NULL) return;

	safestrncpy(ptr->name, qrbuf->QRname, sizeof ptr->name);
	ptr->next = rplist;
	rplist = ptr;
}


/*
 * Learn topology from path fields
 */
void network_learn_topology(char *node, char *path) {
	char nexthop[256];
	struct NetMap *nmptr;

	strcpy(nexthop, "");

	if (num_tokens(path, '!') < 3) return;
	for (nmptr = the_netmap; nmptr != NULL; nmptr = nmptr->next) {
		if (!strcasecmp(nmptr->nodename, node)) {
			extract_token(nmptr->nexthop, path, 0, '!', sizeof nmptr->nexthop);
			nmptr->lastcontact = time(NULL);
			++netmap_changed;
			return;
		}
	}

	/* If we got here then it's not in the map, so add it. */
	nmptr = (struct NetMap *) malloc(sizeof (struct NetMap));
	strcpy(nmptr->nodename, node);
	nmptr->lastcontact = time(NULL);
	extract_token(nmptr->nexthop, path, 0, '!', sizeof nmptr->nexthop);
	nmptr->next = the_netmap;
	the_netmap = nmptr;
	++netmap_changed;
}




/*
 * Bounce a message back to the sender
 */
void network_bounce(struct CtdlMessage *msg, char *reason) {
	char *oldpath = NULL;
	char buf[SIZ];
	char bouncesource[SIZ];
	char recipient[SIZ];
	struct recptypes *valid = NULL;
	char force_room[ROOMNAMELEN];
	static int serialnum = 0;
	size_t size;

	lprintf(CTDL_DEBUG, "entering network_bounce()\n");

	if (msg == NULL) return;

	snprintf(bouncesource, sizeof bouncesource, "%s@%s", BOUNCESOURCE, config.c_nodename);

	/* 
	 * Give it a fresh message ID
	 */
	if (msg->cm_fields['I'] != NULL) {
		free(msg->cm_fields['I']);
	}
	snprintf(buf, sizeof buf, "%ld.%04lx.%04x@%s",
		(long)time(NULL), (long)getpid(), ++serialnum, config.c_fqdn);
	msg->cm_fields['I'] = strdup(buf);

	/*
	 * FIXME ... right now we're just sending a bounce; we really want to
	 * include the text of the bounced message.
	 */
	if (msg->cm_fields['M'] != NULL) {
		free(msg->cm_fields['M']);
	}
	msg->cm_fields['M'] = strdup(reason);
	msg->cm_format_type = 0;

	/*
	 * Turn the message around
	 */
	if (msg->cm_fields['R'] == NULL) {
		free(msg->cm_fields['R']);
	}

	if (msg->cm_fields['D'] == NULL) {
		free(msg->cm_fields['D']);
	}

	snprintf(recipient, sizeof recipient, "%s@%s",
		msg->cm_fields['A'], msg->cm_fields['N']);

	if (msg->cm_fields['A'] == NULL) {
		free(msg->cm_fields['A']);
	}

	if (msg->cm_fields['N'] == NULL) {
		free(msg->cm_fields['N']);
	}

	if (msg->cm_fields['U'] == NULL) {
		free(msg->cm_fields['U']);
	}

	msg->cm_fields['A'] = strdup(BOUNCESOURCE);
	msg->cm_fields['N'] = strdup(config.c_nodename);
	msg->cm_fields['U'] = strdup("Delivery Status Notification (Failure)");

	/* prepend our node to the path */
	if (msg->cm_fields['P'] != NULL) {
		oldpath = msg->cm_fields['P'];
		msg->cm_fields['P'] = NULL;
	}
	else {
		oldpath = strdup("unknown_user");
	}
	size = strlen(oldpath) + SIZ;
	msg->cm_fields['P'] = malloc(size);
	snprintf(msg->cm_fields['P'], size, "%s!%s", config.c_nodename, oldpath);
	free(oldpath);

	/* Now submit the message */
	valid = validate_recipients(recipient);
	if (valid != NULL) if (valid->num_error > 0) {
		free(valid);
		valid = NULL;
	}
	if ( (valid == NULL) || (!strcasecmp(recipient, bouncesource)) ) {
		strcpy(force_room, config.c_aideroom);
	}
	else {
		strcpy(force_room, "");
	}
	if ( (valid == NULL) && (strlen(force_room) == 0) ) {
		strcpy(force_room, config.c_aideroom);
	}
	CtdlSubmitMsg(msg, valid, force_room);

	/* Clean up */
	if (valid != NULL) free(valid);
	CtdlFreeMessage(msg);
	lprintf(CTDL_DEBUG, "leaving network_bounce()\n");
}




/*
 * Process a buffer containing a single message from a single file
 * from the inbound queue 
 */
void network_process_buffer(char *buffer, long size) {
	struct CtdlMessage *msg;
	long pos;
	int field;
	struct recptypes *recp = NULL;
	char target_room[ROOMNAMELEN];
	struct ser_ret sermsg;
	char *oldpath = NULL;
	char filename[SIZ];
	FILE *fp;
	char nexthop[SIZ];
	unsigned char firstbyte;
	unsigned char lastbyte;

	/* Validate just a little bit.  First byte should be FF and
	 * last byte should be 00.
	 */
	memcpy(&firstbyte, &buffer[0], 1);
	memcpy(&lastbyte, &buffer[size-1], 1);
	if ( (firstbyte != 255) || (lastbyte != 0) ) {
		lprintf(CTDL_ERR, "Corrupt message!  Ignoring.\n");
		return;
	}

	/* Set default target room to trash */
	strcpy(target_room, TWITROOM);

	/* Load the message into memory */
	msg = (struct CtdlMessage *) malloc(sizeof(struct CtdlMessage));
	memset(msg, 0, sizeof(struct CtdlMessage));
	msg->cm_magic = CTDLMESSAGE_MAGIC;
	msg->cm_anon_type = buffer[1];
	msg->cm_format_type = buffer[2];

	for (pos = 3; pos < size; ++pos) {
		field = buffer[pos];
		msg->cm_fields[field] = strdup(&buffer[pos+1]);
		pos = pos + strlen(&buffer[(int)pos]);
	}

	/* Check for message routing */
	if (msg->cm_fields['D'] != NULL) {
		if (strcasecmp(msg->cm_fields['D'], config.c_nodename)) {

			/* route the message */
			strcpy(nexthop, "");
			if (is_valid_node(nexthop, NULL,
			   msg->cm_fields['D']) == 0) {

				/* prepend our node to the path */
				if (msg->cm_fields['P'] != NULL) {
					oldpath = msg->cm_fields['P'];
					msg->cm_fields['P'] = NULL;
				}
				else {
					oldpath = strdup("unknown_user");
				}
				size = strlen(oldpath) + SIZ;
				msg->cm_fields['P'] = malloc(size);
				snprintf(msg->cm_fields['P'], size, "%s!%s",
					config.c_nodename, oldpath);
				free(oldpath);

				/* serialize the message */
				serialize_message(&sermsg, msg);

				/* now send it */
				if (strlen(nexthop) == 0) {
					strcpy(nexthop, msg->cm_fields['D']);
				}
				snprintf(filename, sizeof filename,
					"./network/spoolout/%s", nexthop);
				fp = fopen(filename, "ab");
				if (fp != NULL) {
					fwrite(sermsg.ser,
						sermsg.len, 1, fp);
					fclose(fp);
				}
				free(sermsg.ser);
				CtdlFreeMessage(msg);
				return;
			}
			
			else {	/* invalid destination node name */

				network_bounce(msg,
"A message you sent could not be delivered due to an invalid destination node"
" name.  Please check the address and try sending the message again.\n");
				msg = NULL;
				return;

			}
		}
	}

	/*
	 * Check to see if we already have a copy of this message, and
	 * abort its processing if so.  (We used to post a warning to Aide>
	 * every time this happened, but the network is now so densely
	 * connected that it's inevitable.)
	 */
	if (network_usetable(msg) != 0) {
		return;
	}

	/* Learn network topology from the path */
	if ((msg->cm_fields['N'] != NULL) && (msg->cm_fields['P'] != NULL)) {
		network_learn_topology(msg->cm_fields['N'], 
					msg->cm_fields['P']);
	}

	/* Is the sending node giving us a very persuasive suggestion about
	 * which room this message should be saved in?  If so, go with that.
	 */
	if (msg->cm_fields['C'] != NULL) {
		safestrncpy(target_room,
			msg->cm_fields['C'],
			sizeof target_room);
	}

	/* Otherwise, does it have a recipient?  If so, validate it... */
	else if (msg->cm_fields['R'] != NULL) {
		recp = validate_recipients(msg->cm_fields['R']);
		if (recp != NULL) if (recp->num_error > 0) {
			network_bounce(msg,
"A message you sent could not be delivered due to an invalid address.\n"
"Please check the address and try sending the message again.\n");
			msg = NULL;
			free(recp);
			return;
		}
		strcpy(target_room, "");	/* no target room if mail */
	}

	/* Our last shot at finding a home for this message is to see if
	 * it has the O field (Originating room) set.
	 */
	else if (msg->cm_fields['O'] != NULL) {
		safestrncpy(target_room,
			msg->cm_fields['O'],
			sizeof target_room);
	}

	/* Strip out fields that are only relevant during transit */
	if (msg->cm_fields['D'] != NULL) {
		free(msg->cm_fields['D']);
		msg->cm_fields['D'] = NULL;
	}
	if (msg->cm_fields['C'] != NULL) {
		free(msg->cm_fields['C']);
		msg->cm_fields['C'] = NULL;
	}

	/* save the message into a room */
	if (PerformNetprocHooks(msg, target_room) == 0) {
		msg->cm_flags = CM_SKIP_HOOKS;
		CtdlSubmitMsg(msg, recp, target_room);
	}
	CtdlFreeMessage(msg);
	free(recp);
}


/*
 * Process a single message from a single file from the inbound queue 
 */
void network_process_message(FILE *fp, long msgstart, long msgend) {
	long hold_pos;
	long size;
	char *buffer;

	hold_pos = ftell(fp);
	size = msgend - msgstart + 1;
	buffer = malloc(size);
	if (buffer != NULL) {
		fseek(fp, msgstart, SEEK_SET);
		fread(buffer, size, 1, fp);
		network_process_buffer(buffer, size);
		free(buffer);
	}

	fseek(fp, hold_pos, SEEK_SET);
}


/*
 * Process a single file from the inbound queue 
 */
void network_process_file(char *filename) {
	FILE *fp;
	long msgstart = (-1L);
	long msgend = (-1L);
	long msgcur = 0L;
	int ch;


	fp = fopen(filename, "rb");
	if (fp == NULL) {
		lprintf(CTDL_CRIT, "Error opening %s: %s\n",
			filename, strerror(errno));
		return;
	}

	lprintf(CTDL_INFO, "network: processing <%s>\n", filename);

	/* Look for messages in the data stream and break them out */
	while (ch = getc(fp), ch >= 0) {
	
		if (ch == 255) {
			if (msgstart >= 0L) {
				msgend = msgcur - 1;
				network_process_message(fp, msgstart, msgend);
			}
			msgstart = msgcur;
		}

		++msgcur;
	}

	msgend = msgcur - 1;
	if (msgstart >= 0L) {
		network_process_message(fp, msgstart, msgend);
	}

	fclose(fp);
	unlink(filename);
}


/*
 * Process anything in the inbound queue
 */
void network_do_spoolin(void) {
	DIR *dp;
	struct dirent *d;
	char filename[256];

	dp = opendir("./network/spoolin");
	if (dp == NULL) return;

	while (d = readdir(dp), d != NULL) {
		if ((strcmp(d->d_name, ".")) && (strcmp(d->d_name, ".."))) {
			snprintf(filename, sizeof filename,
				"./network/spoolin/%s", d->d_name);
			network_process_file(filename);
		}
	}

	closedir(dp);
}


/*
 * Delete any files in the outbound queue that were intended
 * to be sent to nodes which no longer exist.
 */
void network_purge_spoolout(void) {
	DIR *dp;
	struct dirent *d;
	char filename[256];
	char nexthop[256];
	int i;

	dp = opendir("./network/spoolout");
	if (dp == NULL) return;

	while (d = readdir(dp), d != NULL) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;
		snprintf(filename, sizeof filename,
			"./network/spoolout/%s", d->d_name);

		strcpy(nexthop, "");
		i = is_valid_node(nexthop, NULL, d->d_name);
	
		if ( (i != 0) || (strlen(nexthop) > 0) ) {
			unlink(filename);
		}
	}


	closedir(dp);
}



/*
 * receive network spool from the remote system
 */
void receive_spool(int sock, char *remote_nodename) {
	long download_len;
	long bytes_received;
	char buf[SIZ];
	static char pbuf[IGNET_PACKET_SIZE];
	char tempfilename[PATH_MAX];
	long plen;
	FILE *fp;

	strcpy(tempfilename, tmpnam(NULL));
	if (sock_puts(sock, "NDOP") < 0) return;
	if (sock_gets(sock, buf) < 0) return;
	lprintf(CTDL_DEBUG, "<%s\n", buf);
	if (buf[0] != '2') {
		return;
	}
	download_len = extract_long(&buf[4], 0);

	bytes_received = 0L;
	fp = fopen(tempfilename, "w");
	if (fp == NULL) {
		lprintf(CTDL_CRIT, "cannot open download file locally: %s\n",
			strerror(errno));
		return;
	}

	while (bytes_received < download_len) {
		snprintf(buf, sizeof buf, "READ %ld|%ld",
			bytes_received,
		     ((download_len - bytes_received > IGNET_PACKET_SIZE)
		 ? IGNET_PACKET_SIZE : (download_len - bytes_received)));
		if (sock_puts(sock, buf) < 0) {
			fclose(fp);
			unlink(tempfilename);
			return;
		}
		if (sock_gets(sock, buf) < 0) {
			fclose(fp);
			unlink(tempfilename);
			return;
		}
		if (buf[0] == '6') {
			plen = extract_long(&buf[4], 0);
			if (sock_read(sock, pbuf, plen) < 0) {
				fclose(fp);
				unlink(tempfilename);
				return;
			}
			fwrite((char *) pbuf, plen, 1, fp);
			bytes_received = bytes_received + plen;
		}
	}

	fclose(fp);
	if (sock_puts(sock, "CLOS") < 0) {
		unlink(tempfilename);
		return;
	}
	if (sock_gets(sock, buf) < 0) {
		unlink(tempfilename);
		return;
	}
	if (download_len > 0)
		lprintf(CTDL_NOTICE, "Received %ld octets from <%s>",
				download_len, remote_nodename);
	lprintf(CTDL_DEBUG, "%s", buf);
	snprintf(buf, sizeof buf, "mv %s ./network/spoolin/%s.%ld",
		tempfilename, remote_nodename, (long) getpid());
	system(buf);
}



/*
 * transmit network spool to the remote system
 */
void transmit_spool(int sock, char *remote_nodename)
{
	char buf[SIZ];
	char pbuf[4096];
	long plen;
	long bytes_to_write, thisblock, bytes_written;
	int fd;
	char sfname[128];

	if (sock_puts(sock, "NUOP") < 0) return;
	if (sock_gets(sock, buf) < 0) return;
	lprintf(CTDL_DEBUG, "<%s\n", buf);
	if (buf[0] != '2') {
		return;
	}

	snprintf(sfname, sizeof sfname, "./network/spoolout/%s", remote_nodename);
	fd = open(sfname, O_RDONLY);
	if (fd < 0) {
		if (errno != ENOENT) {
			lprintf(CTDL_CRIT, "cannot open upload file locally: %s\n",
				strerror(errno));
		}
		return;
	}
	bytes_written = 0;
	while (plen = (long) read(fd, pbuf, IGNET_PACKET_SIZE), plen > 0L) {
		bytes_to_write = plen;
		while (bytes_to_write > 0L) {
			snprintf(buf, sizeof buf, "WRIT %ld", bytes_to_write);
			if (sock_puts(sock, buf) < 0) {
				close(fd);
				return;
			}
			if (sock_gets(sock, buf) < 0) {
				close(fd);
				return;
			}
			thisblock = atol(&buf[4]);
			if (buf[0] == '7') {
				if (sock_write(sock, pbuf,
				   (int) thisblock) < 0) {
					close(fd);
					return;
				}
				bytes_to_write -= thisblock;
				bytes_written += thisblock;
			} else {
				goto ABORTUPL;
			}
		}
	}

ABORTUPL:
	close(fd);
	if (sock_puts(sock, "UCLS 1") < 0) return;
	if (sock_gets(sock, buf) < 0) return;
	lprintf(CTDL_NOTICE, "Sent %ld octets to <%s>",
			bytes_written, remote_nodename);
	lprintf(CTDL_DEBUG, "<%s\n", buf);
	if (buf[0] == '2') {
		unlink(sfname);
	}
}



/*
 * Poll one Citadel node (called by network_poll_other_citadel_nodes() below)
 */
void network_poll_node(char *node, char *secret, char *host, char *port) {
	int sock;
	char buf[SIZ];

	if (network_talking_to(node, NTT_CHECK)) return;
	network_talking_to(node, NTT_ADD);
	lprintf(CTDL_NOTICE, "Connecting to <%s> at %s:%s\n", node, host, port);

	sock = sock_connect(host, port, "tcp");
	if (sock < 0) {
		lprintf(CTDL_ERR, "Could not connect: %s\n", strerror(errno));
		network_talking_to(node, NTT_REMOVE);
		return;
	}
	
	lprintf(CTDL_DEBUG, "Connected!\n");

	/* Read the server greeting */
	if (sock_gets(sock, buf) < 0) goto bail;
	lprintf(CTDL_DEBUG, ">%s\n", buf);

	/* Identify ourselves */
	snprintf(buf, sizeof buf, "NETP %s|%s", config.c_nodename, secret);
	lprintf(CTDL_DEBUG, "<%s\n", buf);
	if (sock_puts(sock, buf) <0) goto bail;
	if (sock_gets(sock, buf) < 0) goto bail;
	lprintf(CTDL_DEBUG, ">%s\n", buf);
	if (buf[0] != '2') goto bail;

	/* At this point we are authenticated. */
	receive_spool(sock, node);
	transmit_spool(sock, node);

	sock_puts(sock, "QUIT");
bail:	sock_close(sock);
	network_talking_to(node, NTT_REMOVE);
}



/*
 * Poll other Citadel nodes and transfer inbound/outbound network data.
 * Set "full" to nonzero to force a poll of every node, or to zero to poll
 * only nodes to which we have data to send.
 */
void network_poll_other_citadel_nodes(int full_poll) {
	int i;
	char linebuf[256];
	char node[SIZ];
	char host[256];
	char port[256];
	char secret[256];
	int poll = 0;
	char spoolfile[256];

	if (working_ignetcfg == NULL) {
		lprintf(CTDL_DEBUG, "No nodes defined - not polling\n");
		return;
	}

	/* Use the string tokenizer to grab one line at a time */
	for (i=0; i<num_tokens(working_ignetcfg, '\n'); ++i) {
		extract_token(linebuf, working_ignetcfg, i, '\n', sizeof linebuf);
		extract_token(node, linebuf, 0, '|', sizeof node);
		extract_token(secret, linebuf, 1, '|', sizeof secret);
		extract_token(host, linebuf, 2, '|', sizeof host);
		extract_token(port, linebuf, 3, '|', sizeof port);
		if ( (strlen(node) > 0) && (strlen(secret) > 0) 
		   && (strlen(host) > 0) && strlen(port) > 0) {
			poll = full_poll;
			if (poll == 0) {
				snprintf(spoolfile, sizeof spoolfile,
					"./network/spoolout/%s", node);
				if (access(spoolfile, R_OK) == 0) {
					poll = 1;
				}
			}
			if (poll) {
				network_poll_node(node, secret, host, port);
			}
		}
	}

}







/*
 * network_do_queue()
 * 
 * Run through the rooms doing various types of network stuff.
 */
void network_do_queue(void) {
	static time_t last_run = 0L;
	struct RoomProcList *ptr;
	int full_processing = 1;

	/*
	 * Run the full set of processing tasks no more frequently
	 * than once every n seconds
	 */
	if ( (time(NULL) - last_run) < config.c_net_freq ) {
		full_processing = 0;
	}

	/*
	 * This is a simple concurrency check to make sure only one queue run
	 * is done at a time.  We could do this with a mutex, but since we
	 * don't really require extremely fine granularity here, we'll do it
	 * with a static variable instead.
	 */
	if (doing_queue) return;
	doing_queue = 1;

	/* Load the IGnet Configuration into memory */
	load_working_ignetcfg();

	/*
	 * Poll other Citadel nodes.  Maybe.  If "full_processing" is set
	 * then we poll everyone.  Otherwise we only poll nodes we have stuff
	 * to send to.
	 */
	network_poll_other_citadel_nodes(full_processing);

	/*
	 * Load the network map and filter list into memory.
	 */
	read_network_map();
	filterlist = load_filter_list();

	/* 
	 * Go ahead and run the queue
	 */
	if (full_processing) {
		lprintf(CTDL_DEBUG, "network: loading outbound queue\n");
		ForEachRoom(network_queue_room, NULL);

		lprintf(CTDL_DEBUG, "network: running outbound queue\n");
		while (rplist != NULL) {
			network_spoolout_room(rplist->name);
			ptr = rplist;
			rplist = rplist->next;
			free(ptr);
		}
	}

	lprintf(CTDL_DEBUG, "network: processing inbound queue\n");
	network_do_spoolin();

	/* Save the network map back to disk */
	write_network_map();

	/* Free the filter list in memory */
	free_filter_list(filterlist);
	filterlist = NULL;

	network_purge_spoolout();

	lprintf(CTDL_DEBUG, "network: queue run completed\n");

	if (full_processing) {
		last_run = time(NULL);
	}

	doing_queue = 0;
}


/*
 * cmd_netp() - authenticate to the server as another Citadel node polling
 *	      for network traffic
 */
void cmd_netp(char *cmdbuf)
{
	char node[256];
	char pass[256];
	int v;

	char secret[256];
	char nexthop[256];

	/* Authenticate */
	extract_token(node, cmdbuf, 0, '|', sizeof node);
	extract_token(pass, cmdbuf, 1, '|', sizeof pass);

	if (doing_queue) {
		lprintf(CTDL_WARNING, "Network node <%s> refused - spooling", node);
		cprintf("%d spooling - try again in a few minutes\n",
			ERROR + RESOURCE_BUSY);
		return;
	}

	/* load the IGnet Configuration to check node validity */
	load_working_ignetcfg();
	v = is_valid_node(nexthop, secret, node);

	if (v != 0) {
		lprintf(CTDL_WARNING, "Unknown node <%s>\n", node);
		cprintf("%d authentication failed\n",
			ERROR + PASSWORD_REQUIRED);
		return;
	}

	if (strcasecmp(pass, secret)) {
		lprintf(CTDL_WARNING, "Bad password for network node <%s>", node);
		cprintf("%d authentication failed\n", ERROR + PASSWORD_REQUIRED);
		return;
	}

	if (network_talking_to(node, NTT_CHECK)) {
		lprintf(CTDL_WARNING, "Duplicate session for network node <%s>", node);
		cprintf("%d Already talking to %s right now\n", ERROR + RESOURCE_BUSY, node);
		return;
	}

	safestrncpy(CC->net_node, node, sizeof CC->net_node);
	network_talking_to(node, NTT_ADD);
	lprintf(CTDL_NOTICE, "Network node <%s> logged in\n", CC->net_node);
	cprintf("%d authenticated as network node '%s'\n", CIT_OK,
		CC->net_node);
}



/*
 * Module entry point
 */
char *serv_network_init(void)
{
	CtdlRegisterProtoHook(cmd_gnet, "GNET", "Get network config");
	CtdlRegisterProtoHook(cmd_snet, "SNET", "Set network config");
	CtdlRegisterProtoHook(cmd_netp, "NETP", "Identify as network poller");
	CtdlRegisterProtoHook(cmd_nsyn, "NSYN", "Synchronize room to node");
	CtdlRegisterSessionHook(network_do_queue, EVT_TIMER);
	return "$Id$";
}
