#!/usr/bin/env python3
"""Compile actual driver routines with fake DMA/page/workqueue services.

No module is loaded and no device node is opened. The extracted functions are
compiled unchanged; mocks model their kernel dependencies, not the algorithm.
"""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
DRIVER = HERE.parents[1] / 'xdma'

def extract(path, begin, end):
    text = path.read_text()
    return text[text.index(begin):text.index(end, text.index(begin))]

with tempfile.TemporaryDirectory(prefix='xdma-tests-') as tmp:
    tmp = Path(tmp)
    code = extract(DRIVER / 'cdev_sgdma.c', '/* Pin every vector',
                   'static int ioctl_do_perf_start(')
    (tmp / 'sgdma_under_test.inc').write_text(code)
    code = extract(DRIVER / 'libxdma.c', 'static int transfer_init(',
                   '#ifdef __LIBXDMA_DEBUG__')
    code += extract(DRIVER / 'libxdma.c', 'static void transfer_destroy(',
                    'static int transfer_build(')
    (tmp / 'descriptors_under_test.inc').write_text(code)
    for test in ('test_sgdma', 'test_descriptors'):
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-g', '-O1',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-Wno-sign-compare',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(tmp), '-I', str(DRIVER),
                        str(HERE / (test + '.c')), '-o', str(tmp / test)],
                       check=True)
        subprocess.run([str(tmp / test)], check=True)
