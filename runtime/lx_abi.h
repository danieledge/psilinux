/* lx_abi.h - the Linux ARM EABI as PsiLinux's programs see it: error
 * numbers, flags and the structures that cross the system call boundary.
 * Values are the kernel's (asm-generic and arch/arm), checked against musl
 * 1.2.6. Everything is prefixed LX_ so the EPOC and host builds can include
 * their own system headers beside it. */
#ifndef LX_ABI_H
#define LX_ABI_H

typedef unsigned char lx_u8;
typedef unsigned short lx_u16;
typedef unsigned int lx_u32;
typedef int lx_s32;
typedef unsigned long long lx_u64;
typedef long long lx_s64;

/* error numbers */
#define LX_EPERM 1
#define LX_ENOENT 2
#define LX_ESRCH 3
#define LX_EINTR 4
#define LX_EIO 5
#define LX_ENXIO 6
#define LX_E2BIG 7
#define LX_ENOEXEC 8
#define LX_EBADF 9
#define LX_ECHILD 10
#define LX_EAGAIN 11
#define LX_ENOMEM 12
#define LX_EACCES 13
#define LX_EFAULT 14
#define LX_EBUSY 16
#define LX_EEXIST 17
#define LX_EXDEV 18
#define LX_ENODEV 19
#define LX_ENOTDIR 20
#define LX_EISDIR 21
#define LX_EINVAL 22
#define LX_ENFILE 23
#define LX_EMFILE 24
#define LX_ENOTTY 25
#define LX_ETXTBSY 26
#define LX_EFBIG 27
#define LX_ENOSPC 28
#define LX_ESPIPE 29
#define LX_EROFS 30
#define LX_EMLINK 31
#define LX_EPIPE 32
#define LX_ERANGE 34
#define LX_EDEADLK 35
#define LX_ENAMETOOLONG 36
#define LX_ENOLCK 37
#define LX_ENOSYS 38
#define LX_ENOTEMPTY 39
#define LX_ELOOP 40
#define LX_ENOTSOCK 88
#define LX_EDESTADDRREQ 89
#define LX_EMSGSIZE 90
#define LX_EPROTOTYPE 91
#define LX_ENOPROTOOPT 92
#define LX_EPROTONOSUPPORT 93
#define LX_EOPNOTSUPP 95
#define LX_EAFNOSUPPORT 97
#define LX_EADDRINUSE 98
#define LX_EADDRNOTAVAIL 99
#define LX_ENETDOWN 100
#define LX_ENETUNREACH 101
#define LX_ECONNABORTED 103
#define LX_ECONNRESET 104
#define LX_ENOBUFS 105
#define LX_EISCONN 106
#define LX_ENOTCONN 107
#define LX_ETIMEDOUT 110
#define LX_ECONNREFUSED 111
#define LX_EHOSTUNREACH 113
#define LX_EALREADY 114
#define LX_EINPROGRESS 115
#define LX_ENOTSUP LX_EOPNOTSUPP
#define LX_ERESTARTSYS 512     /* internal: restart the call after a signal handler */

/* open flags (arch/arm) */
#define LX_O_ACCMODE 3
#define LX_O_RDONLY 0
#define LX_O_WRONLY 1
#define LX_O_RDWR 2
#define LX_O_CREAT 0100
#define LX_O_EXCL 0200
#define LX_O_NOCTTY 0400
#define LX_O_TRUNC 01000
#define LX_O_APPEND 02000
#define LX_O_NONBLOCK 04000
#define LX_O_DSYNC 010000
#define LX_O_ASYNC 020000
#define LX_O_DIRECTORY 040000
#define LX_O_NOFOLLOW 0100000
#define LX_O_DIRECT 0200000
#define LX_O_LARGEFILE 0400000
#define LX_O_NOATIME 01000000
#define LX_O_CLOEXEC 02000000
#define LX_O_PATH 010000000

#define LX_AT_FDCWD (-100)
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_REMOVEDIR 0x200
#define LX_AT_SYMLINK_FOLLOW 0x400
#define LX_AT_EACCESS 0x200
#define LX_AT_EMPTY_PATH 0x1000

/* file modes */
#define LX_S_IFMT 0170000
#define LX_S_IFSOCK 0140000
#define LX_S_IFLNK 0120000
#define LX_S_IFREG 0100000
#define LX_S_IFBLK 0060000
#define LX_S_IFDIR 0040000
#define LX_S_IFCHR 0020000
#define LX_S_IFIFO 0010000
#define LX_S_ISDIR(m) (((m) & LX_S_IFMT) == LX_S_IFDIR)
#define LX_S_ISREG(m) (((m) & LX_S_IFMT) == LX_S_IFREG)
#define LX_S_ISLNK(m) (((m) & LX_S_IFMT) == LX_S_IFLNK)

