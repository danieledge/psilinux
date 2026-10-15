/* lx_vfs.h - the file system's parts (lx_vfs.c, lx_root.c, lx_dev.c) */
#ifndef LX_VFS_H
#define LX_VFS_H

enum { LX_FS_ROOT = 1, LX_FS_NATIVE, LX_FS_DEV, LX_FS_PROC };

#define LX_META_NAME "_LXMETA"     /* the overlay's per-folder notes (links, modes, whiteouts) */

/* what a path names (a lookup's answer) */
typedef struct lx_vn {
	int fs;
	char path[LX_PATH_MAX];     /* canonical Linux path */
	char native[LX_PATH_MAX];   /* the file on a Psion disk, if there is one */
	int upper;                  /* the root: it is in the writable layer */
	int img;                    /* the root: its entry in ROOT.IMG, or -1 */
	lx_u32 mode;
	lx_s64 size;
	lx_s64 mtime;               /* microseconds since 1970 */
	lx_u64 ino;
	lx_u64 rdev;
	char link[LX_PATH_MAX];     /* a symbolic link's target */
	int dev;                    /* /dev: which device */
	int pid;                    /* /proc: whose */
	int kind;                   /* /proc: which file */
	int fd;                     /* /proc/N/fd/<fd> */
} lx_vn;

int lx_vn_lookup(lx_proc *p, const char *path, lx_vn *v);
struct lx_file *lx_proc_fd_file(lx_vn *v);   /* /proc/N/fd/<fd> of a pipe or socket: that file (a reference) */
void lx_vn_stat(const lx_vn *v, struct lx_stat64 *st);
void lx_native_join(char *out, int max, const char *nroot, const char *rel);
void lx_name_in(const char *in, char *out, int max);
void lx_name_out(const char *in, char *out, int max);
void lx_dlist_add(void *dl, const char *name, int type, lx_u64 ino);
void lx_list_native(void *dl, const char *native, const char *lpath);
const char *lx_strchr(const char *s, int c);
int lx_mount_list(char *buf, int size);
int lx_mount_info(char *buf, int size);
int lx_vfs_mount(lx_proc *p, const char *src, const char *at, const char *fs, lx_u32 flags);
int lx_vfs_umount(lx_proc *p, const char *at);
int lx_vfs_mkdir(lx_proc *p, int dirfd, const char *path, lx_u32 mode);
int lx_vfs_unlink(lx_proc *p, int dirfd, const char *path, int dir);
int lx_vfs_rename(lx_proc *p, int olddir, const char *from, int newdir, const char *to);
int lx_vfs_symlink(lx_proc *p, const char *target, int dirfd, const char *path);
int lx_vfs_chmod(lx_proc *p, int dirfd, const char *path, lx_u32 mode, int follow);
int lx_vfs_utime(lx_proc *p, int dirfd, const char *path, lx_s64 mtime, int follow);

/* lx_root.c: the root (ROOT.IMG + the writable layer) */
void lx_root_init(void);
int lx_root_lookup(const char *upper_native, const char *rel, lx_vn *v);
void lx_root_list(lx_vn *v, void *dl);
lx_file *lx_root_open(lx_proc *p, lx_vn *v, int exists, int flags, lx_u32 mode, int *err);
int lx_root_mkdir(lx_proc *p, const char *path, lx_u32 mode);
int lx_root_unlink(lx_proc *p, lx_vn *v, int dir);
int lx_root_rename(lx_proc *p, lx_vn *from, lx_vn *to, int to_exists);
int lx_root_symlink(lx_proc *p, const char *path, const char *target);
int lx_root_chmod(lx_proc *p, lx_vn *v, lx_u32 mode);
int lx_root_utime(lx_proc *p, lx_vn *v, lx_s64 mtime);
int lx_root_info(lx_u32 *entries, lx_u32 *bytes);

/* lx_dev.c: /dev, /proc, pipes, memory files */
int lx_dev_lookup(lx_proc *p, const char *rel, lx_vn *v);
void lx_dev_list(lx_proc *p, lx_vn *v, void *dl);
lx_file *lx_dev_open(lx_proc *p, lx_vn *v, int flags, int *err);
int lx_proc_lookup(lx_proc *p, const char *rel, lx_vn *v);
void lx_proc_list(lx_proc *p, lx_vn *v, void *dl);
lx_file *lx_proc_open(lx_proc *p, lx_vn *v, int flags, int *err);
lx_file *lx_memfile_new(const char *data, int len);
int lx_file_describe(lx_file *f, char *buf, int size);   /* /proc/N/fd/M's target */
lx_file *lx_eventfd_new(lx_u32 initval, int flags);

#endif
