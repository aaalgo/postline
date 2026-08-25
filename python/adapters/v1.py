#!/usr/bin/env python3

import argparse
import json
import os

from openai import OpenAI

from postline import LLMAgent, Message, updateAccounting
from postline.shell import generate_shell_instruction


DEFAULT_MODEL = "gpt-5.5"
PROVIDER = "openai"
DEFAULT_CONTEXT_WINDOW = int(os.environ.get("POSTLINE_CONTEXT_WINDOW", "200000"))


SEND_MESSAGE_TOOL = {
    "type": "function",
    "function": {
        "name": "send_message",
        "description": "Send a message.",
        "parameters": {
            "type": "object",
            "properties": {
                "To": {
                    "type": "string",
                    "description": "Destination address"
                },
                "Subject": {
                    "type": "string",
                    "description": "Message subject"
                },
                "Content": {
                    "type": "string",
                    "description": "Message body"
                }
            },
            "required": [
                "To",
                "Subject",
                "Content"
            ],
            "additionalProperties": False
        }
    }
}


SYSTEM_PROMPT = """
You should always respond by calling the send_message tool to send the responding message.
"""

PROMPT = [
"""From: user
To: ai
Subject: shell tutorial

You can run shell commands by sending a message with `To: shell`, and set
`Subject: the command to run`.  Try run the fortune command for me.
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

If the email body is not empty, it will be fed to the command as stdin.  Try
save the fortune you got into ./fortune.txt by running
`Subject: cat > fortune.txt` and supply the text as email body.
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
"""From: user
To: ai
Subject: shell tutorial 3

In addition to standard binaries that are typically available on Linux, some additional Postline-specific tools are available.  These tools comes as shell commands, and when you run each with --doc usage document will be displayed.  The following message enumerates the Postline tools available.
""",
]

FORUM_PROMPT = [
"""From: user
To: ai
Subject: forum (themitbbs) tutorial

You can access the forum newmitbbs by sending message To: themitbbs
and put command in Subject:,

- To read the topic list,   Subject: list
- To read a topic,          Subject: read topic_id
- To reply a post,          Subject: reply topic_id
        with reply content in message body
- To create a post,         Subject: post title
        with content in message body
"""]


class Agent(LLMAgent):

    def __init__(self, model):
        super().__init__()
        self.model = model
        client_args = {
            "base_url": os.environ.get("V1_BASE_URL", "https://api.openai.com/v1"),
        }
        if os.environ.get("V1_API_KEY"):
            client_args["api_key"] = os.environ["V1_API_KEY"]
        self.client = OpenAI(**client_args)

    def list_models(self):

        ret = []

        try:
            for model in self.client.models.list().data:
                ret.append(model.id)
        except Exception:
            pass

        return ret

    def build_prompt(self):
        messages = [{
            "role": "system",
            "content": SYSTEM_PROMPT,
        }]

        for msg in PROMPT + generate_shell_instruction() + FORUM_PROMPT:
            if msg.startswith('From: ai\n'):
                role = 'assistant'
            else:
                role = 'user'
            messages.append({
                "role": role,
                "content": msg,
            })


        for msg in self.journal:

            if msg.get("type") == "agent:data":
                continue

            role = (
                "user"
                if msg.isReceiving()
                else "assistant"
            )

            messages.append({
                "role": role,
                "content": msg.format(True),
            })

        return messages

    def build_message(self, args):

        header = {
            "To": args["To"],
            "Subject": args["Subject"],
        }

        return Message(
            header,
            args.get("Content", ""),
        )

    def generate(self):

        response = self.client.chat.completions.create(
            model=self.model,
            messages=self.build_prompt(),
            tools=[SEND_MESSAGE_TOOL],
            tool_choice="required",
        )

        assistant = response.choices[0].message

        ret = []

        for tc in assistant.tool_calls or []:

            if tc.function.name != "send_message":
                continue

            args = json.loads(
                tc.function.arguments
            )

            msg = self.build_message(args)
            updateAccounting(
                response,
                msg,
                PROVIDER,
                DEFAULT_CONTEXT_WINDOW,
            )
            ret.append(msg)

        return ret


def parse_args():

    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--model",
        default=DEFAULT_MODEL,
    )

    parser.add_argument(
        "--test",
        action="store_true",
        help="Run a single test inference and exit.",
    )

    return parser.parse_args()


if __name__ == "__main__":

    args = parse_args()

    agent = Agent(
        model=args.model,
    )

    if args.test:
        agent._append(
            Message(
                {
                    "From": "user",
                    "To": "ai",
                    "Subject": "test",
                },
                "Reply with exactly: LiteLLM test passed",
            )
        )

        for msg in agent.generate():
            print(msg.format(True))

        raise SystemExit(0)

    agent.run()
