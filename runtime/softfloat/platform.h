/* platform.h - SoftFloat 3e's platform settings for PsiLinux (both
 * compilers: gcc 3.0 for EPOC, gcc 14 for the host harness). Not part of
 * the SoftFloat release: written for PsiLinux (the rest of this folder is
 * Berkeley SoftFloat 3e, unchanged: see COPYING.txt). */
#define LITTLEENDIAN 1
/* static inline works the same in gnu89 and C99 */
#define INLINE static __inline__
#define THREAD_LOCAL
#define INLINE_LEVEL 5
