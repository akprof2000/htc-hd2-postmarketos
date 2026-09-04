# Замена звукового вывода Rockbox: вместо SDL пишем прямо в
# /dev/msm_pcm_out. На этом телефоне у SDL выводить звук некуда —
# ALSA-карт нет, OSS нет, поэтому он молчал бы.
#
# Правка точечная: сохраняем всю логику Rockbox (подготовку буфера
# делает его же обработчик sdl_audio_callback), меняем только приёмник —
# открытие устройства и поток, который кормит его буферами.
import io
import sys

p = '/home/akozlov/rockbox/firmware/target/hosted/sdl/pcm-sdl.c'
s = io.open(p, encoding='utf-8', errors='replace').read()

if 'msm_pcm_out' in s:
    print('уже пропатчено')
    sys.exit(0)

# 1. заголовки и состояние нашего вывода
s = s.replace('''static struct pcm_udata
{''', '''/* ── вывод звука на HTC HD2 ──────────────────────────────────────
 * У SDL на этом телефоне нет ни одного рабочего звукового драйвера
 * (ALSA-карт нет, OSS нет), поэтому играем сами: легаси-интерфейс MSM
 * принимает только целые буферы того размера, который сам назвал в
 * GET_CONFIG. Последовательность вызовов взята из проверенного
 * pcmplay.c.
 */
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>

#define MSM_AUDIO_START         _IOW('a', 0, unsigned)
#define MSM_AUDIO_STOP          _IOW('a', 1, unsigned)
#define MSM_AUDIO_GET_CONFIG    _IOR('a', 3, unsigned)
#define MSM_AUDIO_SET_CONFIG    _IOW('a', 4, unsigned)
#define MSM_AUDIO_SET_VOLUME    _IOW('a', 10, unsigned)
#define MSM_AUDIO_SWITCH_DEVICE _IOW('a', 32, unsigned)
#define MSM_DEV_SPKR_MONO      0x1081513u
#define MSM_DEV_HEADSET_STEREO 0x107ac8au

struct msm_audio_config {
    uint32_t buffer_size, buffer_count, channel_count, sample_rate,
             type, unused[3];
};

static int msm_fd = -1;
static unsigned char *msm_buf;
static size_t msm_bufsize;
static pthread_t msm_thread;
static volatile int msm_run, msm_playing;

static struct pcm_udata
{''')

# 2. вместо открытия звука через SDL — открываем нашу плату
old_open = '''    /* Open the audio device and start playing sound! */
#if SDL_MAJOR_VERSION > 1
    if((pcm_devid = SDL_OpenAudioDevice(audiodev, 0, &wanted_spec, &obtained, SDL_AUDIO_ALLOW_SAMPLES_CHANGE)) == 0) {
#else
    if(SDL_OpenAudio(&wanted_spec, &obtained) < 0) {
#endif
        panicf("Unable to open audio: %s", SDL_GetError());
        return;
    }
'''
new_open = '''    /* Открываем НАШУ плату вместо SDL. Частота у неё одна — 44100,
     * поэтому пересчёт из частоты дорожки делает тот же механизм
     * Rockbox (SDL_BuildAudioCVT ниже), он для этого и заведён. */
    (void)wanted_spec;
    msm_open_device();
    obtained.freq = 44100;
    obtained.format = AUDIO_S16SYS;
    obtained.channels = 2;
    obtained.silence = 0;
    obtained.samples = msm_bufsize / 4;
    obtained.size = msm_bufsize;
'''
if old_open not in s:
    print('НЕ НАЙДЕН блок открытия звука')
    sys.exit(1)
s = s.replace(old_open, new_open)

# 3. пуск и останов — через наш поток, а не через паузу SDL
s = s.replace('''    pcm_data = addr;
    pcm_data_size = size;

#if SDL_MAJOR_VERSION > 1
    SDL_PauseAudioDevice(pcm_devid, 0);
#else
    SDL_PauseAudio(0);
#endif
}''', '''    pcm_data = addr;
    pcm_data_size = size;
    msm_playing = 1;
}''')

s = s.replace('''#if SDL_MAJOR_VERSION > 1
    SDL_PauseAudioDevice(pcm_devid, 1);
#else
    SDL_PauseAudio(1);
#endif
''', '''    msm_playing = 0;
''')

# 4. сам поток и открытие устройства — перед первым использованием
s = s.replace('''static void sdl_audio_callback(void *handle, Uint8 *stream, int len);''',
'''static void sdl_audio_callback(void *handle, Uint8 *stream, int len);

/* поток подкачки: берёт у Rockbox готовые куски и отдаёт их плате */
static void *msm_feeder(void *arg)
{
    (void)arg;
    while (msm_run) {
        if (!msm_playing || msm_fd < 0) {
            usleep(20000);
            continue;
        }
        SDL_LockMutex(audio_lock);
        sdl_audio_callback(&udata, msm_buf, (int)msm_bufsize);
        SDL_UnlockMutex(audio_lock);
        if (write(msm_fd, msm_buf, msm_bufsize) < 0)
            usleep(10000);
    }
    return NULL;
}

static void msm_open_device(void)
{
    if (msm_fd >= 0)
        return;
    int ctl = open("/dev/msm_audio_ctl", O_RDWR);
    if (ctl >= 0) {
        /* наушники, если воткнуты, иначе громкий динамик */
        unsigned hp = 0;
        int f = open("/sys/class/switch/h2w/state", O_RDONLY);
        if (f >= 0) {
            char b[8] = {0};
            if (read(f, b, sizeof(b) - 1) > 0)
                hp = (b[0] != '0');
            close(f);
        }
        unsigned sw[2] = { hp ? MSM_DEV_HEADSET_STEREO : MSM_DEV_SPKR_MONO, 0 };
        ioctl(ctl, MSM_AUDIO_SWITCH_DEVICE, &sw);
        unsigned vol = 90;
        ioctl(ctl, MSM_AUDIO_SET_VOLUME, &vol);
        close(ctl);
    }
    msm_fd = open("/dev/msm_pcm_out", O_RDWR);
    if (msm_fd < 0)
        return;
    struct msm_audio_config c;
    if (ioctl(msm_fd, MSM_AUDIO_GET_CONFIG, &c) < 0) {
        close(msm_fd);
        msm_fd = -1;
        return;
    }
    c.channel_count = 2;
    c.sample_rate = 44100;
    ioctl(msm_fd, MSM_AUDIO_SET_CONFIG, &c);
    ioctl(msm_fd, MSM_AUDIO_START, 0);
    msm_bufsize = c.buffer_size;
    free(msm_buf);
    msm_buf = malloc(msm_bufsize);
    if (!msm_run) {
        msm_run = 1;
        pthread_create(&msm_thread, NULL, msm_feeder, NULL);
    }
}''')

io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('пропатчено')
