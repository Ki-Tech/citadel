/*
 * $Id$
 *
 * DNS lookup for SMTP sender
 *
 */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <stdio.h>

#ifdef HAVE_RESOLV_H
#include <arpa/nameser.h>
#ifdef HAVE_ARPA_NAMESER_COMPAT_H
#include <arpa/nameser_compat.h>
#endif
#include <resolv.h>
#endif

#include "sysdep_decls.h"
#include "citadel.h"
#include "domain.h"
#include "server.h"
#include "tools.h"
#include "internet_addressing.h"


/*
 * get_hosts() checks the Internet configuration for various types of
 * entries and returns them in the same format as getmx() does -- fill the
 * buffer with a delimited list of hosts and return the number of hosts.
 * 
 * This is used to fetch MX smarthosts, SpamAssassin hosts, etc.
 */
int get_hosts(char *mxbuf, char *rectype) {
	int config_lines;
	int i;
	char buf[256];
	char host[256], type[256];
	int total_smarthosts = 0;

	if (inetcfg == NULL) return(0);
	strcpy(mxbuf, "");

	config_lines = num_tokens(inetcfg, '\n');
	for (i=0; i<config_lines; ++i) {
		extract_token(buf, inetcfg, i, '\n', sizeof buf);
		extract_token(host, buf, 0, '|', sizeof host);
		extract_token(type, buf, 1, '|', sizeof type);

		if (!strcasecmp(type, rectype)) {
			strcat(mxbuf, host);
			strcat(mxbuf, "|");
			++total_smarthosts;
		}
	}

	return(total_smarthosts);
}


/*
 * Compare the preference of two MX records.  First check by the actual
 * number listed in the MX record.  If they're identical, randomize the
 * result.
 */
int mx_compare_pref(const void *mx1, const void *mx2) {
	int pref1;
	int pref2;

	pref1 = ((const struct mx *)mx1)->pref;
	pref2 = ((const struct mx *)mx2)->pref;

	if (pref1 > pref2) {
		return(1);
	}
	else if (pref1 < pref2) {
		return(0);
	}
	else {
		return(rand() % 2);
	}
}


/* 
 * getmx()
 *
 * Return one or more MX's for a mail destination.
 *
 * Upon success, it fills 'mxbuf' with one or more MX hosts, separated by
 * vertical bar characters, and returns the number of hosts as its return
 * value.  If no MX's are found, it returns 0.
 *
 */
int getmx(char *mxbuf, char *dest) {

#ifdef HAVE_RESOLV_H
	union {
			u_char bytes[1024];
			HEADER header;
    } answer;
#else
	char buf[SIZ];
	FILE *fp;
#endif

	int ret;
	unsigned char *startptr, *endptr, *ptr;
	char expanded_buf[1024];
	unsigned short pref, type;
	int n = 0;
	int qdcount;

	struct mx *mxrecs = NULL;
	int num_mxrecs = 0;
	
	/* If we're configured to send all mail to a smart-host, then our
	 * job here is really easy.
	 */
	n = get_hosts(mxbuf, "smarthost");
	if (n > 0) return(n);

	/*
	 * No smart-host?  Look up the best MX for a site.
	 */

#ifndef HAVE_RESOLV_H

	/*
	 * On systems with b0rken or non-standard resolver libraries, learn
	 * the MX records by calling "nslookup" from the command line.
	 *
	 * Someday.
	 *
	 */

	return(0);

#else /* HAVE_RESOLV_H */

	/*
	 * Make a call to the standard resolver library.
	 */

	ret = res_query(
		dest,
		C_IN, T_MX, (unsigned char *)answer.bytes, sizeof(answer)  );

	if (ret < 0) {
		mxrecs = malloc(sizeof(struct mx));
		mxrecs[0].pref = 0;
		strcpy(mxrecs[0].host, dest);
		num_mxrecs = 1;
	}
	else {

		/* If we had to truncate, shrink the number to avoid fireworks */
		if (ret > sizeof(answer))
			ret = sizeof(answer);
	
		startptr = &answer.bytes[0];		/* start and end of buffer */
		endptr = &answer.bytes[ret];
		ptr = startptr + HFIXEDSZ;	/* advance past header */
	
		for (qdcount = ntohs(answer.header.qdcount); qdcount--; ptr += ret + QFIXEDSZ) {
			if ((ret = dn_skipname(ptr, endptr)) < 0) {
				lprintf(CTDL_DEBUG, "dn_skipname error\n");
				return(0);
			}
		}
	
		while(1) {
			memset(expanded_buf, 0, sizeof(expanded_buf));
			ret = dn_expand(startptr,
					endptr,
					ptr,
					expanded_buf,
					sizeof(expanded_buf)
					);
			if (ret < 0) break;
			ptr += ret;
	
			GETSHORT(type, ptr);
			ptr += INT16SZ + INT32SZ;
			GETSHORT(n, ptr);
	
			if (type != T_MX) {
				ptr += n;
			}
	
			else {
				GETSHORT(pref, ptr);
				ret = dn_expand(startptr,
						endptr,
						ptr,
						expanded_buf,
						sizeof(expanded_buf)
						);
				ptr += ret;
	
				++num_mxrecs;
				if (mxrecs == NULL) {
					mxrecs = malloc(sizeof(struct mx));
				}
				else {
					mxrecs = realloc(mxrecs,
					    (sizeof(struct mx) * num_mxrecs) );
				}
	
				mxrecs[num_mxrecs - 1].pref = pref;
				strcpy(mxrecs[num_mxrecs - 1].host,
				       expanded_buf);
			}
		}
	}
#endif /* HAVE_RESOLV_H */

	/* Sort the MX records by preference */
	if (num_mxrecs > 1) {
		qsort(mxrecs, num_mxrecs, sizeof(struct mx), mx_compare_pref);
	}

	strcpy(mxbuf, "");
	for (n=0; n<num_mxrecs; ++n) {
		strcat(mxbuf, mxrecs[n].host);
		strcat(mxbuf, "|");
	}
	free(mxrecs);
	return(num_mxrecs);
}
