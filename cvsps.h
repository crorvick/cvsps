#ifndef CVSPS_H
#define CVSPS_H

extern struct hash_table * file_hash;
extern const char * tag_flag_descr[];




CvsFile * create_cvsfile();
CvsFileRevision * cvs_file_add_revision(CvsFile *, const char *);
void cvs_file_add_symbol(CvsFile * file, const char * rev, const char * tag);
char * cvs_file_add_branch(CvsFile *, const char *, const char *);
PatchSet * get_patch_set(const char *, const char *, const char *, const char *);
PatchSetMember * create_patch_set_member();
CvsFileRevision * file_get_revision(CvsFile *, const char *);
void patch_set_add_member(PatchSet * ps, PatchSetMember * psm);

#endif /* CVSPS_H */
