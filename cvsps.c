#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <search.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <regex.h>

#include <cbtcommon/hash.h>
#include <cbtcommon/list.h>
#include <cbtcommon/text_util.h>
#include <cbtcommon/debug.h>
#include <cbtcommon/rcsid.h>

#include "cache.h"
#include "cvsps_types.h"
#include "cvsps.h"
#include "util.h"

RCSID("$Id: cvsps.c,v 4.53 2003/03/13 00:18:20 david Exp $");

#define CVS_LOG_BOUNDARY "----------------------------\n"
#define CVS_FILE_BOUNDARY "=============================================================================\n"

enum
{
    NEED_FILE,
    NEED_SYMS,
    NEED_EOS,
    NEED_START_LOG,
    NEED_REVISION,
    NEED_DATE_AUTHOR_STATE,
    NEED_EOM
};

/* true globals */
struct hash_table * file_hash;

const char * tag_flag_descr[] = {
    "",
    "**FUNKY**",
    "**INVALID**",
    "**INVALID**"
};

/* static globals */
static int ps_counter;
static void * ps_tree, * ps_tree_bytime;
static struct hash_table * global_symbols;
static char strip_path[PATH_MAX];
static int strip_path_len;
static time_t cache_date;
static int update_cache;
static int ignore_cache;
static int do_write_cache;
static int statistics;
static const char * test_log_file;

/* settable via options */
static int timestamp_fuzz_factor = 300;
static const char * restrict_author;
static int have_restrict_log;
static regex_t restrict_log;
static const char * restrict_file;
static time_t restrict_date_start;
static time_t restrict_date_end;
static const char * restrict_branch;
static struct list_head show_patch_set_ranges;
static int summary_first;
static const char * norc = "";
static const char * patch_set_dir;
static const char * restrict_tag_start;
static const char * restrict_tag_end;
static int restrict_tag_ps_start;
static int restrict_tag_ps_end;

static void parse_args(int, char *[]);
static void load_from_cvs();
static void init_strip_path();
static CvsFile * parse_file(const char *);
static CvsFileRevision * parse_revision(CvsFile * file, char * rev_str);
static void assign_pre_revision(PatchSetMember *, CvsFileRevision * rev);
static void check_print_patch_set(PatchSet *);
static void print_patch_set(PatchSet *);
static void show_ps_tree_node(const void *, const VISIT, const int);
static int compare_patch_sets(const void *, const void *);
static int compare_patch_sets_bytime(const void *, const void *);
static int is_revision_metadata(const char *);
static int patch_set_contains_member(PatchSet *, const char *);
static int patch_set_affects_branch(PatchSet *, const char *);
static void do_cvs_diff(PatchSet *);
static PatchSet * create_patch_set();
static PatchSetRange * create_patch_set_range();
static void parse_sym(CvsFile *, char *);
static void print_statistics(void);
static void resolve_global_symbols();
static int revision_affects_branch(CvsFileRevision *, const char *);
static int is_vendor_branch(const char *);
static void set_psm_initial(PatchSetMember * psm);
static int check_rev_funk(PatchSet *, CvsFileRevision *);
static CvsFileRevision * rev_follow_branch(CvsFileRevision *, const char *);
static int before_tag(CvsFileRevision * rev, const char * tag);

int main(int argc, char *argv[])
{
    debuglvl = DEBUG_APPERROR|DEBUG_SYSERROR;

    INIT_LIST_HEAD(&show_patch_set_ranges);

    parse_args(argc, argv);
    file_hash = create_hash_table(1023);
    global_symbols = create_hash_table(111);

    if (!ignore_cache)
    {
	int save_fuzz_factor = timestamp_fuzz_factor;

	/* the timestamp fuzz should only be in effect when loading from
	 * CVS, not re-fuzzed when loading from cache.  This is a hack
	 * working around bad use of global variables
	 */

	timestamp_fuzz_factor = 0;

	if ((cache_date = read_cache()) < 0)
	    update_cache = 1;

	timestamp_fuzz_factor = save_fuzz_factor;
    }
    
    if (update_cache)
    {
	load_from_cvs();
	do_write_cache = 1;
    }

    resolve_global_symbols();

    if (do_write_cache)
	write_cache(cache_date, ps_tree_bytime);

    if (statistics)
    {
	printf("Statistics:\n");
	print_statistics();
    }

    ps_counter = 0;
    twalk(ps_tree_bytime, show_ps_tree_node);
    if (summary_first++)
    {
	ps_counter = 0;
	twalk(ps_tree_bytime, show_ps_tree_node);
    }

    exit(0);
}

