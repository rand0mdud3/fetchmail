/* Copyright 1996 by Eric S. Raymond
 * All rights reserved.
 * For license terms, see the file COPYING in this directory.
 */

/***********************************************************************
  module:       driver.c
  project:      fetchmail
  programmer:   Eric S. Raymond
  description:  Generic driver for mail fetch method protocols

 ***********************************************************************/

#include  <config.h>
#include  <stdio.h>
#include  <malloc.h>
#include  <varargs.h>
#include  <sys/time.h>
#include  <signal.h>

#ifdef KERBEROS_V4
#include <krb.h>
#include <des.h>
#include <netinet/in.h>		/* must be included before "socket.h".*/
#include <netdb.h>
#endif /* KERBEROS_V4 */
#include  "socket.h"
#include  "fetchmail.h"
#include  "smtp.h"

#define	SMTP_PORT	25	/* standard SMTP service port */

static struct method *protocol;

static int alarmed; /* A flag to indicate that SIGALRM happened */
int timeout = CLIENT_TIMEOUT;

char tag[TAGLEN];
static int tagnum;
#define GENSYM	(sprintf(tag, "a%04d", ++tagnum), tag)

/*********************************************************************
  function:      
  description:   hack message headers so replies will work properly

  arguments:
    after        where to put the hacked header
    before       header to hack
    host         name of the pop header

  return value:  none.
  calls:         none.
 *********************************************************************/

static void reply_hack(buf, host)
/* hack local mail IDs -- code by Eric S. Raymond 20 Jun 1996 */
char *buf;
const char *host;
{
    const char *from;
    int state = 0;
    char mycopy[POPBUFSIZE+1];

    if (strncmp("From: ", buf, 6)
	&& strncmp("To: ", buf, 4)
	&& strncmp("Reply-", buf, 6)
	&& strncmp("Cc: ", buf, 4)
	&& strncmp("Bcc: ", buf, 5)) {
	return;
    }

    strcpy(mycopy, buf);
    for (from = mycopy; *from; from++)
    {
	switch (state)
	{
	case 0:   /* before header colon */
	    if (*from == ':')
		state = 1;
	    break;

	case 1:   /* we've seen the colon, we're looking for addresses */
	    if (*from == '"')
		state = 2;
	    else if (*from == '(')
		state = 3;    
	    else if (*from == '<' || isalnum(*from))
		state = 4;
	    break;

	case 2:   /* we're in a quoted human name, copy and ignore */
	    if (*from == '"')
		state = 1;
	    break;

	case 3:   /* we're in a parenthesized human name, copy and ignore */
	    if (*from == ')')
		state = 1;
	    break;

	case 4:   /* the real work gets done here */
	    /*
	     * We're in something that might be an address part,
	     * either a bare unquoted/unparenthesized text or text
	     * enclosed in <> as per RFC822.
	     */
	    /* if the address part contains an @, don't mess with it */
	    if (*from == '@')
		state = 5;

	    /* If the address token is not properly terminated, ignore it. */
	    else if (*from == ' ' || *from == '\t')
		state = 1;

	    /*
	     * On proper termination with no @, insert hostname.
	     * Case '>' catches <>-enclosed mail IDs.  Case ',' catches
	     * comma-separated bare IDs.  Cases \r and \n catch the case
	     * of a single ID alone on the line.
	     */
	    else if (strchr(">,\r\n", *from))
	    {
		strcpy(buf, "@");
		strcat(buf, host);
		buf += strlen(buf);
		state = 1;
	    }

	    /* everything else, including alphanumerics, just passes through */
	    break;

	case 5:   /* we're in a remote mail ID, no need to append hostname */
	    if (*from == '>' || *from == ',' || isspace(*from))
		state = 1;
	    break;
	}

	/* all characters from the old buffer get copied to the new one */
	*buf++ = *from;
    }
    *buf++ = '\0';
}

/*********************************************************************
  function:      nxtaddr
  description:   Parse addresses in succession out of a specified RFC822
                 header.  Note 1: RFC822 escaping with \ is *not* handled.
                 Note 2: it is important that this routine not stop on \r,
                 since we use \r as a marker for RFC822 continuations below.
  arguments:     
    hdr          header line to be parsed, NUL to continue in previous hdr

  return value:  next address, or NUL if there is no next address
  calls:         none
 *********************************************************************/

