#!/bin/bash
set -eu
S=$HOME/leo-pmos/arch/arm/mach-msm/Makefile
cp "$S" /tmp/m_a; grep -v 'msm_kexec' "$S" > /tmp/m_b
cd /tmp
diff -u m_a m_b | sed 's|^--- m_a.*|--- a/arch/arm/mach-msm/Makefile|; s|^+++ m_b.*|+++ b/arch/arm/mach-msm/Makefile|' > /mnt/c/Projects/HTC-HD2-T8585/out/05-drop-missing-msm_kexec.patch
cat /mnt/c/Projects/HTC-HD2-T8585/out/05-drop-missing-msm_kexec.patch
cp /mnt/c/Projects/HTC-HD2-T8585/out/05-drop-missing-msm_kexec.patch $HOME/pmaports/device/archived/linux-htc-leo/
