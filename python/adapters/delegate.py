#!/usr/bin/env python3

from postline import Message, Service


def message_body(msg):
    _, separator, body = msg.format(True).partition("\n\n")
    assert separator
    return body


class Delegate(Service):

    def __init__(self):
        super().__init__()
        self.peer_addresses: list[str] = []

    def on_call(self, msg, resp):
        from_address = msg.get("From")
        body = message_body(msg)

        if self.peer_addresses and from_address == self.peer_addresses[-1]:
            self.peer_addresses.pop()
            resp.append(Message({
                "Subject": msg.get("Subject"),
            }, body))
            return

        to_address, separator, subject = msg.get("Subject").partition(":")
        assert separator
        assert to_address

        self.peer_addresses.append(to_address)
        resp.append(Message({
            "To": to_address,
            "Subject": subject,
        }, body))


def main():
    Delegate().run()


if __name__ == "__main__":
    main()
