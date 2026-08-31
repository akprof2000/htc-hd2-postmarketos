#!/bin/bash
set -eu
S=$HOME/leo-pmos/arch/arm/mach-msm/smd_rpcrouter.c
cp "$S" /tmp/a.c; cp "$S" /tmp/b.c
python3 - <<'PY'
src = open('/tmp/b.c').read()
anchor = "\txprt->priv = xprt_info;\n\n\treturn 0;\n}"
block = "\txprt->priv = xprt_info;\n\n" + \
"#if defined(CONFIG_MACH_HTCLEO)\n" + \
"\t/* WinCE boot (HaRET): reset the modem router session left by WinMo. */\n" + \
"\tif (strcmp(xprt->name, \"rpcrouter_loopback_xprt\") &&\n" + \
"\t    !htcleo_is_nand_boot()) {\n" + \
"\t\tunion rr_control_msg kick;\n\n" + \
"\t\tprintk(KERN_ERR \"[RRKICK] start on %s\", xprt->name);\n" + \
"\t\txprt_info->remote_pid = 0;\n" + \
"\t\tmemset(&kick, 0, sizeof(kick));\n" + \
"\t\tkick.cmd = RPCROUTER_CTRL_CMD_BYE;\n" + \
"\t\trpcrouter_send_control_msg(xprt_info, &kick);\n" + \
"\t\tmsleep(50);\n" + \
"\t\tkick.cmd = RPCROUTER_CTRL_CMD_HELLO;\n" + \
"\t\trpcrouter_send_control_msg(xprt_info, &kick);\n" + \
"\t\tmsleep(50);\n" + \
"\t\tprocess_control_msg(xprt_info, &kick, sizeof(kick));\n" + \
"\t\tmsleep(100);\n" + \
"\t\tprintk(KERN_ERR \"[RRKICK] done\");\n" + \
"\t}\n" + \
"#endif\n\n" + \
"\treturn 0;\n}"
assert anchor in src
open('/tmp/b.c','w').write(src.replace(anchor, block, 1))
print("вставка ок")
PY
cd /tmp
diff -u a.c b.c | sed 's|^--- a.c.*|--- a/arch/arm/mach-msm/smd_rpcrouter.c|; s|^+++ b.c.*|+++ b/arch/arm/mach-msm/smd_rpcrouter.c|' > /mnt/c/Projects/HTC-HD2-T8585/out/04-rpcrouter-wince-kick.patch
mkdir -p /tmp/tt/arch/arm/mach-msm && cp "$S" /tmp/tt/arch/arm/mach-msm/
cd /tmp/tt && patch -p1 --dry-run < /mnt/c/Projects/HTC-HD2-T8585/out/04-rpcrouter-wince-kick.patch
