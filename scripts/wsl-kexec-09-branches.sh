#!/bin/bash
echo "=== ветки ==="
timeout 60 curl -sS "https://api.github.com/repos/tytung/android_kernel_htcleo-2.6.32/branches?per_page=100" \
 | grep -o '"name": "[^"]*"' | sed 's/"name": //' | tr '\n' ' '
echo ""
echo "=== теги ==="
timeout 60 curl -sS "https://api.github.com/repos/tytung/android_kernel_htcleo-2.6.32/tags?per_page=100" \
 | grep -o '"name": "[^"]*"' | sed 's/"name": //' | tr '\n' ' '
echo ""
echo "=== текущая ветка клона ==="
cd $HOME/leo-ics && git branch -a 2>/dev/null | head -5 && git log --oneline -3
