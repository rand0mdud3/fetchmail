/**
 * \file uid.c -- UIDL handling for POP3 servers without LAST
 *
 * For license terms, see the file COPYING in this directory.
 */

#include "config.h"
#include "fetchmail.h"

#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "i18n.h"
#include "sdump.h"

/*
 * Machinery for handling UID lists live here.  This is currently used
 * by POP3, but may also be useful for making the IMAP4 querying logic
 * UID-oriented.
 *
 * These functions are also used by the rest of the code to maintain
 * string lists.
 *
 * Here's the theory:
 *
 * At start of a query, we have a (possibly empty) list of UIDs to be
 * considered seen in `oldsaved'.  These are messages that were left in
 * the mailbox and *not deleted* on previous queries (we don't need to
 * remember the UIDs of deleted messages because ... well, they're gone!)
 * This list is initially set up by initialize_saved_list() from the
 * .fetchids file.
 *
 * Early in the query, during the execution of the protocol-specific
 * getrange code, the driver expects that the host's `newsaved' member
 * will be filled with a list of UIDs and message numbers representing
 * the mailbox state.  If this list is empty, the server did
 * not respond to the request for a UID listing.
 *
 * Each time a message is fetched, we can check its UID against the
 * `oldsaved' list to see if it is old.
 *
 * Each time a message-id is seen, we mark it with MARK_SEEN.
 *
 * Each time a message is deleted, we mark its id UID_DELETED in the
 * `newsaved' member.  When we want to assert that an expunge has been
 * done on the server, we call expunge_uid() to register that all
 * deleted messages are gone by marking them UID_EXPUNGED.
 *
 * At the end of the query, the `newsaved' member becomes the
 * `oldsaved' list.  The old `oldsaved' list is freed.
 *
 * At the end of the fetchmail run, seen and non-EXPUNGED members of all
 * current `oldsaved' lists are flushed out to the .fetchids file to
 * be picked up by the next run.  If there are no un-expunged
 * messages, the file is deleted.
 *
 * One disadvantage of UIDL is that all the UIDs have to be downloaded
 * before a search for new messages can be done. Typically, new messages
 * are appended to mailboxes. Hence, downloading all UIDs just to download
 * a few new mails is a waste of bandwidth. If new messages are always at
 * the end of the mailbox, fast UIDL will decrease the time required to
 * download new mails.
 *
 * During fast UIDL, the UIDs of all messages are not downloaded! The first
 * unseen message is searched for by using a binary search on UIDs. UIDs
 * after the first unseen message are downloaded as and when needed.
 *
 * The advantages of fast UIDL are (this is noticeable only when the
 * mailbox has too many mails):
 *
 * - There is no need to download the UIDs of all mails right at the start.
 * - There is no need to save all the UIDs in memory separately in
 * `newsaved' list.
 * - There is no need to download the UIDs of seen mail (except for the
 * first binary search).
 * - The first new mail is downloaded considerably faster.
 *
 * The disadvantages are:
 *
 * - Since all UIDs are not downloaded, it is not possible to swap old and
 * new list. The current state of the mailbox is essentially a merged state
 * of old and new mails.
 * - If an intermediate mail has been temporarily refused (say, due to 4xx
 * code from the smtp server), this mail may not get downloaded.
 * - If 'flush' is used, such intermediate mails will also get deleted.
 *
 * The first two disadvantages can be overcome by doing a linear search
 * once in a while (say, every 10th poll). Also, with flush, fast UIDL
 * should be disabled.
 *
 * Note: some comparisons (those used for DNS address lists) are caseblind!
 */

int dofastuidl = 0;

#ifdef POP3_ENABLE
/** UIDs associated with un-queried hosts */
static struct idlist *scratchlist;

/** Read saved IDs from \a idfile and attach to each host in \a hostlist. */
static int dump_saved_uid(struct uid_db_record *rec, void *unused)
{
    char *t;

    (void)unused;

    t = sdump(rec->id, rec->id_len);
    report_build(stdout, " %s\n", t);
    free(t);

    return 0;
}

/** Read saved IDs from \a idfile and attach to each host in \a hostlist.
 * Returns 0 for success, or a non-zero error code. */
