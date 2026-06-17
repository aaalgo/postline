import json
from dataclasses import dataclass,field
from typing import List
from ._postline import Message, Service
from .accounting import updateAccounting



DEFAULT_PROMPT = []

@dataclass
class Frame:
    return_to: str | None = None
    expect_from: str | None = None
    pending: List[Message] = field(default_factory=list)


class LLMAgent(Service):

    def __init__(self):
        super().__init__()
        self.journal = []
        self.models = None
        self.stack = [Frame()]

    def generate(self):
        """
        Generate outgoing Postline messages.

        Returns:
            List[Message]
        """
        raise NotImplementedError()

    def list_models(self):
        # return a list of str, each is the string ID of a model
        raise NotImplementedError()

    def append(self, msg):
        self.journal.append(msg)


    def on_memory(self, msg):
        self.append(msg)

    def on_call(self, msg, resp):
        if msg.get("Subject") == "/list_models":
            if self.models is None:
                self.models = self.list_models()
            body = json.dumps(sorted(self.models), indent=2)
            resp.append(Message({}, body))
            return

        msg.setReceiving()
        self.append(msg)

        From = msg.get("From")
        top = self.stack[-1]
        if From == top.expect_from:
            # we are waiting reply from From
            top.expect_from = None
        else:
            top = Frame(return_to = From)
            self.stack.append(top)

        if len(top.pending) == 0:
            top.pending = self.generate()
            assert len(top.pending) > 0

        # find non-returning message to send out
        for i in range(len(top.pending)):
            out = top.pending[i]
            To = out.get("To")
            if To != top.return_to:
                out = top.pending.pop(i)
                top.expect_from = To
                resp.append(out)
                self.append(out)
                return

        # stack rewind

        if len(top.pending) == 1:
            out = top.pending.pop(0)
        else:
            # there are multiple messages to To/ to.return_to
            body = "Warning: multiple returning messages are queued.\n\n"
            for i, msg in enumerate(top.pending):
                body += f"\n#{i}, Subject: {msg.get('Subject')}\n{msg.body()}\n\n"
            out = Message({"To": top.return_to}, body)

        self.stack.pop()
        resp.append(out)
        self.append(out)


