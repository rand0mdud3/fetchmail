/*
 * For license terms, see the file COPYING in this directory.
 */

/***********************************************************************
  module:       options.c
  project:      fetchmail
  programmer:   Eric S. Raymond, <esr@thyrsus.com>
  description:	command-line option processing

 ***********************************************************************/

#include <config.h>
#include <stdio.h>
#include <pwd.h>

#include "getopt.h"
#include "fetchmail.h"

#define LA_HELP		1
#define LA_VERSION	2 
#define LA_CHECK	3
#define LA_SILENT	4 
#define LA_VERBOSE	5 
#define LA_DAEMON	6
#define LA_QUIT		7
#define LA_LOGFILE	8
#define LA_RCFILE	9
#define LA_IDFILE	10
#define LA_PROTOCOL	11
#define LA_PORT		12
#define LA_AUTHENTICATE	13
#define LA_USERNAME	14
#define LA_ALL          15
#define LA_KILL		16
#define	LA_KEEP		17
#define LA_FLUSH        18
#define LA_NOREWRITE	19
#define LA_REMOTEFILE	20
#define LA_SMTPHOST	21
#define LA_MDA		22
#define LA_YYDEBUG	23

static char *shortoptions = "?Vcsvd:qL:f:i:p:P:A:u:akKFnr:S:m:y";
static struct option longoptions[] = {
  {"help",	no_argument,	   (int *) 0, LA_HELP        },
  {"version",   no_argument,       (int *) 0, LA_VERSION     },
  {"check",	no_argument,	   (int *) 0, LA_CHECK       },
  {"silent",    no_argument,       (int *) 0, LA_SILENT      },
  {"verbose",   no_argument,       (int *) 0, LA_VERBOSE     },
  {"daemon",	required_argument, (int *) 0, LA_DAEMON      },
  {"quit",	no_argument,	   (int *) 0, LA_QUIT        },
  {"logfile",	required_argument, (int *) 0, LA_LOGFILE     },
  {"fetchmailrc",required_argument,(int *) 0, LA_RCFILE      },
  {"idfile",	required_argument, (int *) 0, LA_IDFILE      },

  {"protocol",	required_argument, (int *) 0, LA_PROTOCOL    },
  {"proto",	required_argument, (int *) 0, LA_PROTOCOL    },
  {"port",	required_argument, (int *) 0, LA_PORT        },
  {"auth",	required_argument, (int *) 0, LA_AUTHENTICATE},

  {"user",	required_argument, (int *) 0, LA_USERNAME    },
  {"username",  required_argument, (int *) 0, LA_USERNAME    },

  {"all",	no_argument,       (int *) 0, LA_ALL         },
  {"kill",	no_argument,	   (int *) 0, LA_KILL        },
  {"keep",      no_argument,       (int *) 0, LA_KEEP        },
  {"flush",	no_argument,	   (int *) 0, LA_FLUSH       },
  {"norewrite",	no_argument,	   (int *) 0, LA_NOREWRITE   },

  {"remote",    required_argument, (int *) 0, LA_REMOTEFILE  },
  {"smtphost",	required_argument, (int *) 0, LA_SMTPHOST    },
  {"mda",	required_argument, (int *) 0, LA_MDA         },

  {"yydebug",	no_argument,	   (int *) 0, LA_YYDEBUG     },

  {(char *) 0,  no_argument,       (int *) 0, 0              }
};


/*********************************************************************
  function:      parsecmdline
  description:   parse/validate the command line options.
  arguments:
    argc         argument count.
    argv         argument strings.
    queryctl     pointer to a struct hostrec to receive the parsed 
                 options.

  return value:  if positive, argv index of last parsed option + 1
		 (presumes one or more server names follows).
		 if zero, the command line switches are such that
		 no server names are required (e.g. --version).
		 if negative, the command line is has one or more
	   	 syntax errors.
  calls:         none.  
  globals:       writes outlevel, versioninfo, yydebug, logfile, 
		 poll_interval, quitmode, rcfile
 *********************************************************************/

int parsecmdline (argc,argv,queryctl)
int argc;
char **argv;
struct hostrec *queryctl;
{
    int c;
    int ocount = 0;     /* count of destinations specified */
    int errflag = 0;   /* TRUE when a syntax error is detected */
    int option_index;

    memset(queryctl, '\0', sizeof(struct hostrec));    /* start clean */

