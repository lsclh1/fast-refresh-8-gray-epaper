"""Build original FR7 test patterns or play a local video over USB serial."""

import argparse
import json
from pathlib import Path
import struct
import subprocess
from time import monotonic
import zlib

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT, SOURCES, FPS = 250, 122, 128, 25
MAX_BYTES = 6 * 1024 * 1024


def card():
    image = Image.new('L', (WIDTH, HEIGHT))
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default(size=14)
    for shade in range(8):
        row, column = divmod(shade, 4)
        left, top = 10 + column * 59, 4 + row * 60
        draw.rectangle((left, top, left + 52, top + 40), fill=shade, outline=7)
        draw.text((left + 26, top + 50), f'G{shade}', font=font, fill=7, anchor='mm')
    return image


def transition(source, target):
    for frame in range(30):
        image = Image.new('L', (WIDTH, HEIGHT))
        draw = ImageDraw.Draw(image)
        draw.rectangle((16, 24, 113, 92), fill=source if frame < 5 else target, outline=7)
        draw.rectangle((135, 24, 232, 92), fill=target, outline=7)
        draw.text((64, 109), f'G{source} to G{target}', fill=7, anchor='mm')
        draw.text((183, 109), f'G{target} reference', fill=7, anchor='mm')
        yield image


def motion():
    for frame in range(125):
        image = Image.new('L', (WIDTH, HEIGHT))
        draw = ImageDraw.Draw(image)
        for shade in range(8):
            left, top = 9 + shade * 30, 7
            draw.rectangle((left, top, left + 21, top + 18), fill=shade, outline=7)
        x = 12 + min(frame, 124 - frame) * 3
        for band in range(6):
            top = 38 + band * 12
            draw.rectangle((8, top, 241, top + 7), fill=band + 1)
            draw.rectangle((x, top, x + 34, top + 7), fill=7 - band)
        yield image


def video_frames(path):
    levels = json.loads((ROOT / 'data/gray-card-fr7.json').read_text(encoding='utf-8'))['levels']
    palette = [level['relative_1s'] for level in levels]
    lookup = [min(range(8), key=lambda shade: abs(palette[shade] - value * 100 / 255))
              for value in range(256)]
    command = ['ffmpeg', '-v', 'error', '-i', str(path), '-an', '-vf',
               'fps=25,scale=250:122:force_original_aspect_ratio=increase,crop=250:122,format=gray',
               '-f', 'rawvideo', '-']
    with subprocess.Popen(command, stdout=subprocess.PIPE) as decoder:
        while pixels := decoder.stdout.read(WIDTH * HEIGHT):
            if len(pixels) != WIDTH * HEIGHT:
                raise RuntimeError('FFmpeg returned a partial frame')
            yield Image.frombytes('L', (WIDTH, HEIGHT), pixels).point(lookup)
        if decoder.wait():
            raise RuntimeError('FFmpeg could not decode the input')


def pack(image):
    # SSD1680 RAM is gate-major, bottom to top along the visible source axis.
    native = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM).transpose(Image.Transpose.TRANSPOSE)
    padded = Image.new('L', (SOURCES, WIDTH))
    padded.paste(native, (0, 0))
    pixels = padded.tobytes()
    return bytes(a | (b << 4) for a, b in zip(pixels[::2], pixels[1::2]))


def encode(frames):
    records = bytearray()
    crc = count = 0
    previous = None
    for image in frames:
        packed = pack(image)
        data = zlib.compress(packed, 6, wbits=-15)
        is_delta = False
        if previous is not None:
            # Integer XOR avoids a Python loop over every pixel.
            delta = (int.from_bytes(packed, 'little') ^ int.from_bytes(previous, 'little')).to_bytes(len(packed), 'little')
            compressed = zlib.compress(delta, 6, wbits=-15)
            if len(compressed) < len(data):
                data, is_delta = compressed, True
        records.extend(struct.pack('<H', len(data) | (0x8000 if is_delta else 0)))
        records.extend(data)
        crc = zlib.crc32(packed, crc)
        previous = packed
        count += 1
        if len(records) + 16 > MAX_BYTES or count > 15000:
            raise ValueError('Clip is too large for the ESP32-S3 player. Try a shorter clip.')
    if not count:
        raise ValueError('No frames to play')
    return struct.pack('<4sIII', b'FRV7', count, FPS, crc) + records, count, crc


def response(port, prefix, timeout=6):
    until = monotonic() + timeout
    while monotonic() < until:
        line = port.readline().decode('ascii', errors='replace').strip()
        if line.startswith(('PV ERROR', 'PV REJECT', 'PV STOP')):
            raise RuntimeError(line)
        if line.startswith(prefix):
            return line
        if line.startswith('PV PROGRESS'):
            print(line, flush=True)
    raise TimeoutError(f'The board did not report {prefix}')


def play(device, asset, frames, crc):
    import serial

    port = serial.Serial(baudrate=460800, timeout=.25, write_timeout=4)
    port.port = device
    port.dtr = port.rts = False
    port.open()
    with port:
        port.write(b'HELLO\n')
        print(response(port, 'PV FR7-VIDEO READY'), flush=True)
        asset_crc = zlib.crc32(asset)
        port.write(f'LOAD {len(asset)} {asset_crc:08x}\n'.encode('ascii'))
        chunk = int(response(port, 'PV READY ').split('chunk=')[1])
        for size in range(0, len(asset), chunk):
            part = asset[size:size + chunk]
            if port.write(part) != len(part):
                raise RuntimeError('Incomplete serial write')
            if response(port, 'PV ACK ') != f'PV ACK bytes={size + len(part)}':
                raise RuntimeError('Unexpected upload byte count')
        loaded = response(port, 'PV LOADED ')
        if not loaded.endswith(f'crc={asset_crc:08x}'):
            raise RuntimeError('Board asset checksum differs')
        try:
            port.write(b'PLAY\n')
            print(response(port, 'PV START ', 10), flush=True)
            result = response(port, 'PV END ', frames / FPS + 20)
        except BaseException:
            port.write(b'!')
            raise
    values = dict(item.split('=', 1) for item in result.split()[2:])
    if int(values['submitted']) != frames or int(values['crc'], 16) != crc:
        raise RuntimeError(f'Playback ended with a frame or CRC mismatch: {result}')
    print(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pattern', choices=('card', 'transition', 'motion', 'video'))
    parser.add_argument('--input', type=Path, help='local source for the video mode')
    parser.add_argument('--from', dest='source', type=int, choices=range(8), default=0)
    parser.add_argument('--to', dest='target', type=int, choices=range(8), default=7)
    parser.add_argument('--port', help='USB serial port, such as COM10 or /dev/ttyACM0')
    parser.add_argument('--output', type=Path, help='also save a generated .frv file')
    args = parser.parse_args()
    if args.pattern == 'video' and not args.input:
        parser.error('video requires --input')
    patterns = {'card': lambda: (card(),), 'transition': lambda: transition(args.source, args.target),
                'motion': motion, 'video': lambda: video_frames(args.input)}
    asset, frames, crc = encode(patterns[args.pattern]())
    print(f'{frames} frame(s) at {FPS} FPS, {len(asset)} bytes, frame CRC {crc:08x}', flush=True)
    if args.output:
        args.output.write_bytes(asset)
    if args.port:
        play(args.port, asset, frames, crc)


if __name__ == '__main__':
    main()
