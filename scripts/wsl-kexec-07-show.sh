#!/bin/bash
sed -n '325,345p' $HOME/leo-ics/arch/arm/mm/proc-v7.S | cat -n | sed 's/^/  /'