/* fcntl */
#define LX_F_DUPFD 0
#define LX_F_GETFD 1
#define LX_F_SETFD 2
#define LX_F_GETFL 3
#define LX_F_SETFL 4
#define LX_F_GETLK 5
#define LX_F_SETLK 6
#define LX_F_SETLKW 7
#define LX_F_SETOWN 8
#define LX_F_GETOWN 9
#define LX_F_GETLK64 12
#define LX_F_SETLK64 13
#define LX_F_SETLKW64 14
#define LX_F_DUPFD_CLOEXEC 1030
#define LX_FD_CLOEXEC 1

/* lseek */
#define LX_SEEK_SET 0
#define LX_SEEK_CUR 1
#define LX_SEEK_END 2

/* access */
#define LX_F_OK 0
#define LX_X_OK 1
#define LX_W_OK 2
#define LX_R_OK 4

/* mmap */
#define LX_PROT_READ 1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC 4
#define LX_MAP_SHARED 1
#define LX_MAP_PRIVATE 2
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANONYMOUS 0x20
#define LX_MREMAP_MAYMOVE 1

/* signals */
#define LX_NSIG 65
#define LX_SIGHUP 1
#define LX_SIGINT 2
#define LX_SIGQUIT 3
#define LX_SIGILL 4
#define LX_SIGTRAP 5
#define LX_SIGABRT 6
#define LX_SIGBUS 7
#define LX_SIGFPE 8
#define LX_SIGKILL 9
#define LX_SIGUSR1 10
#define LX_SIGSEGV 11
#define LX_SIGUSR2 12
#define LX_SIGPIPE 13
#define LX_SIGALRM 14
#define LX_SIGTERM 15
#define LX_SIGSTKFLT 16
#define LX_SIGCHLD 17
#define LX_SIGCONT 18
#define LX_SIGSTOP 19
#define LX_SIGTSTP 20
#define LX_SIGTTIN 21
#define LX_SIGTTOU 22
#define LX_SIGURG 23
#define LX_SIGXCPU 24
#define LX_SIGXFSZ 25
#define LX_SIGVTALRM 26
#define LX_SIGPROF 27
#define LX_SIGWINCH 28
#define LX_SIGIO 29
#define LX_SIGPWR 30
#define LX_SIGSYS 31
#define LX_SIG_DFL 0
#define LX_SIG_IGN 1
#define LX_SA_NOCLDSTOP 1
#define LX_SA_NOCLDWAIT 2
#define LX_SA_SIGINFO 4
#define LX_SA_RESTORER 0x04000000
#define LX_SA_ONSTACK 0x08000000
#define LX_SA_RESTART 0x10000000
#define LX_SA_NODEFER 0x40000000
#define LX_SA_RESETHAND 0x80000000
#define LX_SIG_BLOCK 0
#define LX_SIG_UNBLOCK 1
#define LX_SIG_SETMASK 2
#define LX_SS_ONSTACK 1
#define LX_SS_DISABLE 2

/* the sigaction the kernel takes (rt_sigaction) */
struct lx_ksigaction {
	lx_u32 handler;
	lx_u32 flags;
	lx_u32 restorer;
	lx_u32 mask[2];
};

/* wait */
#define LX_WNOHANG 1
#define LX_WUNTRACED 2
#define LX_WCONTINUED 8
#define LX_P_ALL 0
#define LX_P_PID 1
#define LX_P_PGID 2
#define LX_WEXITED 4

/* clone */
#define LX_CLONE_VM 0x100
#define LX_CLONE_FS 0x200
#define LX_CLONE_FILES 0x400
#define LX_CLONE_SIGHAND 0x800
#define LX_CLONE_VFORK 0x4000
#define LX_CLONE_THREAD 0x10000
#define LX_CLONE_SETTLS 0x80000
#define LX_CLONE_PARENT_SETTID 0x100000
#define LX_CLONE_CHILD_CLEARTID 0x200000
#define LX_CLONE_CHILD_SETTID 0x1000000

/* clocks */
#define LX_CLOCK_REALTIME 0
#define LX_CLOCK_MONOTONIC 1
#define LX_CLOCK_PROCESS_CPUTIME_ID 2
#define LX_CLOCK_THREAD_CPUTIME_ID 3
#define LX_CLOCK_MONOTONIC_RAW 4
#define LX_CLOCK_REALTIME_COARSE 5
#define LX_CLOCK_MONOTONIC_COARSE 6
#define LX_CLOCK_BOOTTIME 7
#define LX_TIMER_ABSTIME 1

