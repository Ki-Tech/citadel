/*
 * $Id$
 *
 * This module is an SMTP and ESMTP implementation for the Citadel system.
 * It is compliant with all of the following:
 *
 * RFC  821 - Simple Mail Transfer Protocol
 * RFC  876 - Survey of SMTP Implementations
 * RFC 1047 - Duplicate messages and SMTP
 * RFC 1854 - command pipelining
 * RFC 1869 - Extended Simple Mail Transfer Protocol
 * RFC 1870 - SMTP Service Extension for Message Size Declaration
 * RFC 1893 - Enhanced Mail System Status Codes
 * RFC 2033 - Local Mail Transfer Protocol
 * RFC 2034 - SMTP Service Extension for Returning Enhanced Error Codes
 * RFC 2197 - SMTP Service Extension for Command Pipelining
 * RFC 2487 - SMTP Service Extension for Secure SMTP over TLS
 * RFC 2554 - SMTP Service Extension for Authentication
 * RFC 2821 - Simple Mail Transfer Protocol
 * RFC 2822 - Internet Message Format
 * RFC 2920 - SMTP Service Extension for Command Pipelining
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
#include <ctype.h>
#include <string.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "citadel.h"
#include "server.h"
#include "sysdep_decls.h"
#include "citserver.h"
#include "support.h"
#include "config.h"
#include "control.h"
#include "serv_extensions.h"
#include "room_ops.h"
#include "user_ops.h"
#include "policy.h"
#include "database.h"
#include "msgbase.h"
#include "tools.h"
#include "internet_addressing.h"
#include "genstamp.h"
#include "domain.h"
#include "clientsocket.h"
#include "locate_host.h"

#ifdef HAVE_OPENSSL
#include "serv_crypto.h"
#endif



#ifndef HAVE_SNPRINTF
#include "snprintf.h"
#endif

struct citsmtp {		/* Information about the current session */
	int command_state;
	char helo_node[SIZ];
	struct ctdluser vrfy_buffer;
	int vrfy_count;
	char vrfy_match[SIZ];
	char from[SIZ];
	char recipients[SIZ];
	int number_of_recipients;
	int delivery_mode;
	int message_originated_locally;
	int is_lmtp;
};

enum {				/* Command states for login authentication */
	smtp_command,
	smtp_user,
	smtp_password
};

enum {				/* Delivery modes */
	smtp_deliver_local,
	smtp_deliver_remote
};

#define SMTP		((struct citsmtp *)CtdlGetUserData(SYM_SMTP))
#define SMTP_RECPS	((char *)CtdlGetUserData(SYM_SMTP_RECPS))
#define SMTP_ROOMS	((char *)CtdlGetUserData(SYM_SMTP_ROOMS))


int run_queue_now = 0;	/* Set to 1 to ignore SMTP send retry times */



/*****************************************************************************/
/*                      SMTP SERVER (INBOUND) STUFF                          */
/*****************************************************************************/




/*
 * Here's where our SMTP session begins its happy day.
 */
void smtp_greeting(void) {

	strcpy(CC->cs_clientname, "SMTP session");
	CC->internal_pgm = 1;
	CC->cs_flags |= CS_STEALTH;
	CtdlAllocUserData(SYM_SMTP, sizeof(struct citsmtp));
	CtdlAllocUserData(SYM_SMTP_RECPS, SIZ);
	CtdlAllocUserData(SYM_SMTP_ROOMS, SIZ);
	snprintf(SMTP_RECPS, SIZ, "%s", "");
	snprintf(SMTP_ROOMS, SIZ, "%s", "");

	cprintf("220 %s ESMTP Citadel/UX server ready.\r\n", config.c_fqdn);
}

/*
 * LMTP is like SMTP but with some extra bonus footage added.
 */
void lmtp_greeting(void) {
	smtp_greeting();
	SMTP->is_lmtp = 1;
}


/*
 * Login greeting common to all auth methods
 */
void smtp_auth_greeting(void) {
		cprintf("235 2.0.0 Hello, %s\r\n", CC->user.fullname);
		lprintf(CTDL_NOTICE, "SMTP authenticated %s\n", CC->user.fullname);
		CC->internal_pgm = 0;
		CC->cs_flags &= ~CS_STEALTH;
}


/*
 * Implement HELO and EHLO commands.
 *
 * which_command:  0=HELO, 1=EHLO, 2=LHLO
 */
void smtp_hello(char *argbuf, int which_command) {

	safestrncpy(SMTP->helo_node, argbuf, sizeof SMTP->helo_node);

	if ( (which_command != 2) && (SMTP->is_lmtp) ) {
		cprintf("500 Only LHLO is allowed when running LMTP\r\n");
		return;
	}

	if ( (which_command == 2) && (SMTP->is_lmtp == 0) ) {
		cprintf("500 LHLO is only allowed when running LMTP\r\n");
		return;
	}

	if (which_command == 0) {
		cprintf("250 Hello %s (%s [%s])\r\n",
			SMTP->helo_node,
			CC->cs_host,
			CC->cs_addr
		);
	}
	else {
		if (which_command == 1) {
			cprintf("250-Hello %s (%s [%s])\r\n",
				SMTP->helo_node,
				CC->cs_host,
				CC->cs_addr
			);
		}
		else {
			cprintf("250-Greetings and joyous salutations.\r\n");
		}
		cprintf("250-HELP\r\n");
		cprintf("250-SIZE %ld\r\n", config.c_maxmsglen);
		cprintf("250-PIPELINING\r\n");
		cprintf("250-AUTH LOGIN PLAIN\r\n");
		cprintf("250-AUTH=LOGIN PLAIN\r\n");
#ifdef HAVE_OPENSSL
		cprintf("250-STARTTLS\r\n");
#endif
		cprintf("250 ENHANCEDSTATUSCODES\r\n");
	}
}



/*
 * Implement HELP command.
 */
