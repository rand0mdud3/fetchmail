/*
 * imap.c -- IMAP2bis/IMAP4 protocol methods
 *
 * Copyright 1997 by Eric S. Raymond
 * For license terms, see the file COPYING in this directory.
 */

#include  "config.h"
#include  <stdio.h>
#include  <string.h>
#include  <ctype.h>
#if defined(STDC_HEADERS)
#include  <stdlib.h>
#endif
#include  "fetchmail.h"
#include  "socket.h"

#ifdef KERBEROS_V4
#ifdef KERBEROS_V5
#include <kerberosIV/des.h>
#include <kerberosIV/krb.h>
#else
#if defined (__bsdi__)
#include <des.h>
#define krb_get_err_text(e) (krb_err_txt[e])
#endif
#if defined(__NetBSD__) || (__FreeBSD__) || defined(__linux__)
#define krb_get_err_text(e) (krb_err_txt[e])
#endif
#include <krb.h>
#endif
#endif /* KERBEROS_V4 */
#include  "i18n.h"

#ifdef GSSAPI
#ifdef HAVE_GSSAPI_H
#include <gssapi.h>
#endif
#ifdef HAVE_GSSAPI_GSSAPI_H
#include <gssapi/gssapi.h>
#endif
#ifdef HAVE_GSSAPI_GSSAPI_GENERIC_H
#include <gssapi/gssapi_generic.h>
#endif
#ifndef HAVE_GSS_C_NT_HOSTBASED_SERVICE
#define GSS_C_NT_HOSTBASED_SERVICE gss_nt_service_name
#endif
#endif

#include "md5.h"

#if OPIE_ENABLE
#include <opie.h>
#endif /* OPIE_ENABLE */

#ifndef strstr		/* glibc-2.1 declares this as a macro */
extern char *strstr();	/* needed on sysV68 R3V7.1. */
#endif /* strstr */

/* imap_version values */
#define IMAP2		-1	/* IMAP2 or IMAP2BIS, RFC1176 */
#define IMAP4		0	/* IMAP4 rev 0, RFC1730 */
#define IMAP4rev1	1	/* IMAP4 rev 1, RFC2060 */

static int count, unseen, deletions, imap_version, preauth; 
static int expunged, expunge_period, saved_timeout;
static flag do_idle;
static char capabilities[MSGBUFSIZE+1];
static unsigned int *unseen_messages;

int imap_ok(int sock, char *argbuf)
/* parse command response */
{
    char buf[MSGBUFSIZE+1];

    do {
	int	ok;
	char	*cp;

	if ((ok = gen_recv(sock, buf, sizeof(buf))))
	    return(ok);

	/* all tokens in responses are caseblind */
	for (cp = buf; *cp; cp++)
	    if (islower(*cp))
		*cp = toupper(*cp);

	/* interpret untagged status responses */
	if (strstr(buf, "* CAPABILITY"))
	    strncpy(capabilities, buf + 12, sizeof(capabilities));
	else if (strstr(buf, "EXISTS"))
	{
	    count = atoi(buf+2);
	    /*
	     * Nasty kluge to handle RFC2177 IDLE.  If we know we're idling
	     * we can't wait for the tag matching the IDLE; we have to tell the
	     * server the IDLE is finished by shipping back a DONE when we
	     * see an EXISTS.  Only after that will a tagged response be
	     * shipped.  The idling flag also gets cleared on a timeout.
	     */
	    if (stage == STAGE_IDLE)
	    {
		/* we do our own write and report here to disable tagging */
		SockWrite(sock, "DONE\r\n", 6);
		if (outlevel >= O_MONITOR)
		    report(stdout, "IMAP> DONE\n");

		mytimeout = saved_timeout;
		stage = STAGE_FETCH;
	    }
	}
	else if (strstr(buf, "PREAUTH"))
	    preauth = TRUE;
    } while
	(tag[0] != '\0' && strncmp(buf, tag, strlen(tag)));

    if (tag[0] == '\0')
    {
	if (argbuf)
	    strcpy(argbuf, buf);
	return(PS_SUCCESS); 
    }
    else
    {
	char	*cp;

	/* skip the tag */
	for (cp = buf; !isspace(*cp); cp++)
	    continue;
	while (isspace(*cp))
	    cp++;

        if (strncmp(cp, "OK", 2) == 0)
	{
	    if (argbuf)
		strcpy(argbuf, cp);
	    return(PS_SUCCESS);
	}
	else if (strncmp(cp, "BAD", 3) == 0)
	    return(PS_ERROR);
	else if (strncmp(cp, "NO", 2) == 0)
	{
	    if (stage == STAGE_GETAUTH) 
		return(PS_AUTHFAIL);	/* RFC2060, 6.2.2 */
	    else
		return(PS_ERROR);
	}
	else
	    return(PS_PROTOCOL);
    }
}

#if OPIE_ENABLE
static int do_otp(int sock, struct query *ctl)
{
    int i, rval;
    char buffer[128];
    char challenge[OPIE_CHALLENGE_MAX+1];
    char response[OPIE_RESPONSE_MAX+1];

    gen_send(sock, "AUTHENTICATE X-OTP");

    if (rval = gen_recv(sock, buffer, sizeof(buffer)))
	return rval;

    if ((i = from64tobits(challenge, buffer)) < 0) {
	report(stderr, _("Could not decode initial BASE64 challenge\n"));
	return PS_AUTHFAIL;
    };


    to64frombits(buffer, ctl->remotename, strlen(ctl->remotename));

    if (outlevel >= O_MONITOR)
	report(stdout, "IMAP> %s\n", buffer);

    /* best not to count on the challenge code handling multiple writes */
    strcat(buffer, "\r\n");
    SockWrite(sock, buffer, strlen(buffer));

    if (rval = gen_recv(sock, buffer, sizeof(buffer)))
	return rval;

    if ((i = from64tobits(challenge, buffer)) < 0) {
	report(stderr, _("Could not decode OTP challenge\n"));
	return PS_AUTHFAIL;
    };

    rval = opiegenerator(challenge, !strcmp(ctl->password, "opie") ? "" : ctl->password, response);
    if ((rval == -2) && !run.poll_interval) {
	char secret[OPIE_SECRET_MAX+1];
	fprintf(stderr, _("Secret pass phrase: "));
	if (opiereadpass(secret, sizeof(secret), 0))
	    rval = opiegenerator(challenge, secret, response);
	memset(secret, 0, sizeof(secret));
    };

    if (rval)
	return(PS_AUTHFAIL);

    to64frombits(buffer, response, strlen(response));

    if (outlevel >= O_MONITOR)
	report(stdout, "IMAP> %s\n", buffer);
    strcat(buffer, "\r\n");
    SockWrite(sock, buffer, strlen(buffer));

    if (rval = gen_recv(sock, buffer, sizeof(buffer)))
	return rval;

    if (strstr(buffer, "OK"))
	return PS_SUCCESS;
    else
	return PS_AUTHFAIL;
};
#endif /* OPIE_ENABLE */

