#ifndef CVS_DIRECT_H
#define CVS_DIRECT_H

typedef struct _CvsServerCtx CvsServerCtx;

CvsServerCtx * open_cvs_server(char * root, int);
void close_cvs_server(CvsServerCtx*);
void cvs_rdiff(CvsServerCtx *, const char *, const char *, const char *, const char *);
void cvs_rupdate(CvsServerCtx *, const char *, const char *, const char *, int, const char *);
void cvs_diff(CvsServerCtx *, const char *, const char *, const char *, const char *, const char *);
FILE * cvs_rlog_open(CvsServerCtx *, const char *, const char *);
char * cvs_rlog_fgets(char *, int, CvsServerCtx *);
void cvs_rlog_close(CvsServerCtx *);
void cvs_version(CvsServerCtx *, char *, char *);

#endif /* CVS_DIRECT_H */
