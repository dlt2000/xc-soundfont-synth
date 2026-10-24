# SPDX-License-Identifier: MIT
"""用确定性平台替身编译真实组件，生成自有 SF2 波形并验证解析/输出/API 约定。"""
import argparse
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def chunk(tag, payload):
    return tag + struct.pack('<I', len(payload)) + payload + (b'\0' if len(payload) & 1 else b'')


def group(tag, *children):
    return chunk(b'LIST', tag + b''.join(children))


def make_fixture(path):
    """生成一个 key=60..67、两个 preset 的最小循环正弦音色，不含外部采样。"""
    samples = [int(12000 * math.sin(2 * math.pi * i / 256)) for i in range(256)] + [0] * 46
    phdr = b''.join(struct.pack('<20sHHHIII', name, preset, bank, bag, 0, 0, 0)
                    for name, preset, bank, bag in [(b'Test', 0, 0, 0), (b'Alternate', 3, 2, 1), (b'EOP', 0, 0, 2)])
    pbag = b''.join(struct.pack('<HH', i, 0) for i in range(3))
    pgen = struct.pack('<HHHHHH', 41, 0, 41, 0, 0, 0)
    inst = struct.pack('<20sH20sH', b'Sine', 0, b'EOI', 1)
    ibag = struct.pack('<HHHH', 0, 0, 4, 0)
    igen = b''.join(struct.pack('<HH', op, amount) for op, amount in [(43, 60 | (67 << 8)), (54, 1), (58, 69), (53, 0), (0, 0)])
    shdr = struct.pack('<20sIIIIIBbHH', b'Sine', 0, 256, 0, 256, 44100, 69, 0, 0, 1)
    shdr += struct.pack('<20sIIIIIBbHH', b'EOS', 0, 0, 0, 0, 0, 0, 0, 0, 1)
    payload = b'sfbk' + group(b'INFO', chunk(b'ifil', struct.pack('<HH', 2, 1)), chunk(b'INAM', b'XC test\0'))
    payload += group(b'sdta', chunk(b'smpl', struct.pack('<' + 'h' * len(samples), *samples)))
    payload += group(b'pdta', chunk(b'phdr', phdr), chunk(b'pbag', pbag), chunk(b'pmod', bytes(10)),
                     chunk(b'pgen', pgen), chunk(b'inst', inst), chunk(b'ibag', ibag),
                     chunk(b'imod', bytes(10)), chunk(b'igen', igen), chunk(b'shdr', shdr))
    path.write_bytes(chunk(b'RIFF', payload))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang'), help='主机 clang/gcc 路径，需要本机 C 工具链与链接库')
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if not compiler:
        parser.error('找不到主机编译器，请通过 --cc 指定 clang/gcc')
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='xc-sf-test-') as directory:
        work = Path(directory)
        fixture, bad = work / 'test.sf2', work / 'bad.sf2'
        make_fixture(fixture)
        bad.write_bytes(b'not a soundfont')
        shim = work / 'stubs'
        for name in ['esp_heap_caps.h', 'esp_log.h', 'esp_timer.h', 'freertos/FreeRTOS.h',
                     'freertos/queue.h', 'freertos/semphr.h', 'freertos/task.h']:
            target = shim / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('#include "platform.h"\n', encoding='utf-8')
        executable = work / ('test_synth.exe' if os.name == 'nt' else 'test_synth')
        command = [compiler, '-std=c11', '-D_CRT_SECURE_NO_WARNINGS', '-I' + str(shim),
                   '-I' + str(root / 'tests/host'), '-I' + str(root / 'include'),
                   str(root / 'tests/host/test_synth.c'), '-o', str(executable)]
        if os.name != 'nt':
            command.append('-lm')
        subprocess.run(command, check=True)
        subprocess.run([str(executable), str(fixture), str(bad)], check=True)


if __name__ == '__main__':
    main()
