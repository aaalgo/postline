#!/usr/bin/env python3
import argparse

from anthropic import Anthropic
from postline import LLMAgent


DEFAULT_MODEL = "claude-sonnet-4-5"
PROVIDER = "anthropic"


class Agent (LLMAgent):

    def __init__ (self, model=DEFAULT_MODEL):
        super().__init__(Anthropic(), model, PROVIDER)

    def create_response(self):
        return self.client.messages.create(
            model=self.model,
            max_tokens=4096,
            messages=self.history,
        )

    def extract_message_text(self, response):
        return response.content[0].text


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