static char *nxtaddr(hdr)
char *hdr;
{
    static char	*hp, *tp, address[POPBUFSIZE+1];
    static	state;

    if (hdr)
    {
	hp = hdr;
	state = 0;
    }

    for (; *hp; hp++)
    {
	switch (state)
	{
	case 0:   /* before header colon */
	    if (*hp == '\n')
		return(NULL);
	    else if (*hp == ':')
	    {
		state = 1;
		tp = address;
	    }
	    break;

	case 1:   /* we've seen the colon, now grab the address */
	    if ((*hp == '\n') || (*hp == ','))  /* end of address list */
	    {
	        *tp++ = '\0';
		return(address);
	    }
	    else if (*hp == '"') /* quoted string */
	    {
	        state = 2;
		*tp++ = *hp;
	    }
	    else if (*hp == '(') /* address comment -- ignore */
		state = 3;    
	    else if (*hp == '<') /* begin <address> */
	    {
		state = 4;
		tp = address;
	    }
	    else if (isspace(*hp)) /* ignore space */
	        state = 1;
	    else   /* just take it */
	    {
		state = 1;
		*tp++ = *hp;
	    }
	    break;

	case 2:   /* we're in a quoted string, copy verbatim */
	    if (*hp == '\n')
		return(NULL);
	    if (*hp != '"')
	        *tp++ = *hp;
	    else if (*hp == '"')
	    {
	        *tp++ = *hp;
		state = 1;
	    }
	    break;

	case 3:   /* we're in a parenthesized comment, ignore */
	    if (*hp == '\n')
		return(NULL);
	    else if (*hp == ')')
		state = 1;
	    break;

	case 4:   /* possible <>-enclosed address */
	    if (*hp == '>') /* end of address */
	    {
		*tp++ = '\0';
		state = 1;
		return(address);
	    }
	    else if (*hp == '<')  /* nested <> */
	        tp = address;
	    else if (*hp == '"') /* quoted address */
	    {
	        *tp++ = *hp;
		state = 5;
	    }
	    else  /* just copy address */
		*tp++ = *hp;
	    break;

	case 5:   /* we're in a quoted address, copy verbatim */
	    if (*hp == '\n')  /* mismatched quotes */
		return(NULL);
	    if (*hp != '"')  /* just copy it if it isn't a quote */
	        *tp++ = *hp;
	    else if (*hp == '"')  /* end of quoted string */
	    {
	        *tp++ = *hp;
		state = 4;
	    }
	    break;
	}
    }

    return(NULL);
}

/*********************************************************************
  function:      gen_readmsg
  description:   Read the message content 

 as described in RFC 1225.
  arguments:     
    socket       ... to which the server is connected.
    mboxfd       open file descriptor to which the retrieved message will
                 be written.
    len          length of text 
    delimited    does the protocol use a message delimiter?
    queryctl     host control block

  return value:  zero if success else PS_* return code.
  calls:         SockGets.
  globals:       reads outlevel. 
 *********************************************************************/

static int gen_readmsg (socket, mboxfd, len, delimited, queryctl)
int socket;
int mboxfd;
long len;
int delimited;
struct hostrec *queryctl;
{ 
    char buf [MSGBUFSIZE+1]; 
    char fromBuf[MSGBUFSIZE+1];
    char *bufp, *headers, *unixfrom, *fromhdr, *tohdr, *cchdr, *bcchdr;
    int n, oldlen;
    int inheaders;
    int lines,sizeticker;
    time_t now;
    /* This keeps the retrieved message count for display purposes */
    static int msgnum = 0;  