static void load_from_cvs()
{
    FILE * cvsfp;
    char buff[BUFSIZ];
    int state = NEED_FILE;
    CvsFile * file = NULL;
    PatchSetMember * psm = NULL;
    char datebuff[20];
    char authbuff[AUTH_STR_MAX];
    char logbuff[LOG_STR_MAX + 1];
    int loglen = 0;
    int have_log = 0;
    char cmd[BUFSIZ];
    char date_str[64];

    init_strip_path();

    if (cache_date > 0)
    {
	struct tm * tm = gmtime(&cache_date);
	strftime(date_str, 64, "%b %d, %Y %H:%M:%S GMT", tm);

	/* this command asks for logs using two different date
	 * arguments, separated by ';' (see man rlog).  The first
	 * gets all revisions more recent than date, the second 
	 * gets a single revision no later than date, which combined
	 * get us all revisions that have occurred since last update
	 * and overlaps what we had before by exactly one revision,
	 * which is necessary to fill in the pre_rev stuff for a 
	 * PatchSetMember
	 */
	snprintf(cmd, BUFSIZ, "cvs %s log -d '%s<;%s'", norc, date_str, date_str);
    }
    else
    {
	snprintf(cmd, BUFSIZ, "cvs %s log", norc);
    }
    
    debug(DEBUG_STATUS, "******* USING CMD %s", cmd);

    cache_date = time(NULL);

    /* for testing again and again without bashing a cvs server */
    if (test_log_file)
	cvsfp = fopen(test_log_file, "r");
    else
	cvsfp = popen(cmd, "r");

    if (!cvsfp)
    {
	debug(DEBUG_SYSERROR, "can't open cvs pipe using command %s", cmd);
	exit(1);
    }
    
    while(fgets(buff, BUFSIZ, cvsfp))
    {
	debug(DEBUG_STATUS, "state: %d read line:%s", state, buff);

	switch(state)
	{
	case NEED_FILE:
	    if (strncmp(buff, "RCS file", 8) == 0 && (file = parse_file(buff)))
		state = NEED_SYMS;
	    break;
	case NEED_SYMS:
	    if (strncmp(buff, "symbolic names:", 15) == 0)
		state = NEED_EOS;
	    break;
	case NEED_EOS:
	    if (!isspace(buff[0]))
	    {
		/* see cvsps_types.h for commentary on have_branches */
		file->have_branches = 1;
		state = NEED_START_LOG;
	    }
	    else
		parse_sym(file, buff);
	    break;
	case NEED_START_LOG:
	    if (strcmp(buff, CVS_LOG_BOUNDARY) == 0)
		state = NEED_REVISION;
	    break;
	case NEED_REVISION:
	    if (strncmp(buff, "revision", 8) == 0)
	    {
		char new_rev[REV_STR_MAX];
		CvsFileRevision * rev;

		strcpy(new_rev, buff + 9);
		chop(new_rev);

		/* 
		 * rev may already exist (think cvsps -u), in which
		 * case parse_revision is a hash lookup
		 */
		rev = parse_revision(file, new_rev);

		/* 
		 * in the simple case, we are copying rev to psm->pre_rev
		 * (psm refers to last patch set processed at this point)
		 * since generally speaking the log is reverse chronological.
		 * This breaks down slightly when branches are introduced 
		 */

		assign_pre_revision(psm, rev);

		/*
		 * if this is a new revision, it will have no post_psm associated.
		 * otherwise we are (probably?) hitting the overlap in cvsps -u 
		 */
		if (!rev->post_psm)
		{
		    psm = rev->post_psm = create_patch_set_member();
		    psm->post_rev = rev;
		    psm->file = file;
		    state = NEED_DATE_AUTHOR_STATE;
		}
		else
		{
		    /* we hit this in cvsps -u mode, we are now up-to-date
		     * w.r.t this particular file. skip all of the rest 
		     * of the info (revs and logs) until we hit the next file
		     */
		    psm = NULL;
		    state = NEED_EOM;
		}
	    }
	    break;
	case NEED_DATE_AUTHOR_STATE:
	    if (strncmp(buff, "date:", 5) == 0)
	    {
		char * p;

		strncpy(datebuff, buff + 6, 19);
		datebuff[19] = 0;

		strcpy(authbuff, "unknown");
		p = strstr(buff, "author: ");
		if (p)
		{
		    char * op;
		    p += 8;
		    op = strchr(p, ';');
		    if (op)
		    {
			strzncpy(authbuff, p, op - p + 1);
		    }
		}
		
		/* read the 'state' tag to see if this is a dead revision */
		p = strstr(buff, "state: ");
		if (p)
		{
		    char * op;
		    p += 7;
		    op = strchr(p, ';');
		    if (op)
			if (strncmp(p, "dead", MIN(4, op - p)) == 0)
			    psm->post_rev->dead = 1;
		}

		state = NEED_EOM;
	    }
	    break;
	case NEED_EOM:
	    if (strcmp(buff, CVS_LOG_BOUNDARY) == 0)
	    {
		if (psm)
		{
		    PatchSet * ps = get_patch_set(datebuff, logbuff, authbuff, psm->post_rev->branch);
		    patch_set_add_member(ps, psm);
		}

		logbuff[0] = 0;
		loglen = 0;
		have_log = 0;
		state = NEED_REVISION;
	    }
	    else if (strcmp(buff, CVS_FILE_BOUNDARY) == 0)
	    {
		if (psm)
		{
		    PatchSet * ps = get_patch_set(datebuff, logbuff, authbuff, psm->post_rev->branch);
		    patch_set_add_member(ps, psm);
		    assign_pre_revision(psm, NULL);
		}

		logbuff[0] = 0;
		loglen = 0;
		have_log = 0;
		psm = NULL;
		file = NULL;
		state = NEED_FILE;
	    }
	    else
	    {
		/* other "blahblah: information;" messages can 
		 * follow the stuff we pay attention to
		 */
		if (have_log || !is_revision_metadata(buff))
		{
		    /* if the log buffer is full, that's it.  
		     * 
		     * Also, read lines (fgets) always have \n in them
		     * which we count on.  So if truncation happens,
		     * be careful to put a \n on.
		     * 
		     * Buffer has LOG_STR_MAX + 1 for room for \0 if
		     * necessary
		     */
		    if (loglen < LOG_STR_MAX)
		    {
			int len = strlen(buff);
			
			if (len >= LOG_STR_MAX - loglen)
			{
			    debug(DEBUG_APPERROR, "WARNING: maximum log length exceeded, truncating log");
			    len = LOG_STR_MAX - loglen;
			    buff[len - 1] = '\n';
			}

			debug(DEBUG_STATUS, "appending %s to log", buff);
			memcpy(logbuff + loglen, buff, len);
			loglen += len;
			logbuff[loglen] = 0;
			have_log = 1;
		    }
		}
		else 
		{
		    debug(DEBUG_STATUS, "ignoring unhandled info %s", buff);
		}
	    }

	    break;
	}
    }

    if (state == NEED_SYMS)
    {
	debug(DEBUG_APPERROR, "Error: 'symbolic names' not found in log output.");
	debug(DEBUG_APPERROR, "       Perhaps you should try running with --norc");
	exit(1);
    }

    if (state != NEED_FILE)
    {
	debug(DEBUG_APPERROR, "Error: Log file parsing error.  Use -v to debug");
	exit(1);
    }
    
    if (test_log_file)
	fclose(cvsfp);
    else 
	pclose(cvsfp);
}

static void usage(const char * str1, const char * str2)
{
    if (str1)
	debug(DEBUG_APPERROR, "\nbad usage: %s %s\n", str1, str2);

    debug(DEBUG_APPERROR, "Usage: cvsps [-x] [-u] [-z <fuzz>] [-s <patchset>] [-a <author>] ");
    debug(DEBUG_APPERROR, "             [-f <file>] [-d <date1> [-d <date2>]] [-b <branch>]");
    debug(DEBUG_APPERROR, "             [-v] [-h]");
    debug(DEBUG_APPERROR, "");
    debug(DEBUG_APPERROR, "Where:");
    debug(DEBUG_APPERROR, "  -x ignore (and rebuild) cvsps.cache file");
    debug(DEBUG_APPERROR, "  -u update cvsps.cache file");
    debug(DEBUG_APPERROR, "  -z <fuzz> set the timestamp fuzz factor for identifying patch sets");
    debug(DEBUG_APPERROR, "  -s <patchset> generate a diff for a given patchset");
    debug(DEBUG_APPERROR, "  -a <author> restrict output to patchsets created by author");
    debug(DEBUG_APPERROR, "  -l <regex> restrict output to patchsets matching <regex> in log message");
    debug(DEBUG_APPERROR, "  -f <file> restrict output to patchsets involving file");
    debug(DEBUG_APPERROR, "  -d <date1> -d <date2> if just one date specified, show");
    debug(DEBUG_APPERROR, "     revisions newer than date1.  If two dates specified,");
    debug(DEBUG_APPERROR, "     show revisions between two dates.");
    debug(DEBUG_APPERROR, "  -r <tag1> -r <tag2> if just one tag specified, show");
    debug(DEBUG_APPERROR, "     revisions since tag1. If two tags specified, show");
    debug(DEBUG_APPERROR, "     revisions between the two tags.");
    debug(DEBUG_APPERROR, "  -b <branch> restrict output to patchsets affecting history of branch");
    debug(DEBUG_APPERROR, "  -p <directory> output patchsets to individual files in <directory>");
    debug(DEBUG_APPERROR, "  -v show verbose parsing messages");
    debug(DEBUG_APPERROR, "  -t show some brief memory usage statistics");
    debug(DEBUG_APPERROR, "  --norc when invoking cvs, ignore the .cvsrc file");
    debug(DEBUG_APPERROR, "  -h display this informative message");
    debug(DEBUG_APPERROR, "  --summary-first when multiple patchsets are shown, put all summaries first");
    debug(DEBUG_APPERROR, "\ncvsps version %s\n", VERSION);

    exit(1);
}

