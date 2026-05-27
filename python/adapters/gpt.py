#!/usr/bin/env python3
import argparse
import importlib
import os
import sys
import openai

from postline import LLMAgent


DEFAULT_MODEL = "gpt-5.4-mini"
PROVIDER = "openai"
TOOL_NAME = "postline_send"
AI_ADDRESS = "ai"
USER_ADDRESS = "user"

PROMPT = [
"""From: user
To: ai
Subject: Hello

On this platform we communicate with emails.  You must send exactly one
Postline message by calling the postline_send custom tool.  The tool input is
the raw Postline email message.  Do not emit ordinary assistant text.
""",
"""From: ai
To: user
Subject: Re: Hello

Understood.  I will communicate by sending Postline messages through the
postline_send tool.
""",
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
]

TOOLS = [
    {
        "type": "custom",
        "name": TOOL_NAME,
        "description": (
            "Send one raw Postline email message. The input must include "
            "Postline headers such as From, To, and Subject, followed by a "
            "blank line and the message body."
        ),
        "format": {
            "type": "text",
        },
    }
]

TOOL_CHOICE = {
    "type": "custom",
    "name": TOOL_NAME,
}

class Agent (LLMAgent):

    def __init__ (self, model=DEFAULT_MODEL):
        self.next_call_number = 1
        self.pending_call = None
        self.generated_tool_call = None

        super().__init__(openai.OpenAI(), model, PROVIDER, PROMPT)
        self.close_pending_delivery()

    def next_call_id(self):
        call_id = f"postline_call_{self.next_call_number:06d}"
        self.next_call_number += 1
        return call_id

    def close_pending_delivery(self):
        if self.pending_call is None:
            return

        self.history.append({
            "type": "custom_tool_call_output",
            "call_id": self.pending_call["call_id"],
            "output": "delivered",
        })
        self.pending_call = None

    def append_incoming(self, msg, content):
        if self.pending_call is not None:
            expected_from = self.pending_call["to"]
            if msg.get("From") == expected_from:
                self.history.append({
                    "type": "custom_tool_call_output",
                    "call_id": self.pending_call["call_id"],
                    "output": content,
                })
                self.pending_call = None
                return

            self.close_pending_delivery()

        self.history.append({
            "role": "user",
            "content": content,
        })

    def append_outgoing(self, msg, content):
        self.close_pending_delivery()

        tool_call = self.generated_tool_call
        self.generated_tool_call = None
        if tool_call is not None and tool_call.get("input") == content:
            call_id = tool_call["call_id"]
            item = {
                "type": "custom_tool_call",
                "call_id": call_id,
                "name": TOOL_NAME,
                "input": content,
            }
            if tool_call.get("id") is not None:
                item["id"] = tool_call["id"]
            if tool_call.get("namespace") is not None:
                item["namespace"] = tool_call["namespace"]
            self.history.append(item)
        else:
            call_id = self.next_call_id()
            self.history.append({
                "type": "custom_tool_call",
                "call_id": call_id,
                "name": TOOL_NAME,
                "input": content,
            })

        to_addr = msg.get("To")
        if to_addr == USER_ADDRESS or to_addr is None:
            self.history.append({
                "type": "custom_tool_call_output",
                "call_id": call_id,
                "output": "delivered",
            })
            return

        self.pending_call = {
            "call_id": call_id,
            "to": to_addr,
        }

    def append(self, msg):
        content = msg.format(True)
        if msg.isReceiving():
            self.append_incoming(msg, content)
        else:
            self.append_outgoing(msg, content)

    def before_generate(self):
        self.close_pending_delivery()

    def create_response(self):
        return self.client.responses.create(
            model=self.model,
            input=self.history,
            tools=TOOLS,
            tool_choice=TOOL_CHOICE,
        )

    def find_tool_call(self, response):
        for item in response.output:
            data = self.object_to_dict(item)
            if data.get("type") == "custom_tool_call" \
                    and data.get("name") == TOOL_NAME:
                return data
        return None

    def extract_message_text(self, response):
        tool_call = self.find_tool_call(response)
        if tool_call is None:
            text = getattr(response, "output_text", "")
            raise RuntimeError(
                f"response did not contain {TOOL_NAME} custom tool call: {text}"
            )

        self.generated_tool_call = tool_call
        return tool_call["input"]

def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model",
        default=DEFAULT_MODEL,
        help=f"Model name. Defaults to {DEFAULT_MODEL}",
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    agent = Agent(args.model)
    agent.run()
