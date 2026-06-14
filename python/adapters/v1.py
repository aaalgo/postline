#!/usr/bin/env python3
import argparse
import os

from openai import OpenAI
from postline import LLMAgent


OPENROUTER_URL = "https://openrouter.ai/api/v1"
OPENROUTER_MODEL = "google/gemma-3-4b-it"
DEFAULT_MODEL = OPENROUTER_MODEL


class Agent (LLMAgent):

    def __init__ (self, url, model, headers):
        if url.startswith("https://openrouter.ai/"):
            api_key = os.environ.get("OPENROUTER_API_KEY")
            provider = "openrouter"
        else:
            api_key = os.environ.get("OPENAI_API_KEY")
            provider = "openai"

        client = OpenAI(
            api_key=api_key,
            base_url=url,
            default_headers=headers,
        )
        super().__init__(client, model, provider)

    def load_models(self):
        self.models = {}

    def create_response(self):
        return self.client.chat.completions.create(
            model=self.model,
            messages=self.history,
        )

    def extract_message_text(self, response):
        return response.choices[0].message.content


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--provider",
        default="openrouter",
        help="provider",
    )
    parser.add_argument(
        "--model",
        default=None,
        help=f"Model name. Defaults to {DEFAULT_MODEL}",
    )
    args = parser.parse_args()
    if args.provider == 'openai':
        assert False, "Not supported"
    elif args.provider == 'openrouter':
        args.url = OPENROUTER_URL
        if args.model is None:
            args.model = OPENROUTER_MODEL
    else:
        assert args.provider is None

    return args


if __name__ == "__main__":
    args = parse_args()
    agent = Agent(args.url, args.model, {})
    agent.run()
