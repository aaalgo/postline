#!/usr/bin/env python3
import os
from playwright.sync_api import sync_playwright


profile = os.environ.get("THEMITBBS_PROFILE")
if not profile:
    raise RuntimeError("THEMITBBS_PROFILE is not set")

with sync_playwright() as p:
    ctx = p.chromium.launch_persistent_context(
        user_data_dir=profile,
        headless=False,
    )
    page = ctx.new_page()
    page.goto("https://themitbbs.com/")
    print("Login and close browser window when done.")
    ctx.on("close", lambda _: print("browser closed"))
    ctx.wait_for_event("close")