    /* read the message content from the server */
    inheaders = 1;
    headers = unixfrom = fromhdr = tohdr = cchdr = bcchdr = NULL;
    lines = 0;
    sizeticker = 0;
    while (delimited || len > 0) {
	if ((n = SockGets(socket,buf,sizeof(buf))) < 0)
	    return(PS_SOCKET);
	len -= n;
	bufp = buf;
	if (buf[0] == '\0' || buf[0] == '\r' || buf[0] == '\n')
	    inheaders = 0;
	if (*bufp == '.') {
	    bufp++;
	    if (delimited && *bufp == 0)
		break;  /* end of message */
	}
	strcat(bufp, "\n");
     
	if (inheaders)
        {
	    if (!queryctl->norewrite)
		reply_hack(bufp, queryctl->servername);

	    if (!lines)
	    {
		oldlen = strlen(bufp);
		headers = malloc(oldlen + 1);
		if (headers == NULL)
		    return(PS_IOERR);
		(void) strcpy(headers, bufp);
		bufp = headers;
	    }
	    else
	    {
		int	newlen;

		/*
		 * We deal with RFC822 continuation lines here.
		 * Replace previous '\n' with '\r' so nxtaddr 
		 * and reply_hack will be able to see past it.
		 * (We know this safe because SocketGets stripped
		 * out all carriage returns in the read loop above
		 * and we haven't reintroduced any since then.)
		 * We'll undo this before writing the header.
		 */
		if (isspace(bufp[0]))
		    headers[oldlen-1] = '\r';

		newlen = oldlen + strlen(bufp);
		headers = realloc(headers, newlen + 1);
		if (headers == NULL)
		    return(PS_IOERR);
		strcpy(headers + oldlen, bufp);
		bufp = headers + oldlen;
		oldlen = newlen;
	    }

	    if (!strncmp(bufp,"From ",5))
		unixfrom = bufp;
	    else if (!strncasecmp("From: ", bufp, 6))
		fromhdr = bufp;
	    else if (!strncasecmp("To: ", bufp, 4))
		tohdr = bufp;
	    else if (!strncasecmp("Cc: ", bufp, 4))
		cchdr = bufp;
	    else if (!strncasecmp("Bcc: ", bufp, 5))
		bcchdr = bufp;

	    goto skipwrite;
	}
	else if (headers)
	{
	    char	*cp;

	    if (!queryctl->mda[0])
	    {
		if (SMTP_from(mboxfd, nxtaddr(fromhdr)) != SM_OK)
		    return(PS_SMTP);
#ifdef SMTP_RESEND
		/*
		 * This is what we'd do if fetchmail were a real MDA
		 * a la sendmail -- crack all the destination headers
		 * and send to every address we can reach via SMTP.
		 */
		if (tohdr && (cp = nxtaddr(tohdr)) != (char *)NULL)
		    do {
			if (SMTP_rcpt(mboxfd, cp) == SM_UNRECOVERABLE)
			    return(PS_SMTP);
		    } while
			(cp = nxtaddr(NULL));
		if (cchdr && (cp = nxtaddr(cchdr)) != (char *)NULL)
		    do {
			if (SMTP_rcpt(mboxfd, cp) == SM_UNRECOVERABLE)
			    return(PS_SMTP);
		    } while
			(cp = nxtaddr(NULL));
		if (bcchdr && (cp = nxtaddr(bcchdr)) != (char *)NULL)
		    do {
			if (SMTP_rcpt(mboxfd, cp) == SM_UNRECOVERABLE)
			    return(PS_SMTP);
		    } while
			(cp = nxtaddr(NULL));
#else
		/*
		 * Since we're really only fetching mail for one user
		 * per host query, we can be simpler
		 */
		if (SMTP_rcpt(mboxfd, queryctl->localname) == SM_UNRECOVERABLE)
		    return(PS_SMTP);
#endif /* SMTP_RESEND */
		SMTP_data(mboxfd);
		if (outlevel == O_VERBOSE)
		    fputs("SMTP> ", stderr);
	    }

	    /* change continuation markers back to regular newlines */
	    for (cp = headers; cp < headers +  oldlen; cp++)
		if (*cp == '\r')
		    *cp = '\n';

	    /*
	     * Strictly speaking, we ought to replace all LFs
	     * with CR-LF before shipping to an SMTP listener.
	     * Since most SMTP listeners built since the mid-1980s
	     * (and *all* Unix listeners) accept lone LF as equivalent
	     * to CR-LF, we'll skip this particular contortion.
	     */
	    if (write(mboxfd,headers,oldlen) < 0)
	    {
		free(headers);
		headers = NULL;
		perror("gen_readmsg: writing RFC822 headers");
		return(PS_IOERR);
	    }
	    else if (outlevel == O_VERBOSE)
		fputs("#", stderr);
	    free(headers);
	    headers = NULL;
	}

	/* SMTP byte-stuffing */
	if (*bufp == '.' && queryctl->mda[0] == 0)
	    write(mboxfd, ".", 1);

	/* write this line to the file -- comment about CR-LF vs. LF applies */
	if (write(mboxfd,bufp,strlen(bufp)) < 0)
	{
	    perror("gen_readmsg: writing message text");
	    return(PS_IOERR);
	}
	else if (outlevel == O_VERBOSE)
	    fputc('*', stderr);

    skipwrite:;

	/* write the message size dots */
	sizeticker += strlen(bufp);
	while (sizeticker >= SIZETICKER)
	{
	    if (outlevel > O_SILENT && outlevel < O_VERBOSE)
		fputc('.',stderr);
	    sizeticker -= SIZETICKER;

	    /* reset timeout so we don't choke on very long messages */
	    alarm(timeout);
	}
	lines++;
    }

