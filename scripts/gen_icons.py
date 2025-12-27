#!/usr/bin/env python3
import os, sys, subprocess, re
from pathlib import Path
import io
try:
    from PIL import Image
except ImportError:
    Image = None

# NOTE:
#   This script clones the full Google Material Design Icons repository.
#   That repository is huge (~22 GB at the time of writing).
#   Make sure you have enough disk space and a decent connection.

REPO_URL = "https://github.com/google/material-design-icons"
CACHE_DIR = Path("../build/material-design-icons")
OUT_H   = Path("../src/material_xpm.h")

COLOR_LIGHT = "#1f2937"  # (light theme)
COLOR_DARK  = "#e5e7eb"  # (dark theme)

BG_LIGHT = "#f0f0f0"   # something like btnface in light
BG_DARK  = "#0b0f14"   # almost black background in dark

# preferred variants in the repo (if one is not available, we will take the first one found)
PREFERRED_THEMES = [
    "materialiconsoutlined",
    "materialiconsround",
    "materialicons",
    "materialiconssharp",
    "materialiconstwotone",
]

# wxART_* -> [preferred material names...]
# wxAE_ART_* (custom ids) -> [preferred material icon names...]
#
# Keep this mapping "truthy": only include ids that your code actually asks
# the wxArtProvider for (wxAE_ART_*). This script then generates XPM variants
# for those ids.
MAP = {
    "wxAE_ART_REFRESH": ["sync"],
    "wxAE_ART_DELETE": ["delete"],
    "wxAE_ART_NEW": ["note_add", "add"],
    "wxAE_ART_FILE_OPEN": ["file_open"],
    "wxAE_ART_FILE_SAVE": ["save"],
    "wxAE_ART_FILE_SAVE_AS": ["save_as", "save_alt"],
    "wxAE_ART_QUIT": ["logout", "exit_to_app"],
    "wxAE_ART_GO_BACK": ["arrow_back"],
    "wxAE_ART_GO_FORWARD": ["arrow_forward"],
    "wxAE_ART_FIND": ["search"],
    "wxAE_ART_FIND_AND_REPLACE": ["find_replace"],
    "wxAE_ART_UNDO": ["undo"],
    "wxAE_ART_REDO": ["redo"],
    "wxAE_ART_CUT": ["content_cut"],
    "wxAE_ART_COPY": ["content_copy"],
    "wxAE_ART_PASTE": ["content_paste"],
    "wxAE_ART_PRINT": ["print"],

    "wxAE_ART_FOLDER": ["folder"],
    "wxAE_ART_FOLDER_OPEN": ["folder_open"],
    "wxAE_ART_NORMAL_FILE": ["description", "insert_drive_file"],
    "wxAE_ART_EXECUTABLE_FILE": ["terminal", "code"],

    "wxAE_ART_SYSLIBRARY": ["library_books", "local_library"],
    "wxAE_ART_USRLIBRARY": ["library_add", "local_library"],

    "wxAE_ART_GO_UP": ["arrow_upward"],
    "wxAE_ART_GO_TO_PARENT": ["drive_file_move", "subdirectory_arrow_left"],
    "wxAE_ART_PLUS": ["add"],
    "wxAE_ART_MINUS": ["remove"],
    "wxAE_ART_EDIT": ["edit"],
    "wxAE_ART_LIST_VIEW": ["view_list"],
    "wxAE_ART_REPORT_VIEW": ["table_rows", "view_headline"],
    "wxAE_ART_TIP": ["lightbulb"],
    "wxAE_ART_INFORMATION": ["info"],
    "wxAE_ART_QUESTION": ["help"],
    "wxAE_ART_DEVBOARD": ["developer_board"],
    "wxAE_ART_PLAY": ["play_arrow"],
    "wxAE_ART_CHECK": ["check_circle"],
    "wxAE_ART_SERMON": ["monitor_heart"],
    "wxAE_ART_SOURCE_FORMAT": ["format_align_justify"],
    "wxAE_ART_SETTINGS": ["settings"],
    "wxAE_ART_SELECT_ALL": ["select_all"],
    "wxAE_ART_CHECK_FOR_UPDATES": ["update"],
}

def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"cmd failed: {' '.join(cmd)}\n{r.stderr}")
    return r.stdout

def run_bytes(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        stderr = r.stderr.decode("utf-8", errors="replace")
        raise RuntimeError(f"cmd failed: {' '.join(cmd)}\n{stderr}")
    return r.stdout

def ensure_repo():
    if (CACHE_DIR / ".git").exists():
        return
    CACHE_DIR.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "clone", "--depth", "1", REPO_URL, str(CACHE_DIR)])

def git_ls_files():
    out = run(["git", "ls-files"], cwd=CACHE_DIR)
    return out.splitlines()

def pick_svg(files, icon_name):
    # candidates where path contains /<icon_name>/ and ends with /24px.svg
    cands = [f for f in files if f"/{icon_name}/" in f and f.endswith("/24px.svg") and f.startswith("src/")]
    if not cands:
        return None

    # preferred topics
    for theme in PREFERRED_THEMES:
        themed = [f for f in cands if f"/{theme}/24px.svg" in f]
        if themed:
            # if there are more, take the first one (repo usually has 1)
            return themed[0]

    return cands[0]

def magick_cmd():
    # ImageMagick can be "magick" or "convert"
    for c in ("magick", "convert"):
        if shutil_which(c):
            return c
    raise RuntimeError("ImageMagick not found (magick/convert).")

def shutil_which(name):
    from shutil import which
    return which(name)

# --- Dithered “AA” for XPM (binary alpha via ordered dithering) ---

