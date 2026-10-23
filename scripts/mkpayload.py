#!/usr/bin/env python3
"""Build the XNUP payload for booting the XNU kernel on device."""
import argparse
import struct
import sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--physbase', default='')
    ap.add_argument('--usb', action='store_true')
    ap.add_argument('--rdres', default='')
    ap.add_argument('--checkpoint', default='')
    ap.add_argument('items', nargs='*')
    args = ap.parse_args()

    body = b''
    def add(name, data):
        nonlocal body
        body += struct.pack('<16sII', name.encode(), len(data), 0) + data + b'\0' * (-len(data) % 16)

    for item in args.items:
        name, path = item.split('=', 1)
        with open(path, 'rb') as f:
            add(name, f.read())
    if args.cmdline:
        add('cmdline', args.cmdline.encode() + b'\0')
    if args.physbase:
        add('physbase', args.physbase.lower().replace('0x', '').encode() + b'\0')
    if args.rdres:
        add('rdres', args.rdres.lower().replace('0x', '').encode() + b'\0')
    if args.usb:
        add('usb', b'1\0')
    if args.checkpoint:
        add('checkpoint', args.checkpoint.encode() + b'\0')
    body += b'\0' * 24

    with open(args.output, 'wb') as f:
        f.write(struct.pack('<II', 0x50554E58, len(body)) + body)
    print(f'{args.output}: {len(body) + 8} bytes')

if __name__ == '__main__':
    main()
