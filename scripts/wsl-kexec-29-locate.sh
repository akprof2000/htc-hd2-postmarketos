#!/bin/bash
F=$HOME/kexec-tools-2.0.20/kexec/arch/arm/kexec-zImage-arm.c
echo "=== вызовы locate_hole / add_buffer ==="
grep -n 'locate_hole\|add_buffer\|kernel_mem_size\|mem_min\|mem_max\|base =' "$F" | head -25
echo "=== порядок в my_load ==="
grep -n 'get_memory_ranges\|file_type\[i\].load\|probe' $HOME/kexec-tools-2.0.20/kexec/kexec.c | head -12