DITHER_MAX_PX = 24        # dither only for small ones (16/24); leave 32 “clean”
SUPERSAMPLE   = 8         # render SVG in larger, then downscale
ALPHA_LOW     = 8         # below = always transparent
ALPHA_HIGH    = 247       # above = always opaque

BG_LIGHT = "#f0f0f0"   # btnface-ish
BG_WHITE = "#ffffff"   # for white areas / dialogs
BG_DARK  = "#0b0f14"   # almost black background in dark theme

def _blend_hex(fg_hex, bg_hex, fg_w):
    fg_hex = fg_hex.lstrip("#")
    bg_hex = bg_hex.lstrip("#")
    fr, fg, fb = int(fg_hex[0:2],16), int(fg_hex[2:4],16), int(fg_hex[4:6],16)
    br, bg, bb = int(bg_hex[0:2],16), int(bg_hex[2:4],16), int(bg_hex[4:6],16)
    r = int(br + (fr - br) * fg_w + 0.5)
    g = int(bg + (fg - bg) * fg_w + 0.5)
    b = int(bb + (fb - bb) * fg_w + 0.5)
    return f"#{r:02x}{g:02x}{b:02x}"

def _xpm_from_alpha_3(alpha_img, fg_hex, bg_hex, var_name):
    w, h = alpha_img.size
    apx = alpha_img.load()

    mid_hex = _blend_hex(fg_hex, bg_hex, 0.65)  # 65% fg, 35% bg (ladíš podle oka)

    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            a = apx[x, y]
            if a < 32:
                row.append(' ')
            elif a > 220:
                row.append('.')
            else:
                row.append('+')
        rows.append("".join(row))

    out = []
    out.append(f"static const char * const {var_name}[] = {{\n")
    out.append(f"\"{w} {h} 3 1 \",\n")
    out.append("\"  c None\",\n")
    out.append(f"\". c {fg_hex}\",\n")
    out.append(f"\"+ c {mid_hex}\",\n")
    for r in rows:
        out.append(f"\"{r}\",\n")
    out.append("};\n")
    return "".join(out)

def _render_svg_to_png_bytes(svg_path, render_px):
    magick = magick_cmd()
    cmd = [
        magick,
        "-background", "none",
        "-density", "384",
        str(svg_path),
        "-resize", f"{render_px}x{render_px}",
        "-alpha", "on",
        "png:-"
    ]
    return run_bytes(cmd)

def svg_to_xpm(svg_path, size_px, color_hex, bg_hex, var_name):
    # For small icons: supersampling + downscale + dither from alpha => nicer edges in XPM.
    # For larger ones (e.g. 32): keep the original IM conversion (without dithering “graininess”).
    if Image is not None and size_px <= DITHER_MAX_PX:
        png = _render_svg_to_png_bytes(svg_path, size_px * SUPERSAMPLE)
        img = Image.open(io.BytesIO(png)).convert("RGBA")

        try:
            resample = Image.Resampling.LANCZOS
        except AttributeError:
            resample = Image.LANCZOS

        img = img.resize((size_px, size_px), resample=resample)
        alpha = img.getchannel("A")
        return _xpm_from_alpha_3(alpha, color_hex, bg_hex, var_name)

    # Fallback: original IM pipeline (without “AA” in XPM)
    magick = "magick" if shutil_which("magick") else "convert"
    cmd = [
        magick,
        "-background", "none",
        "-density", "384",
        str(svg_path),
        "-resize", f"{size_px}x{size_px}",
        "-alpha", "on",
        "-channel", "RGB",
        "-fill", color_hex,
        "-colorize", "100",
        "xpm:-"
    ]
    xpm = run(cmd)
    return normalize_xpm(xpm, var_name)


def normalize_xpm(xpm_text, var_name):
    # Outputs from IM can be:
    # static char *xpm__[] = {
    # static char * xpm[] = {
    # static const char *xpm[] = {
    # We want: static const char * const <var_name>[] = {

    patterns = [
        r'static\s+char\s*\*\s*\w+\s*\[\]\s*=\s*\{',
        r'static\s+const\s+char\s*\*\s*\w+\s*\[\]\s*=\s*\{',
    ]
    for pat in patterns:
        new_text, n = re.subn(
            pat,
            f'static const char * const {var_name}[] = {{',
            xpm_text,
            count=1
        )
        if n:
            return new_text

    # If the rename failed, you better fail, so you know right away
    raise RuntimeError("normalize_xpm(): could not rewrite XPM header line")


def main():
    ensure_repo()
    files = git_ls_files()

    OUT_H.parent.mkdir(parents=True, exist_ok=True)

    chunks = []
    chunks.append("// Auto-generated. Do not edit.\n")
    chunks.append("// Source icons: google/material-design-icons (Apache-2.0)\n\n")

    for wx_id, names in MAP.items():
        chosen = None
        chosen_name = None
        for n in names:
            p = pick_svg(files, n)
            if p:
                chosen = CACHE_DIR / p
                chosen_name = n
                break

        if not chosen:
            print(f"[WARN] no SVG found for {wx_id} ({names})", file=sys.stderr)
            continue

        # 16/32 + light/dark
        for mode, color in (("light", COLOR_LIGHT), ("dark", COLOR_DARK)):
            for px in (16, 20, 24, 32):
                var = f"mdi_{chosen_name}_{mode}_{px}"
                bg = BG_DARK if mode == "dark" else BG_LIGHT
                xpm = svg_to_xpm(chosen, px, color, bg, var)
                chunks.append(f"// {wx_id} -> {chosen_name} ({mode}, {px}px)\n")
                chunks.append(xpm)
                if not xpm.endswith("\n"):
                    chunks.append("\n")
                chunks.append("\n")

    OUT_H.write_text("".join(chunks), encoding="utf-8")
    print(f"Written: {OUT_H}")

if __name__ == "__main__":
    main()