void smtp_help(void) {
	cprintf("214-Commands accepted:\r\n");
	cprintf("214-    DATA\r\n");
	cprintf("214-    EHLO\r\n");
	cprintf("214-    EXPN\r\n");
	cprintf("214-    HELO\r\n");
	cprintf("214-    HELP\r\n");
	cprintf("214-    MAIL\r\n");
	cprintf("214-    NOOP\r\n");
	cprintf("214-    QUIT\r\n");
	cprintf("214-    RCPT\r\n");
	cprintf("214-    RSET\r\n");
	cprintf("214-    VRFY\r\n");
	cprintf("214     \r\n");
}


/*
 *
 */
void smtp_get_user(char *argbuf) {
	char buf[SIZ];
	char username[SIZ];

	CtdlDecodeBase64(username, argbuf, SIZ);
	lprintf(CTDL_DEBUG, "Trying <%s>\n", username);
	if (CtdlLoginExistingUser(username) == login_ok) {
		CtdlEncodeBase64(buf, "Password:", 9);
		cprintf("334 %s\r\n", buf);
		SMTP->command_state = smtp_password;
	}
	else {
		cprintf("500 5.7.0 No such user.\r\n");
		SMTP->command_state = smtp_command;
	}
}


/*
 *
 */
void smtp_get_pass(char *argbuf) {
	char password[SIZ];

	CtdlDecodeBase64(password, argbuf, SIZ);
	lprintf(CTDL_DEBUG, "Trying <%s>\n", password);
	if (CtdlTryPassword(password) == pass_ok) {
		smtp_auth_greeting();
	}
	else {
		cprintf("535 5.7.0 Authentication failed.\r\n");
	}
	SMTP->command_state = smtp_command;
}


/*
 *
 */
void smtp_auth(char *argbuf) {
	char buf[SIZ];
	char method[SIZ];
	char encoded_authstring[SIZ];
	char decoded_authstring[SIZ];
	char ident[SIZ];
	char user[SIZ];
	char pass[SIZ];

	if (CC->logged_in) {
		cprintf("504 5.7.4 Already logged in.\r\n");
		return;
	}

	extract_token(method, argbuf, 0, ' ');

	if (!strncasecmp(method, "login", 5) ) {
		if (strlen(argbuf) >= 7) {
			smtp_get_user(&argbuf[6]);
		}
		else {
			CtdlEncodeBase64(buf, "Username:", 9);
			cprintf("334 %s\r\n", buf);
			SMTP->command_state = smtp_user;
		}
		return;
	}

	if (!strncasecmp(method, "plain", 5) ) {
		extract_token(encoded_authstring, argbuf, 1, ' ');
		CtdlDecodeBase64(decoded_authstring,
				encoded_authstring,
				strlen(encoded_authstring) );
		strcpy(ident, decoded_authstring);
		strcpy(user, &decoded_authstring[strlen(ident) + 1] );
		strcpy(pass, &decoded_authstring[strlen(ident) + strlen(user) + 2] );

		if (CtdlLoginExistingUser(user) == login_ok) {
			if (CtdlTryPassword(pass) == pass_ok) {
				smtp_auth_greeting();
				return;
			}
		}
		cprintf("504 5.7.4 Authentication failed.\r\n");
	}

	if (strncasecmp(method, "login", 5) ) {
		cprintf("504 5.7.4 Unknown authentication method.\r\n");
		return;
	}

}


/*
 * Back end for smtp_vrfy() command
 */
void smtp_vrfy_backend(struct ctdluser *us, void *data) {

	if (!fuzzy_match(us, SMTP->vrfy_match)) {
		++SMTP->vrfy_count;
		memcpy(&SMTP->vrfy_buffer, us, sizeof(struct ctdluser));
	}
}


/* 
 * Implements the VRFY (verify user name) command.
 * Performs fuzzy match on full user names.
 */
void smtp_vrfy(char *argbuf) {
	SMTP->vrfy_count = 0;
	strcpy(SMTP->vrfy_match, argbuf);
	ForEachUser(smtp_vrfy_backend, NULL);

	if (SMTP->vrfy_count < 1) {
		cprintf("550 5.1.1 String does not match anything.\r\n");
	}
	else if (SMTP->vrfy_count == 1) {
		cprintf("250 %s <cit%ld@%s>\r\n",
			SMTP->vrfy_buffer.fullname,
			SMTP->vrfy_buffer.usernum,
			config.c_fqdn);
	}
	else if (SMTP->vrfy_count > 1) {
		cprintf("553 5.1.4 Request ambiguous: %d users matched.\r\n",
			SMTP->vrfy_count);
	}

}



/*
 * Back end for smtp_expn() command
 */
void smtp_expn_backend(struct ctdluser *us, void *data) {

	if (!fuzzy_match(us, SMTP->vrfy_match)) {

		if (SMTP->vrfy_count >= 1) {
			cprintf("250-%s <cit%ld@%s>\r\n",
				SMTP->vrfy_buffer.fullname,
				SMTP->vrfy_buffer.usernum,
				config.c_fqdn);
		}

		++SMTP->vrfy_count;
		memcpy(&SMTP->vrfy_buffer, us, sizeof(struct ctdluser));
	}
}


/* 
 * Implements the EXPN (expand user name) command.
 * Performs fuzzy match on full user names.
 */
void smtp_expn(char *argbuf) {
	SMTP->vrfy_count = 0;
	strcpy(SMTP->vrfy_match, argbuf);
	ForEachUser(smtp_expn_backend, NULL);

	if (SMTP->vrfy_count < 1) {
		cprintf("550 5.1.1 String does not match anything.\r\n");
	}
	else if (SMTP->vrfy_count >= 1) {
		cprintf("250 %s <cit%ld@%s>\r\n",
			SMTP->vrfy_buffer.fullname,
			SMTP->vrfy_buffer.usernum,
			config.c_fqdn);
	}
}


/*
 * Implements the RSET (reset state) command.
 * Currently this just zeroes out the state buffer.  If pointers to data
 * allocated with mallok() are ever placed in the state buffer, we have to
 * be sure to phree() them first!
 *
 * Set do_response to nonzero to output the SMTP RSET response code.
 */
