#!/bin/bash
# Патч 09: под HaRET канал RPCCALL остаётся открытым от Windows Mobile,
# поэтому smd_open() не порождает SMD_EVENT_OPEN — маршрутизатор никогда не
# регистрирует транспорт, и весь RPC мёртв (динамик, BT, звук разговора).
# После открытия проверяем реальное состояние и досылаем событие сами.
set -eu
F=$HOME/leo-pmos/arch/arm/mach-msm/rpcrouter_smd_xprt.c
cp "$F" /tmp/x_a.c
python3 - <<'PY'
s=open('/tmp/x_a.c').read()
old="""	smd_disable_read_intr(smd_remote_xprt.channel);

	return 0;
}"""
new="""	smd_disable_read_intr(smd_remote_xprt.channel);

	/* Загрузка через HaRET: канал уже открыт Windows Mobile, перехода
	 * состояния нет, значит SMD_EVENT_OPEN не придёт никогда. Без него
	 * маршрутизатор не регистрирует транспорт и RPC не работает вовсе.
	 * Проверяем готовность канала и досылаем событие вручную. */
	if (smd_write_avail(smd_remote_xprt.channel) > 0) {
		pr_info("rpcrouter: канал уже открыт, досылаем OPEN");
		msm_rpcrouter_xprt_notify(&smd_remote_xprt.xprt,
					  RPCROUTER_XPRT_EVENT_OPEN);
	}

	return 0;
}"""
assert old in s, "anchor probe"
open('/tmp/x_b.c','w').write(s.replace(old,new,1))
print("вставка ок")
PY
cd /tmp
diff -u x_a.c x_b.c | sed 's|^--- x_a.c.*|--- a/arch/arm/mach-msm/rpcrouter_smd_xprt.c|; s|^+++ x_b.c.*|+++ b/arch/arm/mach-msm/rpcrouter_smd_xprt.c|' > /mnt/c/Projects/HTC-HD2-T8585/out/09-rpc-force-open-event.patch
mkdir -p /tmp/t9/arch/arm/mach-msm && cp "$F" /tmp/t9/arch/arm/mach-msm/
cd /tmp/t9 && patch -p1 --dry-run < /mnt/c/Projects/HTC-HD2-T8585/out/09-rpc-force-open-event.patch
