#!/bin/sh
# what the terminal shows in the x11 screenshot: real commands, run here
cd /c/Documents
P='~/Documents $'
echo "$P ls"; ls
echo "$P cat Shopping.txt"; cat Shopping.txt
echo "$P"
exec sleep 600
