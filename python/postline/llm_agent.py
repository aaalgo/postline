import pickle
import json
import os
import sys

from ._postline import Message, Service
from .accounting import updateAccounting


DEFAULT_PROMPT = [
"""From: user
To: ai
Subject: Hello

On this platform we communicate with emails.  Try respond in email format.
""",
"""From: ai
To: user
Subject: Re: Hello

Understood.  We will use email to communicate.
""",
"""From: user
To: ai
Subject: shell tutorial

You can run shell commands by sending a message with `To: shell`, and set `Subject: the command to run`.  Try run the fortune command for me.
""",
"""From: ai
To: shell
Subject: fortune

""",
"""From: shell
To: ai
Subject: exit status 0

--stdout
Life is to you a dashing and bold adventure.
""",
"""From: ai
To: user
Subject: Re: shell tutorial

I successfully ran the fortune command. Here's what it returned:

"Life is to you a dashing and bold adventure."

""",
"""From: user
To: ai
Subject: shell tutorial 2

If the email body is not empty, it will be fed to the command as stdin.  Try save the fortune you got into ./fortune.txt by running `Subject: cat > fortune.txt` and supply the text as email body.
""",
"""From: ai
To: shell
Subject: cat > fortune.txt

Life is to you a dashing and bold adventure.
""",
"""From: shell
To: ai
Subject: exit status 0

""",
"""From: ai
To: user
Subject: Re: shell tutorial 2

The file is created.
"""
]


class LLMAgent (Service):

    def __init__ (self, client, model, provider, prompt=DEFAULT_PROMPT):
        super().__init__()
        self.client = client
        self.model = model
        self.provider = provider
        self.models = []
        self.model_ids = set()
        self.history = []
        self.ai_debug_file = open(self.ai_debug_filename(), "wb")

        self.load_models()
        for txt in prompt:
            self.append(Message.parse(txt))

    def ai_debug_filename(self):
        program = os.path.splitext(os.path.basename(sys.argv[0]))[0]
        if not program:
            program = "python"
        return f"ai_{program}_{os.getpid()}.pkl"

    def dump_ai_debug(self, step, obj):
        pickle.dump((step, obj), self.ai_debug_file)
        self.ai_debug_file.flush()

    def object_to_dict(self, obj):
        if hasattr(obj, "to_dict"):
            return obj.to_dict()
        if hasattr(obj, "model_dump"):
            return obj.model_dump()
        if isinstance(obj, dict):
            return obj
        return dict(obj)

    def load_models(self):
        for model in self.client.models.list():
            model_id = getattr(model, "id", None)
            model_dict = self.object_to_dict(model)
            if model_id is None:
                model_id = model_dict.get("id")
            if model_id is not None:
                self.model_ids.add(model_id)
            self.models.append(model_dict)

    def append(self, msg):
        if msg.isReceiving():
            role = "user"
        else:
            role = "assistant"

        self.history.append({
            "role": role,
            "content": msg.format(True),
        })

    def before_generate(self):
        pass

    def create_response(self):
        raise NotImplementedError()

    def extract_message_text(self, response):
        raise NotImplementedError()

    def generate (self):
        self.before_generate()

        print(self.history, file=sys.stderr)
        sys.stderr.flush()

        self.dump_ai_debug(0, self.history)
        response = self.create_response()
        self.dump_ai_debug(1, response)
        text = self.extract_message_text(response)
        self.dump_ai_debug(2, text)


        #print("=" * 20, file=sys.stderr)
        #print(text, file=sys.stderr)
        #print("=" * 20, file=sys.stderr)
        #sys.stderr.flush()

        try:
            msg = Message.parse(text)
        except:
            msg = Message({"Subject": "error"}, text)
            pass
        updateAccounting(response, msg, self.provider)
        return msg

    def on_init (self, resp):
        pass

    def on_memory (self, msg):
        self.append(msg)

    def on_call (self, msg, resp):
        if msg.get('Subject') == '/list_models':
            resp.append(Message({}, json.dumps(self.models, indent=2)))
            return

        self.append(msg)
        model = msg.get("Model-Requested")
        if model in self.model_ids:
            self.model = model
        reply = self.generate()
        self.append(reply)
        resp.append(reply)
