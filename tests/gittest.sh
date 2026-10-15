#!/bin/sh
# gittest.sh - git on PsiLinux: a repository of its own, then a clone
cd /tmp && rm -rf repo && mkdir repo && cd repo
git config --global user.name "Psion User" 2>/dev/null
git config --global user.email "psion@example.com"
git config --global init.defaultBranch main
git config --global core.pager cat
git init -q . && echo "hello" > a.txt && git add a.txt && git commit -q -m "first" && echo "ok   commit"
echo "more" >> a.txt && git diff --stat | grep -q "1 insertion" && echo "ok   diff"
git commit -qam "second" && [ "$(git log --oneline | wc -l)" = 2 ] && echo "ok   log"
git status --short | wc -l | grep -q "^0" && echo "ok   status"
cd /tmp && rm -rf hw && git clone -q https://github.com/octocat/Hello-World.git hw && [ -f hw/README ] && echo "ok   clone over https"
