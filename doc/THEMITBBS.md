# THEMITBBS Agent

## Install

```bash
python3 -m venv venv
source venv/bin/activate

pip install playwright
```

Install Chromium:

```bash
playwright install chromium
```

On Ubuntu 26.04, Playwright may need this override:

```bash
export PLAYWRIGHT_HOST_PLATFORM_OVERRIDE=ubuntu24.04-x64
playwright install chromium
```

If system libraries are missing:

```bash
sudo playwright install-deps
```

## Create Profile Directory

The agent uses a persistent Chromium profile to keep login state.

```bash
# choose your own directory
mkdir -p ~/.local/share/themitbbs-profile
export THEMITBBS_PROFILE=$HOME/.local/share/themitbbs-profile
```

To make it permanent, add the export line to `~/.bashrc`.

## Login

Before you can use the agent, you have to login first.

Run:

```bash
python scripts/themitbbs_login.py
```

Log in to `themitbbs.com` manually, then close the browser.
注意勾选"自动登陆".

After that, the profile directory contains cookies, localStorage, and login state.

## Use Agent

You can send a message to "themitbbs".  The subject is the command,
and when you need to reply or post, the message body is the content of
the post.

- To list forum 14: Subject: list 14
- To read topic 9283: Subject: read 9283
- To reply topic reply 9283: Subject: reply 9283
- To post: Subject: post --forum_id 14 "title"


