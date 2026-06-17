#!/usr/bin/env python3
"""
search_arxiv.py

Small command-line arXiv search tool for humans and agents.

Examples:
  ./search_arxiv.py "RNA folding"
  ./search_arxiv.py "RNA folding" -n 5 --sort submitted --md compact
  ./search_arxiv.py "RNA folding" --field title --md tiny
  ./search_arxiv.py --id 2001.04020
  ./search_arxiv.py 'cat:cs.LG AND all:"RNA folding"' --raw-query --format jsonl
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import textwrap
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass, asdict
from typing import Iterable, Optional


ARXIV_API_URL = "https://export.arxiv.org/api/query"
ATOM_NS = {"atom": "http://www.w3.org/2005/Atom"}
ARXIV_NS = {"arxiv": "http://arxiv.org/schemas/atom"}


@dataclass
class Paper:
    arxiv_id: str
    version: Optional[str]
    title: str
    authors: list[str]
    summary: str
    published: str
    updated: str
    categories: list[str]
    primary_category: Optional[str]
    abs_url: str
    pdf_url: str
    doi: Optional[str] = None
    comment: Optional[str] = None
    journal_ref: Optional[str] = None


def normalize_space(s: str) -> str:
    return " ".join((s or "").split())


def truncate(s: str, max_chars: Optional[int]) -> str:
    s = normalize_space(s)
    if max_chars is None or max_chars <= 0 or len(s) <= max_chars:
        return s
    return s[: max_chars - 1].rstrip() + "…"


def parse_arxiv_id(entry_id: str) -> tuple[str, Optional[str]]:
    # entry_id is usually http://arxiv.org/abs/2001.04020v2
    tail = entry_id.rstrip("/").split("/")[-1]
    m = re.match(r"(.+?)(v\d+)$", tail)
    if m:
        return m.group(1), m.group(2)
    return tail, None


def build_search_query(args: argparse.Namespace) -> str:
    if args.id:
        return ""

    parts: list[str] = []

    if args.raw_query:
        if not args.query:
            raise SystemExit("error: QUERY is required unless --id is used")
        parts.append(args.query)
    else:
        if args.query:
            field_prefix = {
                "all": "all",
                "title": "ti",
                "abstract": "abs",
                "author": "au",
                "category": "cat",
            }[args.field]
            q = args.query
            if " " in q and not (q.startswith('"') and q.endswith('"')):
                q = f'"{q}"'
            parts.append(f"{field_prefix}:{q}")

    for cat in args.cat or []:
        parts.append(f"cat:{cat}")

    for author in args.author or []:
        a = author
        if " " in a and not (a.startswith('"') and a.endswith('"')):
            a = f'"{a}"'
        parts.append(f"au:{a}")

    for title in args.title or []:
        t = title
        if " " in t and not (t.startswith('"') and t.endswith('"')):
            t = f'"{t}"'
        parts.append(f"ti:{t}")

    for abstract in args.abstract or []:
        a = abstract
        if " " in a and not (a.startswith('"') and a.endswith('"')):
            a = f'"{a}"'
        parts.append(f"abs:{a}")

    if not parts:
        raise SystemExit("error: QUERY is required unless --id is used")

    return " AND ".join(parts)


def arxiv_sort(args: argparse.Namespace) -> tuple[str, str]:
    sort_map = {
        "relevance": "relevance",
        "submitted": "submittedDate",
        "updated": "lastUpdatedDate",
    }
    sort_by = sort_map[args.sort]

    if args.asc and args.desc:
        raise SystemExit("error: choose only one of --asc or --desc")

    if args.asc:
        order = "ascending"
    elif args.desc:
        order = "descending"
    else:
        order = "descending" if args.sort in {"submitted", "updated"} else "descending"

    return sort_by, order


def fetch_arxiv(args: argparse.Namespace) -> str:
    params = {
        "start": str(args.start),
        "max_results": str(args.max_results),
    }

    if args.id:
        params["id_list"] = ",".join(args.id)
    else:
        params["search_query"] = build_search_query(args)
        sort_by, order = arxiv_sort(args)
        params["sortBy"] = sort_by
        params["sortOrder"] = order

    url = ARXIV_API_URL + "?" + urllib.parse.urlencode(params)

    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": args.user_agent,
            "Accept": "application/atom+xml",
        },
    )

    if args.verbose:
        print(url, file=sys.stderr)

    with urllib.request.urlopen(req, timeout=args.timeout) as resp:
        return resp.read().decode("utf-8", errors="replace")


def text_or_none(parent: ET.Element, path: str, ns: Optional[dict[str, str]] = None) -> Optional[str]:
    el = parent.find(path, ns or ATOM_NS)
    if el is None or el.text is None:
        return None
    return normalize_space(el.text)


def parse_feed(xml_text: str) -> list[Paper]:
    root = ET.fromstring(xml_text)
    papers: list[Paper] = []

    for e in root.findall("atom:entry", ATOM_NS):
        entry_id = text_or_none(e, "atom:id") or ""
        arxiv_id, version = parse_arxiv_id(entry_id)

        title = text_or_none(e, "atom:title") or ""
        summary = text_or_none(e, "atom:summary") or ""

        authors = [
            normalize_space(a.findtext("atom:name", default="", namespaces=ATOM_NS))
            for a in e.findall("atom:author", ATOM_NS)
        ]
        authors = [a for a in authors if a]

        categories = [
            c.attrib.get("term", "")
            for c in e.findall("atom:category", ATOM_NS)
            if c.attrib.get("term")
        ]

        primary = e.find("arxiv:primary_category", ARXIV_NS)
        primary_category = primary.attrib.get("term") if primary is not None else None

        abs_url = ""
        pdf_url = ""
        for link in e.findall("atom:link", ATOM_NS):
            href = link.attrib.get("href", "")
            rel = link.attrib.get("rel", "")
            typ = link.attrib.get("type", "")
            title_attr = link.attrib.get("title", "")
            if rel == "alternate":
                abs_url = href
            if typ == "application/pdf" or title_attr == "pdf":
                pdf_url = href

        if not abs_url and arxiv_id:
            abs_url = f"https://arxiv.org/abs/{arxiv_id}"
        if not pdf_url and arxiv_id:
            pdf_url = f"https://arxiv.org/pdf/{arxiv_id}"

        papers.append(
            Paper(
                arxiv_id=arxiv_id,
                version=version,
                title=title,
                authors=authors,
                summary=summary,
                published=text_or_none(e, "atom:published") or "",
                updated=text_or_none(e, "atom:updated") or "",
                categories=categories,
                primary_category=primary_category,
                abs_url=abs_url,
                pdf_url=pdf_url,
                doi=text_or_none(e, "arxiv:doi", ARXIV_NS),
                comment=text_or_none(e, "arxiv:comment", ARXIV_NS),
                journal_ref=text_or_none(e, "arxiv:journal_ref", ARXIV_NS),
            )
        )

    return papers


def visible_authors(authors: list[str], limit: Optional[int]) -> str:
    if limit is None or limit <= 0 or len(authors) <= limit:
        return ", ".join(authors)
    rest = len(authors) - limit
    return ", ".join(authors[:limit]) + f", et al. (+{rest})"


def date_only(dt: str) -> str:
    return dt[:10] if dt else ""


def render_markdown(papers: list[Paper], args: argparse.Namespace) -> str:
    if not papers:
        return "_No arXiv results._\n"

    lines: list[str] = []

    for i, p in enumerate(papers, 1):
        title = truncate(p.title, args.title_chars)
        lines.append(f"## {i}. {title}")
        lines.append("")

        if args.md == "tiny":
            lines.append(f"- **arXiv:** {p.arxiv_id}{p.version or ''}")
            if p.published:
                lines.append(f"- **Date:** {date_only(p.published)}")
            if not args.no_links:
                lines.append(f"- **PDF:** {p.pdf_url}")
            lines.append("")
            continue

        lines.append(f"- **arXiv:** {p.arxiv_id}{p.version or ''}")
        if p.published:
            lines.append(f"- **Published:** {date_only(p.published)}")
        if p.updated and p.updated[:10] != p.published[:10]:
            lines.append(f"- **Updated:** {date_only(p.updated)}")

        if not args.no_authors and p.authors:
            lines.append(f"- **Authors:** {visible_authors(p.authors, args.authors)}")

        if args.md == "full" and not args.no_categories and p.categories:
            lines.append(f"- **Categories:** {', '.join(p.categories)}")
        elif args.md == "compact" and not args.no_categories and p.primary_category:
            lines.append(f"- **Category:** {p.primary_category}")

        if p.doi and args.md == "full":
            lines.append(f"- **DOI:** {p.doi}")

        if not args.no_links:
            lines.append(f"- **Abs:** {p.abs_url}")
            lines.append(f"- **PDF:** {p.pdf_url}")

        if not args.no_abstract:
            lines.append("")
            max_chars = args.abstract_chars
            if max_chars is None:
                max_chars = 1200 if args.md == "full" else 500
            lines.append(truncate(p.summary, max_chars))

        if args.md == "full":
            if p.comment:
                lines.append("")
                lines.append(f"**Comment:** {p.comment}")
            if p.journal_ref:
                lines.append("")
                lines.append(f"**Journal:** {p.journal_ref}")

        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def render_json(papers: list[Paper]) -> str:
    return json.dumps([asdict(p) for p in papers], ensure_ascii=False, indent=2) + "\n"


def render_jsonl(papers: list[Paper]) -> str:
    return "".join(json.dumps(asdict(p), ensure_ascii=False) + "\n" for p in papers)


def positive_int(s: str) -> int:
    v = int(s)
    if v < 0:
        raise argparse.ArgumentTypeError("must be >= 0")
    return v


def make_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Search arXiv and print token-efficient Markdown/JSON.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    p.add_argument("query", nargs="?", help="Search query. Omit when using --id.")
    p.add_argument("--id", nargs="+", help="Fetch exact arXiv ID(s), e.g. 2001.04020 1706.03762")

    p.add_argument("-n", "--max-results", type=positive_int, default=10)
    p.add_argument("--start", type=positive_int, default=0)

    p.add_argument("--sort", choices=["relevance", "submitted", "updated"], default="relevance")
    direction = p.add_mutually_exclusive_group()
    direction.add_argument("--asc", action="store_true")
    direction.add_argument("--desc", action="store_true")

    p.add_argument("--field", choices=["all", "title", "abstract", "author", "category"], default="all")
    p.add_argument("--raw-query", action="store_true", help="Pass QUERY directly as arXiv search_query syntax.")

    p.add_argument("--cat", action="append", help="Add category filter, e.g. cs.LG or q-bio.BM. Repeatable.")
    p.add_argument("--author", action="append", help="Add author filter. Repeatable.")
    p.add_argument("--title", action="append", help="Add title filter. Repeatable.")
    p.add_argument("--abstract", action="append", help="Add abstract filter. Repeatable.")

    p.add_argument("--format", choices=["markdown", "json", "jsonl"], default="markdown")
    p.add_argument("--md", choices=["full", "compact", "tiny"], default="compact")

    p.add_argument("--abstract-chars", type=positive_int, default=None,
                   help="Truncate abstracts. 0 disables truncation.")
    p.add_argument("--title-chars", type=positive_int, default=220)
    p.add_argument("--authors", type=positive_int, default=3, help="Max authors shown in Markdown. 0 means all.")

    p.add_argument("--no-abstract", action="store_true")
    p.add_argument("--no-authors", action="store_true")
    p.add_argument("--no-categories", action="store_true")
    p.add_argument("--no-links", action="store_true")
    p.add_argument("--pdf-only", action="store_true", help="Print only PDF URLs, one per line.")

    p.add_argument("--timeout", type=float, default=30.0)
    p.add_argument("--delay", type=float, default=0.0,
                   help="Sleep before request. Useful when called repeatedly. arXiv asks clients to be polite.")
    p.add_argument("--user-agent", default="search_arxiv.py/0.1 (mailto:your-email@example.com)")
    p.add_argument("--doc", action="store_true", help="print usage and exit")
    p.add_argument("-v", "--verbose", action="store_true")

    return p


def main(argv: Optional[list[str]] = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)

    if args.doc:
        if __doc__:
            print(textwrap.dedent(__doc__).strip())
            print()
        parser.print_help()
        return 0

    if args.delay > 0:
        time.sleep(args.delay)

    try:
        xml_text = fetch_arxiv(args)
        papers = parse_feed(xml_text)
    except urllib.error.HTTPError as e:
        print(f"HTTP error from arXiv: {e.code} {e.reason}", file=sys.stderr)
        return 2
    except urllib.error.URLError as e:
        print(f"Network error: {e}", file=sys.stderr)
        return 2
    except ET.ParseError as e:
        print(f"Could not parse arXiv response XML: {e}", file=sys.stderr)
        return 2

    if args.pdf_only:
        for p in papers:
            print(p.pdf_url)
        return 0

    if args.format == "json":
        sys.stdout.write(render_json(papers))
    elif args.format == "jsonl":
        sys.stdout.write(render_jsonl(papers))
    else:
        sys.stdout.write(render_markdown(papers, args))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
