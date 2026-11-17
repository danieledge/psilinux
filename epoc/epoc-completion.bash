# epoc-completion.bash - Tab completion for the epoc command (PsiLinux):
# its commands, the programs for "epoc run" (epoc apps), and file names.
# /etc/bash/psilinux.sh sources it from /psion/bin.
_epoc()
{
	local cur=${COMP_WORDS[COMP_CWORD]} cmd=${COMP_WORDS[1]}
	if [ "$COMP_CWORD" -eq 1 ]; then
		COMPREPLY=($(compgen -W "open run apps tasks ps close kill wait path info time locale printers print shot wait-battery wait-file note beep alert confirm clip syswrite version help completion" -- "$cur"))
		return
	fi
	case $cmd in
	run|start)
		if [ "$COMP_CWORD" -eq 2 ]; then
			local IFS=$'\n'
			COMPREPLY=($(compgen -W "$(epoc apps 2>/dev/null | cut -f2)" -- "$cur"))
		else
			COMPREPLY=($(compgen -f -- "$cur"))
		fi ;;
	close|wait) local IFS=$'\n'; COMPREPLY=($(compgen -W "$(epoc tasks 2>/dev/null | cut -f3)" -- "$cur")) ;;
	kill) local IFS=$'\n'; COMPREPLY=($(compgen -W "$(epoc ps 2>/dev/null | awk -F'\t' '$7 != "sys" {print $2}')" -- "$cur")) ;;
	clip) COMPREPLY=($(compgen -W "get set" -- "$cur")) ;;
	syswrite) COMPREPLY=($(compgen -W "on off --force" -- "$cur")) ;;
	help) COMPREPLY=($(compgen -W "exit" -- "$cur")) ;;
	info) COMPREPLY=($(compgen -W "machine rom edition ram_kb free_kb battery_percent battery_mv mains disk" -- "$cur")) ;;
	*) COMPREPLY=($(compgen -f -- "$cur")) ;;
	esac
}
complete -F _epoc epoc
complete -f xdg-open open