    if (alarmed)
       return (0);
    /* write message terminator */
    if (!queryctl->mda[0])
	if (SMTP_eom(mboxfd) != SM_OK)
	    return(PS_SMTP);
    return(0);
}

#ifdef KERBEROS_V4
/*********************************************************************
  function:      kerberos_auth
  description:   Authenticate to the server host using Kerberos V4

  arguments:     
    socket       socket to server
    servername   server name

  return value:  exit code from the set of PS_.* constants defined in 
                 fetchmail.h
  calls:
  globals:       reads outlevel.
 *********************************************************************/

int
kerberos_auth (socket, servername) 
     int socket;
     char *servername;
{
    char * host_primary;
    KTEXT ticket;
    MSG_DAT msg_data;
    CREDENTIALS cred;
    Key_schedule schedule;
    int rem;
  
    /* Get the primary name of the host.  */
    {
	struct hostent * hp = (gethostbyname (servername));
	if (hp == 0)
	{
	    fprintf (stderr, "fetchmail: server %s unknown: n", servername);
	    return (PS_ERROR);
	}
	host_primary = ((char *) (malloc ((strlen (hp -> h_name)) + 1)));
	strcpy (host_primary, (hp -> h_name));
    }
  
    ticket = ((KTEXT) (malloc (sizeof (KTEXT_ST))));
    rem
	= (krb_sendauth (0L, socket, ticket, "pop",
			 host_primary,
			 ((char *) (krb_realmofhost (host_primary))),
			 ((unsigned long) 0),
			 (&msg_data),
			 (&cred),
			 (schedule),
			 ((struct sockaddr_in *) 0),
			 ((struct sockaddr_in *) 0),
			 "KPOPV0.1"));
    free (ticket);
    free (host_primary);
    if (rem != KSUCCESS)
    {
	fprintf (stderr, "fetchmail: kerberos error %s\n", (krb_get_err_text (rem)));
	return (PS_ERROR);
    }
    return (0);
}
#endif /* KERBEROS_V4 */

/*********************************************************************
  function:      do_protocol
  description:   retrieve messages from the specified mail server
                 using a given set of methods

  arguments:     
    queryctl     fully-specified options (i.e. parsed, defaults invoked,
                 etc).
    proto        protocol method pointer

  return value:  exit code from the set of PS_.* constants defined in 
                 fetchmail.h
  calls:
  globals:       reads outlevel.
 *********************************************************************/

int do_protocol(queryctl, proto)
struct hostrec *queryctl;
struct method *proto;
{
    int ok, len;
    int mboxfd = -1;
    char buf [POPBUFSIZE+1], host[HOSTLEN+1];
    int socket;
    void (*sigsave)();
    int num, count, deletions = 0;

    alarmed = 0;
    sigsave = signal(SIGALRM, alarm_handler);
    alarm (timeout);

#ifndef KERBEROS_V4
    if (queryctl->authenticate == A_KERBEROS)
    {
	fputs("fetchmail: Kerberos support not linked.\n", stderr);
	return(PS_ERROR);
    }
#endif /* KERBEROS_V4 */

