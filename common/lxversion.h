/* lxversion.h - PsiLinux's version: the one place it is set (0.47, RN-19).
 * The runner (runtime/lx_core.h: uname, /proc/version, `epoc version`,
 * the log) and the window (About) both take it from here, and build.sh
 * checks that pkg/psilinux.pkg says the same: 0.45's runner still said
 * 0.44 on Dan's netBook. Two digits, as every Coda program. */
#ifndef LXVERSION_H
#define LXVERSION_H
#define LX_VERSION "0.54"
#endif