#ifdef KERBEROS_V4
#if SIZEOF_INT == 4
typedef	int	int32;
#elif SIZEOF_SHORT == 4
typedef	short	int32;
#elif SIZEOF_LONG == 4
typedef	long	int32;
#else
#error Cannot deduce a 32-bit-type
#endif

static int do_rfc1731(int sock, char *truename)
/* authenticate as per RFC1731 -- note 32-bit integer requirement here */
{
    int result = 0, len;
    char buf1[4096], buf2[4096];
    union {
      int32 cint;
      char cstr[4];
    } challenge1, challenge2;
    char srvinst[INST_SZ];
    char *p;
    char srvrealm[REALM_SZ];
    KTEXT_ST authenticator;
    CREDENTIALS credentials;
    char tktuser[MAX_K_NAME_SZ+1+INST_SZ+1+REALM_SZ+1];
    char tktinst[INST_SZ];
    char tktrealm[REALM_SZ];
    des_cblock session;
    des_key_schedule schedule;

    gen_send(sock, "AUTHENTICATE KERBEROS_V4");

    /* The data encoded in the first ready response contains a random
     * 32-bit number in network byte order.  The client should respond
     * with a Kerberos ticket and an authenticator for the principal
     * "imap.hostname@realm", where "hostname" is the first component
     * of the host name of the server with all letters in lower case
     * and where "realm" is the Kerberos realm of the server.  The
     * encrypted checksum field included within the Kerberos
     * authenticator should contain the server provided 32-bit number
     * in network byte order.
     */

    if (result = gen_recv(sock, buf1, sizeof buf1)) {
	return result;
    }

    len = from64tobits(challenge1.cstr, buf1);
    if (len < 0) {
	report(stderr, _("could not decode initial BASE64 challenge\n"));
	return PS_AUTHFAIL;
    }

    /* this patch by Dan Root <dar@thekeep.org> solves an endianess
     * problem. */
    {
	char tmp[4];

	*(int *)tmp = ntohl(*(int *) challenge1.cstr);
	memcpy(challenge1.cstr, tmp, sizeof(tmp));
    }

    /* Client responds with a Kerberos ticket and an authenticator for
     * the principal "imap.hostname@realm" where "hostname" is the
     * first component of the host name of the server with all letters
     * in lower case and where "realm" is the Kerberos realm of the
     * server.  The encrypted checksum field included within the
     * Kerberos authenticator should contain the server-provided
     * 32-bit number in network byte order.
     */

    strncpy(srvinst, truename, (sizeof srvinst)-1);
    srvinst[(sizeof srvinst)-1] = '\0';
    for (p = srvinst; *p; p++) {
      if (isupper(*p)) {
	*p = tolower(*p);
      }
    }

    strncpy(srvrealm, (char *)krb_realmofhost(srvinst), (sizeof srvrealm)-1);
    srvrealm[(sizeof srvrealm)-1] = '\0';
    if (p = strchr(srvinst, '.')) {
      *p = '\0';
    }

    result = krb_mk_req(&authenticator, "imap", srvinst, srvrealm, 0);
    if (result) {
	report(stderr, "krb_mq_req: %s\n", krb_get_err_text(result));
	return PS_AUTHFAIL;
    }

    result = krb_get_cred("imap", srvinst, srvrealm, &credentials);
    if (result) {
	report(stderr, "krb_get_cred: %s\n", krb_get_err_text(result));
	return PS_AUTHFAIL;
    }

    memcpy(session, credentials.session, sizeof session);
    memset(&credentials, 0, sizeof credentials);
    des_key_sched(&session, schedule);

    result = krb_get_tf_fullname(TKT_FILE, tktuser, tktinst, tktrealm);
    if (result) {
	report(stderr, "krb_get_tf_fullname: %s\n", krb_get_err_text(result));
	return PS_AUTHFAIL;
    }

    if (strcmp(tktuser, user) != 0) {
	report(stderr, 
	       _("principal %s in ticket does not match -u %s\n"), tktuser,
		user);
	return PS_AUTHFAIL;
    }

    if (tktinst[0]) {
	report(stderr, 
	       _("non-null instance (%s) might cause strange behavior\n"),
		tktinst);
	strcat(tktuser, ".");
	strcat(tktuser, tktinst);
    }

    if (strcmp(tktrealm, srvrealm) != 0) {
	strcat(tktuser, "@");
	strcat(tktuser, tktrealm);
    }

    result = krb_mk_req(&authenticator, "imap", srvinst, srvrealm,
	    challenge1.cint);
    if (result) {
	report(stderr, "krb_mq_req: %s\n", krb_get_err_text(result));
	return PS_AUTHFAIL;
    }

    to64frombits(buf1, authenticator.dat, authenticator.length);
    if (outlevel >= O_MONITOR) {
	report(stdout, "IMAP> %s\n", buf1);
    }
    strcat(buf1, "\r\n");
    SockWrite(sock, buf1, strlen(buf1));

    /* Upon decrypting and verifying the ticket and authenticator, the
     * server should verify that the contained checksum field equals
     * the original server provided random 32-bit number.  Should the
     * verification be successful, the server must add one to the
     * checksum and construct 8 octets of data, with the first four
     * octets containing the incremented checksum in network byte
     * order, the fifth octet containing a bit-mask specifying the
     * protection mechanisms supported by the server, and the sixth
     * through eighth octets containing, in network byte order, the
     * maximum cipher-text buffer size the server is able to receive.
     * The server must encrypt the 8 octets of data in the session key
     * and issue that encrypted data in a second ready response.  The
     * client should consider the server authenticated if the first
     * four octets the un-encrypted data is equal to one plus the
     * checksum it previously sent.
     */
    
    if (result = gen_recv(sock, buf1, sizeof buf1))
	return result;

    /* The client must construct data with the first four octets
     * containing the original server-issued checksum in network byte
     * order, the fifth octet containing the bit-mask specifying the
     * selected protection mechanism, the sixth through eighth octets
     * containing in network byte order the maximum cipher-text buffer
     * size the client is able to receive, and the following octets
     * containing a user name string.  The client must then append
     * from one to eight octets so that the length of the data is a
     * multiple of eight octets. The client must then PCBC encrypt the
     * data with the session key and respond to the second ready
     * response with the encrypted data.  The server decrypts the data
     * and verifies the contained checksum.  The username field
     * identifies the user for whom subsequent IMAP operations are to
     * be performed; the server must verify that the principal
     * identified in the Kerberos ticket is authorized to connect as
     * that user.  After these verifications, the authentication
     * process is complete.
     */

    len = from64tobits(buf2, buf1);
    if (len < 0) {
	report(stderr, _("could not decode BASE64 ready response\n"));
	return PS_AUTHFAIL;
    }

    des_ecb_encrypt((des_cblock *)buf2, (des_cblock *)buf2, schedule, 0);
    memcpy(challenge2.cstr, buf2, 4);
    if (ntohl(challenge2.cint) != challenge1.cint + 1) {
	report(stderr, _("challenge mismatch\n"));
	return PS_AUTHFAIL;
    }	    

    memset(authenticator.dat, 0, sizeof authenticator.dat);

    result = htonl(challenge1.cint);
    memcpy(authenticator.dat, &result, sizeof result);

    /* The protection mechanisms and their corresponding bit-masks are as
     * follows:
     *
     * 1 No protection mechanism
     * 2 Integrity (krb_mk_safe) protection
     * 4 Privacy (krb_mk_priv) protection
     */
    authenticator.dat[4] = 1;

    len = strlen(tktuser);
    strncpy(authenticator.dat+8, tktuser, len);
    authenticator.length = len + 8 + 1;
    while (authenticator.length & 7) {
	authenticator.length++;
    }
    des_pcbc_encrypt((des_cblock *)authenticator.dat,
	    (des_cblock *)authenticator.dat, authenticator.length, schedule,
	    &session, 1);

    to64frombits(buf1, authenticator.dat, authenticator.length);
    if (outlevel >= O_MONITOR) {
	report(stdout, "IMAP> %s\n", buf1);
    }

    strcat(buf1, "\r\n");
    SockWrite(sock, buf1, strlen(buf1));

    if (result = gen_recv(sock, buf1, sizeof buf1))
	return result;

    if (strstr(buf1, "OK")) {
        return PS_SUCCESS;
    }
    else {
	return PS_AUTHFAIL;
    }
}
#endif /* KERBEROS_V4 */

