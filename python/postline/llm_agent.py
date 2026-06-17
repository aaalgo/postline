import json

from ._postline import Message, Service
from .accounting import updateAccounting


DEFAULT_PROMPT = []


class LLMAgent(Service):

    def __init__(self):
        super().__init__()
        self.journal = []
        self.models = None

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

        self.append(msg)

        outgoing = self.generate()

        if outgoing is None:
            outgoing = []

        for reply in outgoing:

            self.append(reply)

#            try:
#                updateAccounting(
#                    None,
#                    reply,
#                    self.provider,
#                )
#            except Exception:
#                pass

            resp.append(reply)
