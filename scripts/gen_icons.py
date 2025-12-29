#!/usr/bin/env python3
"""
gen_icons_simple.py

Minimal icon generator for Arduino Editor.

- Uses Google Material Design Icons repository as SVG source.
- Finds matching SVG for each icon name.
- Uses ImageMagick ONLY (magick/convert) to render XPM.
- Generates monochrome icons with transparent background in sizes: 16, 20, 24, 32.
- Two variants:
    * dark  -> #000000
    * light -> #FFFFFF

Output:
  A single header containing XPM arrays and a small lookup table.

Usage:
  python3 gen_icons_simple.py --out ../src/material_xpm.h --icons sync delete note_add
  python3 gen_icons_simple.py --out m.h --icons delete
  python3 gen_icons_simple.py --no-update --icons delete

Notes:
  - The Material repo is huge.
  - By default we use ../build/material-design-icons relative to this script.
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

REPO_URL = "https://github.com/google/material-design-icons"

# Default location (matches your project layout): ../build/material-design-icons
CACHE_DIR = (Path(__file__).resolve().parent.parent / "build" / "material-design-icons").resolve()

SVG_SUFFIX = "/24px.svg"

PREFERRED_THEMES = [
    "materialiconsoutlined",
    "materialiconsround",
    "materialicons",
    "materialiconssharp",
    "materialiconstwotone",
]

SIZES = [16, 20, 24, 32]

VARIANTS = {
    "dark":  "#FFFFFF",  # white glyph for dark UI
    "light": "#000000",  # black glyph for light UI
}

ICONS_DEFAULT = [
    "sync",
    "delete",
    "note_add",
    "file_open",
    "save",
    "save_as",
    "logout",
    "arrow_back",
    "arrow_forward",
    "search",
    "find_replace",
    "find_in_page",
    "adjust",
    "undo",
    "redo",
    "content_cut",
    "content_copy",
    "content_paste",
    "print",
    "folder",
    "folder_open",
    "description",
    "terminal",
    "library_books",
    "library_add",
    "arrow_upward",
    "arrow_downward",
    "drive_file_move",
    "add",
    "remove",
    "edit",
    "view_list",
    "table_rows",
    "lightbulb",
    "info",
    "question_mark",
    "developer_board",
    "play_arrow",
    "check_circle",
    "monitor_heart",
    "format_align_justify",
    "settings",
    "select_all",
    "update",
]


def run(cmd: List[str], cwd: Optional[Path] = None) -> str:
    p = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if p.returncode != 0:
        raise RuntimeError(
            f"Command failed ({p.returncode}): {' '.join(cmd)}\n"
            f"--- stdout ---\n{p.stdout}\n--- stderr ---\n{p.stderr}"
        )
    return p.stdout


def ensure_repo(repo_dir: Path) -> None:
    repo_dir.parent.mkdir(parents=True, exist_ok=True)
    if (repo_dir / ".git").exists():
        run(["git", "fetch", "--all", "--prune"], cwd=repo_dir)
        # material-design-icons uses 'master' (at least historically); this keeps it simple
        run(["git", "checkout", "master"], cwd=repo_dir)
        run(["git", "pull", "--ff-only"], cwd=repo_dir)
    else:
        run(["git", "clone", "--depth", "1", REPO_URL, str(repo_dir)])


def git_ls_files(repo_dir: Path) -> List[str]:
    out = run(["git", "ls-files"], cwd=repo_dir)
    return [ln.strip() for ln in out.splitlines() if ln.strip()]


def pick_svg(files: Iterable[str], icon_name: str) -> Optional[str]:
    needle = f"/{icon_name}/"
    cands = [f for f in files if f.startswith("src/") and needle in f and f.endswith(SVG_SUFFIX)]
    if not cands:
        return None

    for theme in PREFERRED_THEMES:
        themed = [f for f in cands if f"/{theme}/" in f]
        if themed:
            return themed[0]

    return cands[0]


def find_imagemagick() -> Tuple[List[str], bool]:
    """Return (base_cmd, is_magick7). base_cmd is ['magick'] or ['convert']."""
    for exe in ("magick", "convert"):
        try:
            subprocess.run([exe, "-version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            return [exe], (exe == "magick")
        except Exception:
            pass
    raise RuntimeError("ImageMagick not found. Ensure `magick` (IM7) or `convert` (IM6) is in PATH.")


def render_xpm(svg_path: Path, out_xpm: Path, size: int, color_hex: str, im_base: List[str], is_magick7: bool) -> None:
    out_xpm.parent.mkdir(parents=True, exist_ok=True)

    # IM7 uses: magick convert ...
    # IM6 uses: convert ...
    cmd = []
    cmd += im_base
    if is_magick7:
        cmd += ["convert"]

    cmd += [
        "-background", "none",
        str(svg_path),

        # nejdřív na cílovou velikost
        "-resize", f"{size}x{size}",

        # udělej 1-bit masku z alpha kanálu (vyhodí antialias šedou)
        "-alpha", "set",
        "-alpha", "extract",
        "-threshold", "50%",

        # vytvoř barevnou plochu a přenes do ní masku jako opacity
        "(", "-size", f"{size}x{size}", f"xc:{color_hex}", ")",
        "+swap",
        "-compose", "CopyOpacity",
        "-composite",

        str(out_xpm),
    ]

    run(cmd)


def xpm_to_c_array(xpm_text: str, symbol: str) -> str:
    """
    Extract the quoted XPM records and emit a clean C array:

      static const char * const <symbol>[] = {
        "w h colors cpp",
        "  c None",
        ". c #000000",
        ...
      };

    We intentionally drop the leading /* XPM */ comment and the original variable name.
    """
    quoted: List[str] = []
    for raw in xpm_text.splitlines():
        ln = raw.strip()
        if ln.startswith('"') and (ln.endswith('",') or ln.endswith('"')):
            if not ln.endswith(","):
                ln += ","
            quoted.append(ln)

    if not quoted:
        # Fallback: try between braces
        in_brace = False
        for raw in xpm_text.splitlines():
            if "{" in raw:
                in_brace = True
                continue
            if "}" in raw:
                in_brace = False
                continue
            if not in_brace:
                continue
            ln = raw.strip()
            if ln.startswith('"') and (ln.endswith('",') or ln.endswith('"')):
                if not ln.endswith(","):
                    ln += ","
                quoted.append(ln)

    if not quoted:
        raise RuntimeError("Failed to parse XPM output (no quoted records found).")

    body = "\n".join(quoted)
    return f"static const char * const {symbol}[] = {{\n{body}\n}};\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="Output header path (default: ./material_xpm.h next to this script)")
    ap.add_argument("--icons", nargs="*", default=None, help="Icon names (Material) e.g. sync delete note_add")
    ap.add_argument("--repo", default=str(CACHE_DIR), help="Path to cached material-design-icons repo")
    ap.add_argument("--no-update", action="store_true", help="Do not git pull/fetch; assumes repo already present")
    ap.add_argument("--work", default=None, help="Work directory for intermediates (default: ../build/_icons_work)")
    args = ap.parse_args()

    icons = args.icons if args.icons else ICONS_DEFAULT

    repo_dir = Path(args.repo).resolve()
    if args.no_update:
        if not (repo_dir / ".git").exists():
            raise RuntimeError(f"--no-update was given but repo is missing: {repo_dir}")
    else:
        ensure_repo(repo_dir)

    files = git_ls_files(repo_dir)
    im_base, is_magick7 = find_imagemagick()

    work_dir = Path(args.work).resolve() if args.work else (Path(__file__).resolve().parent.parent / "build" / "_icons_work").resolve()
    work_dir.mkdir(parents=True, exist_ok=True)

    out_path = Path(args.out).resolve() if args.out else (Path(__file__).resolve().parent.parent / "src" / "material_xpm.h").resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    arrays: List[str] = []
    index: List[str] = []

    for icon in icons:
        rel = pick_svg(files, icon)
        if not rel:
            raise RuntimeError(f"SVG for icon '{icon}' not found in repo.")
        svg = (repo_dir / rel).resolve()

        for variant_name, color_hex in VARIANTS.items():
            for size in SIZES:
                out_xpm = work_dir / f"{icon}.{variant_name}.{size}.xpm"
                render_xpm(svg, out_xpm, size, color_hex, im_base, is_magick7)

                sym = f"mdi_{icon}_{variant_name}_{size}"
                xpm_text = out_xpm.read_text(encoding="utf-8", errors="replace")
                arrays.append(xpm_to_c_array(xpm_text, sym))
                index.append(f'  {{"{icon}", "{variant_name}", {size}, {sym}}},')

    header: List[str] = []
    header.append("// Auto-generated by gen_icons_simple.py. DO NOT EDIT.\n")
    header.append("#pragma once\n\n")
    header.append("#include <cstddef>\n\n")

    header.extend(arrays)

    out_path.write_text("".join(header), encoding="utf-8")
    print(f"Wrote: {out_path}")


if __name__ == "__main__":
    main()
