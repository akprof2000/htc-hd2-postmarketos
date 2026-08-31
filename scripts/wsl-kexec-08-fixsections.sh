#!/bin/bash
# Современный ассемблер не принимает старую запись флагов секции:
#   .section "имя", #alloc, #execinstr   ->   .section "имя", "ax"
#   .section "имя", #alloc               ->   .section "имя", "a"
# В 2012 году это было нормой; сейчас '#' трактуется как начало комментария.
set -u
S=$HOME/leo-ics
N=$HOME/.local/var/pmbootstrap/chroot_native/leo-ics

echo "=== где встречается ==="
grep -rln '\.section.*#alloc' "$S" --include=*.S --include=*.h 2>/dev/null | sed "s|$S/||" | head -20
echo "--- всего файлов: $(grep -rln '\.section.*#alloc' "$S" --include=*.S --include=*.h 2>/dev/null | wc -l) ---"

for T in "$S" "$N"; do
	sudo find "$T" \( -name '*.S' -o -name '*.h' \) -print0 2>/dev/null \
	| sudo xargs -0 sed -i \
		-e 's/\(\.section[^,]*\), *#alloc, *#execinstr/\1, "ax"/g' \
		-e 's/\(\.section[^,]*\), *#alloc, *#write/\1, "aw"/g' \
		-e 's/\(\.section[^,]*\), *#alloc/\1, "a"/g'
done
echo "=== осталось после правки: $(grep -rc '\.section.*#alloc' "$S" --include=*.S 2>/dev/null | grep -v ':0' | wc -l) ==="
grep -n '\.section ".proc.info.init"' "$S/arch/arm/mm/proc-v7.S"
