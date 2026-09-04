# Вторая правка звука: SDL-подсистема звука на этом телефоне не
# поднимается (драйверов нет), и Rockbox падал в panicf ещё до того, как
# доходил до вывода. Раз играем мы сами через /dev/msm_pcm_out, неудача
# SDL нам безразлична — просто идём дальше и сразу открываем плату.
import io, sys

p = '/home/rockbox/firmware/target/hosted/sdl/pcm-sdl.c'
s = io.open(p, encoding='utf-8', errors='replace').read()

if 'msm_pcm_out' not in s:
    print('НЕТ первой правки — сначала .rbaudio.py')
    sys.exit(1)
if 'SDL audio unavailable' in s:
    print('уже пропатчено')
    sys.exit(0)

old = '''    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        panicf("Could not initialize SDL audio subsystem!");
        return;
    }
'''
new = '''    /* Звуковой подсистемы SDL здесь нет и быть не может: ни ALSA-карт,
     * ни OSS. Раньше это заканчивалось panicf, и звук не заводился
     * вовсе. Играем мы сами (msm_open_device ниже), так что неудачу
     * просто отмечаем и работаем дальше. */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        DEBUGF("SDL audio unavailable, playing through msm_pcm_out\n");
'''
if old not in s:
    print('НЕ НАЙДЕН блок инициализации')
    sys.exit(1)
s = s.replace(old, new)

# плату открываем сразу при запуске: размер буфера нужен ещё до
# первого set_freq
s = s.replace('''    audio_lock = SDL_CreateMutex();''',
'''    audio_lock = SDL_CreateMutex();
    msm_open_device();''')

io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('пропатчено')
