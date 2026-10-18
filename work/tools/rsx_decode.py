#!/usr/bin/env python3
"""Decode a big-endian RSX (NV4097) command buffer dump into methods.

    rsx_decode.py <file.bin> [--summary]

Headers: bits 18-28 count, bits 2-15 method (with the subchannel in 13-15),
bit 30 non-incrementing; 0x20000000 JUMP, bit 0/1 CALL, 0x00020000 RETURN.
--summary groups the stream into draws (BEGIN_END runs) and prints, per draw,
which state groups were set before it."""
import struct, sys
NAMES = {
    0x0050: 'SET_REFERENCE', 0x0064: 'SEMAPHORE_OFFSET', 0x0068: 'SEMAPHORE_ACQUIRE', 0x006C: 'SEMAPHORE_RELEASE',
    0x0180: 'SET_CONTEXT_DMA_NOTIFIES', 0x0208: 'SURFACE_FORMAT', 0x0210: 'SURFACE_COLOR_AOFFSET',
    0x0214: 'SURFACE_ZETA_OFFSET', 0x0218: 'SURFACE_COLOR_BOFFSET', 0x0220: 'SURFACE_COLOR_TARGET',
    0x0300: 'ALPHA_FUNC', 0x0304: 'ALPHA_TEST_ENABLE', 0x0310: 'BLEND_ENABLE', 0x0314: 'BLEND_FUNC_SFACTOR',
    0x0318: 'BLEND_FUNC_DFACTOR', 0x0320: 'BLEND_EQUATION', 0x0324: 'COLOR_MASK', 0x0328: 'STENCIL_TEST_ENABLE',
    0x0A6C: 'DEPTH_FUNC', 0x0A70: 'DEPTH_MASK', 0x0A74: 'DEPTH_TEST_ENABLE', 0x0A60: 'POLY_OFFSET_FILL',
    0x1830: 'CULL_FACE', 0x1834: 'FRONT_FACE', 0x183C: 'CULL_FACE_ENABLE',
    0x08E4: 'SHADER_PROGRAM', 0x1D60: 'SHADER_CONTROL', 0x1E9C: 'VP_UPLOAD_FROM_ID', 0x1EA0: 'VP_START_FROM_ID',
    0x1EFC: 'VP_UPLOAD_CONST_ID', 0x1FF0: 'VP_ATTRIB_EN', 0x1FF4: 'VP_RESULT_EN',
    0x1808: 'BEGIN_END', 0x1814: 'DRAW_ARRAYS', 0x1818: 'INLINE_ARRAY', 0x181C: 'INDEX_ARRAY_OFFSET',
    0x1820: 'INDEX_ARRAY_DMA', 0x1824: 'DRAW_INDEX_ARRAY', 0x1D6C: 'BACK_END_WRITE_SEMAPHORE_RELEASE',
    0x1D70: 'TEXTURE_READ_SEMAPHORE_RELEASE', 0x1738: 'VERTEX_DATA_BASE_OFFSET', 0x173C: 'VERTEX_DATA_BASE_INDEX',
    0x1710: 'INVALIDATE_VERTEX_CACHE', 0x1714: 'INVALIDATE_VERTEX_FILE',
}
def name(m):
    if m in NAMES: return NAMES[m]
    if 0x1680 <= m < 0x1700: return f'VERTEX_DATA_ARRAY_OFFSET[{(m-0x1680)//4}]'
    if 0x1740 <= m < 0x1780: return f'VERTEX_DATA_ARRAY_FORMAT[{(m-0x1740)//4}]'
    if 0x1A00 <= m < 0x1C00: return f'TEXTURE[{(m-0x1A00)//0x20}]+{(m-0x1A00)%0x20:#x}'
    if 0x0B40 <= m < 0x0B80: return f'TEX_CONTROL3[{(m-0x0B40)//4}]'
    if 0x1F00 <= m < 0x1F80: return f'VP_CONST[{(m-0x1F00)//4}]'
    if 0x0B80 <= m < 0x0C00: return f'VP_INST[{(m-0x0B80)//4}]'
    if 0x1840 <= m < 0x1880: return f'TEX_FILTER?{m:#x}'
    return f'm{m:#06x}'
def decode(data):
    w = struct.unpack('>%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    i = 0; out = []
    while i < len(w):
        h = w[i]
        if h == 0: out.append((i, 'NOP', [])); i += 1; continue
        if (h & 0xE0000003) == 0x20000000: out.append((i, f'JUMP {h & 0x1FFFFFFC:#x}', [])); i += 1; continue
        if (h & 3) == 2: out.append((i, f'CALL {h & ~3:#x}', [])); i += 1; continue
        if h == 0x00020000: out.append((i, 'RETURN', [])); i += 1; continue
        cnt = (h >> 18) & 0x7FF; m = h & 0xFFFC; ninc = (h >> 30) & 1
        args = list(w[i + 1:i + 1 + cnt])
        if ninc or cnt <= 1:
            out.append((i, name(m & 0x1FFC), args))
        else:
            out.append((i, name(m & 0x1FFC) + (f' (+{cnt})' if cnt > 1 else ''), args))
        i += 1 + cnt
    return out
if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    ops = decode(d)
    if '--summary' in sys.argv:
        state = []; draws = 0
        for i, n, a in ops:
            if n.startswith('BEGIN_END') and a and a[0]:
                print(f'draw {draws}: prim {a[0]} after ' + ', '.join(sorted(set(state))))
                state = []; draws += 1
            elif not n.startswith('BEGIN_END') and not n.startswith(('DRAW_', 'INLINE', 'NOP')):
                state.append(n.split(' ')[0].split('[')[0])
        print(f'{draws} draws, {len(ops)} commands')
    else:
        for i, n, a in ops:
            print(f'{i*4:05x} {n:40s} ' + ' '.join(f'{x:08x}' for x in a[:6]) + (' ...' if len(a) > 6 else ''))