int initialize_saved_lists(struct query *hostlist, const char *idfile)
{
    struct stat statbuf;
    FILE	*tmpfp;
    struct query *ctl;
    int  err;

    /* make sure lists are initially empty */
    for (ctl = hostlist; ctl; ctl = ctl->next) {
	ctl->skipped = (struct idlist *)NULL;

	init_uid_db(&ctl->oldsaved);
	init_uid_db(&ctl->newsaved);
    }

    errno = 0;

    /*
     * Croak if the uidl directory does not exist.
     * This probably means an NFS mount failed and we can't
     * see a uidl file that ought to be there.
     * Question: is this a portable check? It's not clear
     * that all implementations of lstat() will return ENOTDIR
     * rather than plain ENOENT in this case...
     */
    if (lstat(idfile, &statbuf) < 0) {
	if (errno == ENOTDIR)
	{
	    report(stderr, "lstat: %s: %s\n", idfile, strerror(errno));
	    return PS_IOERR;
	}
    }

    /* let's get stored message UIDs from previous queries */
    if ((tmpfp = fopen(idfile, "r")) != (FILE *)NULL)
    {
	char buf[POPBUFSIZE+1];
	char *host = NULL;	/* pacify -Wall */
	char *user;
	char *id;
	char *atsign;	/* temp pointer used in parsing user and host */
	char *delimp1;
	char saveddelim1;
	char *delimp2;
	char saveddelim2 = '\0';	/* pacify -Wall */

	while (fgets(buf, POPBUFSIZE, tmpfp) != (char *)NULL)
	{
	    /*
	     * At this point, we assume the bug has two fields -- a user@host
	     * part, and an ID part. Either field may contain spurious @ signs.
	     * The previous version of this code presumed one could split at
	     * the rightmost '@'.  This is not correct, as InterMail puts an
	     * '@' in the UIDL.
	     */

	    /* first, skip leading spaces */
	    user = buf + strspn(buf, " \t");

	    /*
	     * First, we split the buf into a userhost part and an id
	     * part ... but id doesn't necessarily start with a '<',
	     * espescially if the POP server returns an X-UIDL header
	     * instead of a Message-ID, as GMX's (www.gmx.net) POP3
	     * StreamProxy V1.0 does.
	     *
	     * this is one other trick. The userhost part
	     * may contain ' ' in the user part, at least in
	     * the lotus notes case.
	     * So we start looking for the '@' after which the
	     * host will follow with the ' ' separator with the id.
	     *
	     * XXX FIXME: There is a case this code cannot handle:
	     * the user name cannot have blanks after a '@'.
	     */
	    if ((delimp1 = strchr(user, '@')) != NULL &&
		(id = strchr(delimp1,' ')) != NULL)
	    {
	        for (delimp1 = id; delimp1 >= user; delimp1--)
		    if ((*delimp1 != ' ') && (*delimp1 != '\t'))
			break;

		/*
		 * It should be safe to assume that id starts after
		 * the " " - after all, we're writing the " "
		 * ourselves in write_saved_lists() :-)
		 */
		id = id + strspn(id, " ");

		delimp1++; /* but what if there is only white space ?!? */
		/* we have at least one @, else we are not in this branch */
		saveddelim1 = *delimp1;		/* save char after token */
		*delimp1 = '\0';		/* delimit token with \0 */

		/* now remove trailing white space chars from id */
		if ((delimp2 = strpbrk(id, " \t\n")) != NULL ) {
		    saveddelim2 = *delimp2;
		    *delimp2 = '\0';
		}

		atsign = strrchr(user, '@');
		/* we have at least one @, else we are not in this branch */
		*atsign = '\0';
		host = atsign + 1;

		/* find uidl db and save it */
		for (ctl = hostlist; ctl; ctl = ctl->next) {
		    if (strcasecmp(host, ctl->server.queryname) == 0
			    && strcasecmp(user, ctl->remotename) == 0) {
			uid_db_insert(&ctl->oldsaved, id, UID_SEEN);
			break;
		    }
		}
		/*
		 * If it's not in a host we're querying,
		 * save it anyway.  Otherwise we'd lose UIDL
		 * information any time we queried an explicit
		 * subset of hosts.
		 */
		if (ctl == (struct query *)NULL) {
		    /* restore string */
		    *delimp1 = saveddelim1;
		    *atsign = '@';
		    if (delimp2 != NULL) {
			*delimp2 = saveddelim2;
		    }
		    save_str(&scratchlist, buf, UID_SEEN);
		}
	    }
	}
	err  = ferror(tmpfp);
	err |= fclose(tmpfp);	/* not checking should be safe, mode was "r" */
				/* bit-wise or, we only care about non-zero */
    } else {
	err = (errno != ENOENT);
    }
    if (err) {
	report(stderr, GT_("Open or read error while reading idfile %s: %s\n"),
		idfile, strerror(errno));
	return PS_IOERR;
    }

    if (outlevel >= O_DEBUG)
    {
	struct idlist	*idp;

	for (ctl = hostlist; ctl; ctl = ctl->next)
	    {
		report_build(stdout, GT_("Old UID list from %s:\n"),
			     ctl->server.pollname);

		if (!uid_db_n_records(&ctl->oldsaved))
		    report_build(stdout, "%s\n", GT_(" <empty>"));
		else
		    traverse_uid_db(&ctl->oldsaved, dump_saved_uid, NULL);

		report_complete(stdout, "\n");
	    }

	report_build(stdout, GT_("Scratch list of UIDs:\n"));
	if (!scratchlist)
		report_build(stdout, "%s\n", GT_(" <empty>"));
	else for (idp = scratchlist; idp; idp = idp->next) {
		char *t = sdump(idp->id, strlen(idp->id)-1);
		report_build(stdout, " %s\n", t);
		free(t);
	}
	report_complete(stdout, "\n");
    }
    return PS_SUCCESS;
}

