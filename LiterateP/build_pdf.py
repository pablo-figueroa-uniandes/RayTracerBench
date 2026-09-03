#!/usr/bin/env python3
"""Builds RayTracerBench-LiterateP.pdf from README.md + the numbered chapters.

Dependency-free: no pip packages, no pandoc, no LaTeX. Converts the small,
self-imposed Markdown subset these files actually use (headers, fenced code
blocks with a language tag, bold, inline code, tables, links, horizontal
rules, blockquotes) to HTML with a hand-rolled pass, then shells out to a
local, already-installed Google Chrome to print that HTML to PDF headlessly
-- the same tool the existing PDF's own metadata (Producer: Skia/PDF,
Creator: HeadlessChrome) proves it was built with.

Usage: python3 LiterateP/build_pdf.py
"""

import html
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

LITERATE_P_DIR = Path(__file__).resolve().parent
OUTPUT_PDF = LITERATE_P_DIR / "RayTracerBench-LiterateP.pdf"

CHAPTER_FILES = [LITERATE_P_DIR / "README.md"] + [
    LITERATE_P_DIR / f"{n:02d}-{slug}.md"
    for n, slug in [
        (1, "core-data-model"),
        (2, "shared-raytrace-core"),
        (3, "metal-shaders"),
        (4, "cpu-renderer"),
        (5, "gpu-renderer"),
        (6, "mesh-generation"),
        (7, "scene-export"),
        (8, "app-shell"),
        (9, "ui-controls"),
        (10, "image-display-view"),
        (11, "tests"),
        (12, "build-system"),
        (13, "raster-renderer"),
        (14, "pipeline-visualization"),
    ]
]


def slugify(text):
    slug = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return slug or "section"


def render_inline(text):
    """Handles inline code, bold, and links -- in that order, so `code` spans
    are protected (as HTML entities) before ** or [ ] inside them are touched."""
    parts = []
    i = 0
    for m in re.finditer(r"`([^`]+)`", text):
        parts.append(("text", text[i : m.start()]))
        parts.append(("code", m.group(1)))
        i = m.end()
    parts.append(("text", text[i:]))

    out = []
    for kind, chunk in parts:
        if kind == "code":
            out.append(f"<code>{html.escape(chunk)}</code>")
            continue
        chunk = html.escape(chunk)
        # Bold: **text**
        chunk = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", chunk)
        # Links: [text](url)
        chunk = re.sub(
            r"\[([^\]]+)\]\(([^)]+)\)",
            lambda m: f'<a href="{m.group(2)}">{m.group(1)}</a>',
            chunk,
        )
        out.append(chunk)
    return "".join(out)


def render_table(lines):
    header_cells = [c.strip() for c in lines[0].strip().strip("|").split("|")]
    body_lines = lines[2:]  # line[1] is the --- separator row
    out = ["<table>", "<thead><tr>"]
    for cell in header_cells:
        out.append(f"<th>{render_inline(cell)}</th>")
    out.append("</tr></thead><tbody>")
    for line in body_lines:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        out.append("<tr>")
        for cell in cells:
            out.append(f"<td>{render_inline(cell)}</td>")
        out.append("</tr>")
    out.append("</tbody></table>")
    return "".join(out)


def markdown_to_html(markdown_text, anchor_prefix):
    lines = markdown_text.split("\n")
    out = []
    i = 0
    n = len(lines)
    used_anchors = set()

    while i < n:
        line = lines[i]

        # Fenced code block
        fence_match = re.match(r"^```(\w*)\s*$", line)
        if fence_match:
            lang = fence_match.group(1)
            code_lines = []
            i += 1
            while i < n and not lines[i].startswith("```"):
                code_lines.append(lines[i])
                i += 1
            i += 1  # skip closing fence
            code_html = html.escape("\n".join(code_lines))
            lang_attr = f' data-lang="{lang}"' if lang else ""
            out.append(f"<pre{lang_attr}><code>{code_html}</code></pre>")
            continue

        # Heading
        heading_match = re.match(r"^(#{1,6})\s+(.*)$", line)
        if heading_match:
            level = len(heading_match.group(1))
            text = heading_match.group(2).strip()
            base = f"{anchor_prefix}-{slugify(text)}"
            anchor = base
            suffix = 2
            while anchor in used_anchors:
                anchor = f"{base}-{suffix}"
                suffix += 1
            used_anchors.add(anchor)
            out.append(f'<h{level} id="{anchor}">{render_inline(text)}</h{level}>')
            i += 1
            continue

        # Horizontal rule
        if re.match(r"^-{3,}\s*$", line):
            out.append("<hr>")
            i += 1
            continue

        # Blockquote (abstract paragraphs)
        if line.startswith(">"):
            quote_lines = []
            while i < n and lines[i].startswith(">"):
                quote_lines.append(re.sub(r"^>\s?", "", lines[i]))
                i += 1
            out.append(f"<blockquote><p>{render_inline(' '.join(quote_lines))}</p></blockquote>")
            continue

        # Table: a line containing '|' followed by a '---' separator line
        if "|" in line and i + 1 < n and re.match(r"^\s*\|?[\s:|-]+\|?\s*$", lines[i + 1]) and "-" in lines[i + 1]:
            table_lines = [line, lines[i + 1]]
            i += 2
            while i < n and "|" in lines[i] and lines[i].strip():
                table_lines.append(lines[i])
                i += 1
            out.append(render_table(table_lines))
            continue

        # Unordered list
        if re.match(r"^\s*-\s+", line):
            list_items = []
            while i < n and re.match(r"^\s*-\s+", lines[i]):
                item_lines = [re.sub(r"^\s*-\s+", "", lines[i])]
                i += 1
                # Continuation lines (indented, part of the same bullet)
                while i < n and lines[i].strip() and not re.match(r"^\s*-\s+", lines[i]) and not lines[i].startswith("#") and not lines[i].startswith("```"):
                    item_lines.append(lines[i].strip())
                    i += 1
                list_items.append(" ".join(item_lines))
            out.append("<ul>")
            for item in list_items:
                out.append(f"<li>{render_inline(item)}</li>")
            out.append("</ul>")
            continue

        # Blank line
        if not line.strip():
            i += 1
            continue

        # Paragraph: accumulate until a blank line or a line starting a new block
        para_lines = [line]
        i += 1
        while (
            i < n
            and lines[i].strip()
            and not lines[i].startswith("#")
            and not lines[i].startswith("```")
            and not re.match(r"^-{3,}\s*$", lines[i])
            and not lines[i].startswith(">")
            and not re.match(r"^\s*-\s+", lines[i])
        ):
            para_lines.append(lines[i])
            i += 1
        out.append(f"<p>{render_inline(' '.join(para_lines))}</p>")

    return "\n".join(out)


