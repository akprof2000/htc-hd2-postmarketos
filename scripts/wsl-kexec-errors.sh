#!/bin/bash
grep -nE '\berror:|Error 1' $HOME/.local/var/pmbootstrap/log.txt 2>/dev/null | tail -25