void smtp_rset(int do_response) {
	int is_lmtp;

	/*
	 * Our entire SMTP state is discarded when a RSET command is issued,
	 * but we need to preserve this one little piece of information, so
	 * we save it for later.
	 */
	is_lmtp = SMTP->is_lmtp;

	memset(SMTP, 0, sizeof(struct citsmtp));

	/*
	 * It is somewhat ambiguous whether we want to log out when a RSET
	 * command is issued.  Here's the code to do it.  It is commented out
	 * because some clients (such as Pine) issue RSET commands before
	 * each message, but still expect to be logged in.
	 *
	 * if (CC->logged_in) {
	 *	logout(CC);
	 * }
	 */

	/*
	 * Reinstate this little piece of information we saved (see above).
	 */
	SMTP->is_lmtp = is_lmtp;

	if (do_response) {
		cprintf("250 2.0.0 Zap!\r\n");
	}
}

/*
 * Clear out the portions of the state buffer that need to be cleared out
 * after the DATA command finishes.
 */
void smtp_data_clear(void) {
	strcpy(SMTP->from, "");
	strcpy(SMTP->recipients, "");
	SMTP->number_of_recipients = 0;
	SMTP->delivery_mode = 0;
	SMTP->message_originated_locally = 0;
}



/*
 * Implements the "MAIL From:" command
 */
void smtp_mail(char *argbuf) {
	char user[SIZ];
	char node[SIZ];
	char name[SIZ];

	if (strlen(SMTP->from) != 0) {
		cprintf("503 5.1.0 Only one sender permitted\r\n");
		return;
	}

	if (strncasecmp(argbuf, "From:", 5)) {
		cprintf("501 5.1.7 Syntax error\r\n");
		return;
	}

	strcpy(SMTP->from, &argbuf[5]);
	striplt(SMTP->from);
	stripallbut(SMTP->from, '<', '>');

	/* We used to reject empty sender names, until it was brought to our
	 * attention that RFC1123 5.2.9 requires that this be allowed.  So now
	 * we allow it, but replace the empty string with a fake
	 * address so we don't have to contend with the empty string causing
	 * other code to fail when it's expecting something there.
	 */
	if (strlen(SMTP->from) == 0) {
		strcpy(SMTP->from, "someone@somewhere.org");
	}

	/* If this SMTP connection is from a logged-in user, force the 'from'
	 * to be the user's Internet e-mail address as Citadel knows it.
	 */
	if (CC->logged_in) {
		strcpy(SMTP->from, CC->cs_inet_email);
		cprintf("250 2.1.0 Sender ok <%s>\r\n", SMTP->from);
		SMTP->message_originated_locally = 1;
		return;
	}

	else if (SMTP->is_lmtp) {
		/* Bypass forgery checking for LMTP */
	}

	/* Otherwise, make sure outsiders aren't trying to forge mail from
	 * this system.
	 */
	else {
		process_rfc822_addr(SMTP->from, user, node, name);
		if (CtdlHostAlias(node) != hostalias_nomatch) {
			cprintf("550 5.1.8 "
				"You must log in to send mail from %s\r\n",
				node);
			strcpy(SMTP->from, "");
			return;
		}
	}

	cprintf("250 2.0.0 Sender ok\r\n");
}



/*
 * Implements the "RCPT To:" command
 */
void smtp_rcpt(char *argbuf) {
	char recp[SIZ];
	char message_to_spammer[SIZ];
	struct recptypes *valid = NULL;

	if (strlen(SMTP->from) == 0) {
		cprintf("503 5.5.1 Need MAIL before RCPT\r\n");
		return;
	}

	if (strncasecmp(argbuf, "To:", 3)) {
		cprintf("501 5.1.7 Syntax error\r\n");
		return;
	}

	strcpy(recp, &argbuf[3]);
	striplt(recp);
	stripallbut(recp, '<', '>');

	if ( (strlen(recp) + strlen(SMTP->recipients) + 1 ) >= SIZ) {
		cprintf("452 4.5.3 Too many recipients\r\n");
		return;
	}

	/* RBL check */
	if ( (!CC->logged_in)
	   && (!SMTP->is_lmtp) ) {
		if (rbl_check(message_to_spammer)) {
			cprintf("550 %s\r\n", message_to_spammer);
			/* no need to phree(valid), it's not allocated yet */
			return;
		}
	}

	valid = validate_recipients(recp);
	if (valid->num_error > 0) {
		cprintf("599 5.1.1 Error: %s\r\n", valid->errormsg);
		phree(valid);
		return;
	}

	if (valid->num_internet > 0) {
		if ( (SMTP->message_originated_locally == 0)
		   && (SMTP->is_lmtp == 0) ) {
			cprintf("551 5.7.1 <%s> - relaying denied\r\n", recp);
			phree(valid);
			return;
		}
	}

	cprintf("250 2.1.5 RCPT ok <%s>\r\n", recp);
	if (strlen(SMTP->recipients) > 0) {
		strcat(SMTP->recipients, ",");
	}
	strcat(SMTP->recipients, recp);
	SMTP->number_of_recipients += 1;
}




/*
 * Implements the DATA command
 */