PAGE_CSS = """
@page { size: Letter; margin: 0.85in 0.75in; }
body {
    font-family: -apple-system, "Helvetica Neue", Arial, sans-serif;
    color: #1a1a1a;
    line-height: 1.5;
    font-size: 10.5pt;
}
h1, h2, h3 { font-family: Georgia, "Times New Roman", serif; }
h1 { font-size: 20pt; margin-top: 0; }
h2 { font-size: 15pt; border-bottom: 1px solid #ccc; padding-bottom: 4px; }
h3 { font-size: 12.5pt; }
.chapter { page-break-before: always; }
.titlepage { page-break-after: always; text-align: center; padding-top: 2.5in; }
.titlepage h1 { font-size: 28pt; }
.titlepage p { color: #555; font-size: 12pt; }
.toc { page-break-after: always; }
.toc ol { line-height: 2; }
.toc a { text-decoration: none; color: #1a1a1a; }
pre {
    background: #f6f6f6;
    border: 1px solid #ddd;
    border-radius: 4px;
    padding: 8px 10px;
    font-size: 8.5pt;
    overflow-x: auto;
    white-space: pre-wrap;
    word-wrap: break-word;
}
code {
    font-family: "SF Mono", Menlo, Consolas, monospace;
    background: #f0f0f0;
    padding: 1px 4px;
    border-radius: 3px;
    font-size: 0.92em;
}
pre code { background: none; padding: 0; }
blockquote {
    border-left: 3px solid #999;
    margin-left: 0;
    padding-left: 14px;
    color: #333;
    font-style: italic;
}
table { border-collapse: collapse; width: 100%; margin: 10px 0; font-size: 9.5pt; }
th, td { border: 1px solid #ccc; padding: 5px 8px; text-align: left; vertical-align: top; }
th { background: #f0f0f0; }
a { color: #0645ad; }
hr { border: none; border-top: 1px solid #ccc; margin: 1.2em 0; }
"""


def build_html():
    chapter_htmls = []
    toc_entries = []

    for path in CHAPTER_FILES:
        text = path.read_text()
        anchor_prefix = path.stem  # e.g. "README" or "07-scene-export"
        body_html = markdown_to_html(text, anchor_prefix)

        first_h1 = re.search(r"^#\s+(.*)$", text, re.MULTILINE)
        title = first_h1.group(1).strip() if first_h1 else path.stem
        chapter_anchor = f"{anchor_prefix}-{slugify(title)}"
        toc_entries.append((title, chapter_anchor))

        chapter_htmls.append(f'<section class="chapter">{body_html}</section>')

    toc_html = "<ol>" + "".join(
        f'<li><a href="#{anchor}">{html.escape(title)}</a></li>' for title, anchor in toc_entries
    ) + "</ol>"

    return f"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>RayTracerBench, a Literate Program</title>
<style>{PAGE_CSS}</style>
</head>
<body>
<div class="titlepage">
<h1>RayTracerBench</h1>
<p>A Literate Programming Account of the Source Tree</p>
</div>
<div class="toc">
<h2>Table of Contents</h2>
{toc_html}
</div>
{''.join(chapter_htmls)}
</body>
</html>
"""


def main():
    if not Path(CHROME).exists():
        sys.exit(f"error: Google Chrome not found at {CHROME}")

    for path in CHAPTER_FILES:
        if not path.exists():
            sys.exit(f"error: missing chapter file {path}")

    document_html = build_html()

    with tempfile.TemporaryDirectory() as tmp_dir:
        html_path = Path(tmp_dir) / "literate.html"
        html_path.write_text(document_html)

        subprocess.run(
            [
                CHROME,
                "--headless",
                "--disable-gpu",
                f"--print-to-pdf={OUTPUT_PDF}",
                "--no-pdf-header-footer",
                str(html_path),
            ],
            check=True,
        )

    print(f"Wrote {OUTPUT_PDF} ({OUTPUT_PDF.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
