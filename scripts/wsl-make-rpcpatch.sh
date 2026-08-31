#!/bin/bash
# Правим копию smd_rpcrouter.c и генерируем корректный патч утилитой diff.
set -eu
S=$HOME/leo-pmos/arch/arm/mach-msm/smd_rpcrouter.c
cp "$S" /tmp/a.c
cp "$S" /tmp/b.c
python3 - <<'PY'
src = open('/tmp/b.c').read()
anchor = "\txprt->priv = xprt_info;\n\n\treturn 0;\n}"
block = """\txprt->priv = xprt_info;

#if defined(CONFIG_MACH_HTCLEO)
\t/* WinCE boot (HaRET): the modem router still holds the session WinMo
\t * opened - our HELLO alone is ignored and no NEW_SERVER broadcasts
\t * ever arrive (pmic_rpc fails with -113). Replay the kick 2.6.32 did
\t * in rpcrouter_init(): BYE to reset, HELLO, then feed a local HELLO
\t * so this side marks the transport initialized. */
\tif (strcmp(xprt->name, "rpcrouter_loopback_xprt") &&
\t    !htcleo_is_nand_boot()) {
\t\tunion rr_control_msg kick;

\t\txprt_info->remote_pid = 0;
\t\tmemset(&kick, 0, sizeof(kick));
\t\tkick.cmd = RPCROUTER_CTRL_CMD_BYE;
\t\trpcrouter_send_control_msg(xprt_info, &kick);
\t\tmsleep(50);
\t\tkick.cmd = RPCROUTER_CTRL_CMD_HELLO;
\t\trpcrouter_send_control_msg(xprt_info, &kick);
\t\tmsleep(50);
\t\tprocess_control_msg(xprt_info, &kick, sizeof(kick));
\t\tmsleep(100);
\t}
#endif

\treturn 0;
}"""
assert anchor in src, "якорь не найден"
open('/tmp/b.c','w').write(src.replace(anchor, block, 1))
print("вставка ок")
PY
cd /tmp
diff -u a.c b.c | sed 's|^--- a.c.*|--- a/arch/arm/mach-msm/smd_rpcrouter.c|; s|^+++ b.c.*|+++ b/arch/arm/mach-msm/smd_rpcrouter.c|' > /mnt/c/Projects/HTC-HD2-T8585/out/04-rpcrouter-wince-kick.patch
head -5 /mnt/c/Projects/HTC-HD2-T8585/out/04-rpcrouter-wince-kick.patch
wc -l /mnt/c/Projects/HTC-HD2-T8585/out/04-rpcrouter-wince-kick.patch
# проверка применимости на чистом файле
cp "$S" /tmp/t.c
cd /tmp && patch --dry-run -p1 -d / -i /dev/null >/dev/null 2>&1 || true
mkdir -p /tmp/tt/arch/arm/mach-msm && cp "$S" /tmp/tt/arch/arm/mach-msm/
cd /tmp/tt && patch -p1 --dry-run < /mnt/c/Projects/HTC-HD2-T8585/out/04-rpcrouter-wince-kick.patch
