# Третья правка звука Rockbox: вывод на Bluetooth-колонку.
# Если есть файл /run/a2dp.sink (адрес колонки), PCM идёт не в
# /dev/msm_pcm_out, а в именованный канал /run/a2dp.pcm, откуда его
# читает bta2dp и гонит на колонку. Темп задаёт читатель: канал
# заполняется, write() блокируется — тот же принцип, что у платы.
import io, sys
p = '/home/rockbox/firmware/target/hosted/sdl/pcm-sdl.c'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'a2dp.sink' in s:
    print('уже пропатчено'); sys.exit(0)
old = '''	msm_fd = open("/dev/msm_pcm_out", O_RDWR);'''
if old not in s:
    old = '''    msm_fd = open("/dev/msm_pcm_out", O_RDWR);'''
if old not in s:
    print('НЕ НАЙДЕНО открытие платы'); sys.exit(1)
new = '''    /* Колонка по Bluetooth: пока есть /run/a2dp.sink, звук уходит в
     * канал к bta2dp, а не в плату. Открытие блокируется, пока bta2dp
     * не откроет канал на чтение, — это и есть ожидание колонки. */
    if (access("/run/a2dp.sink", F_OK) == 0) {
        msm_fd = open("/run/a2dp.pcm", O_WRONLY);
        if (msm_fd >= 0) {
            msm_bufsize = 4096;
            free(msm_buf);
            msm_buf = malloc(msm_bufsize);
            if (!msm_run) {
                msm_run = 1;
                pthread_create(&msm_thread, NULL, msm_feeder, NULL);
            }
            fprintf(stderr, "МСМ: звук в канал bta2dp (колонка)%c", 10);
            return;
        }
    }
''' + old
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('пропатчено')
