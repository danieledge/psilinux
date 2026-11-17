# psilinux-welcome.sh - (PsiLinux 0.37) the welcome banner when you log in on
# PsiLinux's window: Alpine's logo and this system's figures, as fastfetch
# shows them (/usr/lib/psilinux/welcome). Only for an interactive bash on the
# terminal: not for scripts, not over ssh. Off with "touch ~/.hushlogin" (the
# fortune goes too), or with PsiLinux's Tools > Show welcome banner, which
# brings back the short fortune of earlier versions.
if [ -n "$BASH_VERSION" ] && [ -t 0 ] && [ -t 1 ] && [ -z "$SSH_CONNECTION$SSH_TTY" ] && [ ! -e "$HOME/.hushlogin" ]; then
	case $- in
	*i*)
		if ! { [ -r /usr/lib/psilinux/welcome ] && . /usr/lib/psilinux/welcome && psilinux_welcome; }; then
			[ -x /usr/bin/fortune ] && { echo; fortune -s; echo; }
		fi
		unset -f psilinux_welcome _psw_line _psw_key _psw_used _psw_size 2>/dev/null
		;;
	esac
fi