    /* lacking methods, there are some options that may fail */
    if (!proto->is_old)
    {
	/* check for unsupported options */
	if (queryctl->flush) {
	    fprintf(stderr,
		    "Option --flush is not supported with %s\n",
		    proto->name);
            alarm(0);
            signal(SIGALRM, sigsave);
	    return(PS_SYNTAX);
	}
	else if (queryctl->fetchall) {
	    fprintf(stderr,
		    "Option --all is not supported with %s\n",
		    proto->name);
            alarm(0);
            signal(SIGALRM, sigsave);
	    return(PS_SYNTAX);
	}
    }

    tagnum = 0;
    tag[0] = '\0';	/* nuke any tag hanging out from previous query */
    protocol = proto;

    /* open a socket to the mail server */
    if ((socket = Socket(queryctl->servername,
			 queryctl->port ? queryctl->port : protocol->port))<0 
         || alarmed)
    {
	perror("fetchmail, connecting to host");
	ok = PS_SOCKET;
	goto closeUp;
    }

#ifdef KERBEROS_V4
    if (queryctl->authentication == A_KERBEROS)
    {
	ok = (kerberos_auth (socket, queryctl->servername));
	if (ok != 0)
	    goto cleanUp;
    }
#endif /* KERBEROS_V4 */

    /* accept greeting message from mail server */
    ok = (protocol->parse_response)(socket, buf);
    if (alarmed || ok != 0)
	goto cleanUp;

    /* try to get authorized to fetch mail */
    ok = (protocol->getauth)(socket, queryctl, buf);
    if (alarmed || ok == PS_ERROR)
	ok = PS_AUTHFAIL;
    if (alarmed || ok != 0)
	goto cleanUp;

    /* compute count, and get UID list if possible */
    if ((protocol->getrange)(socket, queryctl, &count) != 0 || alarmed)
	goto cleanUp;

    /* show user how many messages we downloaded */
    if (outlevel > O_SILENT && outlevel < O_VERBOSE)
	if (count == 0)
	    fprintf(stderr, "No mail from %s\n", queryctl->servername);
	else
	    fprintf(stderr,
		    "%d message%s from %s.\n",
		    count, count > 1 ? "s" : "", 
		    queryctl->servername);

    if ((count > 0) && (!check_only))
    {
	if (queryctl->mda[0] == '\0')
	    if ((mboxfd = Socket(queryctl->smtphost, SMTP_PORT)) < 0
		|| SMTP_ok(mboxfd, NULL) != SM_OK
		|| SMTP_helo(mboxfd, queryctl->servername) != SM_OK 
                || alarmed)
	    {
		ok = PS_SMTP;
		close(mboxfd);
		mboxfd = -1;
		goto cleanUp;
	    }
    
	/* read, forward, and delete messages */
	for (num = 1; num <= count; num++)
	{
	    int	treat_as_new = 
		!protocol->is_old 
		|| !(protocol->is_old)(socket, queryctl, num);

	    /* we may want to reject this message if it's old */
	    if (treat_as_new || queryctl->fetchall)
	    {
		/* request a message */
		(protocol->fetch)(socket, num, &len);

		if (outlevel > O_SILENT)
		{
		    fprintf(stderr, "reading message %d", num);
		    if (len > 0)
			fprintf(stderr, " (%d bytes)", len);
		    if (outlevel == O_VERBOSE)
			fputc('\n', stderr);
		    else
			fputc(' ', stderr);
		}

		/* open the delivery pipe now if we're using an MDA */
		if (queryctl->mda[0])
		    if ((mboxfd = openmailpipe(queryctl)) < 0)
			goto cleanUp;

		/* read the message and ship it to the output sink */
		ok = gen_readmsg(socket, mboxfd,
				 len, 
				 protocol->delimited,
				 queryctl);

		/* close the delivery pipe, we'll reopen before next message */
		if (queryctl->mda[0])
		    if ((ok = closemailpipe(mboxfd)) != 0 || alarmed)
			goto cleanUp;

		/* tell the server we got it OK and resynchronize */
		if (protocol->trail)
		    (protocol->trail)(socket, queryctl, num);
		if (alarmed || ok != 0)
		    goto cleanUp;
	    }

	    /*
	     * At this point in flow of control, either we've bombed
	     * on a protocol error or had delivery refused by the SMTP
	     * server (unlikely -- I've never seen it) or we've seen
	     * `accepted for delivery' and the message is shipped.
	     * It's safe to mark the message seen and delete it on the
	     * server now.
	     */

	    /* maybe we delete this message now? */
	    if (protocol->delete
		&& !queryctl->keep
		&& (treat_as_new || queryctl->flush))
	    {
		deletions++;
		if (outlevel > O_SILENT && outlevel < O_VERBOSE) 
		    fprintf(stderr, " flushed\n", num);
		ok = (protocol->delete)(socket, queryctl, num);
		if (alarmed || ok != 0)
		    goto cleanUp;
	    }
	    else if (outlevel > O_SILENT && outlevel < O_VERBOSE) 
	    {
		/* nuke it from the unseen-messages list */
		delete_uid(&queryctl->newsaved, num);
		fprintf(stderr, " not flushed\n", num);
	    }
	}

	/* remove all messages flagged for deletion */
        if (protocol->expunge_cmd && deletions > 0)
	{
	    ok = gen_transact(socket, protocol->expunge_cmd);
	    if (alarmed || ok != 0)
		goto cleanUp;
        }

	ok = gen_transact(socket, protocol->exit_cmd);
	if (alarmed || ok == 0)
	    ok = PS_SUCCESS;
	close(socket);
	goto closeUp;
    }
    else if (check_only) {
      ok = ((count > 0) ? PS_SUCCESS : PS_NOMAIL);
      goto closeUp;
    }
    else {
	ok = gen_transact(socket, protocol->exit_cmd);
	if (ok == 0)
	    ok = PS_NOMAIL;
	close(socket);
	goto closeUp;
    }

cleanUp:
    if (ok != 0 && ok != PS_SOCKET)
    {
	gen_transact(socket, protocol->exit_cmd);
	close(socket);
    }

closeUp:
    if (mboxfd != -1)
    {
	SMTP_quit(mboxfd);
	close(mboxfd);
    }
    alarm(0);
    signal(SIGALRM, sigsave);
    return(ok);
}