/** Assert that all UIDs marked deleted in query \a ctl have actually been
expunged. */
static int mark_as_expunged_if(struct uid_db_record *rec, void *unused)
{
    (void)unused;

    if (rec->status == UID_DELETED) rec->status = UID_EXPUNGED;
    return 0;
}

void expunge_uids(struct query *ctl)
{
    traverse_uid_db(dofastuidl ? &ctl->oldsaved : &ctl->newsaved,
		     mark_as_expunged_if, NULL);
}

static const char *str_uidmark(int mark)
{
	static char buf[20];

	switch(mark) {
		case UID_UNSEEN:
			return "UNSEEN";
		case UID_SEEN:
			return "SEEN";
		case UID_EXPUNGED:
			return "EXPUNGED";
		case UID_DELETED:
			return "DELETED";
		default:
			if (snprintf(buf, sizeof(buf), "MARK=%d", mark) < 0)
				return "ERROR";
			else
				return buf;
	}
}

static int dump_uid_db_record(struct uid_db_record *rec, void *arg)
{
	unsigned *n_recs;
	char *t;

	n_recs = (unsigned int *)arg;
	--*n_recs;

	t = sdump(rec->id, rec->id_len);
	report_build(stdout, " %s = %s\n", t, str_uidmark(rec->status));
	free(t);

	return 0;
}

static void dump_uid_db(struct uid_db *db)
{
	unsigned n_recs;

	n_recs = uid_db_n_records(db);
	if (!n_recs) {
		report_build(stdout, GT_(" <empty>"));
		return;
	}

	traverse_uid_db(db, dump_uid_db_record, &n_recs);
}

/** Finish a successful query */
void uid_swap_lists(struct query *ctl)
{
    /* debugging code */
    if (outlevel >= O_DEBUG)
    {
	if (dofastuidl) {
	    report_build(stdout, GT_("Merged UID list from %s:\n"), ctl->server.pollname);
	    dump_uid_db(&ctl->oldsaved);
	} else {
	    report_build(stdout, GT_("New UID list from %s:\n"), ctl->server.pollname);
	    dump_uid_db(&ctl->newsaved);
	}
	report_complete(stdout, "\n");
    }

    /*
     * Don't swap UID lists unless we've actually seen UIDLs.
     * This is necessary in order to keep UIDL information
     * from being heedlessly deleted later on.
     *
     * Older versions of fetchmail did
     *
     *     free_str_list(&scratchlist);
     *
     * after swap.  This was wrong; we need to preserve the UIDL information
     * from unqueried hosts.  Unfortunately, not doing this means that
     * under some circumstances UIDLs can end up being stored forever --
     * specifically, if a user description is removed from .fetchmailrc
     * with UIDLs from that account in .fetchids, there is no way for
     * them to ever get garbage-collected.
     */
    if (uid_db_n_records(&ctl->newsaved))
    {
	swap_uid_db_data(&ctl->newsaved, &ctl->oldsaved);
	clear_uid_db(&ctl->newsaved);
    }
    /* in fast uidl, there is no need to swap lists: the old state of
     * mailbox cannot be discarded! */
    else if (outlevel >= O_DEBUG && !dofastuidl)
	report(stdout, GT_("not swapping UID lists, no UIDs seen this query\n"));
}