#ifdef GSSAPI
#define GSSAUTH_P_NONE      1
#define GSSAUTH_P_INTEGRITY 2
#define GSSAUTH_P_PRIVACY   4

static int do_gssauth(int sock, char *hostname, char *username)
{
    gss_buffer_desc request_buf, send_token;
    gss_buffer_t sec_token;
    gss_name_t target_name;
    gss_ctx_id_t context;
    gss_OID mech_name;
    gss_qop_t quality;
    int cflags;
    OM_uint32 maj_stat, min_stat;
    char buf1[8192], buf2[8192], server_conf_flags;
    unsigned long buf_size;
    int result;

    /* first things first: get an imap ticket for host */
    sprintf(buf1, "imap@%s", hostname);
    request_buf.value = buf1;
    request_buf.length = strlen(buf1) + 1;
    maj_stat = gss_import_name(&min_stat, &request_buf, GSS_C_NT_HOSTBASED_SERVICE,
        &target_name);
    if (maj_stat != GSS_S_COMPLETE) {
        report(stderr, _("Couldn't get service name for [%s]\n"), buf1);
        return PS_AUTHFAIL;
    }
    else if (outlevel >= O_DEBUG) {
        maj_stat = gss_display_name(&min_stat, target_name, &request_buf,
            &mech_name);
        report(stderr, _("Using service name [%s]\n"),request_buf.value);
        maj_stat = gss_release_buffer(&min_stat, &request_buf);
    }

    gen_send(sock, "AUTHENTICATE GSSAPI");

    /* upon receipt of the GSSAPI authentication request, server returns
     * null data ready response. */
    if (result = gen_recv(sock, buf1, sizeof buf1)) {
        return result;
    }

    /* now start the security context initialisation loop... */
    sec_token = GSS_C_NO_BUFFER;
    context = GSS_C_NO_CONTEXT;
    if (outlevel >= O_VERBOSE)
        report(stdout, _("Sending credentials\n"));
    do {
        send_token.length = 0;
	send_token.value = NULL;
        maj_stat = gss_init_sec_context(&min_stat, 
				        GSS_C_NO_CREDENTIAL,
            				&context, 
					target_name, 
					GSS_C_NO_OID, 
					GSS_C_MUTUAL_FLAG | GSS_C_SEQUENCE_FLAG, 
					0, 
					GSS_C_NO_CHANNEL_BINDINGS, 
					sec_token, 
					NULL, 
					&send_token, 
					NULL, 
					NULL);
        if (maj_stat!=GSS_S_COMPLETE && maj_stat!=GSS_S_CONTINUE_NEEDED) {
            report(stderr, _("Error exchanging credentials\n"));
            gss_release_name(&min_stat, &target_name);
            /* wake up server and await NO response */
            SockWrite(sock, "\r\n", 2);
            if (result = gen_recv(sock, buf1, sizeof buf1))
                return result;
            return PS_AUTHFAIL;
        }
        to64frombits(buf1, send_token.value, send_token.length);
        gss_release_buffer(&min_stat, &send_token);
	strcat(buf1, "\r\n");
        SockWrite(sock, buf1, strlen(buf1));
        if (outlevel >= O_MONITOR)
            report(stdout, "IMAP> %s\n", buf1);
        if (maj_stat == GSS_S_CONTINUE_NEEDED) {
	    if (result = gen_recv(sock, buf1, sizeof buf1)) {
	        gss_release_name(&min_stat, &target_name);
	        return result;
	    }
	    request_buf.length = from64tobits(buf2, buf1 + 2);
	    request_buf.value = buf2;
	    sec_token = &request_buf;
        }
    } while (maj_stat == GSS_S_CONTINUE_NEEDED);
    gss_release_name(&min_stat, &target_name);

    /* get security flags and buffer size */
    if (result = gen_recv(sock, buf1, sizeof buf1)) {
        return result;
    }
    request_buf.length = from64tobits(buf2, buf1 + 2);
    request_buf.value = buf2;

    maj_stat = gss_unwrap(&min_stat, context, &request_buf, &send_token,
        &cflags, &quality);
    if (maj_stat != GSS_S_COMPLETE) {
        report(stderr, _("Couldn't unwrap security level data\n"));
        gss_release_buffer(&min_stat, &send_token);
        return PS_AUTHFAIL;
    }
    if (outlevel >= O_DEBUG)
        report(stdout, _("Credential exchange complete\n"));
    /* first octet is security levels supported. We want none, for now */
    server_conf_flags = ((char *)send_token.value)[0];
    if ( !(((char *)send_token.value)[0] & GSSAUTH_P_NONE) ) {
        report(stderr, _("Server requires integrity and/or privacy\n"));
        gss_release_buffer(&min_stat, &send_token);
        return PS_AUTHFAIL;
    }
    ((char *)send_token.value)[0] = 0;
    buf_size = ntohl(*((long *)send_token.value));
    /* we don't care about buffer size if we don't wrap data */
    gss_release_buffer(&min_stat, &send_token);
    if (outlevel >= O_DEBUG) {
        report(stdout, _("Unwrapped security level flags: %s%s%s\n"),
            server_conf_flags & GSSAUTH_P_NONE ? "N" : "-",
            server_conf_flags & GSSAUTH_P_INTEGRITY ? "I" : "-",
            server_conf_flags & GSSAUTH_P_PRIVACY ? "C" : "-");
        report(stdout, _("Maximum GSS token size is %ld\n"),buf_size);
    }

    /* now respond in kind (hack!!!) */
    buf_size = htonl(buf_size); /* do as they do... only matters if we do enc */
    memcpy(buf1, &buf_size, 4);
    buf1[0] = GSSAUTH_P_NONE;
    strcpy(buf1+4, username); /* server decides if princ is user */
    request_buf.length = 4 + strlen(username) + 1;
    request_buf.value = buf1;
    maj_stat = gss_wrap(&min_stat, context, 0, GSS_C_QOP_DEFAULT, &request_buf,
        &cflags, &send_token);
    if (maj_stat != GSS_S_COMPLETE) {
        report(stderr, _("Error creating security level request\n"));
        return PS_AUTHFAIL;
    }
    to64frombits(buf1, send_token.value, send_token.length);
    if (outlevel >= O_DEBUG) {
        report(stdout, _("Requesting authorization as %s\n"), username);
        report(stdout, "IMAP> %s\n",buf1);
    }
    strcat(buf1, "\r\n");
    SockWrite(sock, buf1, strlen(buf1));

    /* we should be done. Get status and finish up */
    if (result = gen_recv(sock, buf1, sizeof buf1))
        return result;
    if (strstr(buf1, "OK")) {
        /* flush security context */
        if (outlevel >= O_DEBUG)
            report(stdout, _("Releasing GSS credentials\n"));
        maj_stat = gss_delete_sec_context(&min_stat, &context, &send_token);
        if (maj_stat != GSS_S_COMPLETE) {
            report(stderr, _("Error releasing credentials\n"));
            return PS_AUTHFAIL;
        }
        /* send_token may contain a notification to the server to flush
         * credentials. RFC 1731 doesn't specify what to do, and since this
         * support is only for authentication, we'll assume the server
         * knows enough to flush its own credentials */
        gss_release_buffer(&min_stat, &send_token);
        return PS_SUCCESS;
    }

    return PS_AUTHFAIL;
}	
#endif /* GSSAPI */