static void parse_args(int argc, char *argv[])
{
    int i = 1;
    while (i < argc)
    {
	if (strcmp(argv[i], "-z") == 0)
	{
	    if (++i >= argc)
		usage("argument to -z missing", "");

	    timestamp_fuzz_factor = atoi(argv[i++]);
	    continue;
	}
	
	if (strcmp(argv[i], "-s") == 0)
	{
	    PatchSetRange * range;
	    char * min_str, * max_str;

	    if (++i >= argc)
		usage("argument to -s missing", "");

	    min_str = strtok(argv[i++], ",");
	    do
	    {
		range = create_patch_set_range();

		max_str = strrchr(min_str, '-');
		if (max_str)
		    *max_str++ = '\0';
		else
		    max_str = min_str;

		range->min_counter = atoi(min_str);

		if (*max_str)
		    range->max_counter = atoi(max_str);
		else
		    range->max_counter = INT_MAX;

		list_add(&range->link, show_patch_set_ranges.prev);
	    }
	    while ((min_str = strtok(NULL, ",")));

	    continue;
	}
	
	if (strcmp(argv[i], "-a") == 0)
	{
	    if (++i >= argc)
		usage("argument to -a missing", "");

	    restrict_author = argv[i++];
	    continue;
	}

	if (strcmp(argv[i], "-l") == 0)
	{
	    int err;

	    if (++i >= argc)
		usage("argument to -l missing", "");

	    if ((err = regcomp(&restrict_log, argv[i++], REG_NOSUB)) != 0)
	    {
		char errbuf[256];
		regerror(err, &restrict_log, errbuf, 256);
		usage("bad regex to -l", errbuf);
	    }

	    have_restrict_log = 1;

	    continue;
	}

	if (strcmp(argv[i], "-f") == 0)
	{
	    if (++i >= argc)
		usage("argument to -f missing", "");

	    restrict_file = argv[i++];
	    continue;
	}
	
	if (strcmp(argv[i], "-d") == 0)
	{
	    time_t *pt;

	    if (++i >= argc)
		usage("argument to -d missing", "");

	    pt = (restrict_date_start == 0) ? &restrict_date_start : &restrict_date_end;
	    convert_date(pt, argv[i++]);
	    continue;
	}

	if (strcmp(argv[i], "-r") == 0)
	{
	    if (++i >= argc)
		usage("argument to -r missing", "");

	    if (restrict_tag_start)
		restrict_tag_end = argv[i];
	    else
		restrict_tag_start = argv[i];

	    i++;
	    continue;
	}

	if (strcmp(argv[i], "-u") == 0)
	{
	    update_cache = 1;
	    i++;
	    continue;
	}
	
	if (strcmp(argv[i], "-x") == 0)
	{
	    ignore_cache = 1;
	    update_cache = 1;
	    i++;
	    continue;
	}

	if (strcmp(argv[i], "-b") == 0)
	{
	    if (++i >= argc)
		usage("argument to -b missing", "");

	    restrict_branch = argv[i++];
	    /* Warn if the user tries to use TRUNK. Should eventually
	     * go away as TRUNK may be a valid branch within CVS
	     */
	    if (strcmp(restrict_branch, "TRUNK") == 0)
		debug(DEBUG_APPERROR, "Warning: The HEAD branch of CVS is called HEAD, not TRUNK");
	    continue;
	}

	if (strcmp(argv[i], "-p") == 0)
	{
	    if (++i >= argc)
		usage("argument to -p missing", "");
	    
	    patch_set_dir = argv[i++];
	    continue;
	}

	if (strcmp(argv[i], "-v") == 0)
	{
	    debuglvl = ~0;
	    i++;
	    continue;
	}
	
	if (strcmp(argv[i], "-t") == 0)
	{
	    statistics = 1;
	    i++;
	    continue;
	}

	if (strcmp(argv[i], "--summary-first") == 0)
	{
	    summary_first = 1;
	    i++;
	    continue;
	}

	if (strcmp(argv[i], "-h") == 0)
	    usage(NULL, NULL);

	if (strcmp(argv[i], "--norc") == 0)
	{
	    norc = "-f";
	    i++;
	    continue;
	}

	if (strcmp(argv[i], "--test-log") == 0)
	{
	    if (++i >= argc)
		usage("argument to --test-log missing", "");

	    test_log_file = argv[i++];
	    continue;
	}

	usage("invalid argument", argv[i]);
    }
}

static void init_strip_path()
{
    FILE * fp;
    char root_buff[PATH_MAX], rep_buff[PATH_MAX], *p;
    int len;

    if (!(fp = fopen("CVS/Root", "r")))
    {
	debug(DEBUG_SYSERROR, "Can't open CVS/Root");
	exit(1);
    }
    
    if (!fgets(root_buff, PATH_MAX, fp))
    {
	debug(DEBUG_APPERROR, "Error reading CVSROOT");
	exit(1);
    }

    fclose(fp);

    p = strrchr(root_buff, ':');

    if (!p)
	p = root_buff;
    else 
	p++;

    len = strlen(root_buff) - 1;
    root_buff[len] = 0;
    if (root_buff[len - 1] == '/')
	root_buff[--len] = 0;

    if (!(fp = fopen("CVS/Repository", "r")))
    {
	debug(DEBUG_SYSERROR, "Can't open CVS/Repository");
	exit(1);
    }

    if (!fgets(rep_buff, PATH_MAX, fp))
    {
	debug(DEBUG_APPERROR, "Error reading repository path");
	exit(1);
    }
    
    rep_buff[strlen(rep_buff) - 1] = 0;

    /* some CVS have the CVSROOT string as part of the repository
     * string (initial substring).  handle that case.
     */
    if (strncmp(p, rep_buff, strlen(p)) == 0)
        strip_path_len = snprintf(strip_path, PATH_MAX, "%s/", rep_buff);
    else
        strip_path_len = snprintf(strip_path, PATH_MAX, "%s/%s/", p, rep_buff);

    if (strip_path_len < 0)
    {
	debug(DEBUG_APPERROR, "strip_path overflow");
	exit(1);
    }

    debug(DEBUG_STATUS, "strip_path: %s", strip_path);
}