struct lx_timespec32 { lx_s32 tv_sec; lx_s32 tv_nsec; };
struct lx_timespec64 { lx_s64 tv_sec; lx_s32 tv_nsec; lx_s32 pad; };
struct lx_timeval32 { lx_s32 tv_sec; lx_s32 tv_usec; };

/* struct stat64 (arch/arm) */
struct lx_stat64 {
	lx_u64 st_dev;
	lx_u32 pad0;
	lx_u32 st_ino32;
	lx_u32 st_mode;
	lx_u32 st_nlink;
	lx_u32 st_uid;
	lx_u32 st_gid;
	lx_u64 st_rdev;
	lx_u32 pad3;
	lx_u32 pad3b;          /* (st_size is 8-aligned) */
	lx_s64 st_size;
	lx_u32 st_blksize;
	lx_u32 pad4;
	lx_u64 st_blocks;
	lx_u32 st_atime_;
	lx_u32 st_atime_nsec;
	lx_u32 st_mtime_;
	lx_u32 st_mtime_nsec;
	lx_u32 st_ctime_;
	lx_u32 st_ctime_nsec;
	lx_u64 st_ino;
};

/* struct statx (statx(2)), as much as is filled in */
struct lx_statx_ts { lx_s64 tv_sec; lx_u32 tv_nsec; lx_s32 pad; };
struct lx_statx {
	lx_u32 stx_mask;
	lx_u32 stx_blksize;
	lx_u64 stx_attributes;
	lx_u32 stx_nlink;
	lx_u32 stx_uid;
	lx_u32 stx_gid;
	lx_u16 stx_mode;
	lx_u16 pad1;
	lx_u64 stx_ino;
	lx_u64 stx_size;
	lx_u64 stx_blocks;
	lx_u64 stx_attributes_mask;
	struct lx_statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
	lx_u32 stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
	lx_u64 spare[14];
};

struct lx_statfs64 {
	lx_u32 f_type;
	lx_u32 f_bsize;
	lx_u64 f_blocks;
	lx_u64 f_bfree;
	lx_u64 f_bavail;
	lx_u64 f_files;
	lx_u64 f_ffree;
	lx_u32 f_fsid[2];
	lx_u32 f_namelen;
	lx_u32 f_frsize;
	lx_u32 f_flags;
	lx_u32 f_spare[4];
};

struct lx_dirent64 {
	lx_u64 d_ino;
	lx_s64 d_off;
	lx_u16 d_reclen;
	lx_u8 d_type;
	char d_name[1];
};
#define LX_DT_UNKNOWN 0
#define LX_DT_FIFO 1
#define LX_DT_CHR 2
#define LX_DT_DIR 4
#define LX_DT_BLK 6
#define LX_DT_REG 8
#define LX_DT_LNK 10
#define LX_DT_SOCK 12

struct lx_iovec { lx_u32 base; lx_u32 len; };

struct lx_utsname { char s[6][65]; };

struct lx_rlimit64 { lx_u64 cur, max; };
struct lx_rlimit32 { lx_u32 cur, max; };
#define LX_RLIMIT_STACK 3
#define LX_RLIMIT_NOFILE 7

struct lx_sysinfo {
	lx_s32 uptime;
	lx_u32 loads[3];
	lx_u32 totalram, freeram, sharedram, bufferram, totalswap, freeswap;
	lx_u16 procs, pad;
	lx_u32 totalhigh, freehigh, mem_unit;
	char reserved[8];
};

struct lx_rusage { struct lx_timeval32 utime, stime; lx_s32 rest[14]; };
struct lx_tms { lx_s32 utime, stime, cutime, cstime; };

struct lx_pollfd { lx_s32 fd; short events; short revents; };
#define LX_POLLIN 0x001
#define LX_POLLPRI 0x002
#define LX_POLLOUT 0x004
#define LX_POLLERR 0x008
#define LX_POLLHUP 0x010
#define LX_POLLNVAL 0x020
#define LX_POLLRDNORM 0x040
#define LX_POLLWRNORM 0x100