static void hmac_md5 (unsigned char *password,  size_t pass_len,
                      unsigned char *challenge, size_t chal_len,
                      unsigned char *response,  size_t resp_len)
{
    int i;
    unsigned char ipad[64];
    unsigned char opad[64];
    unsigned char hash_passwd[16];

    MD5_CTX ctx;
    
    if (resp_len != 16)
        return;

    if (pass_len > sizeof (ipad))
    {
        MD5Init (&ctx);
        MD5Update (&ctx, password, pass_len);
        MD5Final (hash_passwd, &ctx);
        password = hash_passwd; pass_len = sizeof (hash_passwd);
    }

    memset (ipad, 0, sizeof (ipad));
    memset (opad, 0, sizeof (opad));
    memcpy (ipad, password, pass_len);
    memcpy (opad, password, pass_len);

    for (i=0; i<64; i++) {
        ipad[i] ^= 0x36;
        opad[i] ^= 0x5c;
    }

    MD5Init (&ctx);
    MD5Update (&ctx, ipad, sizeof (ipad));
    MD5Update (&ctx, challenge, chal_len);
    MD5Final (response, &ctx);

    MD5Init (&ctx);
    MD5Update (&ctx, opad, sizeof (opad));
    MD5Update (&ctx, response, resp_len);
    MD5Final (response, &ctx);
}

#if NTLM_ENABLE
#include "ntlm.h"

static tSmbNtlmAuthRequest   request;		   
static tSmbNtlmAuthChallenge challenge;
static tSmbNtlmAuthResponse  response;

/*
 * NTLM support by Grant Edwards.
 *
 * Handle MS-Exchange NTLM authentication method.  This is the same
 * as the NTLM auth used by Samba for SMB related services. We just
 * encode the packets in base64 instead of sending them out via a
 * network interface.
 * 
 * Much source (ntlm.h, smb*.c smb*.h) was borrowed from Samba.
 */

