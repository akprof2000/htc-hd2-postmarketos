#!/bin/sh
set -eu
export PATH="$HOME/.local/bin:$PATH"
A="$HOME/pmaports/device/archived/linux-htc-leo"
F="arch/arm/mach-msm/include/mach/entry-macro-vic.S"
w=$(mktemp -d); mkdir -p "$w/a/$(dirname $F)" "$w/b/$(dirname $F)"
curl -s "https://raw.githubusercontent.com/qsd8k-legacy/android_kernel_htc_htcleo/fb2ba086ea96647f38539664ebf0aa6eca61d7bb/$F" -o "$w/a/$F"
cp "$w/a/$F" "$w/b/$F"
# добавляем проверку 0xFFFF, как в рабочем ядре 2.6.32 tytung
python3 - "$w/b/$F" <<'PY'
import sys,io
p=sys.argv[1]
s=io.open(p,encoding='utf-8',newline='').read()
old="\tcmp\t\irqnr, #0xffffffff\n\t.endm"
new="\tcmp\t\irqnr, #0xffffffff\n\tmovne\t\tmp, #0xff\n\torrne\t\tmp, #0xff00\n\tcmpne\t\irqnr, \tmp\n\t.endm"
assert old in s, "шаблон не найден"
io.open(p,'w',encoding='utf-8',newline='').write(s.replace(old,new))
print("правка внесена")
PY
cd "$w" && diff -u "a/$F" "b/$F" > "$A/02-vic-spurious-irq-0xffff.patch" || true
cat "$A/02-vic-spurious-irq-0xffff.patch"
