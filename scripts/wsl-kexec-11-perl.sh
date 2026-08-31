#!/bin/bash
# kernel/timeconst.pl использует defined(@array) - синтаксис, удалённый в Perl 5.22.
# Замена на проверку самого массива даёт то же поведение.
set -u
N=$HOME/.local/var/pmbootstrap/chroot_native
echo "=== было ==="
grep -n 'defined(@' $HOME/leo-ics/kernel/timeconst.pl
for T in $HOME/leo-ics $N/leo-ics; do
	sudo sed -i 's/if (!defined(@val)) {/if (!@val) {/; s/defined(@\([a-zA-Z_]*\))/@\1/g' "$T/kernel/timeconst.pl"
done
echo "=== стало ==="
grep -n 'if (!@val)\|@val)' $HOME/leo-ics/kernel/timeconst.pl | head -5
echo "=== проверка синтаксиса ==="
perl -c $HOME/leo-ics/kernel/timeconst.pl 2>&1 | tail -2