static int do_imap_ntlm(int sock, struct query *ctl)
{
    char msgbuf[2048];
    int result,len;
  
    gen_send(sock, "AUTHENTICATE NTLM");

    if ((result = gen_recv(sock, msgbuf, sizeof msgbuf)))
	return result;
  
    if (msgbuf[0] != '+')
	return PS_AUTHFAIL;
  
    buildSmbNtlmAuthRequest(&request,ctl->remotename,NULL);

    if (outlevel >= O_DEBUG)
	dumpSmbNtlmAuthRequest(stdout, &request);

    memset(msgbuf,0,sizeof msgbuf);
    to64frombits (msgbuf, (unsigned char*)&request, SmbLength(&request));
  
    if (outlevel >= O_MONITOR)
	report(stdout, "IMAP> %s\n", msgbuf);
  
    strcat(msgbuf,"\r\n");
    SockWrite (sock, msgbuf, strlen (msgbuf));

    if ((gen_recv(sock, msgbuf, sizeof msgbuf)))
	return result;
  
    len = from64tobits ((unsigned char*)&challenge, msgbuf);
    
    if (outlevel >= O_DEBUG)
	dumpSmbNtlmAuthChallenge(stdout, &challenge);
    
    buildSmbNtlmAuthResponse(&challenge, &response,ctl->remotename,ctl->password);
  
    if (outlevel >= O_DEBUG)
	dumpSmbNtlmAuthResponse(stdout, &response);
  
    memset(msgbuf,0,sizeof msgbuf);
    to64frombits (msgbuf, (unsigned char*)&response, SmbLength(&response));

    if (outlevel >= O_MONITOR)
	report(stdout, "IMAP> %s\n", msgbuf);
      
    strcat(msgbuf,"\r\n");

    SockWrite (sock, msgbuf, strlen (msgbuf));
  
    if ((result = gen_recv (sock, msgbuf, sizeof msgbuf)))
	return result;
  
    if (strstr (msgbuf, "OK"))
	return PS_SUCCESS;
    else
	return PS_AUTHFAIL;
}
#endif /* NTLM */