static CvsFile * parse_file(const char * buff)
{
    CvsFile * retval;
    char fn[PATH_MAX];
    int len = strlen(buff + 10);
    char * p;
    
    /* chop the ",v" string and the "LF" */
    len -= 3;
    memcpy(fn, buff + 10, len);
    fn[len] = 0;
    
    if (strncmp(fn, strip_path, strip_path_len) != 0)
    {
	/* FIXME: a subdirectory may have a different Repository path
	 * than it's parent.  we'll fail the above test since strip_path
	 * is global for the entire checked out tree (recursively).
	 *
	 * For now just ignore such files
	 */
	debug(DEBUG_APPERROR, "*** file %s doesn't match strip_path %s. ignoring", 
	      fn, strip_path);
	return NULL;
    }

    /* remove from beginning the 'strip_path' string */
    len -= strip_path_len;
    memmove(fn, fn + strip_path_len, len);
    fn[len] = 0;

    /* check if file is in the 'Attic/' and remove it */
    if ((p = strrchr(fn, '/')) &&
	p - fn >= 5 && strncmp(p - 5, "Attic", 5) == 0)
    {
	memmove(p - 5, p + 1, len - (p - fn + 1));
	len -= 6;
	fn[len] = 0;
    }

    debug(DEBUG_STATUS, "stripped filename %s", fn);

    retval = (CvsFile*)get_hash_object(file_hash, fn);

    if (!retval)
    {
	if ((retval = create_cvsfile()))
	{
	    retval->filename = xstrdup(fn);
	    put_hash_object_ex(file_hash, retval->filename, retval, HT_NO_KEYCOPY, NULL, NULL);
	}
	else
	{
	    debug(DEBUG_SYSERROR, "malloc failed");
	    exit(1);
	}

	debug(DEBUG_STATUS, "new file: %s", retval->filename);
    }
    else
    {
	debug(DEBUG_STATUS, "existing file: %s", retval->filename);
    }

    return retval;
}

PatchSet * get_patch_set(const char * dte, const char * log, const char * author, const char * branch)
{
    PatchSet * retval = NULL, **find = NULL;

    if (!(retval = create_patch_set()))
    {
	debug(DEBUG_SYSERROR, "malloc failed for PatchSet");
	return NULL;
    }

    convert_date(&retval->date, dte);
    retval->author = get_string(author);
    retval->descr = xstrdup(log);
    retval->branch = get_string(branch);

    find = (PatchSet**)tsearch(retval, &ps_tree, compare_patch_sets);

    if (*find != retval)
    {
	debug(DEBUG_STATUS, "found existing patch set");
	free(retval->descr);
	free(retval);
	retval = *find;
    }
    else
    {
	debug(DEBUG_STATUS, "new patch set!");
	debug(DEBUG_STATUS, "%s %s %s", retval->author, retval->descr, dte);
	if (tsearch(retval, &ps_tree_bytime, compare_patch_sets_bytime) == retval)
		abort();
    }

    return retval;
}

static int get_branch_ext(char * buff, const char * rev, int * leaf)
{
    char * p;
    int len = strlen(rev);

    /* allow get_branch(buff, buff) without destroying contents */
    memmove(buff, rev, len);
    buff[len] = 0;

    p = strrchr(buff, '.');
    if (!p)
	return 0;
    *p++ = 0;

    if (leaf)
	*leaf = atoi(p);

    return 1;
}

static int get_branch(char * buff, const char * rev)
{
    return get_branch_ext(buff, rev, NULL);
}

/* 
 * the goal if this function is to determine what revision to assign to
 * the psm->pre_rev field.  usually, the log file is strictly 
 * reverse chronological, so rev is direct ancestor to psm, 
 * 
 * This all breaks down at branch points however
 */

static void assign_pre_revision(PatchSetMember * psm, CvsFileRevision * rev)
{
    char pre[REV_STR_MAX], post[REV_STR_MAX];

    if (!psm)
	return;
    
    if (!rev)
    {
	/* if psm was last rev. for file, it's either an 
	 * INITIAL, or first rev of a branch.  to test if it's 
	 * the first rev of a branch, do get_branch twice - 
	 * this should be the bp.
	 */
	if (get_branch(post, psm->post_rev->rev) && 
	    get_branch(pre, post))
	{
	    psm->pre_rev = file_get_revision(psm->file, pre);
	    list_add(&psm->post_rev->link, &psm->pre_rev->branch_children);
	}
	else
	{
	    set_psm_initial(psm);
	}
	return;
    }

    /* 
     * is this canditate for 'pre' on the same branch as our 'post'? 
     * this is the normal case
     */
    if (!get_branch(pre, rev->rev))
    {
	debug(DEBUG_APPERROR, "get_branch malformed input (1)");
	return;
    }

    if (!get_branch(post, psm->post_rev->rev))
    {
	debug(DEBUG_APPERROR, "get_branch malformed input (2)");
	return;
    }

    if (strcmp(pre, post) == 0)
    {
	psm->pre_rev = rev;
	rev->pre_psm = psm;
	return;
    }
    
    /* branches don't match. new_psm must be head of branch,
     * so psm is oldest rev. on branch. or oldest
     * revision overall.  if former, derive predecessor.  
     * use get_branch to chop another rev. off of string.
     *
     * FIXME:
     * There's also a weird case.  it's possible to just re-number
     * a revision to any future revision. i.e. rev 1.9 becomes 2.0
     * It's not widely used.  In those cases of discontinuity,
     * we end up stamping the predecessor as 'INITIAL' incorrectly
     *
     */
    if (!get_branch(pre, post))
    {
	set_psm_initial(psm);
	return;
    }
    
    psm->pre_rev = file_get_revision(psm->file, pre);
    list_add(&psm->post_rev->link, &psm->pre_rev->branch_children);
}

