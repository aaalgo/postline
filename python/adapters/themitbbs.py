#!/usr/bin/env python3
import argparse
import time
import json
import os
import shlex
import sys
from urllib.parse import urljoin

from playwright.sync_api import sync_playwright
from postline import Message, Service


BASE = "https://themitbbs.com/"
FORUM_URL = "https://themitbbs.com/viewforum.php?f=14"


def reply(status, value):
    return Message({
        "Subject": status,
        "Content-Type": "json",
    }, json.dumps(value, ensure_ascii=False, indent=2))


def make_reply_subject(title):
    title = (title or "").strip()
    if title.startswith("Re:"):
        return title
    return f"Re: {title}"    


class TheMitbbs(Service):

    def __init__(self):
        super().__init__()

        profile = os.environ.get("THEMITBBS_PROFILE")
        if not profile:
            raise RuntimeError("THEMITBBS_PROFILE is not set")

        self.pw = sync_playwright().start()
        self.ctx = self.pw.chromium.launch_persistent_context(
            user_data_dir=profile,
            headless=True,
            locale="zh-CN",
            args=[
                "--lang=zh-CN,zh",
                "--font-render-hinting=medium",
            ],
        )
        self.reply_forms = {}
        self.page = self.ctx.new_page()

    def close(self):
        try:
            self.ctx.close()
        finally:
            self.pw.stop()

    def on_call(self, msg, resp):
        try:
            result = self.dispatch(msg.get("Subject"), msg.body())
            resp.append(reply("ok", result))
        except Exception as e:
            print(f"[themitbbs] error: {e}", file=sys.stderr)
            resp.append(reply("error", {
                "error": str(e),
                "type": e.__class__.__name__,
            }))

    def dispatch(self, subject, body):
        args = parse_args(subject)

        if args.cmd == "list":
            return self.cmd_list(args)

        if args.cmd == "read":
            return self.cmd_read(args)

        if args.cmd == "reply":
            return self.cmd_reply(args, body)

        if args.cmd == "post":
            return self.cmd_post(args, body)

        raise RuntimeError(f"unknown command: {args.cmd}")

    def goto(self, url):
        self.page.goto(url, wait_until="networkidle")
        return self.page

    def cmd_list(self, args):
        FORUM_URL = "https://themitbbs.com/viewforum.php?f={}"
        url = FORUM_URL.format(args.forum_id)
        page = self.goto(url)

        data = page.evaluate("""
        () => {

          function parseTopics(root) {
            const rows = [];

            for (const li of root.querySelectorAll("li.row")) {

              const titleA = li.querySelector("a.topictitle");
              if (!titleA)
                continue;

              const authorA =
                li.querySelector(".left-box a.username, .left-box a.username-coloured") ||
                li.querySelector("a.username, a.username-coloured");

              const timeEl =
                li.querySelector(".left-box time") ||
                li.querySelector("time");
                const topicUrl = new URL(titleA.getAttribute("href"), location.href);
                const topicId = Number(topicUrl.searchParams.get("t") || 0);

                rows.push({
                  topic_id: topicId,
                  author: authorA ? authorA.textContent.trim() : "",
                  title: titleA.textContent.trim(),
                  date: timeEl
                      ? (timeEl.getAttribute("datetime") || timeEl.textContent.trim())
                      : "",
                });                

                }

            return rows;
          }

          const hotRoot =
            document.querySelector(".toptopics-summary");

          const recentRoot =
            document.querySelector(".forumbg");

          return {
            hot_topics: hotRoot ? parseTopics(hotRoot) : [],
            recent_topics: recentRoot ? parseTopics(recentRoot) : [],
          };
        }
        """)

        if args.limit:
            data["hot_topics"] = data["hot_topics"][:args.limit]
            data["recent_topics"] = data["recent_topics"][:args.limit]

        return data

    def cmd_read(self, args):
        TOPIC_URL = "https://themitbbs.com/viewtopic.php?t={}"
        url = TOPIC_URL.format(args.topic_id)
        page = self.goto(url)

        data = page.evaluate("""
        () => {
          function text(el) {
            return el ? el.textContent.trim() : "";
          }

          function absUrl(href) {
            return href ? new URL(href, location.href).href : "";
          }

          function parsePosts(root=document) {
            const posts = [];

            for (const post of root.querySelectorAll("div.post[id^='p']")) {
              const titleA = post.querySelector(".postbody h3 a");

              const authorA =
                post.querySelector("p.author a.username, p.author a.username-coloured") ||
                post.querySelector("dl.postprofile a.username, dl.postprofile a.username-coloured");

              const timeEl = post.querySelector("p.author time");
              const contentEl = post.querySelector(".postbody .content");

              posts.push({
                id: post.id || "",
                author: text(authorA),
                title: text(titleA),
                date: timeEl
                  ? (timeEl.getAttribute("datetime") || text(timeEl))
                  : "",
                content: contentEl ? contentEl.innerText.trim() : "",
              });
            }

            return posts;
          }

          function parseReplyForm() {
            const form = document.querySelector("#qr_postform");
            if (!form)
              return null;

            const get = (name) => {
              const el = form.querySelector(`[name="${name}"]`);
              return el ? el.value : "";
            };

            return {
              action: absUrl(form.getAttribute("action")),
              subject: get("subject"),
              creation_time: get("creation_time"),
              form_token: get("form_token"),
              topic_cur_post_id: get("topic_cur_post_id"),
              topic_id: get("topic_id"),
              forum_id: get("forum_id"),
              attach_sig: get("attach_sig") || "1"
            };
          }

          const topicTitleA = document.querySelector("h2.topic-title a");

          return {
            topic_id: Number(new URL(location.href).searchParams.get("t") || 0),
            title: text(topicTitleA) || document.title,
            posts: parsePosts(document),
            reply_form: parseReplyForm()
          };
        }
        """)

        if data.get("reply_form"):
            tid = int(data["reply_form"]["topic_id"] or data["id"] or args.id)

            first_title = ""
            if data.get("posts"):
                first_title = data["posts"][0].get("title", "")

            subject = make_reply_subject(first_title or data.get("title", ""))

            f = data["reply_form"]

            self.reply_forms[tid] = {
                "action": f["action"],
                "post_data": {
                    "subject": subject,
                    "message": "",
                    "creation_time": f["creation_time"],
                    "form_token": f["form_token"],
                    "topic_cur_post_id": f["topic_cur_post_id"],
                    "topic_id": f["topic_id"],
                    "forum_id": f["forum_id"],
                    "attach_sig": f.get("attach_sig", "1"),
                    "post": "提交",
                },
                "cached_at": time.time(),
            }

            data.pop("reply_form", None)

        return data

    def cmd_reply(self, args, body):
        form = self.reply_forms.get(args.topic_id)
        if not form:
            return {
                "ok": False,
                "error": f"no cached reply form for topic {args.id}; run read {args.id} first",
            }

        content = sys.stdin.read()
        if not content.strip():
            return {
                "ok": False,
                "error": "empty reply content",
            }

        post_data = dict(form["post_data"])
        post_data["message"] = body

        resp = self.page.request.post(
            form["action"],
            form=post_data,
            headers={
                "Referer": f"https://themitbbs.com/viewtopic.php?t={args.id}",
            },
        )

        return {
            "ok": resp.ok,
            "status": resp.status,
            "topic_id": args.id,
            "url": resp.url,
        }

    def cmd_post(self, args, body):
        forum_id = int(args.forum_id)
        title = args.title
        content = body or ""

        if not title.strip():
            return {"ok": False, "error": "empty title"}

        if not content.strip():
            return {"ok": False, "error": "empty body"}

        url = f"https://themitbbs.com/posting.php?mode=post&f={forum_id}"
        page = self.goto(url)

        # 用真实页面提交，避免误带 save/preview 触发保存草稿
        page.fill('#postform input[name="subject"]', title)
        page.fill('#postform textarea[name="message"]', content)

        with page.expect_navigation(wait_until="networkidle"):
            page.click('#postform input[name="post"]')

        final_url = page.url
        html_title = page.title()

        # 成功后 phpBB 通常跳到新 topic，URL 里会有 t=
        topic_id = page.evaluate("""
        () => {
          const u = new URL(location.href);
          return Number(u.searchParams.get("t") || 0);
        }
        """)
        page.screenshot(path="/home/wdong/public_html/forum.png", full_page=True)

        return {
            "ok": True,
            "status": "submitted",
            "forum_id": forum_id,
            "topic_id": topic_id,
            "url": final_url,
            "title": html_title,
        }


def parse_args(subject):
    parser = argparse.ArgumentParser(prog="themitbbs", add_help=False)

    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", add_help=False)
    p.add_argument("forum_id", nargs="?", type=int, default=14)
    p.add_argument("--limit", type=int, default=50)    

    p = sub.add_parser("read", add_help=False)
    p.add_argument("topic_id", type=int)

    p = sub.add_parser("reply", add_help=False)
    p.add_argument("topic_id", type=int)

    p = sub.add_parser("post", add_help=False)
    p.add_argument("--forum_id", type=int, default=14)
    p.add_argument("title", type=str)

    return parser.parse_args(shlex.split(subject or ""))


def main():
    svc = TheMitbbs()
    try:
        svc.run()
    finally:
        svc.close()


if __name__ == "__main__":
    main()