static int do_cram_md5 (int sock, struct query *ctl)
/* authenticate as per RFC2195 */
{
    int result;
    int len;
    unsigned char buf1[1024];
    unsigned char msg_id[768];
    unsigned char response[16];
    unsigned char reply[1024];

    gen_send (sock, "AUTHENTICATE CRAM-MD5");

    /* From RFC2195:
     * The data encoded in the first ready response contains an
     * presumptively arbitrary string of random digits, a timestamp, and the
     * fully-qualified primary host name of the server.  The syntax of the
     * unencoded form must correspond to that of an RFC 822 'msg-id'
     * [RFC822] as described in [POP3].
     */

    if ((result = gen_recv (sock, buf1, sizeof (buf1)))) {
	return result;
    }

    len = from64tobits (msg_id, buf1);
    if (len < 0) {
	report (stderr, _("could not decode BASE64 challenge\n"));
	return PS_AUTHFAIL;
    } else if (len < sizeof (msg_id)) {
        msg_id[len] = 0;
    } else {
        msg_id[sizeof (msg_id)-1] = 0;
    }
    if (outlevel >= O_DEBUG) {
        report (stdout, "decoded as %s\n", msg_id);
    }

    /* The client makes note of the data and then responds with a string
     * consisting of the user name, a space, and a 'digest'.  The latter is
     * computed by applying the keyed MD5 algorithm from [KEYED-MD5] where
     * the key is a shared secret and the digested text is the timestamp
     * (including angle-brackets).
     */

    hmac_md5 (ctl->password, strlen (ctl->password),
              msg_id, strlen (msg_id),
              response, sizeof (response));

#ifdef HAVE_SNPRINTF
    snprintf (reply, sizeof (reply),
#else
    sprintf(reply,
#endif
              "%s %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", 
              ctl->remotename,
              response[0], response[1], response[2], response[3],
              response[4], response[5], response[6], response[7],
              response[8], response[9], response[10], response[11],
              response[12], response[13], response[14], response[15]);

    if (outlevel >= O_DEBUG) {
        report (stdout, "replying with %s\n", reply);
    }

    to64frombits (buf1, reply, strlen (reply));
    if (outlevel >= O_MONITOR) {
	report (stdout, "IMAP> %s\n", buf1);
    }

    /* PMDF5.2 IMAP has a bug that requires this to be a single write */
    strcat (buf1, "\r\n");
    SockWrite (sock, buf1, strlen (buf1));

    if ((result = gen_recv (sock, buf1, sizeof (buf1))))
	return result;

    if (strstr (buf1, "OK")) {
        return PS_SUCCESS;
    } else {
	return PS_AUTHFAIL;
    }
}

int imap_canonicalize(char *result, char *raw, int maxlen)
/* encode an IMAP password as per RFC1730's quoting conventions */
{
    int i, j;

    j = 0;
    for (i = 0; i < strlen(raw) && i < maxlen; i++)
    {
	if ((raw[i] == '\\') || (raw[i] == '"'))
	    result[j++] = '\\';
	result[j++] = raw[i];
    }
    result[j] = '\0';

    return(i);
}

int imap_getauth(int sock, struct query *ctl, char *greeting)
/* apply for connection authorization */
{
    int ok = 0;

    /* probe to see if we're running IMAP4 and can use RFC822.PEEK */
    capabilities[0] = '\0';
    if ((ok = gen_transact(sock, "CAPABILITY")) == PS_SUCCESS)
    {
	/* UW-IMAP server 10.173 notifies in all caps */
	if (strstr(capabilities, "IMAP4REV1"))
	{
	    imap_version = IMAP4rev1;
	    if (outlevel >= O_DEBUG)
		report(stdout, _("Protocol identified as IMAP4 rev 1\n"));
	}
	else
	{
	    imap_version = IMAP4;
	    if (outlevel >= O_DEBUG)
		report(stdout, _("Protocol identified as IMAP4 rev 0\n"));
	}
    }
    else if (ok == PS_ERROR)
    {
	imap_version = IMAP2;
	if (outlevel >= O_DEBUG)
	    report(stdout, _("Protocol identified as IMAP2 or IMAP2BIS\n"));
    }
    else
	return(ok);

    peek_capable = (imap_version >= IMAP4);

    /* 
     * Assumption: expunges are cheap, so we want to do them
     * after every message unless user said otherwise.
     */
    if (NUM_SPECIFIED(ctl->expunge))
	expunge_period = NUM_VALUE_OUT(ctl->expunge);
    else
	expunge_period = 1;

    /* 
     * If either (a) we saw a PREAUTH token in the greeting, or
     * (b) the user specified ssh preauthentication, then we're done.
     */
    if (preauth || ctl->server.preauthenticate == A_SSH)
    {
        preauth = FALSE;  /* reset for the next session */
        return(PS_SUCCESS);
    }

    /* 
     * Handle idling.  We depend on coming through here on startup
     * and after each timeout (including timeouts during idles).
     */
    if (strstr(capabilities, "IDLE") && ctl->idle)
    {
	do_idle = TRUE;
	if (outlevel >= O_VERBOSE)
	    report(stdout, "will idle after poll\n");
    }

#if OPIE_ENABLE
    if ((ctl->server.protocol == P_IMAP) && strstr(capabilities, "AUTH=X-OTP"))
    {
	if (outlevel >= O_DEBUG)
	    report(stdout, _("OTP authentication is supported\n"));
	if (do_otp(sock, ctl) == PS_SUCCESS)
	    return(PS_SUCCESS);
    };
#endif /* OPIE_ENABLE */

#ifdef GSSAPI
    if (strstr(capabilities, "AUTH=GSSAPI"))
    {
        if (ctl->server.protocol == P_IMAP_GSS)
        {
            if (outlevel >= O_DEBUG)
                report(stdout, _("GSS authentication is supported\n"));
            return do_gssauth(sock, ctl->server.truename, ctl->remotename);
        }
    }
    else if (ctl->server.protocol == P_IMAP_GSS)
    {
        report(stderr, 
	       _("Required GSS capability not supported by server\n"));
        return(PS_AUTHFAIL);
    }
#endif /* GSSAPI */

#ifdef KERBEROS_V4
    if (strstr(capabilities, "AUTH=KERBEROS_V4"))
    {
	if (outlevel >= O_DEBUG)
	    report(stdout, _("KERBEROS_V4 authentication is supported\n"));

	if (ctl->server.protocol == P_IMAP_K4)
	{
	    if ((ok = do_rfc1731(sock, ctl->server.truename)))
	    {
		if (outlevel >= O_MONITOR)
		    report(stdout, "IMAP> *\n");
		SockWrite(sock, "*\r\n", 3);
	    }
	    
	    return(ok);
	}
	/* else fall through to ordinary AUTH=LOGIN case */
    }
    else if (ctl->server.protocol == P_IMAP_K4)
    {
	report(stderr, 
	       _("Required KERBEROS_V4 capability not supported by server\n"));
	return(PS_AUTHFAIL);
    }
#endif /* KERBEROS_V4 */

    if (strstr(capabilities, "AUTH=CRAM-MD5"))
    {
        if (outlevel >= O_DEBUG)
            report (stdout, _("CRAM-MD5 authentication is supported\n"));
        if (ctl->server.protocol != P_IMAP_LOGIN)
        {
            if ((ok = do_cram_md5 (sock, ctl)))
            {
                if (outlevel >= O_MONITOR)
                    report (stdout, "IMAP> *\n");
                SockWrite (sock, "*\r\n", 3);
            }
            return ok;
        }
    }
    else if (ctl->server.protocol == P_IMAP_CRAM_MD5)
    {
        report(stderr,
               _("Required CRAM-MD5 capability not supported by server\n"));
        return(PS_AUTHFAIL);
    }

#ifdef NTLM_ENABLE
    if (strstr (capabilities, "AUTH=NTLM"))
    {
        if (outlevel >= O_DEBUG)
            report (stdout, _("NTLM authentication is supported\n"));
        return do_imap_ntlm (sock, ctl);
    }
#endif /* NTLM_ENABLE */

#ifdef __UNUSED__	/* The Cyrus IMAP4rev1 server chokes on this */
    /* this handles either AUTH=LOGIN or AUTH-LOGIN */
    if ((imap_version >= IMAP4rev1) && (!strstr(capabilities, "LOGIN"))) {
      report(stderr, 
	     _("Required LOGIN capability not supported by server\n"));
      return PS_AUTHFAIL;
    };
#endif /* __UNUSED__ */

    {
	/* these sizes guarantee no buffer overflow */
	char	remotename[NAMELEN*2+1], password[PASSWORDLEN*2+1];

	imap_canonicalize(remotename, ctl->remotename, NAMELEN);
	imap_canonicalize(password, ctl->password, PASSWORDLEN);
	ok = gen_transact(sock, "LOGIN \"%s\" \"%s\"", remotename, password);
    }

    if (ok)
	return(ok);
    
    return(PS_SUCCESS);
}

static int internal_expunge(int sock)
/* ship an expunge, resetting associated counters */
{
    int	ok;

    if ((ok = gen_transact(sock, "EXPUNGE")))
	return(ok);

    expunged += deletions;
    unseen -= deletions;
    deletions = 0;

#ifdef IMAP_UID	/* not used */
    expunge_uids(ctl);
#endif /* IMAP_UID */

    return(PS_SUCCESS);
}

static int imap_getrange(int sock, 
			 struct query *ctl, 
			 const char *folder, 
			 int *countp, int *newp, int *bytes)
/* get range of messages to be fetched */
{
    int ok;
    char buf[MSGBUFSIZE+1], *cp;

    /* find out how many messages are waiting */
    *bytes = -1;

    if (pass > 1)
    {
	/* 
	 * We have to have an expunge here, otherwise the re-poll will
	 * infinite-loop picking up un-expunged messages -- unless the
	 * expunge period is one and we've been nuking each message 
	 * just after deletion.
	 */
	ok = 0;
	if (deletions && expunge_period != 1)
	    ok = internal_expunge(sock);
	count = -1;
	if (do_idle)
	{
	    stage = STAGE_IDLE;
	    saved_timeout = mytimeout;
	    mytimeout = 0;
	}
	if (ok || gen_transact(sock, do_idle ? "IDLE" : "NOOP"))
	{
	    report(stderr, _("re-poll failed\n"));
	    return(ok);
	}
	else if (count == -1)	/* no EXISTS response to NOOP/IDLE */
	{
	    count = 0;
	}
    }
    else
    {
	ok = gen_transact(sock, 
			  check_only ? "EXAMINE \"%s\"" : "SELECT \"%s\"",
			  folder ? folder : "INBOX");
	if (ok != 0)
	{
	    report(stderr, _("mailbox selection failed\n"));
	    return(ok);
	}
    }

    *countp = count;

    /* OK, now get a count of unseen messages and their indices */

    if (unseen_messages)
	free(unseen_messages);
    unseen_messages = xmalloc(count * sizeof(unsigned int));
    memset(unseen_messages, 0, count * sizeof(unsigned int));
    unseen = 0;

    gen_send(sock, "SEARCH UNSEEN");
    do {
	ok = gen_recv(sock, buf, sizeof(buf));
	if (ok != 0)
	{
	    report(stderr, _("search for unseen messages failed\n"));
	    return(PS_PROTOCOL);
	}
	else if ((cp = strstr(buf, "* SEARCH")))
	{
	    char	*ep;

	    cp += 8;	/* skip "* SEARCH" */

	    while (*cp && unseen < count)
	    {
		/* skip whitespace */
		while (*cp && isspace(*cp))
		    cp++;
		if (*cp) 
		{
		    /*
		     * Message numbers are between 1 and 2^32 inclusive,
		     * so unsigned int is large enough.
		     */
		    unseen_messages[unseen]=(unsigned int)strtol(cp,&ep,10);

		    if (outlevel >= O_DEBUG)
			report(stdout, 
			       _("%u is unseen\n"), 
			       unseen_messages[unseen]);
		
		    unseen++;
		    cp = ep;
		}
	    }
	}
    } while
	(tag[0] != '\0' && strncmp(buf, tag, strlen(tag)));

    *newp = unseen;
    expunged = 0;

    return(PS_SUCCESS);
}

static int imap_getsizes(int sock, int count, int *sizes)
/* capture the sizes of all messages */
{
    char buf [MSGBUFSIZE+1];

    /*
     * Some servers (as in, PMDF5.1-9.1 under OpenVMS 6.1)
     * won't accept 1:1 as valid set syntax.  Some implementors
     * should be taken out and shot for excessive anality.
     *
     * Microsoft Exchange (brain-dead piece of crap that it is) 
     * sometimes gets its knickers in a knot about bodiless messages.
     * You may see responses like this:
     *
     *	fetchmail: IMAP> A0004 FETCH 1:9 RFC822.SIZE
     *	fetchmail: IMAP< * 2 FETCH (RFC822.SIZE 1187)
     *	fetchmail: IMAP< * 3 FETCH (RFC822.SIZE 3954)
     *	fetchmail: IMAP< * 4 FETCH (RFC822.SIZE 1944)
     *	fetchmail: IMAP< * 5 FETCH (RFC822.SIZE 2933)
     *	fetchmail: IMAP< * 6 FETCH (RFC822.SIZE 1854)
     *	fetchmail: IMAP< * 7 FETCH (RFC822.SIZE 34054)
     *	fetchmail: IMAP< * 8 FETCH (RFC822.SIZE 5561)
     *	fetchmail: IMAP< * 9 FETCH (RFC822.SIZE 1101)
     *	fetchmail: IMAP< A0004 NO The requested item could not be found.
     *
     * This means message 1 has only headers.  For kicks and grins
     * you can telnet in and look:
     *	A003 FETCH 1 FULL
     *	A003 NO The requested item could not be found.
     *	A004 fetch 1 rfc822.header
     *	A004 NO The requested item could not be found.
     *	A006 FETCH 1 BODY
     *	* 1 FETCH (BODY ("TEXT" "PLAIN" ("CHARSET" "US-ASCII") NIL NIL "7BIT" 35 3))
     *	A006 OK FETCH completed.
     *
     * To get around this, we terminate the read loop on a NO and count
     * on the fact that the sizes array has been preinitialized with a
     * known-bad size value.
     */
    if (count == 1)
	gen_send(sock, "FETCH 1 RFC822.SIZE", count);
    else
	gen_send(sock, "FETCH 1:%d RFC822.SIZE", count);
    for (;;)
    {
	int num, size, ok;

	if ((ok = gen_recv(sock, buf, sizeof(buf))))
	    return(ok);
	else if (strstr(buf, "OK") || strstr(buf, "NO"))
	    break;
	else if (sscanf(buf, "* %d FETCH (RFC822.SIZE %d)", &num, &size) == 2)
	    sizes[num - 1] = size;
    }

    return(PS_SUCCESS);
}

static int imap_is_old(int sock, struct query *ctl, int number)
/* is the given message old? */
{
    flag seen = TRUE;
    int i;

    /* 
     * Expunges change the fetch numbers, but unseen_messages contains
     * indices from before any expungees were done.  So neither the
     * argument nor the values in message_sequence need to be decremented.
     */

    seen = TRUE;
    for (i = 0; i < unseen; i++)
	if (unseen_messages[i] == number)
	{
	    seen = FALSE;
	    break;
	}

    return(seen);
}

static int imap_fetch_headers(int sock, struct query *ctl,int number,int *lenp)
/* request headers of nth message */
{
    char buf [MSGBUFSIZE+1];
    int	num;

    /* expunges change the fetch numbers */
    number -= expunged;

    /*
     * This is blessed by RFC1176, RFC1730, RFC2060.
     * According to the RFCs, it should *not* set the \Seen flag.
     */
    gen_send(sock, "FETCH %d RFC822.HEADER", number);

    /* looking for FETCH response */
    do {
	int	ok;

	if ((ok = gen_recv(sock, buf, sizeof(buf))))
	    return(ok);
    } while
	(sscanf(buf+2, "%d FETCH (%*s {%d}", &num, lenp) != 2);

    if (num != number)
	return(PS_ERROR);
    else
	return(PS_SUCCESS);
}

static int imap_fetch_body(int sock, struct query *ctl, int number, int *lenp)
/* request body of nth message */
{
    char buf [MSGBUFSIZE+1], *cp;
    int	num;

    /* expunges change the fetch numbers */
    number -= expunged;

    /*
     * If we're using IMAP4, we can fetch the message without setting its
     * seen flag.  This is good!  It means that if the protocol exchange
     * craps out during the message, it will still be marked `unseen' on
     * the server.
     *
     * However...*don't* do this if we're using keep to suppress deletion!
     * In that case, marking the seen flag is the only way to prevent the
     * message from being re-fetched on subsequent runs (and according
     * to RFC2060 p.43 this fetch should set Seen as a side effect).
     *
     * According to RFC2060, and Mark Crispin the IMAP maintainer,
     * FETCH %d BODY[TEXT] and RFC822.TEXT are "functionally 
     * equivalent".  However, we know of at least one server that
     * treats them differently in the presence of MIME attachments;
     * the latter form downloads the attachment, the former does not.
     * The server is InterChange, and the fool who implemented this
     * misfeature ought to be strung up by his thumbs.  
     *
     * When I tried working around this by disable use of the 4rev1 form,
     * I found that doing this breaks operation with M$ Exchange.
     * Annoyingly enough, Exchange's refusal to cope is technically legal
     * under RFC2062.  Trust Microsoft, the Great Enemy of interoperability
     * standards, to find a way to make standards compliance irritating....
     */
    switch (imap_version)
    {
    case IMAP4rev1:	/* RFC 2060 */
	if (!ctl->keep)
	    gen_send(sock, "FETCH %d BODY.PEEK[TEXT]", number);
	else
	    gen_send(sock, "FETCH %d BODY[TEXT]", number);
	break;

    case IMAP4:		/* RFC 1730 */
	if (!ctl->keep)
	    gen_send(sock, "FETCH %d RFC822.TEXT.PEEK", number);
	else
	    gen_send(sock, "FETCH %d RFC822.TEXT", number);
	break;

    default:		/* RFC 1176 */
	gen_send(sock, "FETCH %d RFC822.TEXT", number);
	break;
    }

    /* looking for FETCH response */
    do {
	int	ok;

	if ((ok = gen_recv(sock, buf, sizeof(buf))))
	    return(ok);
    } while
	(!strstr(buf+4, "FETCH") || sscanf(buf+2, "%d", &num) != 1);

    if (num != number)
	return(PS_ERROR);

    /*
     * Try to extract a length from the FETCH response.  RFC2060 requires
     * it to be present, but at least one IMAP server (Novell GroupWise)
     * botches this.
     */
    if ((cp = strchr(buf, '{')))
	*lenp = atoi(cp + 1);
    else
	*lenp = -1;	/* missing length part in FETCH reponse */

    return(PS_SUCCESS);
}

static int imap_trail(int sock, struct query *ctl, int number)
/* discard tail of FETCH response after reading message text */
{
    /* expunges change the fetch numbers */
    /* number -= expunged; */

    for (;;)
    {
	char buf[MSGBUFSIZE+1];
	int ok;

	if ((ok = gen_recv(sock, buf, sizeof(buf))))
	    return(ok);

	/* UW IMAP returns "OK FETCH", Cyrus returns "OK Completed" */
	if (strstr(buf, "OK"))
	    break;

#ifdef __UNUSED__
	/*
	 * Any IMAP server that fails to set Seen on a BODY[TEXT]
	 * fetch violates RFC2060 p.43 (top).  This becomes an issue
	 * when keep is on, because seen messages aren't deleted and
	 * get refetched on each poll.  As a workaround, if keep is on
	 * we can set the Seen flag explicitly.
	 *
	 * This code isn't used yet because we don't know of any IMAP
	 * servers broken in this way.
	 */
	if (ctl->keep)
	    if ((ok = gen_transact(sock,
			imap_version == IMAP4 
				? "STORE %d +FLAGS.SILENT (\\Seen)"
				: "STORE %d +FLAGS (\\Seen)", 
			number)))
		return(ok);
#endif /* __UNUSED__ */
    }

    return(PS_SUCCESS);
}

static int imap_delete(int sock, struct query *ctl, int number)
/* set delete flag for given message */
{
    int	ok;

    /* expunges change the fetch numbers */
    number -= expunged;

    /*
     * Use SILENT if possible as a minor throughput optimization.
     * Note: this has been dropped from IMAP4rev1.
     *
     * We set Seen because there are some IMAP servers (notably HP
     * OpenMail) that do message-receipt DSNs, but only when the seen
     * bit is set.  This is the appropriate time -- we get here right
     * after the local SMTP response that says delivery was
     * successful.
     */
    if ((ok = gen_transact(sock,
			imap_version == IMAP4 
				? "STORE %d +FLAGS.SILENT (\\Seen \\Deleted)"
				: "STORE %d +FLAGS (\\Seen \\Deleted)", 
			number)))
	return(ok);
    else
	deletions++;

    /*
     * We do an expunge after expunge_period messages, rather than
     * just before quit, so that a line hit during a long session
     * won't result in lots of messages being fetched again during
     * the next session.
     */
    if (NUM_NONZERO(expunge_period) && (deletions % expunge_period) == 0)
	internal_expunge(sock);

    return(PS_SUCCESS);
}

static int imap_logout(int sock, struct query *ctl)
/* send logout command */
{
    /* if any un-expunged deletions remain, ship an expunge now */
    if (deletions)
	internal_expunge(sock);

#ifdef USE_SEARCH
    /* Memory clean-up */
    if (unseen_messages)
	free(unseen_messages);
#endif /* USE_SEARCH */

    return(gen_transact(sock, "LOGOUT"));
}

const static struct method imap =
{
    "IMAP",		/* Internet Message Access Protocol */
#if INET6_ENABLE
    "imap",
    "imaps",
#else /* INET6_ENABLE */
    143,                /* standard IMAP2bis/IMAP4 port */
    993,                /* ssl IMAP2bis/IMAP4 port */
#endif /* INET6_ENABLE */
    TRUE,		/* this is a tagged protocol */
    FALSE,		/* no message delimiter */
    imap_ok,		/* parse command response */
    imap_canonicalize,	/* deal with embedded slashes and spaces */
    imap_getauth,	/* get authorization */
    imap_getrange,	/* query range of messages */
    imap_getsizes,	/* get sizes of messages (used for ESMTP SIZE option) */
    imap_is_old,	/* no UID check */
    imap_fetch_headers,	/* request given message headers */
    imap_fetch_body,	/* request given message body */
    imap_trail,		/* eat message trailer */
    imap_delete,	/* delete the message */
    imap_logout,	/* expunge and exit */
    TRUE,		/* yes, we can re-poll */
};

int doIMAP(struct query *ctl)
/* retrieve messages using IMAP Version 2bis or Version 4 */
{
    return(do_protocol(ctl, &imap));
}

/* imap.c ends here */
