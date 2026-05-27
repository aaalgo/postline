#!/usr/bin/env python3
import json

from tavily import TavilyClient

from postline import Message, Service


class WebSearch(Service):

    def __init__(self):
        super().__init__()
        self.client = TavilyClient()

    def make_reply(self, status, body):
        return Message({
            "Status": status,
            "Content-Type": "json",
        }, json.dumps(body, ensure_ascii=False, separators=(",", ":")))

    def on_call(self, msg, resp):
        query = msg.get("Subject")
        if not query:
            resp.append(self.make_reply("error", {
                "error": "missing Subject",
            }))
            return

        try:
            result = self.client.search(
                query=query,
                search_depth="advanced",
            )
        except Exception as e:
            resp.append(self.make_reply("error", {
                "error": str(e),
                "type": e.__class__.__name__,
                "query": query,
            }))
            return

        resp.append(self.make_reply("ok", result))


def main():
    WebSearch().run()


if __name__ == "__main__":
    main()
