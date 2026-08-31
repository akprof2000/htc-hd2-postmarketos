# Что пришлось починить в ядре, чтобы pmOS пошла под HaRET

Порт `htc-leo` в postmarketOS тестировался только с загрузчиком cLK. При запуске
того же ядра через HaRET (из-под Windows Mobile) всплыли два дефекта. Оба найдены
по логам с аппарата, ни один не описан ни в вики, ни в исходниках.

## Как вообще удалось увидеть логи

Экран молчит: в ядре `htc-leo` **не собрана консоль на фреймбуфере**
(`# CONFIG_FRAMEBUFFER_CONSOLE is not set`, зато `CONFIG_DUMMY_CONSOLE=y`).
Поэтому чёрный экран ничего не означает — его видно и при успешной загрузке.

Спасает `CONFIG_ANDROID_RAM_CONSOLE_EARLY_INIT=y`: ядро пишет лог в фиксированные
256 КБ физической памяти по адресу `0x2FFC0000`, и эта область переживает
перезагрузку. Схема снятия лога:

1. загрузить pmOS через `\PMOS\haret.exe` (зависнет);
2. **перезагрузить, не обесточивая** — иначе буфер в ОЗУ теряется;
3. загрузить Android (`\ICS\haret.exe`) — у него тот же адрес ram_console;
4. `adb shell "su -c 'cat /proc/last_kmsg'"` — нужен root, без него файл читается как 0 байт.

`X-plore` для этого не годится: файлы в `/proc` сообщают нулевой размер, и он
копирует пустышку.

## Дефект 1 — зависание в con_init()

Лог обрывался на `Console: colour dummy device 80x30`. Дальше в `con_init()`
остаются только `console_unlock()` и `register_console(&vt_console_driver)`.
На рабочем ядре 2.6.32 следом печатается `console [tty0] enabled` — у нас нет.

Обход: `# CONFIG_VT_CONSOLE is not set`. Вывод при этом не теряется, он идёт в
`ram_console`. Возможно, это было следствием дефекта 2 (шторм прерываний) —
стоит перепроверить после того, как система заведётся.

## Дефект 2 — шторм Bad IRQ65535

После отключения VT-консоли ядро прошло дальше и утонуло в
`Bad IRQ65535` (миллионы вызовов). Причина в `entry-macro-vic.S`:

    ldr	\irqnr, [\base, #0xD0]
    ldr	\irqnr, [\base, #0xD4]
    cmp	\irqnr, #0xffffffff

Контроллер прерываний HD2 при входе из Windows Mobile возвращает `0x0000FFFF`,
а не `0xFFFFFFFF`, когда прерываний нет. Проверка не срабатывает, `0xFFFF`
принимается за номер прерывания.

В рабочем ядре [tytung/android_kernel_htcleo-2.6.32](https://github.com/tytung/android_kernel_htcleo-2.6.32)
ровно здесь стоят три дополнительные инструкции. В дерево `qsd8k-legacy`,
на котором построен порт pmOS, они не попали.

Патч: `02-vic-spurious-irq-0xffff.patch`, добавлен в `source=` пакета
`linux-htc-leo`.

## Результат

С обеими правками ядро проходит инициализацию целиком:

    panel type is 10 : board id is 4 : SHARP
    msmfb_probe() installing 480 x 800 panel     <- «зебра» на экране, признак успеха
    input: htcleo-touchscreen
    Device WiFi MAC Address: macaddr=...
    HTC Battery Probe done
    mmc1: Platform slot type: SD

и доходит до монтирования корня. Без initrd и без раздела `pmOS_root` это
заканчивается штатной паникой `No filesystem could mount root` — то есть ядро
исправно.

## Что ещё не решено

С подключённым initrd загрузка обрывается рано, на
`NET: Registered protocol family 16`. initrd при этом грузится корректно
(зарезервированной памяти становится ровно на его размер больше). Одновременно
был убран `initcall_debug`, так что причина пока не доказана — следующий шаг
вернуть его и увидеть точное имя вызова. Если подтвердится, что мешает initrd,
пробовать `initrd_offset 0x01800000` и выше.

Канонический адрес из `arch/arm/mach-msm/Makefile.boot` для HTCLEO:

    zreladdr    = 0x11808000   = ramaddr + 0x8000
    params_phys = 0x11800100   = ramaddr + 0x100
    initrd_phys = 0x12800000   = ramaddr + 0x01000000

## Полезные параметры загрузки

- `lpj=499435` — пропуск калибровки задержки. Значение подтвердилось: ядро
  напечатало `998.87 BogoMIPS`. Считается как BogoMIPS x 500000 / HZ, у нас HZ=1000.
- `ignore_loglevel` — печатать всё.
- `initcall_debug` — печатать каждый вызов инициализации, незаменим для поиска места отказа.
- `PMOS_NO_OUTPUT_REDIRECT` — initramfs pmOS пишет на консоль, а не в лог-файл.