    while (!errflag && 
	   (c = getopt_long(argc,argv,shortoptions,
			    longoptions,&option_index)) != -1) {

	switch (c) {
	case 'V':
	case LA_VERSION:
	    versioninfo = !0;
	    break;
	case 'c':
	case LA_CHECK:
	    check_only = 1;
	    break;
	case 's':
	case LA_SILENT:
	    outlevel = O_SILENT;
	    break;
	case 'v':
	case LA_VERBOSE:
	    outlevel = O_VERBOSE;
	    break;
	case 'd':
	case LA_DAEMON:
	    poll_interval = atoi(optarg);
	    break;
	case 'q':
	case LA_QUIT:
	    quitmode = 1;
	    break;
	case 'L':
	case LA_LOGFILE:
	    logfile = optarg;
	    break;
	case 'f':
	case LA_RCFILE:
	    rcfile = (char *) xmalloc(strlen(optarg)+1);
	    strcpy(rcfile,optarg);
	    break;
	case 'i':
	case LA_IDFILE:
	    idfile = (char *) xmalloc(strlen(optarg)+1);
	    strcpy(idfile,optarg);
	    break;
	case 'p':
	case LA_PROTOCOL:
	    /* XXX -- should probably use a table lookup here */
	    if (strcasecmp(optarg,"pop2") == 0)
		queryctl->protocol = P_POP2;
	    else if (strcasecmp(optarg,"pop3") == 0)
		queryctl->protocol = P_POP3;
	    else if (strcasecmp(optarg,"imap") == 0)
		queryctl->protocol = P_IMAP;
	    else if (strcasecmp(optarg,"apop") == 0)
		queryctl->protocol = P_APOP;
	    else if (strcasecmp(optarg,"kpop") == 0)
	    {
		queryctl->protocol = P_POP3;
		queryctl->port = KPOP_PORT;
		queryctl->authenticate =  A_KERBEROS;
	    }
	    else {
		fprintf(stderr,"Invalid protocol `%s' specified.\n", optarg);
		errflag++;
	    }
	    break;
	case 'P':
	case LA_PORT:
	    queryctl->port = atoi(optarg);
	    break;
	case 'A':
	case LA_AUTHENTICATE:
	    if (strcmp(optarg, "password") == 0)
		queryctl->authenticate = A_PASSWORD;
	    else if (strcmp(optarg, "kerberos") == 0)
		queryctl->authenticate = A_KERBEROS;
	    else {
		fprintf(stderr,"Invalid authentication `%s' specified.\n", optarg);
		errflag++;
	    }
	    break;
	case 'u':
	case LA_USERNAME:
	    strncpy(queryctl->remotename,optarg,sizeof(queryctl->remotename)-1);
	    break;

	case 'a':
	case LA_ALL:
	    queryctl->fetchall = !0;
	    break;
	case 'K':
	case LA_KILL:
	    queryctl->keep = 0;
	    break;
	case 'k':
	case LA_KEEP:
	    queryctl->keep = !0;
	    break;
	case 'F':
	case LA_FLUSH:
	    queryctl->flush = !0;
	    break;
	case 'n':
	case LA_NOREWRITE:
	    queryctl->norewrite = 1;
	    break;
	case 'r':
	case LA_REMOTEFILE:
	    strncpy(queryctl->mailbox,optarg,sizeof(queryctl->mailbox)-1);
	    break;
	case 'S':
	case LA_SMTPHOST:
	    strncpy(queryctl->smtphost,optarg,sizeof(queryctl->smtphost)-1);
	    ocount++;
	    break;
	case 'm':
	case LA_MDA:
	    strncpy(queryctl->mda,optarg,sizeof(queryctl->mda));
	    ocount++;
	    break;
	case 'y':
	case LA_YYDEBUG:
	    yydebug = 1;
	    break;

	case '?':
	case LA_HELP:
	default:
	    errflag++;
	}
    }

    if (errflag || ocount > 1) {
	/* squawk if syntax errors were detected */
	fputs("usage:  fetchmail [options] [server ...]\n", stderr);
	fputs("  Options are as follows:\n",stderr);
	fputs("  -?, --help        display this option help\n", stderr);
	fputs("  -V, --version     display version info\n", stderr);

	fputs("  -c, --check       check for messages without fetching\n", stderr);
	fputs("  -s, --silent      work silently\n", stderr);
	fputs("  -v, --verbose     work noisily (diagnostic output)\n", stderr);
	fputs("  -d, --daemon      run as a daemon once per n seconds\n", stderr);
	fputs("  -q, --quit        kill daemon process\n", stderr);
	fputs("  -L, --logfile     specify logfile name\n", stderr);
	fputs("  -f, --fetchmailrc specify alternate run control file\n", stderr);
	fputs("  -i, --idfile      specify alternate UIDs file\n", stderr);

	fputs("  -p, --protocol    specify pop2, pop3, imap, apop, rpop, kpop\n", stderr);
	fputs("  -P, --port        TCP/IP service port to connect to\n",stderr);
	fputs("  -A, --auth        authentication type (password or kerberos)\n",stderr);

	fputs("  -u, --username    specify users's login on server\n", stderr);
	fputs("  -a, --all         retrieve old and new messages\n", stderr);
	fputs("  -K, --kill        delete new messages after retrieval\n", stderr);
	fputs("  -k, --keep        save new messages after retrieval\n", stderr);
	fputs("  -F, --flush       delete old messages from server\n", stderr);
	fputs("  -n, --norewrite    don't rewrite header addresses\n", stderr);

	fputs("  -S, --smtphost    set SMTP forwarding host\n", stderr);
	fputs("  -r, --remote      specify remote folder name\n", stderr);
	return(-1);
    }

    return(optind);
}

