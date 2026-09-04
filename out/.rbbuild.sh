#!/bin/sh
# Сборка Rockbox. Идёт под эмуляцией armv7, поэтому долго; вывод пишем
# в файл, чтобы следить со стороны.
cd /home/rockbox/build || exit 1
echo "начало: $(date +%H:%M:%S)" > /home/rbbuild.log
make -j4 >> /home/rbbuild.log 2>&1
echo "код возврата: $?" >> /home/rbbuild.log
echo "конец: $(date +%H:%M:%S)" >> /home/rbbuild.log
ls -la /home/rockbox/build/rockbox >> /home/rbbuild.log 2>&1
