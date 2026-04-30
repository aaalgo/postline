#!/usr/bin/env python3

import sys
from postline import Message


def main():

    fd_in = sys.stdin.fileno()
    fd_out = sys.stdout.fileno()

    msg = Message()
    msg.set('type', 'agent:hello')
    msg.set('spawn_type', 0)
    msg.set('history_mode', 0)
    msg.write(fd_out)

    while True:
        try:
            msg = Message.read(fd_in)
        except EOFError:
            break

        if msg.get('type') == 'agent:bye':
            break

        From = msg.get("From")
        To = msg.get("To")
        msg.set("From", To)
        msg.set("To", From)
        msg.write(fd_out)


if __name__ == "__main__":
    main()
