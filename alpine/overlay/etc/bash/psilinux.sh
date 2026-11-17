# psilinux.sh - bash on a Psion (PsiLinux's Alpine mode): sourced by
# /etc/bash/bashrc for interactive shells. Your own settings go in
# ~/.bashrc. (No bash-completion: it takes seconds to load on a 5mx;
# file and command names still complete with Tab.)

# the prompt: psion:~# (short, for a narrow screen)
PS1='\h:\w\$ '
# a short history, kept between sessions
HISTSIZE=200
HISTFILESIZE=200
HISTCONTROL=ignoreboth
shopt -s checkwinsize histappend
# (0.36) exit with jobs still running or stopped warns first; a second exit leaves
shopt -s checkjobs
# epoc's Tab completion (the command is the runner's: /psion/bin)
[ -r /psion/bin/epoc-completion.bash ] && . /psion/bin/epoc-completion.bash

alias ll='ls -l'
alias la='ls -A'
alias l='ls -CF'
alias ..='cd ..'

# a fortune at login (touch ~/.hushlogin to go without)
if shopt -q login_shell && [ -x /usr/bin/fortune ] && [ ! -e ~/.hushlogin ]; then
	echo; fortune -s; echo
fi
