/* $Id$ */
char *safestrncpy(char *dest, const char *src, size_t n);
int num_tokens (const char *source, char tok);
void extract_token(char *dest, const char *source, int parmnum, char separator);
int extract_int (const char *source, int parmnum);
long extract_long (const char *source, int parmnum);
unsigned long extract_unsigned_long(const char *source, int parmnum);
void CtdlEncodeBase64(char *dest, const char *source, size_t sourcelen);
int CtdlDecodeBase64(char *dest, const char *source, size_t length);
void striplt(char *);
int haschar(const char *st, int ch);
void remove_token(char *source, int parmnum, char separator);
void fmt_date(char *buf, size_t n, time_t thetime, int seconds);
int is_msg_in_mset(char *mset, long msgnum);
char *memreadline(char *start, char *buf, int maxlen);

#ifndef HAVE_STRNCASECMP
int strncasecmp(char *, char *, int);
#endif
#ifndef HAVE_STRCASECMP
#define strcasecmp(x,y) strncasecmp(x,y,INT_MAX);
#endif

#define extract(dest,source,parmnum)	extract_token(dest,source,parmnum,(char)'|')
#define num_parms(source)		num_tokens(source,(char)'|')
void stripout(char *str, char leftboundary, char rightboundary);
void stripallbut(char *str, char leftboundary, char rightboundary);

char *myfgets(char *s, int size, FILE *stream);
void urlesc(char *outbuf, char *strbuf);
char *CtdlTempFileName(char *prefix1, int prefix2);
FILE *CtdlTempFile(void);
char *bmstrstr(char *text, char *pattern,
        int (*cmpfunc)(const char *, const char *, size_t) );

char *ascmonths[12];