static void check_print_patch_set(PatchSet * ps)
{
    /*
     * Ignore the 'BRANCH ADD' patchsets 
     */
    if (ps->branch_add)
	return;

    ps_counter++;

    if (restrict_date_start > 0 &&
	(ps->date < restrict_date_start ||
	 (restrict_date_end > 0 && ps->date > restrict_date_end)))
	return;

    if (restrict_author && strcmp(restrict_author, ps->author) != 0)
	return;

    if (have_restrict_log && regexec(&restrict_log, ps->descr, 0, NULL, 0) != 0)
	return;

    if (restrict_file && !patch_set_contains_member(ps, restrict_file))
	return;

    if (restrict_branch && !patch_set_affects_branch(ps, restrict_branch))
	return;
    
    /* the funk_factor overrides the restrict_tag_start and end */
    if (ps->funk_factor < 0)
	return;

    if (ps->funk_factor > 0)
	goto ok;

    /* resolve the ps.id of the start and end tag restrictions if necessary */
    if (restrict_tag_start && restrict_tag_ps_start == 0) 
    {
	if (ps->tag && strcmp(ps->tag, restrict_tag_start) == 0)
	    restrict_tag_ps_start = ps_counter;
    }

    if (restrict_tag_end && restrict_tag_ps_end == 0)
    {
	if (ps->tag && strcmp(ps->tag, restrict_tag_end) == 0)
	    restrict_tag_ps_end = ps_counter;
    }

    /* 
     * check the restriction, note: these id's may not be resolved, and
     * that comes into play here.  keep in mind we may pass through
     *  the tree multiple times.
     */
    if (restrict_tag_start)
    {
	if (restrict_tag_ps_start == 0 || ps_counter <= restrict_tag_ps_start)
	{
	    if (ps_counter == restrict_tag_ps_start)
		debug(DEBUG_STATUS, "PatchSet %d matches tag %s.", ps_counter, restrict_tag_start);

	    return;
	}

	if (restrict_tag_end && restrict_tag_ps_end > 0 && ps_counter > restrict_tag_ps_end)
	    return;
    }

 ok:
    if (!list_empty(&show_patch_set_ranges))
    {
	struct list_head * next = show_patch_set_ranges.next;
	
	while (next != &show_patch_set_ranges)
	{
	    PatchSetRange *range = list_entry(next, PatchSetRange, link);
	    if (range->min_counter <= ps_counter &&
		ps_counter <= range->max_counter)
	    {
		break;
	    }
	    next = next->next;
	}
	
	if (next == &show_patch_set_ranges)
	    return;
    }

    if (patch_set_dir)
    {
	char path[PATH_MAX];

	snprintf(path, PATH_MAX, "%s/%d.patch", patch_set_dir, ps_counter);

	fflush(stdout);
	close(1);
	if (open(path, O_WRONLY|O_TRUNC|O_CREAT, 0666) < 0)
	{
	    debug(DEBUG_SYSERROR, "can't open patch file %s", path);
	    exit(1);
	}

	fprintf(stderr, "Directing PatchSet %d to file %s\n", ps_counter, path);
    }

    /*
     * If the summary_first option is in effect, there will be 
     * two passes through the tree.  the first with summary_first == 1
     * the second with summary_first == 2.  if the option is not
     * in effect, there will be one pass with summary_first == 0
     *
     * When the -s option is in effect, the show_patch_set_ranges
     * list will be non-empty.
     */
    if (summary_first <= 1)
	print_patch_set(ps);
    if (!list_empty(&show_patch_set_ranges) && summary_first != 1)
	do_cvs_diff(ps);

    fflush(stdout);
}

static void print_patch_set(PatchSet * ps)
{
    struct tm * tm;
    struct list_head * next;

    tm = localtime(&ps->date);
    next = ps->members.next;
    
    /* this '---...' is different from the 28 hyphens that separate cvs log output */
    printf("---------------------\n");
    printf("PatchSet %d %s\n", ps_counter, ps->funk_factor > 0 ? "(FUNKY)" :"");
    printf("Date: %d/%02d/%02d %02d:%02d:%02d\n", 
	   1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday, 
	   tm->tm_hour, tm->tm_min, tm->tm_sec);
    printf("Author: %s\n", ps->author);
    printf("Branch: %s\n", ps->branch);
    printf("Tag: %s %s\n", ps->tag ? ps->tag : "(none)", tag_flag_descr[ps->tag_flags]);
    printf("Log:\n%s\n", ps->descr);
    printf("Members: \n");

    while (next != &ps->members)
    {
	PatchSetMember * psm = list_entry(next, PatchSetMember, link);

	printf("\t%s:%s->%s%s\n", 
	       psm->file->filename, 
	       psm->pre_rev ? psm->pre_rev->rev : "INITIAL", 
	       psm->post_rev->rev, 
	       psm->post_rev->dead ? "(DEAD)": "");
	next = next->next;
    }
    
    printf("\n");
}

static void show_ps_tree_node(const void * nodep, const VISIT which, const int depth)
{
    PatchSet * ps;

    switch(which)
    {
    case postorder:
    case leaf:
	ps = *(PatchSet**)nodep;
	check_print_patch_set(ps);
	break;

    default:
	break;
    }
}

static int compare_patch_sets(const void * v_ps1, const void * v_ps2)
{
    const PatchSet * ps1 = (const PatchSet *)v_ps1;
    const PatchSet * ps2 = (const PatchSet *)v_ps2;
    long diff;
    int ret;

    /* We order by (author, descr, date), but because of the fuzz factor
     * we treat times within a certain distance as equal IFF the author
     * and descr match.  If we allow the fuzz, but then the author or
     * descr don't match, return the date diff (if any) in order to get
     * the ordering right.
     */

    ret = strcmp(ps1->author, ps2->author);
    if (ret)
	    return ret;

    ret = strcmp(ps1->descr, ps2->descr);
    if (ret)
	    return ret;

    ret = strcmp(ps1->branch, ps2->branch);
    if (ret)
	return ret;

    diff = ps1->date - ps2->date;

    if (labs(diff) > timestamp_fuzz_factor)
	return (diff < 0) ? -1 : 1;
    return 0;
}

static int compare_patch_sets_bytime(const void * v_ps1, const void * v_ps2)
{
    const PatchSet * ps1 = (const PatchSet *)v_ps1;
    const PatchSet * ps2 = (const PatchSet *)v_ps2;
    long diff;
    int ret;

    /* When doing a time-ordering of patchsets, we don't need to
     * fuzzy-match the time.  We've already done fuzzy-matching so we
     * know that insertions are unique at this point.
     */

    diff = ps1->date - ps2->date;
    if (diff)
	return (diff < 0) ? -1 : 1;

    ret = strcmp(ps1->author, ps2->author);
    if (ret)
	return ret;

    ret = strcmp(ps1->descr, ps2->descr);
    if (ret)
	return ret;

    ret = strcmp(ps1->branch, ps2->branch);
    return ret;
}


static int is_revision_metadata(const char * buff)
{
    char * p1, *p2;
    int len;

    if (!(p1 = strchr(buff, ':')))
	return 0;

    p2 = strchr(buff, ' ');
    
    if (p2 && p2 < p1)
	return 0;

    len = strlen(buff);

    /* lines have LF at end */
    if (len > 1 && buff[len - 2] == ';')
	return 1;

    return 0;
}

static int patch_set_contains_member(PatchSet * ps, const char * file)
{
    struct list_head * next = ps->members.next;

    while (next != &ps->members)
    {
	PatchSetMember * psm = list_entry(next, PatchSetMember, link);
	
	if (strstr(psm->file->filename, file))
	    return 1;

	next = next->next;
    }

    return 0;
}

static int patch_set_affects_branch(PatchSet * ps, const char * branch)
{
    struct list_head * next = ps->members.next;

    while (next != &ps->members)
    {
	PatchSetMember * psm = list_entry(next, PatchSetMember, link);

	if (revision_affects_branch(psm->post_rev, branch))
	    return 1;

	next = next->next;
    }

    return 0;
}