void smtp_data(void) {
	char *body;
	struct CtdlMessage *msg;
	long msgnum;
	char nowstamp[SIZ];
	struct recptypes *valid;
	int scan_errors;
	int i;
	char result[SIZ];

	if (strlen(SMTP->from) == 0) {
		cprintf("503 5.5.1 Need MAIL command first.\r\n");
		return;
	}

	if (SMTP->number_of_recipients < 1) {
		cprintf("503 5.5.1 Need RCPT command first.\r\n");
		return;
	}

	cprintf("354 Transmit message now - terminate with '.' by itself\r\n");
	
	datestring(nowstamp, sizeof nowstamp, time(NULL), DATESTRING_RFC822);
	body = mallok(4096);

	if (body != NULL) snprintf(body, 4096,
		"Received: from %s (%s [%s])\n"
		"	by %s; %s\n",
			SMTP->helo_node,
			CC->cs_host,
			CC->cs_addr,
			config.c_fqdn,
			nowstamp);
	
	body = CtdlReadMessageBody(".", config.c_maxmsglen, body, 1);
	if (body == NULL) {
		cprintf("550 5.6.5 "
			"Unable to save message: internal error.\r\n");
		return;
	}

	lprintf(CTDL_DEBUG, "Converting message...\n");
	msg = convert_internet_message(body);

	/* If the user is locally authenticated, FORCE the From: header to
	 * show up as the real sender.  Yes, this violates the RFC standard,
	 * but IT MAKES SENSE.  If you prefer strict RFC adherence over
	 * common sense, you can disable this in the configuration.
	 *
	 * We also set the "message room name" ('O' field) to MAILROOM
	 * (which is Mail> on most systems) to prevent it from getting set
	 * to something ugly like "0000058008.Sent Items>" when the message
	 * is read with a Citadel client.
	 */
	if ( (CC->logged_in) && (config.c_rfc822_strict_from == 0) ) {
		if (msg->cm_fields['A'] != NULL) phree(msg->cm_fields['A']);
		if (msg->cm_fields['N'] != NULL) phree(msg->cm_fields['N']);
		if (msg->cm_fields['H'] != NULL) phree(msg->cm_fields['H']);
		if (msg->cm_fields['F'] != NULL) phree(msg->cm_fields['F']);
		if (msg->cm_fields['O'] != NULL) phree(msg->cm_fields['O']);
		msg->cm_fields['A'] = strdoop(CC->user.fullname);
		msg->cm_fields['N'] = strdoop(config.c_nodename);
		msg->cm_fields['H'] = strdoop(config.c_humannode);
		msg->cm_fields['F'] = strdoop(CC->cs_inet_email);
        	msg->cm_fields['O'] = strdoop(MAILROOM);
	}

	/* Submit the message into the Citadel system. */
	valid = validate_recipients(SMTP->recipients);

	/* If there are modules that want to scan this message before final
	 * submission (such as virus checkers or spam filters), call them now
	 * and give them an opportunity to reject the message.
	 */
	scan_errors = PerformMessageHooks(msg, EVT_SMTPSCAN);

	if (scan_errors > 0) {	/* We don't want this message! */

		if (msg->cm_fields['0'] == NULL) {
			msg->cm_fields['0'] = strdoop(
				"5.7.1 Message rejected by filter");
		}

		sprintf(result, "550 %s\r\n", msg->cm_fields['0']);
	}
	
	else {			/* Ok, we'll accept this message. */
		msgnum = CtdlSubmitMsg(msg, valid, "");
		if (msgnum > 0L) {
			sprintf(result, "250 2.0.0 Message accepted.\r\n");
		}
		else {
			sprintf(result, "550 5.5.0 Internal delivery error\r\n");
		}
	}

	/* For SMTP and ESTMP, just print the result message.  For LMTP, we
	 * have to print one result message for each recipient.  Since there
	 * is nothing in Citadel which would cause different recipients to
	 * have different results, we can get away with just spitting out the
	 * same message once for each recipient.
	 */
	if (SMTP->is_lmtp) {
		for (i=0; i<SMTP->number_of_recipients; ++i) {
			cprintf("%s", result);
		}
	}
	else {
		cprintf("%s", result);
	}

	CtdlFreeMessage(msg);
	phree(valid);
	smtp_data_clear();	/* clear out the buffers now */
}


/*
 * implements the STARTTLS command (Citadel API version)
 */
#ifdef HAVE_OPENSSL
void smtp_starttls(void)
{
	char ok_response[SIZ];
	char nosup_response[SIZ];
	char error_response[SIZ];

	sprintf(ok_response,
		"200 2.0.0 Begin TLS negotiation now\r\n");
	sprintf(nosup_response,
		"554 5.7.3 TLS not supported here\r\n");
	sprintf(error_response,
		"554 5.7.3 Internal error\r\n");
	CtdlStartTLS(ok_response, nosup_response, error_response);
	smtp_rset(0);
}
#endif



/* 
 * Main command loop for SMTP sessions.
 */
void smtp_command_loop(void) {
	char cmdbuf[SIZ];

	time(&CC->lastcmd);
	memset(cmdbuf, 0, sizeof cmdbuf); /* Clear it, just in case */
	if (client_gets(cmdbuf) < 1) {
		lprintf(CTDL_CRIT, "SMTP socket is broken.  Ending session.\n");
		CC->kill_me = 1;
		return;
	}
	lprintf(CTDL_INFO, "SMTP: %s\n", cmdbuf);
	while (strlen(cmdbuf) < 5) strcat(cmdbuf, " ");

	if (SMTP->command_state == smtp_user) {
		smtp_get_user(cmdbuf);
	}

	else if (SMTP->command_state == smtp_password) {
		smtp_get_pass(cmdbuf);
	}

	else if (!strncasecmp(cmdbuf, "AUTH", 4)) {
		smtp_auth(&cmdbuf[5]);
	}

	else if (!strncasecmp(cmdbuf, "DATA", 4)) {
		smtp_data();
	}

	else if (!strncasecmp(cmdbuf, "EXPN", 4)) {
		smtp_expn(&cmdbuf[5]);
	}

	else if (!strncasecmp(cmdbuf, "HELO", 4)) {
		smtp_hello(&cmdbuf[5], 0);
	}

	else if (!strncasecmp(cmdbuf, "EHLO", 4)) {
		smtp_hello(&cmdbuf[5], 1);
	}

	else if (!strncasecmp(cmdbuf, "LHLO", 4)) {
		smtp_hello(&cmdbuf[5], 2);
	}

	else if (!strncasecmp(cmdbuf, "HELP", 4)) {
		smtp_help();
	}

	else if (!strncasecmp(cmdbuf, "MAIL", 4)) {
		smtp_mail(&cmdbuf[5]);
	}

	else if (!strncasecmp(cmdbuf, "NOOP", 4)) {
		cprintf("250 NOOP\r\n");
	}

	else if (!strncasecmp(cmdbuf, "QUIT", 4)) {
		cprintf("221 Goodbye...\r\n");
		CC->kill_me = 1;
		return;
	}

	else if (!strncasecmp(cmdbuf, "RCPT", 4)) {
		smtp_rcpt(&cmdbuf[5]);
	}

	else if (!strncasecmp(cmdbuf, "RSET", 4)) {
		smtp_rset(1);
	}
#ifdef HAVE_OPENSSL
	else if (!strcasecmp(cmdbuf, "STARTTLS")) {
		smtp_starttls();
	}
#endif
	else if (!strncasecmp(cmdbuf, "VRFY", 4)) {
		smtp_vrfy(&cmdbuf[5]);
	}

	else {
		cprintf("502 5.0.0 I'm afraid I can't do that.\r\n");
	}

}




