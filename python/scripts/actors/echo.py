#!/usr/bin/env python3

import sys
from postline import Message


def main():
    f_in = sys.stdin.buffer
    f_out = sys.stdout.buffer

    msg = Message({
        'type': 'actor:hello',
        'spawn_type': 0,
        'history_mode': 0,
        })
    msg.write(f_out)
    f_out.flush()

    while True:
        try:
            msg = Message.read(f_in)
        except EOFError:
            break

        if msg.get('type') == 'actor:bye':
            break

        header = msg.header()
        header['From'], header['To'] = header['To'], header['From']
        msg.write(f_out)
        f_out.flush()


if __name__ == "__main__":
    main()