static void do_cvs_diff(PatchSet * ps)
{
    struct list_head * next = ps->members.next;

    fflush(stdout);
    fflush(stderr);

    while (next != &ps->members)
    {
	PatchSetMember * psm = list_entry(next, PatchSetMember, link);
	char cmdbuff[PATH_MAX * 2+1];
	cmdbuff[PATH_MAX*2] = 0;

	/*
	 * It's possible for pre_rev to be a 'dead' revision.
	 * this happens when a file is added on a branch
	 */
	if (!psm->pre_rev || psm->pre_rev->dead)
	{
	    snprintf(cmdbuff, PATH_MAX * 2, "cvs %s update -p -r %s %s | diff -u /dev/null - | sed -e '1 s|^--- /dev/null|--- %s|g' -e '2 s|^+++ -|+++ %s|g'",
		     norc, psm->post_rev->rev, psm->file->filename, psm->file->filename, psm->file->filename);
	}
	else if (psm->post_rev->dead)
	{
	    snprintf(cmdbuff, PATH_MAX * 2, "cvs %s update -p -r %s %s | diff -u - /dev/null | sed -e '1 s|^--- -|--- %s|g' -e '2 s|^+++ /dev/null|+++ %s|g'",
		     norc, psm->pre_rev->rev, psm->file->filename, psm->file->filename, psm->file->filename);
	    
	}
	else
	{
	    snprintf(cmdbuff, PATH_MAX * 2, "cvs %s diff -u -r %s -r %s %s",
		     norc, psm->pre_rev->rev, psm->post_rev->rev, psm->file->filename);
	}

	system(cmdbuff);

	next = next->next;
    }
}

static CvsFileRevision * parse_revision(CvsFile * file, char * rev_str)
{
    char * p;

    /* The "revision" log line can include extra information 
     * including who is locking the file --- strip that out.
     */
    
    p = rev_str;
    while (isdigit(*p) || *p == '.')
	    p++;
    *p = 0;

    return cvs_file_add_revision(file, rev_str);
}

CvsFileRevision * cvs_file_add_revision(CvsFile * file, const char * rev_str)
{
    CvsFileRevision * rev;

    if (!(rev = (CvsFileRevision*)get_hash_object(file->revisions, rev_str)))
    {
	rev = (CvsFileRevision*)calloc(1, sizeof(*rev));
	rev->rev = get_string(rev_str);
	rev->file = file;
	rev->pre_psm = NULL;
	rev->post_psm = NULL;
	INIT_LIST_HEAD(&rev->branch_children);
	INIT_LIST_HEAD(&rev->tags);
	
	put_hash_object_ex(file->revisions, rev->rev, rev, HT_NO_KEYCOPY, NULL, NULL);

	debug(DEBUG_STATUS, "added revision %s to file %s", rev_str, file->filename);
    }
    else
    {
	debug(DEBUG_STATUS, "found revision %s to file %s", rev_str, file->filename);
    }

    /* 
     * note: we are guaranteed to get here at least once with 'have_branches' == 1.
     * we may pass through once before this, because of symbolic tags, then once
     * always when processing the actual revision logs
     *
     * rev->branch will always be set to something, maybe "HEAD"
     */
    if (!rev->branch && file->have_branches)
    {
	char branch_str[REV_STR_MAX];

	/* determine the branch this revision was committed on */
	if (!get_branch(branch_str, rev->rev))
	{
	    debug(DEBUG_APPERROR, "invalid rev format %s", rev->rev);
	    exit(1);
	}
	
	rev->branch = (char*)get_hash_object(file->branches, branch_str);
	
	/* if there's no branch and it's not on the trunk, blab */
	if (!rev->branch)
	{
	    if (get_branch(branch_str, branch_str))
	    {
		//debug(DEBUG_APPERROR, "warning: revision %s of file %s on unnamed branch", rev->rev, rev->file->filename);
		rev->branch = "#CVSPS_NO_BRANCH";
	    }
	    else
	    {
		rev->branch = "HEAD";
	    }
	}

	debug(DEBUG_STATUS, "revision %s of file %s on branch %s", rev->rev, rev->file->filename, rev->branch);
    }

    return rev;
}

CvsFile * create_cvsfile()
{
    CvsFile * f = (CvsFile*)calloc(1, sizeof(*f));
    if (!f)
	return NULL;

    f->revisions = create_hash_table(53);
    f->branches = create_hash_table(13);
    f->branches_sym = create_hash_table(13);
    f->symbols = create_hash_table(253);
    f->have_branches = 0;

    if (!f->revisions || !f->branches || !f->branches_sym)
    {
	if (f->branches)
	    destroy_hash_table(f->branches, NULL);
	if (f->revisions)
	    destroy_hash_table(f->revisions, NULL);
	free(f);
	return NULL;
    }
   
    return f;
}

static PatchSet * create_patch_set()
{
    PatchSet * ps = (PatchSet*)calloc(1, sizeof(*ps));;
    
    if (ps)
    {
	INIT_LIST_HEAD(&ps->members);
	ps->descr = NULL;
	ps->author = NULL;
	ps->tag = NULL;
	ps->tag_flags = 0;
	ps->branch_add = 0;
	ps->funk_factor = 0;
    }

    return ps;
}

PatchSetMember * create_patch_set_member()
{
    PatchSetMember * psm = (PatchSetMember*)calloc(1, sizeof(*psm));
    psm->pre_rev = NULL;
    psm->post_rev = NULL;
    psm->file = NULL;
    return psm;
}

static PatchSetRange * create_patch_set_range()
{
    PatchSetRange * psr = (PatchSetRange*)calloc(1, sizeof(*psr));
    return psr;
}

CvsFileRevision * file_get_revision(CvsFile * file, const char * r)
{
    CvsFileRevision * rev;

    if (strcmp(r, "INITIAL") == 0)
	return NULL;

    rev = (CvsFileRevision*)get_hash_object(file->revisions, r);
    
    if (!rev)
    {
	debug(DEBUG_APPERROR, "request for non-existent rev %s in file %s", r, file->filename);
	exit(1);
    }

    return rev;
}

/*
 * Parse lines in the format:
 * 
 * <white space>tag_name: <rev>;
 *
 * Handles both regular tags (these go into the symbols hash)
 * and magic-branch-tags (second to last node of revision is 0)
 * which go into branches and branches_sym hashes.  Magic-branch
 * format is hidden in CVS everwhere except the 'cvs log' output.
 */

static void parse_sym(CvsFile * file, char * sym)
{
    char * tag = sym, *eot;
    int leaf, final_branch = -1;
    char rev[REV_STR_MAX];
    char rev2[REV_STR_MAX];
    
    while (*tag && isspace(*tag))
	tag++;

    if (!*tag)
	return;

    eot = strchr(tag, ':');
    
    if (!eot)
	return;

    *eot = 0;
    eot += 2;
    
    if (!get_branch_ext(rev, eot, &leaf))
    {
	debug(DEBUG_APPERROR, "malformed revision");
	exit(1);
    }

    /* 
     * get_branch_ext will leave final_branch alone
     * if there aren't enough '.' in string 
     */
    get_branch_ext(rev2, rev, &final_branch);

    if (final_branch == 0)
    {
	snprintf(rev, REV_STR_MAX, "%s.%d", rev2, leaf);
	debug(DEBUG_STATUS, "got sym: %s for %s", tag, rev);
	
	cvs_file_add_branch(file, rev, tag);
    }
    else
    {
	strcpy(rev, eot);
	chop(rev);

	/* see cvs manual: what is this vendor tag? */
	if (is_vendor_branch(rev))
	    cvs_file_add_branch(file, rev, tag);
	else
	    cvs_file_add_symbol(file, rev, tag);
    }
}