/* terminals */
#define LX_NCCS 32
struct lx_termios {
	lx_u32 c_iflag, c_oflag, c_cflag, c_lflag;
	lx_u8 c_line;
	lx_u8 c_cc[LX_NCCS];
	lx_u8 pad[3];
	lx_u32 c_ispeed, c_ospeed;
};
struct lx_winsize { lx_u16 ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define LX_VINTR 0
#define LX_VQUIT 1
#define LX_VERASE 2
#define LX_VKILL 3
#define LX_VEOF 4
#define LX_VTIME 5
#define LX_VMIN 6
#define LX_VSTART 8
#define LX_VSTOP 9
#define LX_VSUSP 10
#define LX_VEOL 11
#define LX_VREPRINT 12
#define LX_VWERASE 14
#define LX_VLNEXT 15
#define LX_VEOL2 16
/* c_iflag */
#define LX_IGNBRK 0000001
#define LX_BRKINT 0000002
#define LX_INLCR 0000100
#define LX_IGNCR 0000200
#define LX_ICRNL 0000400
#define LX_IXON 0002000
#define LX_IUTF8 0040000
/* c_oflag */
#define LX_OPOST 0000001
#define LX_ONLCR 0000004
/* c_cflag */
#define LX_B38400 0000017
#define LX_CS8 0000060
#define LX_CREAD 0000200
/* c_lflag */
#define LX_ISIG 0000001
#define LX_ICANON 0000002
#define LX_ECHO 0000010
#define LX_ECHOE 0000020
#define LX_ECHOK 0000040
#define LX_ECHONL 0000100
#define LX_NOFLSH 0000200
#define LX_TOSTOP 0000400
#define LX_ECHOCTL 0001000
#define LX_ECHOKE 0004000
#define LX_IEXTEN 0100000
/* ioctls */
#define LX_TCGETS 0x5401
#define LX_TCSETS 0x5402
#define LX_TCSETSW 0x5403
#define LX_TCSETSF 0x5404
#define LX_TCSBRK 0x5409
#define LX_TCXONC 0x540A
#define LX_TCFLSH 0x540B
#define LX_TIOCSCTTY 0x540E
#define LX_TIOCGPGRP 0x540F
#define LX_TIOCSPGRP 0x5410
#define LX_TIOCOUTQ 0x5411
#define LX_TIOCGWINSZ 0x5413
#define LX_TIOCSWINSZ 0x5414
#define LX_FIONREAD 0x541B
#define LX_TIOCNOTTY 0x5422
#define LX_TIOCGSID 0x5429
#define LX_FIONBIO 0x5421
#define LX_FIOCLEX 0x5451
#define LX_FIONCLEX 0x5450

/* sockets */
#define LX_AF_UNIX 1
#define LX_AF_INET 2
#define LX_AF_INET6 10
#define LX_SOCK_STREAM 1
#define LX_SOCK_DGRAM 2
#define LX_SOCK_NONBLOCK 04000
#define LX_SOCK_CLOEXEC 02000000
#define LX_SOL_SOCKET 1
#define LX_SO_REUSEADDR 2
#define LX_SO_TYPE 3
#define LX_SO_ERROR 4
#define LX_SO_KEEPALIVE 9
#define LX_SO_RCVBUF 8
#define LX_SO_SNDBUF 7
#define LX_SO_RCVTIMEO 20
#define LX_SO_SNDTIMEO 21
#define LX_IPPROTO_TCP 6
#define LX_TCP_NODELAY 1
#define LX_MSG_PEEK 2
#define LX_MSG_DONTWAIT 0x40
#define LX_MSG_NOSIGNAL 0x4000
#define LX_SHUT_RD 0
#define LX_SHUT_WR 1
#define LX_SHUT_RDWR 2
struct lx_sockaddr_in { lx_u16 family; lx_u16 port; lx_u32 addr; lx_u8 zero[8]; };
struct lx_msghdr { lx_u32 name; lx_u32 namelen; lx_u32 iov; lx_u32 iovlen; lx_u32 control; lx_u32 controllen; lx_u32 flags; };

/* auxiliary vector */
#define LX_AT_NULL 0
#define LX_AT_PHDR 3
#define LX_AT_PHENT 4
#define LX_AT_PHNUM 5
#define LX_AT_PAGESZ 6
#define LX_AT_BASE 7
#define LX_AT_FLAGS 8
#define LX_AT_ENTRY 9
#define LX_AT_UID 11
#define LX_AT_EUID 12
#define LX_AT_GID 13
#define LX_AT_EGID 14
#define LX_AT_PLATFORM 15
#define LX_AT_HWCAP 16
#define LX_AT_CLKTCK 17
#define LX_AT_SECURE 23
#define LX_AT_RANDOM 25
#define LX_AT_EXECFN 31
#define LX_AT_SYSINFO 32

#endif