/*****************************************************************************/
/*               SMTP CLIENT (OUTBOUND PROCESSING) STUFF                     */
/*****************************************************************************/



/*
 * smtp_try()
 *
 * Called by smtp_do_procmsg() to attempt delivery to one SMTP host
 *
 */
void smtp_try(const char *key, const char *addr, int *status,
	      char *dsn, size_t n, long msgnum)
{
	int sock = (-1);
	char mxhosts[1024];
	int num_mxhosts;
	int mx;
	int i;
	char user[SIZ], node[SIZ], name[SIZ];
	char buf[1024];
	char mailfrom[1024];
	int lp, rp;
	FILE *msg_fp = NULL;
	size_t msg_size;
	size_t blocksize = 0;
	int scan_done;

	/* Parse out the host portion of the recipient address */
	process_rfc822_addr(addr, user, node, name);

	lprintf(CTDL_DEBUG, "Attempting SMTP delivery to <%s> @ <%s> (%s)\n",
		user, node, name);

	/* Load the message out of the database into a temp file */
	msg_fp = tmpfile();
	if (msg_fp == NULL) {
		*status = 4;
		snprintf(dsn, n, "Error creating temporary file");
		return;
	}
	else {
		CtdlRedirectOutput(msg_fp, -1);
		CtdlOutputMsg(msgnum, MT_RFC822, HEADERS_ALL, 0, 1);
		CtdlRedirectOutput(NULL, -1);
		fseek(msg_fp, 0L, SEEK_END);
		msg_size = ftell(msg_fp);
	}


	/* Extract something to send later in the 'MAIL From:' command */
	strcpy(mailfrom, "");
	rewind(msg_fp);
	scan_done = 0;
	do {
		if (fgets(buf, sizeof buf, msg_fp)==NULL) scan_done = 1;
		if (!strncasecmp(buf, "From:", 5)) {
			safestrncpy(mailfrom, &buf[5], sizeof mailfrom);
			striplt(mailfrom);
			for (i=0; i<strlen(mailfrom); ++i) {
				if (!isprint(mailfrom[i])) {
					strcpy(&mailfrom[i], &mailfrom[i+1]);
					i=0;
				}
			}

			/* Strip out parenthesized names */
			lp = (-1);
			rp = (-1);
			for (i=0; i<strlen(mailfrom); ++i) {
				if (mailfrom[i] == '(') lp = i;
				if (mailfrom[i] == ')') rp = i;
			}
			if ((lp>0)&&(rp>lp)) {
				strcpy(&mailfrom[lp-1], &mailfrom[rp+1]);
			}

			/* Prefer brokketized names */
			lp = (-1);
			rp = (-1);
			for (i=0; i<strlen(mailfrom); ++i) {
				if (mailfrom[i] == '<') lp = i;
				if (mailfrom[i] == '>') rp = i;
			}
			if ( (lp>=0) && (rp>lp) ) {
				mailfrom[rp] = 0;
				strcpy(mailfrom, &mailfrom[lp]);
			}

			scan_done = 1;
		}
	} while (scan_done == 0);
	if (strlen(mailfrom)==0) strcpy(mailfrom, "someone@somewhere.org");

	/* Figure out what mail exchanger host we have to connect to */
	num_mxhosts = getmx(mxhosts, node);
	lprintf(CTDL_DEBUG, "Number of MX hosts for <%s> is %d\n", node, num_mxhosts);
	if (num_mxhosts < 1) {
		*status = 5;
		snprintf(dsn, SIZ, "No MX hosts found for <%s>", node);
		return;
	}

	sock = (-1);
	for (mx=0; (mx<num_mxhosts && sock < 0); ++mx) {
		extract(buf, mxhosts, mx);
		lprintf(CTDL_DEBUG, "Trying <%s>\n", buf);
		sock = sock_connect(buf, "25", "tcp");
		snprintf(dsn, SIZ, "Could not connect: %s", strerror(errno));
		if (sock >= 0) lprintf(CTDL_DEBUG, "Connected!\n");
		if (sock < 0) snprintf(dsn, SIZ, "%s", strerror(errno));
	}

	if (sock < 0) {
		*status = 4;	/* dsn is already filled in */
		return;
	}

	/* Process the SMTP greeting from the server */
	if (ml_sock_gets(sock, buf) < 0) {
		*status = 4;
		strcpy(dsn, "Connection broken during SMTP conversation");
		goto bail;
	}
	lprintf(CTDL_DEBUG, "<%s\n", buf);
	if (buf[0] != '2') {
		if (buf[0] == '4') {
			*status = 4;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
		else {
			*status = 5;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
	}

	/* At this point we know we are talking to a real SMTP server */

	/* Do a HELO command */
	snprintf(buf, sizeof buf, "HELO %s\r\n", config.c_fqdn);
	lprintf(CTDL_DEBUG, ">%s", buf);
	sock_write(sock, buf, strlen(buf));
	if (ml_sock_gets(sock, buf) < 0) {
		*status = 4;
		strcpy(dsn, "Connection broken during SMTP HELO");
		goto bail;
	}
	lprintf(CTDL_DEBUG, "<%s\n", buf);
	if (buf[0] != '2') {
		if (buf[0] == '4') {
			*status = 4;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
		else {
			*status = 5;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
	}


	/* HELO succeeded, now try the MAIL From: command */
	snprintf(buf, sizeof buf, "MAIL From: <%s>\r\n", mailfrom);
	lprintf(CTDL_DEBUG, ">%s", buf);
	sock_write(sock, buf, strlen(buf));
	if (ml_sock_gets(sock, buf) < 0) {
		*status = 4;
		strcpy(dsn, "Connection broken during SMTP MAIL");
		goto bail;
	}
	lprintf(CTDL_DEBUG, "<%s\n", buf);
	if (buf[0] != '2') {
		if (buf[0] == '4') {
			*status = 4;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
		else {
			*status = 5;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
	}


	/* MAIL succeeded, now try the RCPT To: command */
	snprintf(buf, sizeof buf, "RCPT To: <%s>\r\n", addr);
	lprintf(CTDL_DEBUG, ">%s", buf);
	sock_write(sock, buf, strlen(buf));
	if (ml_sock_gets(sock, buf) < 0) {
		*status = 4;
		strcpy(dsn, "Connection broken during SMTP RCPT");
		goto bail;
	}
	lprintf(CTDL_DEBUG, "<%s\n", buf);
	if (buf[0] != '2') {
		if (buf[0] == '4') {
			*status = 4;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
		else {
			*status = 5;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
	}


	/* RCPT succeeded, now try the DATA command */
	lprintf(CTDL_DEBUG, ">DATA\n");
	sock_write(sock, "DATA\r\n", 6);
	if (ml_sock_gets(sock, buf) < 0) {
		*status = 4;
		strcpy(dsn, "Connection broken during SMTP DATA");
		goto bail;
	}
	lprintf(CTDL_DEBUG, "<%s\n", buf);
	if (buf[0] != '3') {
		if (buf[0] == '4') {
			*status = 3;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
		else {
			*status = 5;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
	}

	/* If we reach this point, the server is expecting data */
	rewind(msg_fp);
	while (msg_size > 0) {
		blocksize = sizeof(buf);
		if (blocksize > msg_size) blocksize = msg_size;
		fread(buf, blocksize, 1, msg_fp);
		sock_write(sock, buf, blocksize);
		msg_size -= blocksize;
	}
	if (buf[blocksize-1] != 10) {
		lprintf(CTDL_WARNING, "Possible problem: message did not "
			"correctly terminate. (expecting 0x10, got 0x%02x)\n",
				buf[blocksize-1]);
	}

	sock_write(sock, ".\r\n", 3);
	if (ml_sock_gets(sock, buf) < 0) {
		*status = 4;
		strcpy(dsn, "Connection broken during SMTP message transmit");
		goto bail;
	}
	lprintf(CTDL_DEBUG, "%s\n", buf);
	if (buf[0] != '2') {
		if (buf[0] == '4') {
			*status = 4;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
		else {
			*status = 5;
			safestrncpy(dsn, &buf[4], 1023);
			goto bail;
		}
	}

	/* We did it! */
	safestrncpy(dsn, &buf[4], 1023);
	*status = 2;

	lprintf(CTDL_DEBUG, ">QUIT\n");
	sock_write(sock, "QUIT\r\n", 6);
	ml_sock_gets(sock, buf);
	lprintf(CTDL_DEBUG, "<%s\n", buf);
	lprintf(CTDL_INFO, "SMTP delivery to <%s> @ <%s> (%s) succeeded\n",
		user, node, name);

bail:	if (msg_fp != NULL) fclose(msg_fp);
	sock_close(sock);
	return;
}



/*
 * smtp_do_bounce() is caled by smtp_do_procmsg() to scan a set of delivery
 * instructions for "5" codes (permanent fatal errors) and produce/deliver
 * a "bounce" message (delivery status notification).
 */
void smtp_do_bounce(char *instr) {
	int i;
	int lines;
	int status;
	char buf[1024];
	char key[1024];
	char addr[1024];
	char dsn[1024];
	char bounceto[1024];
	int num_bounces = 0;
	int bounce_this = 0;
	long bounce_msgid = (-1);
	time_t submitted = 0L;
	struct CtdlMessage *bmsg = NULL;
	int give_up = 0;
	struct recptypes *valid;
	int successful_bounce = 0;

	lprintf(CTDL_DEBUG, "smtp_do_bounce() called\n");
	strcpy(bounceto, "");

	lines = num_tokens(instr, '\n');


	/* See if it's time to give up on delivery of this message */
	for (i=0; i<lines; ++i) {
		extract_token(buf, instr, i, '\n');
		extract(key, buf, 0);
		extract(addr, buf, 1);
		if (!strcasecmp(key, "submitted")) {
			submitted = atol(addr);
		}
	}

	if ( (time(NULL) - submitted) > SMTP_GIVE_UP ) {
		give_up = 1;
	}



	bmsg = (struct CtdlMessage *) mallok(sizeof(struct CtdlMessage));
	if (bmsg == NULL) return;
	memset(bmsg, 0, sizeof(struct CtdlMessage));

        bmsg->cm_magic = CTDLMESSAGE_MAGIC;
        bmsg->cm_anon_type = MES_NORMAL;
        bmsg->cm_format_type = 1;
        bmsg->cm_fields['A'] = strdoop("Citadel");
        bmsg->cm_fields['O'] = strdoop(MAILROOM);
        bmsg->cm_fields['N'] = strdoop(config.c_nodename);

	if (give_up) bmsg->cm_fields['M'] = strdoop(
"A message you sent could not be delivered to some or all of its recipients\n"
"due to prolonged unavailability of its destination(s).\n"
"Giving up on the following addresses:\n\n"
);

        else bmsg->cm_fields['M'] = strdoop(
"A message you sent could not be delivered to some or all of its recipients.\n"
"The following addresses were undeliverable:\n\n"
);

	/*
	 * Now go through the instructions checking for stuff.
	 */
	for (i=0; i<lines; ++i) {
		extract_token(buf, instr, i, '\n');
		extract(key, buf, 0);
		extract(addr, buf, 1);
		status = extract_int(buf, 2);
		extract(dsn, buf, 3);
		bounce_this = 0;

		lprintf(CTDL_DEBUG, "key=<%s> addr=<%s> status=%d dsn=<%s>\n",
			key, addr, status, dsn);

		if (!strcasecmp(key, "bounceto")) {
			strcpy(bounceto, addr);
		}

		if (
		   (!strcasecmp(key, "local"))
		   || (!strcasecmp(key, "remote"))
		   || (!strcasecmp(key, "ignet"))
		   || (!strcasecmp(key, "room"))
		) {
			if (status == 5) bounce_this = 1;
			if (give_up) bounce_this = 1;
		}

		if (bounce_this) {
			++num_bounces;

			if (bmsg->cm_fields['M'] == NULL) {
				lprintf(CTDL_ERR, "ERROR ... M field is null "
					"(%s:%d)\n", __FILE__, __LINE__);
			}

			bmsg->cm_fields['M'] = reallok(bmsg->cm_fields['M'],
				strlen(bmsg->cm_fields['M']) + 1024 );
			strcat(bmsg->cm_fields['M'], addr);
			strcat(bmsg->cm_fields['M'], ": ");
			strcat(bmsg->cm_fields['M'], dsn);
			strcat(bmsg->cm_fields['M'], "\n");

			remove_token(instr, i, '\n');
			--i;
			--lines;
		}
	}

	/* Deliver the bounce if there's anything worth mentioning */
	lprintf(CTDL_DEBUG, "num_bounces = %d\n", num_bounces);
	if (num_bounces > 0) {

		/* First try the user who sent the message */
		lprintf(CTDL_DEBUG, "bounce to user? <%s>\n", bounceto);
		if (strlen(bounceto) == 0) {
			lprintf(CTDL_ERR, "No bounce address specified\n");
			bounce_msgid = (-1L);
		}

		/* Can we deliver the bounce to the original sender? */
		valid = validate_recipients(bounceto);
		if (valid != NULL) {
			if (valid->num_error == 0) {
				CtdlSubmitMsg(bmsg, valid, "");
				successful_bounce = 1;
			}
		}

		/* If not, post it in the Aide> room */
		if (successful_bounce == 0) {
			CtdlSubmitMsg(bmsg, NULL, config.c_aideroom);
		}

		/* Free up the memory we used */
		if (valid != NULL) {
			phree(valid);
		}
	}

	CtdlFreeMessage(bmsg);
	lprintf(CTDL_DEBUG, "Done processing bounces\n");
}


/*
 * smtp_purge_completed_deliveries() is caled by smtp_do_procmsg() to scan a
 * set of delivery instructions for completed deliveries and remove them.
 *
 * It returns the number of incomplete deliveries remaining.
 */
int smtp_purge_completed_deliveries(char *instr) {
	int i;
	int lines;
	int status;
	char buf[1024];
	char key[1024];
	char addr[1024];
	char dsn[1024];
	int completed;
	int incomplete = 0;

	lines = num_tokens(instr, '\n');
	for (i=0; i<lines; ++i) {
		extract_token(buf, instr, i, '\n');
		extract(key, buf, 0);
		extract(addr, buf, 1);
		status = extract_int(buf, 2);
		extract(dsn, buf, 3);

		completed = 0;

		if (
		   (!strcasecmp(key, "local"))
		   || (!strcasecmp(key, "remote"))
		   || (!strcasecmp(key, "ignet"))
		   || (!strcasecmp(key, "room"))
		) {
			if (status == 2) completed = 1;
			else ++incomplete;
		}

		if (completed) {
			remove_token(instr, i, '\n');
			--i;
			--lines;
		}
	}

	return(incomplete);
}


/*
 * smtp_do_procmsg()
 *
 * Called by smtp_do_queue() to handle an individual message.
 */
void smtp_do_procmsg(long msgnum, void *userdata) {
	struct CtdlMessage *msg;
	char *instr = NULL;
	char *results = NULL;
	int i;
	int lines;
	int status;
	char buf[1024];
	char key[1024];
	char addr[1024];
	char dsn[1024];
	long text_msgid = (-1);
	int incomplete_deliveries_remaining;
	time_t attempted = 0L;
	time_t last_attempted = 0L;
	time_t retry = SMTP_RETRY_INTERVAL;

	lprintf(CTDL_DEBUG, "smtp_do_procmsg(%ld)\n", msgnum);

	msg = CtdlFetchMessage(msgnum);
	if (msg == NULL) {
		lprintf(CTDL_ERR, "SMTP: tried %ld but no such message!\n", msgnum);
		return;
	}

	instr = strdoop(msg->cm_fields['M']);
	CtdlFreeMessage(msg);

	/* Strip out the headers amd any other non-instruction line */
	lines = num_tokens(instr, '\n');
	for (i=0; i<lines; ++i) {
		extract_token(buf, instr, i, '\n');
		if (num_tokens(buf, '|') < 2) {
			remove_token(instr, i, '\n');
			--lines;
			--i;
		}
	}

	/* Learn the message ID and find out about recent delivery attempts */
	lines = num_tokens(instr, '\n');
	for (i=0; i<lines; ++i) {
		extract_token(buf, instr, i, '\n');
		extract(key, buf, 0);
		if (!strcasecmp(key, "msgid")) {
			text_msgid = extract_long(buf, 1);
		}
		if (!strcasecmp(key, "retry")) {
			/* double the retry interval after each attempt */
			retry = extract_long(buf, 1) * 2L;
			if (retry > SMTP_RETRY_MAX) {
				retry = SMTP_RETRY_MAX;
			}
			remove_token(instr, i, '\n');
		}
		if (!strcasecmp(key, "attempted")) {
			attempted = extract_long(buf, 1);
			if (attempted > last_attempted)
				last_attempted = attempted;
		}
	}

	/*
	 * Postpone delivery if we've already tried recently.
	 */
	if (((time(NULL) - last_attempted) < retry) && (run_queue_now == 0)) {
		lprintf(CTDL_DEBUG, "Retry time not yet reached.\n");
		phree(instr);
		return;
	}


	/*
	 * Bail out if there's no actual message associated with this
	 */
	if (text_msgid < 0L) {
		lprintf(CTDL_ERR, "SMTP: no 'msgid' directive found!\n");
		phree(instr);
		return;
	}

	/* Plow through the instructions looking for 'remote' directives and
	 * a status of 0 (no delivery yet attempted) or 3/4 (transient errors
	 * were experienced and it's time to try again)
	 */
	lines = num_tokens(instr, '\n');
	for (i=0; i<lines; ++i) {
		extract_token(buf, instr, i, '\n');
		extract(key, buf, 0);
		extract(addr, buf, 1);
		status = extract_int(buf, 2);
		extract(dsn, buf, 3);
		if ( (!strcasecmp(key, "remote"))
		   && ((status==0)||(status==3)||(status==4)) ) {

			/* Remove this "remote" instruction from the set,
			 * but replace the set's final newline if
			 * remove_token() stripped it.  It has to be there.
			 */
			remove_token(instr, i, '\n');
			if (instr[strlen(instr)-1] != '\n') {
				strcat(instr, "\n");
			}

			--i;
			--lines;
			lprintf(CTDL_DEBUG, "SMTP: Trying <%s>\n", addr);
			smtp_try(key, addr, &status, dsn, sizeof dsn, text_msgid);
			if (status != 2) {
				if (results == NULL) {
					results = mallok(1024);
					memset(results, 0, 1024);
				}
				else {
					results = reallok(results,
						strlen(results) + 1024);
				}
				snprintf(&results[strlen(results)], 1024,
					"%s|%s|%d|%s\n",
					key, addr, status, dsn);
			}
		}
	}

	if (results != NULL) {
		instr = reallok(instr, strlen(instr) + strlen(results) + 2);
		strcat(instr, results);
		phree(results);
	}


	/* Generate 'bounce' messages */
	smtp_do_bounce(instr);

	/* Go through the delivery list, deleting completed deliveries */
	incomplete_deliveries_remaining = 
		smtp_purge_completed_deliveries(instr);


	/*
	 * No delivery instructions remain, so delete both the instructions
	 * message and the message message.
	 */
	if (incomplete_deliveries_remaining <= 0) {
		CtdlDeleteMessages(SMTP_SPOOLOUT_ROOM, msgnum, "");
		CtdlDeleteMessages(SMTP_SPOOLOUT_ROOM, text_msgid, "");
	}


	/*
	 * Uncompleted delivery instructions remain, so delete the old
	 * instructions and replace with the updated ones.
	 */
	if (incomplete_deliveries_remaining > 0) {
		CtdlDeleteMessages(SMTP_SPOOLOUT_ROOM, msgnum, "");
        	msg = mallok(sizeof(struct CtdlMessage));
		memset(msg, 0, sizeof(struct CtdlMessage));
		msg->cm_magic = CTDLMESSAGE_MAGIC;
		msg->cm_anon_type = MES_NORMAL;
		msg->cm_format_type = FMT_RFC822;
		msg->cm_fields['M'] = malloc(strlen(instr)+SIZ);
		snprintf(msg->cm_fields['M'],
			strlen(instr)+SIZ,
			"Content-type: %s\n\n%s\n"
			"attempted|%ld\n"
			"retry|%ld\n",
			SPOOLMIME, instr, (long)time(NULL), (long)retry );
		phree(instr);
		CtdlSubmitMsg(msg, NULL, SMTP_SPOOLOUT_ROOM);
		CtdlFreeMessage(msg);
	}

}



/*
 * smtp_do_queue()
 * 
 * Run through the queue sending out messages.
 */
void smtp_do_queue(void) {
	static int doing_queue = 0;

	/*
	 * This is a simple concurrency check to make sure only one queue run
	 * is done at a time.  We could do this with a mutex, but since we
	 * don't really require extremely fine granularity here, we'll do it
	 * with a static variable instead.
	 */
	if (doing_queue) return;
	doing_queue = 1;

	/* 
	 * Go ahead and run the queue
	 */
	lprintf(CTDL_INFO, "SMTP: processing outbound queue\n");

	if (getroom(&CC->room, SMTP_SPOOLOUT_ROOM) != 0) {
		lprintf(CTDL_ERR, "Cannot find room <%s>\n", SMTP_SPOOLOUT_ROOM);
		return;
	}
	CtdlForEachMessage(MSGS_ALL, 0L,
		SPOOLMIME, NULL, smtp_do_procmsg, NULL);

	lprintf(CTDL_INFO, "SMTP: queue run completed\n");
	run_queue_now = 0;
	doing_queue = 0;
}



/*****************************************************************************/
/*                          SMTP UTILITY COMMANDS                            */
/*****************************************************************************/

void cmd_smtp(char *argbuf) {
	char cmd[SIZ];
	char node[SIZ];
	char buf[SIZ];
	int i;
	int num_mxhosts;

	if (CtdlAccessCheck(ac_aide)) return;

	extract(cmd, argbuf, 0);

	if (!strcasecmp(cmd, "mx")) {
		extract(node, argbuf, 1);
		num_mxhosts = getmx(buf, node);
		cprintf("%d %d MX hosts listed for %s\n",
			LISTING_FOLLOWS, num_mxhosts, node);
		for (i=0; i<num_mxhosts; ++i) {
			extract(node, buf, i);
			cprintf("%s\n", node);
		}
		cprintf("000\n");
		return;
	}

	else if (!strcasecmp(cmd, "runqueue")) {
		run_queue_now = 1;
		cprintf("%d All outbound SMTP will be retried now.\n", CIT_OK);
		return;
	}

	else {
		cprintf("%d Invalid command.\n", ERROR + ILLEGAL_VALUE);
	}

}


/*
 * Initialize the SMTP outbound queue
 */
void smtp_init_spoolout(void) {
	struct ctdlroom qrbuf;

	/*
	 * Create the room.  This will silently fail if the room already
	 * exists, and that's perfectly ok, because we want it to exist.
	 */
	create_room(SMTP_SPOOLOUT_ROOM, 3, "", 0, 1, 0);

	/*
	 * Make sure it's set to be a "system room" so it doesn't show up
	 * in the <K>nown rooms list for Aides.
	 */
	if (lgetroom(&qrbuf, SMTP_SPOOLOUT_ROOM) == 0) {
		qrbuf.QRflags2 |= QR2_SYSTEM;
		lputroom(&qrbuf);
	}
}




/*****************************************************************************/
/*                      MODULE INITIALIZATION STUFF                          */
/*****************************************************************************/


char *serv_smtp_init(void)
{
	CtdlRegisterServiceHook(config.c_smtp_port,	/* On the net... */
				NULL,
				smtp_greeting,
				smtp_command_loop);

	CtdlRegisterServiceHook(0,			/* ...and locally */
				"lmtp.socket",
				lmtp_greeting,
				smtp_command_loop);

	smtp_init_spoolout();
	CtdlRegisterSessionHook(smtp_do_queue, EVT_TIMER);
	CtdlRegisterProtoHook(cmd_smtp, "SMTP", "SMTP utility commands");
	return "$Id$";
}