void cvs_file_add_symbol(CvsFile * file, const char * rev_str, const char * p_tag_str)
{
    CvsFileRevision * rev;
    GlobalSymbol * sym;
    Tag * tag;

    /* get a permanent storage string */
    char * tag_str = get_string(p_tag_str);

    debug(DEBUG_STATUS, "adding symbol to file: %s %s->%s", file->filename, tag_str, rev_str);
    rev = cvs_file_add_revision(file, rev_str);
    put_hash_object_ex(file->symbols, tag_str, rev, HT_NO_KEYCOPY, NULL, NULL);
    
    /*
     * check the global_symbols
     */
    sym = (GlobalSymbol*)get_hash_object(global_symbols, tag_str);
    if (!sym)
    {
	sym = (GlobalSymbol*)malloc(sizeof(*sym));
	sym->tag = tag_str;
	sym->ps = NULL;
	INIT_LIST_HEAD(&sym->tags);

	put_hash_object_ex(global_symbols, sym->tag, sym, HT_NO_KEYCOPY, NULL, NULL);
    }

    tag = (Tag*)malloc(sizeof(*tag));
    tag->tag = tag_str;
    tag->rev = rev;
    tag->sym = sym;
    list_add(&tag->global_link, &sym->tags);
    list_add(&tag->rev_link, &rev->tags);
}

char * cvs_file_add_branch(CvsFile * file, const char * rev, const char * tag)
{
    char * new_tag;
    char * new_rev;

    if (get_hash_object(file->branches, rev))
    {
	debug(DEBUG_STATUS, "attempt to add existing branch %s:%s to %s", 
	      rev, tag, file->filename);
	return NULL;
    }

    /* get permanent storage for the strings */
    new_tag = get_string(tag);
    new_rev = get_string(rev); 

    put_hash_object_ex(file->branches, new_rev, new_tag, HT_NO_KEYCOPY, NULL, NULL);
    put_hash_object_ex(file->branches_sym, new_tag, new_rev, HT_NO_KEYCOPY, NULL, NULL);
    
    return new_tag;
}

static void count_hash(struct hash_table *hash, unsigned int *total, 
	unsigned int *max_val)
{
    int counter = 0;
    struct hash_entry *fh;
    
    reset_hash_iterator(hash);
    while ((fh = next_hash_entry(hash)))
	counter++;

    *total += counter;
    *max_val= MAX(*max_val, counter);
}

static unsigned int num_patch_sets = 0;
static unsigned int num_ps_member = 0, max_ps_member_in_ps = 0;
static unsigned int num_authors = 0, max_author_len = 0, total_author_len = 0;
static unsigned int max_descr_len = 0, total_descr_len = 0;
struct hash_table *author_hash;

static void stat_ps_tree_node(const void * nodep, const VISIT which, const int depth)
{
    int desc_len;
    PatchSet * ps;
    struct list_head * next;
    int counter;
    void * old;

    /* Make sure we have it if we do statistics */
    if (!author_hash)
	author_hash = create_hash_table(1023);

    switch(which)
    {
    case postorder:
    case leaf:
	ps = *(PatchSet**)nodep;
	num_patch_sets++;

	/* Author statistics */
	if (put_hash_object_ex(author_hash, ps->author, ps->author, HT_NO_KEYCOPY, NULL, &old) >= 0 && old)
	{
	    int len = strlen(ps->author);
	    num_authors++;
	    max_author_len = MAX(max_author_len, len);
	    total_author_len += len;
	}

	/* Log message statistics */
	desc_len = strlen(ps->descr);
	max_descr_len = MAX(max_descr_len, desc_len);
	total_descr_len += desc_len;
	
	/* PatchSet member statistics */
	counter = 0;
	next = ps->members.next;
	while (next != &ps->members)
	{
	    counter++;
	    next = next->next;
	}

	num_ps_member += counter;
	max_ps_member_in_ps = MAX(max_ps_member_in_ps, counter);
	break;

    default:
	break;
    }
}

static void print_statistics(void)
{
    /* Statistics data */
    unsigned int num_files = 0, max_file_len = 0, total_file_len = 0;
    unsigned int total_revisions = 0, max_revisions_for_file = 0;
    unsigned int total_branches = 0, max_branches_for_file = 0;
    unsigned int total_branches_sym = 0, max_branches_sym_for_file = 0;

    /* Other vars */
    struct hash_entry *he;
   
    fflush(stdout);

    /* Gather file statistics */
    reset_hash_iterator(file_hash);
    while ((he=next_hash_entry(file_hash)))
    {
	int len = strlen(he->he_key);
	CvsFile *file = (CvsFile *)he->he_obj;
	
	num_files++;
	max_file_len = MAX(max_file_len, len);
	total_file_len += len;

	count_hash(file->revisions, &total_revisions, &max_revisions_for_file);
	count_hash(file->branches, &total_branches, &max_branches_for_file);
	count_hash(file->branches_sym, &total_branches_sym,
	    &max_branches_sym_for_file);
    }

    /* Print file statistics */
    printf("Num files: %d\nMax filename len: %d, Average filename len: %.2f\n",
	    num_files, max_file_len, (float)total_file_len/num_files);

    printf("Max revisions for file: %d, Average revisions for file: %.2f\n",
	  max_revisions_for_file, (float)total_revisions/num_files);
    printf("Max branches for file: %d, Average branches for file: %.2f\n",
	  max_branches_for_file, (float)total_branches/num_files);
    printf("Max branches_sym for file: %d, Average branches_sym for file: %.2f\n",
	  max_branches_sym_for_file, (float)total_branches_sym/num_files);

    /* Gather patchset statistics */
    twalk(ps_tree, stat_ps_tree_node);

    /* Print patchset statistics */
    printf("Num patchsets: %d\n", num_patch_sets);
    printf("Max PS members in PS: %d\nAverage PS members in PS: %.2f\n",
	    max_ps_member_in_ps, (float)num_ps_member/num_patch_sets);
    printf("Num authors: %d, Max author len: %d, Avg. author len: %.2f\n", 
	    num_authors, max_author_len, (float)total_author_len/num_authors);
    printf("Max desc len: %d, Avg. desc len: %.2f\n",
	    max_descr_len, (float)total_descr_len/num_patch_sets);
}

/*
 * Resolve each global symbol to a PatchSet.  This is
 * not necessarily doable, because tagging isn't 
 * necessarily done to the project as a whole, and
 * it's possible that no tag is valid for all files 
 * at a single point in time.  We check for that
 * case though.
 *
 * Implementation: the most recent PatchSet containing
 * a revision (post_rev) tagged by the symbol is considered
 * the 'tagged' PatchSet.
 */