/*********************************************************************
  function:      gen_send
  description:   Assemble command in print style and send to the server

  arguments:     
    socket       socket to which the server is connected.
    fmt          printf-style format

  return value:  none.
  calls:         SockPuts.
  globals:       reads outlevel.
 *********************************************************************/

void gen_send(socket, fmt, va_alist)
int socket;
const char *fmt;
va_dcl {

  char buf [POPBUFSIZE+1];
  va_list ap;

  if (protocol->tagged)
      (void) sprintf(buf, "%s ", GENSYM);
  else
      buf[0] = '\0';

  va_start(ap);
  vsprintf(buf + strlen(buf), fmt, ap);
  va_end(ap);

  SockPuts(socket, buf);

  if (outlevel == O_VERBOSE)
    fprintf(stderr,"> %s\n", buf);
}

/*********************************************************************
  function:      gen_transact
  description:   Assemble command in print style and send to the server.
                 then accept a protocol-dependent response.

  arguments:     
    socket       socket to which the server is connected.
    fmt          printf-style format

  return value:  none.
  calls:         SockPuts.
  globals:       reads outlevel.
 *********************************************************************/

int gen_transact(socket, fmt, va_alist)
int socket;
const char *fmt;
va_dcl {

  int ok;
  char buf [POPBUFSIZE+1];
  va_list ap;

  if (protocol->tagged)
      (void) sprintf(buf, "%s ", GENSYM);
  else
      buf[0] = '\0';

  va_start(ap);
  vsprintf(buf + strlen(buf), fmt, ap);
  va_end(ap);

  SockPuts(socket, buf);
  if (outlevel == O_VERBOSE)
    fprintf(stderr,"> %s\n", buf);

  /* we presume this does its own response echoing */
  ok = (protocol->parse_response)(socket, buf);

  return(ok);
}


/*********************************************************************
  function:      alarm_handler
  description:   In real life process can get stuck waiting for 
                 something. This deadlock is avoided here by this 
                 sending  SIGALRM

  arguments:     
    signal       hopefully SIGALRM

  return value:  none.
  calls:         none
  globals:       sets alarmed to 1
 *********************************************************************/
void 
alarm_handler (int signal)
{
    alarmed = 1;
    fprintf(stderr,"fetchmail: timeout after %d seconds.\n", timeout);
}
