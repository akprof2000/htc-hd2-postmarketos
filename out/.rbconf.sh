#!/bin/sh
# Настройка сборки Rockbox «как приложение» под наш экран 480x800.
# Цель 200 (sdlapp) спрашивает размер экрана и тип сборки — отвечаем
# заранее, чтобы не сидеть в диалоге.
which sdl-config >/dev/null 2>&1 && echo "sdl-config: $(sdl-config --version)" \
    || echo "sdl-config НЕ НАЙДЕН"
mkdir -p /home/rockbox/build
cd /home/rockbox/build || exit 1
printf '200\n480\n800\nN\n' | ../tools/configure 2>&1 | tail -30
echo "=== получился Makefile:"
ls -la Makefile 2>/dev/null || echo "Makefile НЕ создан"