static void resolve_global_symbols()
{
    struct hash_entry * he_sym;
    reset_hash_iterator(global_symbols);
    while ((he_sym = next_hash_entry(global_symbols)))
    {
	GlobalSymbol * sym = (GlobalSymbol*)he_sym->he_obj;
	PatchSet * ps;
	struct list_head * next;

	debug(DEBUG_STATUS, "resolving global symbol %s", sym->tag);

	/*
	 * First pass, determine the most recent PatchSet with a 
	 * revision tagged with the symbolic tag.  This is 'the'
	 * patchset with the tag
	 */

	for (next = sym->tags.next; next != &sym->tags; next = next->next)
	{
	    Tag * tag = list_entry(next, Tag, global_link);
	    CvsFileRevision * rev = tag->rev;

	    ps = rev->post_psm->ps;

	    if (!sym->ps || ps->date > sym->ps->date)
		sym->ps = ps;
	}
	
	/* convenience variable */
	ps = sym->ps;

	if (!ps)
	{
	    debug(DEBUG_APPERROR, "no patchset for tag %s", sym->tag);
	    return;
	}

	ps->tag = sym->tag;

	/* 
	 * Second pass. 
	 * check if this is an invalid patchset, 
	 * check which members are invalid.  determine
	 * the funk factor etc.
	 */
	for (next = sym->tags.next; next != &sym->tags; next = next->next)
	{
	    Tag * tag = list_entry(next, Tag, global_link);
	    CvsFileRevision * rev = tag->rev;
	    CvsFileRevision * next_rev = rev_follow_branch(rev, ps->branch);
	    
	    if (!next_rev)
		continue;
		
	    /*
	     * we want the 'tagged revision' to be valid until after
	     * the date of the 'tagged patchset' or else there's something
	     * funky going on
	     */
	    if (next_rev->post_psm->ps->date < ps->date)
	    {
		int flag = check_rev_funk(ps, next_rev);
		debug(DEBUG_APPERROR, "file %s revision %s tag %s: TAG VIOLATION",
		      rev->file->filename, rev->rev, sym->tag);
		ps->tag_flags |= flag;
	    }
	}
    }
}

static int revision_affects_branch(CvsFileRevision * rev, const char * branch)
{
    /* special case the branch called 'HEAD' */
    if (strcmp(branch, "HEAD") == 0)
    {
	/* look for only one '.' in rev */
	char * p = strchr(rev->rev, '.');
	if (p && !strchr(p + 1, '.'))
	    return 1;
    }
    else
    {
	char * branch_rev = (char*)get_hash_object(rev->file->branches_sym, branch);
	
	if (branch_rev)
	{
	    char post_rev[REV_STR_MAX];
	    char branch[REV_STR_MAX];
	    int file_leaf, branch_leaf;
	    
	    strcpy(branch, branch_rev);
	    
	    /* first get the branch the file rev is on */
	    if (get_branch_ext(post_rev, rev->rev, &file_leaf))
	    {
		branch_leaf = file_leaf;
		
		/* check against branch and all branch ancestor branches */
		do 
		{
		    debug(DEBUG_STATUS, "check %s against %s for %s", branch, post_rev, rev->file->filename);
		    if (strcmp(branch, post_rev) == 0)
			return (file_leaf <= branch_leaf);
		}
		while(get_branch_ext(branch, branch, &branch_leaf));
	    }
	}
    }

    return 0;
}

/*
 * When importing vendor sources, (apparently people do this)
 * the code is added on a 'vendor' branch, which, for some reason
 * doesn't use the magic-branch-tag format.  Try to detect that now
 */
static int is_vendor_branch(const char * rev)
{
    int dots = 0;
    const char *p = rev;

    while (*p)
	if (*p++ == '.')
	    dots++;

    return !(dots&1);
}

void patch_set_add_member(PatchSet * ps, PatchSetMember * psm)
{
    psm->ps = ps;
    list_add(&psm->link, psm->ps->members.prev);
}

static void set_psm_initial(PatchSetMember * psm)
{
    psm->pre_rev = NULL;
    if (psm->post_rev->dead)
    {
	/* 
	 * we expect a 'file xyz initially added on branch abc' here
	 * but there can only be one such member in a given patchset
	 */
	if (psm->ps->branch_add)
	    debug(DEBUG_APPERROR, "branch_add already set!");
	psm->ps->branch_add = 1;
    }
}

/* 
 * look at all revisions starting at rev and going forward until 
 * ps->date and see whether they are invalid or just funky.
 */
static int check_rev_funk(PatchSet * ps, CvsFileRevision * rev)
{
    while (rev)
    {
	PatchSet * next_ps = rev->post_psm->ps;
	struct list_head * next;

	if (next_ps->date > ps->date)
	    break;

	debug(DEBUG_STATUS, "ps->date %d next_ps->date %d rev->rev %s rev->branch %s", 
	      ps->date, next_ps->date, rev->rev, rev->branch);

	for (next = next_ps->members.next; next != &next_ps->members; next = next->next)
	{
	    PatchSetMember * psm = list_entry(next, PatchSetMember, link);
	    if (before_tag(psm->post_rev, ps->tag))
		return TAG_INVALID;
	}

	/*
	 * If the ps->tag is one of the two possible '-r' tags
	 * then the funkyness is even more important.
	 *
	 * In the restrict_tag_start case, this next_ps is chronologically
	 * before ps, but tagwise after, so set the funk_factor so it will
	 * be included.
	 *
	 * The restrict_tag_end case is similar, but backwards.
	 */
	if (restrict_tag_start && strcmp(ps->tag, restrict_tag_start) == 0)
	    next_ps->funk_factor = 1;
	if (restrict_tag_end && strcmp(ps->tag, restrict_tag_end) == 0)
	    next_ps->funk_factor = -1;

	rev = rev_follow_branch(rev, ps->branch);
    }

    return TAG_FUNKY;
}

/* determine if the revision is before the tag */
static int before_tag(CvsFileRevision * rev, const char * tag)
{
    CvsFileRevision * tagged_rev = (CvsFileRevision*)get_hash_object(rev->file->symbols, tag);
    int retval = 0;

    if (tagged_rev && 
	revision_affects_branch(rev, tagged_rev->branch) && 
	rev->post_psm->ps->date <= tagged_rev->post_psm->ps->date)
	retval = 1;

    debug(DEBUG_STATUS, "before_tag: %s %s %s %s %d", 
	  rev->file->filename, tag, rev->rev, tagged_rev ? tagged_rev->rev : "N/A", retval);

    return retval;
}

/* get the next revision from this one following branch if possible */
/* FIXME: not sure if this needs to follow branches leading up to branches? */
static CvsFileRevision * rev_follow_branch(CvsFileRevision * rev, const char * branch)
{
    struct list_head * next;

    /* check for 'main line of inheritance' */
    if (strcmp(rev->branch, branch) == 0)
	return rev->pre_psm ? rev->pre_psm->post_rev : NULL;

    /* look down branches */
    for (next = rev->branch_children.next; next != &rev->branch_children; next = next->next)
    {
	CvsFileRevision * next_rev = list_entry(next, CvsFileRevision, link);
	//debug(DEBUG_STATUS, "SCANNING BRANCH CHILDREN: %s %s", next_rev->branch, branch);
	if (strcmp(next_rev->branch, branch) == 0)
	    return next_rev;
    }
    
    return NULL;
}