/** Finish a query which had errors */
void uid_discard_new_list(struct query *ctl)
{
    /* debugging code */
    if (outlevel >= O_DEBUG)
    {
	/* this is now a merged list! the mails which were seen in this
	 * poll are marked here. */
	report_build(stdout, GT_("Merged UID list from %s:\n"), ctl->server.pollname);
	dump_uid_db(&ctl->oldsaved);
	report_complete(stdout, "\n");
    }

    if (uid_db_n_records(&ctl->newsaved))
    {
	/* new state of mailbox is not reliable */
	if (outlevel >= O_DEBUG)
	    report(stdout, GT_("discarding new UID list\n"));
	clear_uid_db(&ctl->newsaved);
    }
}

/** Reset the number associated with each id */
void uid_reset_num(struct query *ctl)
{
    reset_uid_db_nums(&ctl->oldsaved);
}

/** Write list of seen messages, at end of run. */
static int count_seen_deleted(struct uid_db_record *rec, void *arg)
{
    if (rec->status == UID_SEEN || rec->status == UID_DELETED)
	++*(long *)arg;
    return 0;
}

struct write_saved_info {
    struct query *ctl;
    FILE *fp;
};

static int write_uid_db_record(struct uid_db_record *rec, void *arg)
{
    struct write_saved_info *info;
    int rc;

    if (!(rec->status == UID_SEEN || rec->status == UID_DELETED))
	return 0;

    info = (struct write_saved_info *)arg;
    rc = fprintf(info->fp, "%s@%s %s\n",
		 info->ctl->remotename, info->ctl->server.queryname,
		 rec->id);
    return rc < 0 ? -1 : 0;
}

/** Write new list of UIDs (state) to \a idfile. */
void write_saved_lists(struct query *hostlist, const char *idfile)
{
    long	idcount;
    FILE	*tmpfp;
    struct query *ctl;
    struct idlist *idp;

    /* if all lists are empty, nuke the file */
    idcount = 0;
    for (ctl = hostlist; ctl; ctl = ctl->next)
	traverse_uid_db(&ctl->oldsaved, count_seen_deleted, &idcount);

    /* either nuke the file or write updated last-seen IDs */
    if (!idcount && !scratchlist)
    {
	if (outlevel >= O_DEBUG) {
	    if (access(idfile, F_OK) == 0)
		    report(stdout, GT_("Deleting fetchids file.\n"));
	}
	if (unlink(idfile) && errno != ENOENT)
	    report(stderr, GT_("Error deleting %s: %s\n"), idfile, strerror(errno));
    } else {
	char *newnam = (char *)xmalloc(strlen(idfile) + 2);
	mode_t old_umask;
	strcpy(newnam, idfile);
	strcat(newnam, "_");
	if (outlevel >= O_DEBUG)
	    report(stdout, GT_("Writing fetchids file.\n"));
	(void)unlink(newnam); /* remove file/link first */
	old_umask = umask(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH | S_IXOTH);
	if ((tmpfp = fopen(newnam, "w")) != (FILE *)NULL) {
	    struct write_saved_info info;
	    int errflg = 0;

	    info.fp = tmpfp;

	    for (ctl = hostlist; ctl; ctl = ctl->next) {
		info.ctl = ctl;

		if (traverse_uid_db(&ctl->oldsaved, write_uid_db_record, &info) < 0) {
		    int e = errno;
		    report(stderr, GT_("Write error on fetchids file %s: %s\n"), newnam, strerror(e));
		    errflg = 1;
		    goto bailout;
		}
	    }

	    for (idp = scratchlist; idp; idp = idp->next)
		if (EOF == fputs(idp->id, tmpfp)) {
			    int e = errno;
			    report(stderr, GT_("Write error on fetchids file %s: %s\n"), newnam, strerror(e));
			    errflg = 1;
			    goto bailout;
		}

bailout:
	    (void)fflush(tmpfp); /* return code ignored, we check ferror instead */
	    errflg |= ferror(tmpfp);
	    errflg |= fclose(tmpfp);
	    /* if we could write successfully, move into place;
	     * otherwise, drop */
	    if (errflg) {
		report(stderr, GT_("Error writing to fetchids file %s, old file left in place.\n"), newnam);
		unlink(newnam);
	    } else {
		if (rename(newnam, idfile)) {
		    report(stderr, GT_("Cannot rename fetchids file %s to %s: %s\n"), newnam, idfile, strerror(errno));
		}
	    }
	} else {
	    report(stderr, GT_("Cannot open fetchids file %s for writing: %s\n"), newnam, strerror(errno));
	}
	free(newnam);
	(void)umask(old_umask);
    }
}
#endif /* POP3_ENABLE */

/* uid.c ends here */
