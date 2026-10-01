#!/usr/bin/env python3
"""Generate docs/index.html for Borealis from the library sources.

Extracts, per view class:
  - XML tag (Application::registerXMLView)
  - XML attributes (register*XMLAttribute / BRLS_REGISTER_ENUM_XML_ATTRIBUTE / forwardXMLAttribute)
  - events (Event<T> getters and public members)
  - public methods/fields with their doc comments
Then renders a hand-written guide + the generated reference into one HTML file.

Usage:  python3 docs/gen_docs.py            (writes docs/index.html and docs/api.json)
"""
import base64
import html
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "library", "borealis", "library")
INC = os.path.join(LIB, "include", "borealis")
SRC = os.path.join(LIB, "lib")
OUT_DIR = os.path.join(ROOT, "docs")

HEADER_FILES = [os.path.join(INC, "core", "view.hpp"), os.path.join(INC, "core", "box.hpp")]
for sub in ("views", "views/cells", "views/widgets"):
    d = os.path.join(INC, sub)
    HEADER_FILES += sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".hpp"))

CPP_FILES = [os.path.join(SRC, "core", "view.cpp"), os.path.join(SRC, "core", "box.cpp")]
for sub in ("views", "views/cells", "views/widgets"):
    d = os.path.join(SRC, sub)
    CPP_FILES += sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".cpp"))

# ---------------------------------------------------------------- header parsing

DOC_LINE = re.compile(r"^\s*(?:/\*\*|\*/?|//)\s?(.*?)\s*$")
CLASS_RE = re.compile(r"^\s*(class|struct)\s+(\w+)\s*(?::\s*public\s+([\w:<>]+))?\s*$")
ACCESS_RE = re.compile(r"^\s*(public|protected|private)\s*:\s*$")
EVENT_GETTER_RE = re.compile(r"Event<([^>]*)>\s*\*\s*(get\w*Event)\s*\(")
EVENT_FIELD_RE = re.compile(r"^\s*Event<([^>]*)>\s+(\w+)\s*;")
SKIP_PREFIX = ("friend ", "using ", "template", "#", "BRLS_", "enum ", "typedef struct")


def clean_doc(lines):
    out = []
    for l in lines:
        m = DOC_LINE.match(l)
        t = m.group(1) if m else l.strip()
        if t in ("/**", "*/", "/*"):
            continue
        out.append(t)
    while out and not out[0]:
        out.pop(0)
    while out and not out[-1]:
        out.pop()
    return " ".join(out).replace("  ", " ").strip()


def parse_header(path):
    classes = {}
    lines = open(path, encoding="utf-8").read().split("\n")
    i, n = 0, len(lines)
    pending_doc = []
    cur = None          # current class dict
    depth = 0           # brace depth inside class
    access = "private"
    while i < n:
        line = lines[i]
        s = line.strip()
        if cur is None:
            m = CLASS_RE.match(line)
            if m and i + 1 < n and lines[i + 1].strip() == "{":
                kind, name, base = m.groups()
                cur = {"name": name, "kind": kind, "base": base, "file": os.path.relpath(path, LIB),
                       "line": i + 1, "doc": clean_doc(pending_doc), "methods": [], "fields": [], "events": []}
                classes[name] = cur
                access = "public" if kind == "struct" else "private"
                depth = 0
                pending_doc = []
                i += 2
                depth = 1
                continue
            if s.startswith(("//", "/**", "*", "*/")):
                pending_doc.append(line)
            elif s == "":
                pending_doc = []
            else:
                pending_doc = []
            i += 1
            continue

        # inside a class body
        if s.startswith(("//", "/**", "/*", "*", "*/")):
            pending_doc.append(line)
            i += 1
            continue
        if s == "":
            pending_doc = []
            i += 1
            continue
        am = ACCESS_RE.match(line)
        if am:
            access = am.group(1)
            pending_doc = []
            i += 1
            continue
        # gather a full declaration (may span lines until ; or {)
        decl = s
        j = i
        while not re.search(r"[;{]\s*$", decl) and j + 1 < n and depth == 1:
            j += 1
            decl += " " + lines[j].strip()
            if len(decl) > 600:
                break
        # brace tracking on the raw span
        span = "\n".join(lines[i:j + 1])
        opened = span.count("{")
        closed = span.count("}")
        if depth == 1 and access == "public" and not decl.startswith(SKIP_PREFIX):
            d = decl.rstrip()
            if d.endswith("{"):
                d = d[:-1].rstrip()
            if d.endswith(";"):
                d = d[:-1].rstrip()
            if "(" in d and not d.startswith("return"):
                name_m = re.search(r"(operator\S*|~?\w+)\s*\(", d)
                mname = name_m.group(1) if name_m else d
                entry = {"sig": re.sub(r"\s+", " ", d), "name": mname, "doc": clean_doc(pending_doc)}
                em = EVENT_GETTER_RE.search(d)
                if em:
                    cur["events"].append({"name": em.group(2), "payload": em.group(1).strip() or "void", "doc": entry["doc"]})
                else:
                    cur["methods"].append(entry)
            elif re.match(r"^[\w:<>*&,\s]+\s[*&]?\w+(\s*=\s*[^;]+)?$", d) and " " in d:
                fm = EVENT_FIELD_RE.match(d + ";")
                if fm:
                    cur["events"].append({"name": fm.group(2), "payload": fm.group(1).strip() or "void", "doc": clean_doc(pending_doc)})
                else:
                    cur["fields"].append({"sig": re.sub(r"\s+", " ", d), "doc": clean_doc(pending_doc)})
        pending_doc = []
        # skip inline bodies
        if opened > closed:
            k = j
            bal = opened - closed
            while bal > 0 and k + 1 < n:
                k += 1
                bal += lines[k].count("{") - lines[k].count("}")
            j = k
        elif closed > opened:
            depth -= (closed - opened)
        if depth <= 0 or s.startswith("};"):
            cur = None
        i = j + 1
    return classes


# ---------------------------------------------------------------- cpp parsing

REG_RE = re.compile(r'register(Float|String|Bool|Color|FilePath|Percentage|Auto)XMLAttribute\(\s*"([^"]+)"')
ENUM_RE = re.compile(r'BRLS_REGISTER_ENUM_XML_ATTRIBUTE\(\s*"([^"]+)"\s*,\s*([\w:<>*& ]+?)\s*,\s*[^,]+,\s*(\{.*?\})\s*\)', re.S)
FWD_RE = re.compile(r'forwardXMLAttribute\(\s*"([^"]+)"\s*,\s*([\w>\-]+)(?:\s*,\s*"([^"]+)")?\s*\)')
FUNC_RE = re.compile(r"^[\w:<>*&\s]*?\b(\w+)::(~?\w+)\s*\(", re.M)
XMLVIEW_RE = re.compile(r'registerXMLView\("([^"]+)",\s*(\w+)::create\)')

TYPE_LABEL = {
    "Float": "number", "String": "string", "Bool": "bool", "Color": "color",
    "FilePath": "path", "Percentage": "percent", "Auto": "auto",
}


def owner_at(text, pos, funcs):
    owner = None
    for fpos, cls in funcs:
        if fpos <= pos:
            owner = cls
        else:
            break
    return owner


def parse_cpp(path, attrs):
    text = open(path, encoding="utf-8").read()
    funcs = [(m.start(), m.group(1)) for m in FUNC_RE.finditer(text)]
    for m in REG_RE.finditer(text):
        cls = owner_at(text, m.start(), funcs)
        a = attrs.setdefault(cls, {}).setdefault(m.group(2), {"types": [], "values": None, "forwarded": None})
        t = TYPE_LABEL[m.group(1)]
        if t not in a["types"]:
            a["types"].append(t)
    for m in ENUM_RE.finditer(text):
        cls = owner_at(text, m.start(), funcs)
        vals = re.findall(r'"([^"]+)"', m.group(3))
        a = attrs.setdefault(cls, {}).setdefault(m.group(1), {"types": [], "values": None, "forwarded": None})
        a["types"].append("enum")
        a["values"] = vals
        a["enum"] = m.group(2)
    for m in FWD_RE.finditer(text):
        cls = owner_at(text, m.start(), funcs)
        a = attrs.setdefault(cls, {}).setdefault(m.group(1), {"types": [], "values": None, "forwarded": None})
        a["forwarded"] = {"target": m.group(2).replace("this->", ""), "as": m.group(3) or m.group(1)}


def parse_xml_tags():
    text = open(os.path.join(SRC, "core", "application.cpp"), encoding="utf-8").read()
    return {cls: tag for tag, cls in XMLVIEW_RE.findall(text)}


# ---------------------------------------------------------------- collect

classes = {}
for h in HEADER_FILES:
    classes.update(parse_header(h))
attrs = {}
for c in CPP_FILES:
    parse_cpp(c, attrs)
tags = parse_xml_tags()

for name, c in classes.items():
    c["tag"] = tags.get(name)
    c["attrs"] = attrs.get(name, {})
    chain, b = [], c["base"]
    while b in classes:
        chain.append(b)
        b = classes[b]["base"]
    c["chain"] = chain


def resolve_forwarded(c):
    # for forwarded attributes, find the target's attribute type from a known class
    for aname, a in c["attrs"].items():
        if a["forwarded"] and not a["types"]:
            tgt = a["forwarded"]["as"]
            for other in classes.values():
                if tgt in other["attrs"] and other["attrs"][tgt]["types"]:
                    a["types"] = list(other["attrs"][tgt]["types"])
                    a["values"] = other["attrs"][tgt]["values"]
                    break


for c in classes.values():
    resolve_forwarded(c)

# ---------------------------------------------------------------- manual curation (order, groups, screenshots, blurbs)

GROUPS = [
    ("Base classes", ["View", "Box", "Padding"]),
    ("Frames & navigation", ["AppletFrame", "TabFrame", "Sidebar", "SidebarItem", "SidebarSeparator", "BottomBar", "Hints", "Hint"]),
    ("Scrolling & lists", ["ScrollingFrame", "HScrollingFrame", "RecyclerFrame", "RecyclerCell", "RecyclerHeader", "RecyclerDataSource", "IndexPath"]),
    ("Controls", ["Button", "Slider", "CheckBox", "Label", "Image", "Rectangle", "Header", "ProgressSpinner"]),
    ("Cells", ["DetailCell", "RadioCell", "BooleanCell", "SelectorCell", "InputCell", "InputNumericCell", "SliderCell"]),
    ("Overlays", ["Dialog", "Dropdown", "EditTextDialog"]),
    ("Widgets", ["AccountWidget", "BatteryWidget", "WirelessWidget"]),
]

SHOTS = {
    "View": ("02-layout.png", "Every rectangle here is a View; the coloured squares are plain Rectangle views laid out by their parent Box."),
    "Box": ("02-layout.png", "The demo's Layout tab: rows are Boxes with axis=\"row\" and different alignItems / justifyContent values."),
    "AppletFrame": ("15-about.png", "AppletFrame draws the title bar (icon + title), the content area, and the bottom hint bar."),
    "TabFrame": ("01-components-top.png", "TabFrame = Sidebar on the left + one content view per tab on the right."),
    "Sidebar": ("crop-sidebar.png", "Sidebar items with a separator; the active item is drawn with the accent colour."),
    "Hints": ("crop-hints.png", "Bottom-right \"B Back / A OK\" hints are rendered by Hints from the focused view's registered actions."),
    "BottomBar": ("crop-bottombar.png", "Battery, wireless and clock widgets in the BottomBar at the lower left."),
    "ScrollingFrame": ("05-scroll.png", "A ScrollingFrame with a long Label; scrolls with the stick / wheel / drag and follows focus."),
    "HScrollingFrame": ("crop-hscroll.png", "The row of image tiles under \"HScroll\" is an HScrollingFrame."),
    "RecyclerFrame": ("06-recycler.png", "RecyclerFrame reusing one cell layout for all 151 rows of the Pokedex."),
    "RecyclerCell": ("crop-recycler-cells.png", "Cells are ordinary Boxes inflated from XML (icon + label here) and recycled by reuse identifier."),
    "Button": ("crop-button.png", "style=\"primary\" (filled accent) and style=\"highlight\" (accent text) buttons."),
    "Slider": ("crop-slider.png", "Sliders bound to labels in the Transform tab."),
    "Label": ("04-text.png", "Label test: width / height / alignment / singleLine are live-editable."),
    "Image": ("01-components-top.png", "Image tiles with scalingType variations further down the same tab."),
    "Header": ("crop-header.png", "\"HScroll\" and \"Buttons\" section titles are Header views."),
    "DetailCell": ("crop-detail.png", "The IP / DNS rows are DetailCells: a title with a right-aligned value."),
    "RadioCell": ("crop-radio.png", "RadioCell while unselected and focused; a check mark appears on the right once setSelected(true) is called."),
    "BooleanCell": ("crop-boolean.png", "BooleanCell in the Off state; pressing A flips it to On and fires the callback."),
    "SelectorCell": ("crop-selector.png", "A SelectorCell shows the current choice; pressing it opens a Dropdown (see below)."),
    "InputCell": ("crop-input.png", "InputCell shows the current string; pressing it opens the platform keyboard (EditTextDialog on desktop)."),
    "InputNumericCell": ("crop-input-numeric.png", "\"Input number\" (2448) is an InputNumericCell."),
    "SliderCell": ("crop-slidercell.png", "\"Brightness\" is a SliderCell with its value label on the right."),
    "Dialog": ("11-dialog.png", "A Dialog with a single OK button over a dimmed backdrop."),
    "Dropdown": ("10-dropdown.png", "Dropdown activity pushed by a SelectorCell; the current value carries a check mark."),
    "EditTextDialog": ("12-edit-text-dialog.png", "Desktop text entry overlay with Back / OK hints."),
    "BatteryWidget": ("crop-bottombar.png", "Battery icon in the bottom bar."),
    "WirelessWidget": ("crop-bottombar.png", "Wireless icon in the bottom bar."),
}

# Short, human blurbs where the source has no class-level comment.
BLURB = {
    "View": "Root of every UI element. Owns the Yoga layout node, focus/actions, animations and the full set of common XML attributes below. You rarely instantiate View directly — subclass it (override draw()) or compose Boxes.",
    "Box": "A View that holds children and lays them out with flexbox. All containers (frames, cells, tabs) derive from Box. Inflate one from XML with inflateFromXMLRes() and bind children by id.",
    "Padding": "Empty spacer view. Use it inside a Box to reserve space; has no visual.",
    "AppletFrame": "Full-screen chrome for an Activity: header (icon + title), content slot, and bottom bar with hints. Push one per screen.",
    "TabFrame": "AppletFrame content that pairs a Sidebar with a set of tab views. Add <brls:Tab> children in XML; only the active tab is attached.",
    "Sidebar": "Vertical list of SidebarItems with optional separators. Drives TabFrame but can be used stand-alone.",
    "SidebarItem": "One entry in a Sidebar. Highlights with the accent bar when active.",
    "SidebarSeparator": "Thin horizontal rule between sidebar groups.",
    "BottomBar": "Lower-left status strip: battery, wireless, clock widgets.",
    "Hints": "Renders the bottom-right button hints for every non-hidden action registered on the focused view (and its parents).",
    "Hint": "A single button glyph + label pair inside Hints.",
    "ScrollingFrame": "Vertically scrolling container for one child. Keeps the focused child in view and supports stick/wheel/touch drag.",
    "HScrollingFrame": "Horizontal counterpart of ScrollingFrame.",
    "RecyclerFrame": "Virtualised list. Provide a RecyclerDataSource that returns cells by IndexPath; cells are registered by reuse identifier and recycled as they scroll off-screen.",
    "RecyclerCell": "Base cell for RecyclerFrame. Give it a reuseIdentifier and populate it in the data source.",
    "RecyclerHeader": "Section header cell for RecyclerFrame.",
    "RecyclerDataSource": "Interface your list model implements: number of sections/rows, cell factory, selection callback, heights.",
    "IndexPath": "Section / row address of a cell.",
    "Button": "Focusable push button. Pick a style (primary, highlight, bordered, borderless, default) and hook registerClickAction().",
    "Slider": "Horizontal slider 0..1 driven by left/right, drag or wheel. Subscribe to getProgressEvent().",
    "CheckBox": "Check-mark glyph used by RadioCell; usable on its own.",
    "Label": "Single- or multi-line text with font, size, colour, alignment, animated ticker for overflow and i18n text via @i18n/.",
    "Image": "Draws a texture from @res/ path, memory or nanovg image handle with several scaling modes.",
    "Rectangle": "Solid colour block. Handy as a debug placeholder or as a background layer.",
    "Header": "Section title with the accent bar on the left and an optional subtitle.",
    "ProgressSpinner": "Indeterminate spinner (the OS-style rotating bars).",
    "DetailCell": "List row with a title, optional subtitle and a right-aligned detail value. Base class for the other cells.",
    "RadioCell": "DetailCell with a CheckBox on the right. Toggle via setSelected().",
    "BooleanCell": "On/Off switch row. init(title, initial, callback).",
    "SelectorCell": "Row that opens a Dropdown of string choices; fires the index picked.",
    "InputCell": "Row that opens the system keyboard to edit a string.",
    "InputNumericCell": "Row that opens the numeric keyboard to edit a long.",
    "SliderCell": "Row with an inline Slider and a value label.",
    "Dialog": "Modal box with a message (or any custom view) and up to three buttons. open() pushes it, close() pops.",
    "Dropdown": "Full-screen selection activity used by SelectorCell. Construct with title, values, callback and the current index.",
    "EditTextDialog": "Desktop / no-IME fallback for text input; emits submit / cancel / backspace / clipboard events.",
    "AccountWidget": "Avatar + user name widget for the bottom bar.",
    "BatteryWidget": "Battery level icon for the bottom bar.",
    "WirelessWidget": "Wi-Fi signal icon for the bottom bar.",
}


# ---------------------------------------------------------------- crops (macOS sips; skipped if unavailable)
# name: (source, x, y, w, h)  in 1280x720 logical pixels
CROPS = {
    "crop-button.png": ("01-components-top.png", 460, 512, 790, 120),
    "crop-header.png": ("01-components-top.png", 460, 190, 790, 50),
    "crop-hscroll.png": ("01-components-top.png", 460, 190, 790, 210),
    "crop-sidebar.png": ("01-components-top.png", 25, 96, 385, 545),
    "crop-bottombar.png": ("01-components-top.png", 30, 648, 400, 72),
    "crop-hints.png": ("01-components-top.png", 990, 648, 280, 72),
    "crop-slider.png": ("03-transform.png", 465, 205, 310, 280),
    "crop-radio.png": ("09-settings-cells.png", 470, 118, 790, 72),
    "crop-boolean.png": ("09-settings-cells.png", 470, 192, 790, 68),
    "crop-selector.png": ("09-settings-cells.png", 470, 258, 790, 72),
    "crop-input.png": ("09-settings-cells.png", 470, 328, 790, 72),
    "crop-input-numeric.png": ("09-settings-cells.png", 470, 398, 790, 72),
    "crop-detail.png": ("09-settings-cells.png", 470, 468, 790, 142),
    "crop-slidercell.png": ("13-settings-more.png", 470, 540, 790, 72),
    "crop-recycler-cells.png": ("06-recycler.png", 460, 140, 800, 300),
    "crop-appletframe-header.png": ("15-about.png", 0, 0, 1280, 95),
}


def make_crops():
    import shutil, subprocess
    if not shutil.which("sips"):
        return
    sdir = os.path.join(OUT_DIR, "screenshots")
    for out, (src, x, y, w, h) in CROPS.items():
        subprocess.run(["sips", "-c", str(h), str(w), "--cropOffset", str(y), str(x),
                        os.path.join(sdir, src), "--out", os.path.join(sdir, out)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


make_crops()

# ---------------------------------------------------------------- html helpers


def esc(s):
    return html.escape(s or "", quote=True)


def type_badge(a):
    parts = []
    for t in a["types"]:
        if t == "enum" and a.get("values"):
            parts.append("enum")
        else:
            parts.append(t)
    return " · ".join(dict.fromkeys(parts)) or "?"


def render_attrs(c):
    own = c["attrs"]
    if not own:
        return "<p class='muted'>No attributes of its own.</p>"
    rows = []
    for name in sorted(own):
        a = own[name]
        extra = ""
        if a.get("values"):
            extra = " ".join(f"<code class='val'>{esc(v)}</code>" for v in a["values"])
        if a["forwarded"]:
            fw = a["forwarded"]
            extra += f"<span class='muted'> → forwarded to <code>{esc(fw['target'])}</code>" + (f" as <code>{esc(fw['as'])}</code>" if fw['as'] != name else "") + "</span>"
        rows.append(f"<tr><td><code>{esc(name)}</code></td><td><span class='type'>{esc(type_badge(a))}</span></td><td>{extra}</td></tr>")
    return "<table class='attrs'><thead><tr><th>Attribute</th><th>Type</th><th>Values / notes</th></tr></thead><tbody>" + "".join(rows) + "</tbody></table>"


def render_methods(c):
    if not c["methods"] and not c["fields"] and not c["events"]:
        return ""
    out = []
    if c["events"]:
        out.append("<h4>Events</h4><table class='methods'><tbody>")
        for e in c["events"]:
            out.append(f"<tr><td><code>Event&lt;{esc(e['payload'])}&gt;</code> <code class='fn'>{esc(e['name'])}</code></td><td>{esc(e['doc'])}</td></tr>")
        out.append("</tbody></table>")
    if c["methods"]:
        out.append("<h4>Public methods</h4><table class='methods'><tbody>")
        for m in c["methods"]:
            out.append(f"<tr><td><code>{esc(m['sig'])}</code></td><td>{esc(m['doc'])}</td></tr>")
        out.append("</tbody></table>")
    if c["fields"]:
        out.append("<h4>Public members</h4><table class='methods'><tbody>")
        for f in c["fields"]:
            out.append(f"<tr><td><code>{esc(f['sig'])}</code></td><td>{esc(f['doc'])}</td></tr>")
        out.append("</tbody></table>")
    return "".join(out)


def render_class(name):
    c = classes.get(name)
    if not c:
        return ""
    tag = f"<code class='tag'>&lt;{esc(c['tag'])}&gt;</code>" if c["tag"] else "<span class='muted'>no XML tag — C++ only</span>"
    chain = " → ".join(f"<a href='#{esc(b)}'>{esc(b)}</a>" for b in c["chain"]) if c["chain"] else ""
    shot = ""
    if name in SHOTS:
        f, cap = SHOTS[name]
        shot = f"<figure><img src='screenshots/{f}' alt='{esc(name)} in the demo app' loading='lazy'><figcaption>{esc(cap)}</figcaption></figure>"
    blurb = BLURB.get(name) or c["doc"]
    inherits = ""
    if c["chain"]:
        inherits = f"<p class='inherit'>Also accepts every attribute of {chain}.</p>"
    return f"""
<article class='cls' id='{esc(name)}'>
  <header>
    <h3>{esc(name)}</h3>
    <div class='meta'>{tag} <span class='src'>{esc(c['file'])}:{c['line']}</span>{(' <span class="src">extends ' + chain + '</span>') if chain else ''}</div>
  </header>
  <p class='blurb'>{esc(blurb)}</p>
  {shot}
  <h4>XML attributes</h4>
  {render_attrs(c)}
  {inherits}
  {render_methods(c)}
</article>"""


# ---------------------------------------------------------------- page

def read_snippet(rel, start=None, end=None):
    p = os.path.join(ROOT, rel)
    lines = open(p, encoding="utf-8").read().split("\n")
    if start is not None:
        lines = lines[start - 1:end]
    return "\n".join(lines)


# ---------------------------------------------------------------- style / theme tokens

STYLE_ENTRY_RE = re.compile(r'\{\s*"([\w/]+)"\s*,\s*([\d.]+f?)\s*\}')
THEME_ENTRY_RE = re.compile(r'\{\s*"([\w/]+)"\s*,\s*nvgRGBA?\(([^)]*)\)\s*\}')
GROUP_COMMENT_RE = re.compile(r'^\s*//\s*(.+?)\s*$')


def extract_grouped_entries(text, entry_re):
    """Walks a `Values = { ... };` initializer list, grouping entries under
    the nearest preceding `// Comment` line (blank lines don't reset the group)."""
    groups = []
    current_group = None
    current_entries = []
    for line in text.split("\n"):
        entry = entry_re.search(line)
        if entry:
            if current_group is None:
                current_group = "General"
            current_entries.append((entry.group(1), entry.group(2)))
            continue
        comment = GROUP_COMMENT_RE.match(line)
        if comment:
            if current_entries:
                groups.append((current_group, current_entries))
            current_group = comment.group(1)
            current_entries = []
    if current_entries:
        groups.append((current_group, current_entries))
    return groups


def block_between(text, start_marker):
    start = text.index(start_marker)
    end = text.index("\n};", start)
    return text[start:end]


def style_to_int(v):
    return round(float(v.rstrip("f")), 3)


def rgba_hex(args):
    parts = [int(p.strip()) for p in args.split(",")]
    while len(parts) < 4:
        parts.append(255)
    r, g, b, a = parts
    return f"#{r:02x}{g:02x}{b:02x}" + (f"{a:02x}" if a != 255 else "")


style_src = read_snippet("library/borealis/library/lib/core/style.cpp")
style_groups = extract_grouped_entries(block_between(style_src, "static StyleValues styleValues = {"), STYLE_ENTRY_RE)

theme_src = read_snippet("library/borealis/library/lib/core/theme.cpp")
light_theme_groups = extract_grouped_entries(block_between(theme_src, "static ThemeValues lightThemeValues = {"), THEME_ENTRY_RE)


# ---------------------------------------------------------------- switch icon glyphs

SWITCH_ICON_CASE_RE = re.compile(r'case\s+(BUTTON_\w+)\s*:\s*\n\s*return\s+"\\u([0-9A-Fa-f]{4})"\s*;')
SWITCH_ICON_DEFAULT_RE = re.compile(r'default\s*:\s*\n\s*return\s+"\\u([0-9A-Fa-f]{4})"\s*;')


def extract_switch_icons():
    """Pulls the ControllerButton -> PUA codepoint table out of Hint::getKeyIcon,
    the only place in the library that enumerates FONT_SWITCH_ICONS glyphs."""
    src = read_snippet("library/borealis/library/lib/views/hint.cpp")
    start = src.index("std::string Hint::getKeyIcon(ControllerButton button, bool ignoreKeysSwap)")
    end = src.index("\n}\n", start)
    body = src[start:end]
    rows = [(m.group(1), m.group(2).upper()) for m in SWITCH_ICON_CASE_RE.finditer(body)]
    default = SWITCH_ICON_DEFAULT_RE.search(body)
    if default:
        rows.append(("default (unmapped button)", default.group(1).upper()))
    return rows


switch_icons = extract_switch_icons()
switch_icons_font_b64 = base64.b64encode(
    open(os.path.join(ROOT, "resources", "font", "switch_icons.ttf"), "rb").read()
).decode()


ICON_BITMAP_CPP = 'appletFrame->setIcon("@res/icon/vkplay.png");'
ICON_GLYPH_CPP = 'icon->setText("\\uE0E0"); // BUTTON_A glyph, see table below'


def render_switch_icons_table():
    rows = "".join(
        f"<tr><td><code>{esc(name)}</code></td><td><code>\\u{cp}</code></td><td class='icon-glyph'>&#x{cp};</td></tr>"
        for name, cp in switch_icons
    )
    return f"<table class='attrs'><thead><tr><th>ControllerButton</th><th>Codepoint</th><th>Glyph</th></tr></thead><tbody>{rows}</tbody></table>"


def render_style_table():
    parts = []
    for group, entries in style_groups:
        rows = "".join(f"<tr><td><code>@style/{esc(name)}</code></td><td>{style_to_int(val)}</td></tr>" for name, val in entries)
        parts.append(f"<h4>{esc(group)}</h4><table class='attrs'><thead><tr><th>Token</th><th>Default value</th></tr></thead><tbody>{rows}</tbody></table>")
    return "".join(parts)


def render_theme_table():
    parts = []
    for group, entries in light_theme_groups:
        rows = "".join(
            f"<tr><td><code>@theme/{esc(name)}</code></td>"
            f"<td><span style='display:inline-block;width:12px;height:12px;border-radius:3px;background:{rgba_hex(args)};vertical-align:middle;margin-right:6px;border:1px solid #0003'></span>{rgba_hex(args)}</td></tr>"
            for name, args in entries
        )
        parts.append(f"<h4>{esc(group)}</h4><table class='attrs'><thead><tr><th>Token</th><th>Light value</th></tr></thead><tbody>{rows}</tbody></table>")
    return "".join(parts)


CSS = """
:root{--bg:#f6f7f9;--surface:#fff;--surface-2:#eef0f4;--border:#d9dde5;--text:#1c2029;--dim:#5e6675;--faint:#8b93a3;
--accent:#2d4be0;--accent-soft:#e8ecff;--code-bg:#14171d;--code:#dbe1ea;--tagbg:#e6f7f2;--tag:#0a7a5c;--shadow:0 1px 2px rgba(0,0,0,.04),0 6px 20px -10px rgba(0,0,0,.15)}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#13151a;--surface:#1a1d24;--surface-2:#20242c;--border:#30353f;--text:#eaeef5;--dim:#a3abbb;--faint:#6d7585;
--accent:#00e5b0;--accent-soft:#0f2e28;--code-bg:#0c0e12;--code:#dbe1ea;--tagbg:#0f2e28;--tag:#4ff0c8;--shadow:0 1px 2px rgba(0,0,0,.3),0 10px 30px -14px rgba(0,0,0,.6)}}
:root[data-theme=dark]{--bg:#13151a;--surface:#1a1d24;--surface-2:#20242c;--border:#30353f;--text:#eaeef5;--dim:#a3abbb;--faint:#6d7585;
--accent:#00e5b0;--accent-soft:#0f2e28;--code-bg:#0c0e12;--code:#dbe1ea;--tagbg:#0f2e28;--tag:#4ff0c8;--shadow:0 1px 2px rgba(0,0,0,.3),0 10px 30px -14px rgba(0,0,0,.6)}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.6 'IBM Plex Sans',system-ui,sans-serif}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
code,pre{font-family:'JetBrains Mono',ui-monospace,monospace;font-size:.86em;font-variant-ligatures:none}
code{background:var(--surface-2);padding:.1em .35em;border-radius:4px}
pre{background:var(--code-bg);color:var(--code);padding:14px 16px;border-radius:8px;overflow-x:auto;line-height:1.55;margin:12px 0 18px}
pre code{background:none;padding:0;color:inherit;font-size:12.5px}
h1,h2,h3,h4{font-weight:600;line-height:1.25;margin:0;text-wrap:balance}
h1{font-size:34px}h2{font-size:24px;margin:56px 0 8px;padding-top:16px;border-top:1px solid var(--border)}h3{font-size:19px}h4{font-size:12px;text-transform:uppercase;letter-spacing:.09em;color:var(--faint);margin:22px 0 8px}
p{margin:0 0 12px;max-width:72ch}.muted{color:var(--faint)}
.wrap{display:flex;min-height:100vh}
nav{width:250px;flex:none;position:sticky;top:0;height:100vh;overflow-y:auto;padding:22px 14px;border-right:1px solid var(--border);background:var(--surface);font-size:13px}
nav .brand{font-weight:700;font-size:16px;padding:0 8px 14px}nav .brand small{display:block;color:var(--faint);font-weight:500;font-size:11px}
nav .g{margin:12px 0 4px;padding:0 8px;font-size:10.5px;text-transform:uppercase;letter-spacing:.1em;color:var(--faint);font-weight:600}
nav a{display:block;padding:4px 8px;border-radius:5px;color:var(--dim)}nav a:hover{background:var(--surface-2);color:var(--text);text-decoration:none}
nav a.sub{padding-left:18px;font-family:'JetBrains Mono';font-size:12px}
main{flex:1;min-width:0;padding:44px clamp(20px,4vw,60px) 100px;max-width:1100px}
.lede{color:var(--dim);font-size:17px;max-width:66ch}
.kv{display:grid;grid-template-columns:max-content 1fr;gap:6px 18px;margin:14px 0 20px;font-size:14px}.kv b{font-weight:600}
figure{margin:18px 0 22px}figure img{max-width:100%;border-radius:10px;border:1px solid var(--border);box-shadow:var(--shadow);display:block}
figcaption{font-size:12.5px;color:var(--faint);margin-top:8px}
.cls{margin:36px 0 0;padding:26px 0 0;border-top:1px dashed var(--border)}
.cls header{display:flex;flex-wrap:wrap;align-items:baseline;gap:12px}
.meta{font-size:12.5px;color:var(--faint)}.src{font-family:'JetBrains Mono';font-size:11px}
code.tag{background:var(--tagbg);color:var(--tag);font-weight:600}
.blurb{margin-top:8px}
table{border-collapse:collapse;width:100%;font-size:13.5px;margin:6px 0 10px}
th{text-align:left;font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:var(--faint);font-weight:600;padding:6px 8px;border-bottom:1px solid var(--border)}
td{padding:6px 8px;border-bottom:1px solid var(--border);vertical-align:top}
.type{font-family:'JetBrains Mono';font-size:11.5px;color:var(--dim);white-space:nowrap}
code.val{background:var(--accent-soft);color:var(--accent);margin-right:4px}
.methods td:first-child{width:56%}.methods code{white-space:pre-wrap}code.fn{color:var(--accent)}
.inherit{font-size:13px;color:var(--dim)}
.two{display:grid;grid-template-columns:1fr 1fr;gap:16px}.two>*{min-width:0}main{overflow-x:hidden}table{table-layout:fixed}.attrs td:first-child{width:22%}.attrs td:nth-child(2){width:14%}td code{white-space:pre-wrap;word-break:break-word}@media(max-width:900px){.two{grid-template-columns:1fr}nav{display:none}}
.steps{counter-reset:s;padding:0;margin:0 0 20px;list-style:none}.steps li{counter-increment:s;position:relative;padding-left:36px;margin-bottom:14px}
.steps li::before{content:counter(s);position:absolute;left:0;top:2px;width:24px;height:24px;border-radius:50%;background:var(--accent);color:var(--bg);font-size:12px;font-weight:700;display:flex;align-items:center;justify-content:center}
.note{border-left:3px solid var(--accent);background:var(--surface);padding:10px 14px;border-radius:0 8px 8px 0;margin:14px 0;font-size:14px}
"""

CSS += f"""
@font-face{{font-family:'BorealisSwitchIcons';src:url(data:font/ttf;base64,{switch_icons_font_b64}) format('truetype')}}
.icon-glyph{{font-family:'BorealisSwitchIcons';font-size:20px}}
"""

QUICKSTART_MAIN = """#include <borealis.hpp>

class MainActivity : public brls::Activity
{
  public:
    // Inflate the activity from resources/xml/activity/main.xml
    CONTENT_FROM_XML_RES("activity/main.xml");
};

int main(int argc, char* argv[])
{
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);

    if (!brls::Application::init())
        return EXIT_FAILURE;

    brls::Application::createWindow("My app");   // window title (i18n keys work too)
    brls::Application::setGlobalQuit(true);       // START button quits

    brls::Application::pushActivity(new MainActivity());

    while (brls::Application::mainLoop());
    return EXIT_SUCCESS;
}"""

QUICKSTART_XML = """<brls:AppletFrame iconInterpolation="linear">
    <brls:TabFrame title="My app" icon="@res/img/icon.png">

        <brls:Tab label="Home">
            <brls:Box axis="column" padding="30" width="auto" height="auto">
                <brls:Label text="Hello from Borealis" fontSize="24" marginBottom="20" />
                <brls:Button id="hello" style="primary" text="Press A" />
            </brls:Box>
        </brls:Tab>

        <brls:Separator />

        <brls:Tab label="Settings">
            <brls:View xml="@res/xml/tabs/settings.xml" />
        </brls:Tab>

    </brls:TabFrame>
</brls:AppletFrame>"""

BIND_CPP = """// header
class HomeTab : public brls::Box
{
  public:
    HomeTab();
    static brls::View* create();          // factory used by registerXMLView

  private:
    BRLS_BIND(brls::Button, hello, "hello");   // member `hello` <- view with id="hello"
    BRLS_BIND(brls::Label,  title, "title");
};

// source
HomeTab::HomeTab()
{
    this->inflateFromXMLRes("xml/tabs/home.xml");

    hello->registerClickAction([this](brls::View* v) {
        title->setText("Pressed!");
        return true;                      // action consumed -> click sound plays
    });
}

brls::View* HomeTab::create() { return new HomeTab(); }

// in main(): expose the class to XML as <HomeTab />
brls::Application::registerXMLView("HomeTab", HomeTab::create);"""

CELLS_CPP = read_snippet("library/borealis/demo/src/tab/settings_tab.cpp", 43, 60)
DIALOG_CPP = """auto dialog = new brls::Dialog("Delete this save?");
dialog->addButton("Cancel", [] { });
dialog->addButton("Delete", [dialog] {
    doDelete();
    dialog->close();
});
dialog->setCancelable(true);   // B closes it
dialog->open();"""

RECYCLER_CPP = """class PokemonSource : public brls::RecyclerDataSource
{
  public:
    int numberOfRows(brls::RecyclerFrame* r, int section) override { return pokemons.size(); }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* r, brls::IndexPath index) override
    {
        auto* cell = (PokemonCell*)r->dequeueReusableCell("Cell");
        cell->label->setText(pokemons[index.row].name);
        return cell;
    }

    void didSelectRowAt(brls::RecyclerFrame* r, brls::IndexPath index) override
    {
        brls::Application::pushActivity(new PokemonActivity(pokemons[index.row]));
    }
};

// in the tab constructor
recycler->registerCell("Cell", [] { return PokemonCell::create(); });
recycler->setDataSource(new PokemonSource());"""

ACTIONS_CPP = """// Any View can own gamepad actions. The hint bar picks them up automatically.
view->registerAction("Delete", brls::BUTTON_X, [](brls::View* v) {
    return true;                              // consumed
});
view->registerClickAction(...);              // shortcut for BUTTON_A / "OK"
view->registerAction("Hidden", brls::BUTTON_Y, cb, /*hidden=*/true);

// Focus & lifecycle hooks (virtual, override in subclasses)
void onFocusGained() override;
void onFocusLost() override;
void onLayout() override;                    // after Yoga computed the frame
void draw(NVGcontext* vg, float x, float y, float w, float h,
          brls::Style style, brls::FrameContext* ctx) override;"""


BUILD_CMD = "cmake -B build -DPLATFORM_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release\ncmake --build build -j\n./build/borealis_demo          # resources/ is copied next to the binary"

WIREFRAME_CPP = """// Outline a single view (frame in blue, margins/padding in green):
view->setWireframeEnabled(true);

// Or via XML, on any view:
// <brls:Box wireframe="true" ...>

// Outline an entire subtree from C++:
void enableWireframeRecursive(brls::View* view)
{
    view->setWireframeEnabled(true);
    if (auto* box = dynamic_cast<brls::Box*>(view))
        for (brls::View* child : box->getChildren())
            enableWireframeRecursive(child);
}

// e.g. in Activity::onContentAvailable(), on the activity's content view."""


def code(s, lang="cpp"):
    return f"<pre><code class='lang-{lang}'>{esc(s)}</code></pre>"


BUILD_SH = code(BUILD_CMD, "bash")
nav_items = []
for gname, members in GROUPS:
    present = [m for m in members if m in classes]
    if present:
        nav_items.append(f"<div class='g'>{esc(gname)}</div>" + "".join(f"<a class='sub' href='#{m}'>{m}</a>" for m in present))

ref_sections = []
for gname, members in GROUPS:
    body = "".join(render_class(m) for m in members if m in classes)
    if body:
        ref_sections.append(f"<h2 id='g-{re.sub(r'[^a-z]', '-', gname.lower())}'>{esc(gname)}</h2>{body}")

undocumented = [n for n in classes if not any(n in m for _, m in GROUPS)]

VIEW_COMMON = classes["View"]["attrs"]
BOX_COMMON = classes["Box"]["attrs"]

page = f"""<meta charset="utf-8"><title>Borealis Developer Guide</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:wght@400;500;600;700&family=JetBrains+Mono:wght@400;500;600&display=swap">
<style>{CSS}</style>
<div class="wrap">
<nav>
  <div class="brand">Borealis<small>Developer guide · generated from source</small></div>
  <a href="#quickstart">Quickstart</a>
  <a href="#debugging">Debugging layout</a>
  <a href="#xml">Building UIs: XML</a>
  <a href="#cpp">Building UIs: C++</a>
  <a href="#layout">Layout &amp; common attributes</a>
  <a href="#theming">Themes &amp; styles</a>
  <a href="#style-tokens">Style tokens</a>
  <a href="#theme-tokens">Theme colors</a>
  <a href="#icons">Icons</a>
  <a href="#reference">Component reference</a>
  {''.join(nav_items)}
</nav>
<main>

<h1>Borealis Developer Guide</h1>
<p class="lede">Borealis is a C++17, gamepad-first UI toolkit: XML view trees, flexbox layout (Yoga), vector rendering (nanovg), one code base for desktop, Switch, PS4, PS Vita, Android, iOS and tvOS. This guide covers the app skeleton, the XML/C++ authoring model, and a reference for every view — extracted automatically from <code>library/borealis</code>.</p>
<div class="kv">
  <b>Namespace</b><span><code>brls</code> — include <code>&lt;borealis.hpp&gt;</code></span>
  <b>Layout</b><span>Yoga flexbox — <code>axis</code>, <code>grow</code>, <code>alignItems</code>, <code>justifyContent</code> behave like CSS</span>
  <b>Input model</b><span>One focused view at a time; D-pad / stick moves focus, A = click, B = back. Mouse and touch map onto the same actions.</span>
  <b>Resources</b><span><code>resources/</code> next to the binary (or bundled with libromfs): <code>xml/</code>, <code>i18n/</code>, <code>img/</code>, <code>font/</code></span>
</div>

<h2 id="quickstart">Quickstart: a barebones app</h2>
<ol class="steps">
  <li><b>Clone the template</b> (this repository). It already contains <code>library/borealis</code> as a submodule, a <code>demo/</code> app, and a CMake setup for every target. Run <code>git submodule update --init --recursive</code> after cloning.</li>
  <li><b>Build for desktop</b> to iterate quickly:
{BUILD_SH}
  Other targets: <code>-DPLATFORM_SWITCH=ON</code>, <code>-DPLATFORM_PS4=ON</code>, <code>-DPLATFORM_PSV=ON</code>, <code>-DPLATFORM_ANDROID=ON</code>, <code>-DPLATFORM_IOS=ON</code>, <code>-DPLATFORM_TVOS=ON</code>. On macOS add <code>-DBUNDLE_MACOS_APP=ON</code> for a <code>.app</code>.</li>
  <li><b>Replace <code>demo/src/main.cpp</code></b> with the minimum:
{code(QUICKSTART_MAIN)}</li>
  <li><b>Write the first screen</b> in <code>resources/xml/activity/main.xml</code>:
{code(QUICKSTART_XML, 'xml')}</li>
  <li><b>Run it.</b> You get the title bar, sidebar, hint bar and focus handling for free. Press A on the button — nothing happens yet, which is the next section.</li>
</ol>
<figure><img src="screenshots/01-components-top.png" alt="The demo app on desktop"><figcaption>The demo app built from this template: AppletFrame → TabFrame → Sidebar + tab content, with the hint bar driven by registered actions.</figcaption></figure>

<h2 id="debugging">Debugging layout</h2>
<p>Every <code>View</code> has a <code>wireframe</code> flag (<code>setWireframeEnabled(bool)</code> / <code>isWireframeEnabled()</code>, or the <code>wireframe="true"</code> XML attribute) that draws its frame in blue and its margin/padding boxes in green — the fastest way to see why something isn't sized or positioned the way you expect. It's per-view, so recurse over a <code>Box</code>'s children to outline a whole subtree.</p>
{code(WIREFRAME_CPP)}
<p>Separately, <code>Application::enableDebuggingView(true)</code> toggles the FPS/frametime overlay — useful for performance, not layout.</p>

<h2 id="xml">Building UIs with XML</h2>
<p>Every built-in view has a <code>brls:</code> tag; any class you register with <code>Application::registerXMLView("Name", factory)</code> becomes a <code>&lt;Name /&gt;</code> tag. Attributes are applied in document order through typed setters, so an unknown attribute or an illegal enum value is a fatal error at inflate time — not a silent no-op.</p>
<h4>Value syntax</h4>
<table class="attrs"><thead><tr><th>Form</th><th>Meaning</th><th>Example</th></tr></thead><tbody>
<tr><td><code>123</code> / <code>123px</code></td><td>Number in points (scaled by the window scale)</td><td><code>width="240"</code></td></tr>
<tr><td><code>50%</code></td><td>Percentage of the parent</td><td><code>width="33%"</code></td></tr>
<tr><td><code>auto</code></td><td>Let Yoga size it from content / flex</td><td><code>height="auto"</code></td></tr>
<tr><td><code>#RRGGBB</code> / <code>#RRGGBBAA</code></td><td>Literal colour</td><td><code>backgroundColor="#1e1e1e"</code></td></tr>
<tr><td><code>@theme/…</code></td><td>Colour from the active theme (light/dark)</td><td><code>lineColor="@theme/brls/accent"</code></td></tr>
<tr><td><code>@style/…</code></td><td>Metric from the style sheet</td><td><code>paddingLeft="@style/brls/tab_frame/content_padding_sides"</code></td></tr>
<tr><td><code>@i18n/…</code></td><td>Translated string from <code>resources/i18n/&lt;locale&gt;/*.json</code></td><td><code>text="@i18n/demo/title"</code></td></tr>
<tr><td><code>@res/…</code></td><td>File in <code>resources/</code></td><td><code>image="@res/img/tiles.png"</code></td></tr>
<tr><td><code>true</code> / <code>false</code></td><td>Boolean</td><td><code>focusable="true"</code></td></tr>
</tbody></table>
<h4>Composition</h4>
<ul>
<li><code>&lt;brls:View xml="@res/xml/tabs/settings.xml" /&gt;</code> inlines another XML file.</li>
<li><code>&lt;brls:Tab label="…"&gt;</code> / <code>&lt;brls:Separator /&gt;</code> are only valid inside <code>&lt;brls:TabFrame&gt;</code>.</li>
<li>Give a view an <code>id</code> to reach it from C++ (<code>getView("id")</code> or <code>BRLS_BIND</code>) and to target it with <code>focusUp/Down/Left/Right</code>.</li>
<li>Composite views forward attributes to children (e.g. a cell's <code>title</code> goes to its Label) — these show up as "forwarded" in the reference.</li>
</ul>

<h2 id="cpp">Building UIs with C++</h2>
<p>The pattern used throughout the demo: subclass <code>brls::Box</code>, inflate the XML in the constructor, bind children by id, attach behaviour.</p>
{code(BIND_CPP)}
<div class="two">
<div><h4>Actions, focus and drawing</h4>{code(ACTIONS_CPP)}</div>
<div><h4>Cells &amp; settings rows</h4>{code(CELLS_CPP)}</div>
</div>
<div class="two">
<div><h4>Dialog</h4>{code(DIALOG_CPP)}</div>
<div><h4>Recycler list</h4>{code(RECYCLER_CPP)}</div>
</div>
<h4>Activities</h4>
<p>An <code>Activity</code> is one full screen (usually an AppletFrame). <code>Application::pushActivity(new X())</code> animates it in; B or <code>Application::popActivity()</code> returns. Views can also be pushed alone with the same call. <code>Dialog::open()</code> and <code>Dropdown</code> use the same stack.</p>
<figure><img src="screenshots/07-activity-push.png" alt="Pushed activity"><figcaption>An activity pushed from the Pokedex list: its own AppletFrame header, content and Back button.</figcaption></figure>
<h4>Threads and timing</h4>
<p>UI must be touched from the main thread. Use <code>brls::sync([]{{ … }})</code> from workers, <code>brls::delay(ms, cb)</code> for timers, and <code>brls::Animatable</code> / <code>Application::getAnimationDelta()</code> for animated properties (<code>alpha</code>, positions, etc.).</p>

<h2 id="layout">Layout &amp; common attributes</h2>
<p>Every view carries the Yoga box model. Children of a <code>Box</code> are laid out on its <code>axis</code>; <code>grow</code> distributes leftover space; <code>alignItems</code> / <code>justifyContent</code> position them. The Layout tab of the demo is the fastest way to see each value.</p>
<figure><img src="screenshots/02-layout.png" alt="Layout demo"><figcaption>Rows are <code>axis="row"</code> boxes with <code>alignItems</code> and <code>justifyContent</code> variations.</figcaption></figure>
<p>The complete attribute set of <code>View</code> ({len(VIEW_COMMON)} attributes) and <code>Box</code> ({len(BOX_COMMON)} more) is listed in the reference below and applies to every derived view.</p>

<h2 id="theming">Themes &amp; styles</h2>
<p>Colours live in <code>brls::Theme</code> (two variants: <code>ThemeVariant::LIGHT</code> / <code>DARK</code>, selected with <code>getPlatform()-&gt;setThemeVariant()</code>), metrics in <code>brls::Style</code>. Both are name → value maps you can extend with <code>Theme::addColor</code> / <code>Style::addMetric</code> and reference from XML as <code>@theme/</code> and <code>@style/</code>.</p>
<div class="two">
<figure><img src="screenshots/16-light-components.png" alt="Light theme"><figcaption>Light variant — accent <code>#314FEB</code>.</figcaption></figure>
<figure><img src="screenshots/17-light-settings.png" alt="Light theme settings"><figcaption>Same views, light theme, settings cells.</figcaption></figure>
</div>
<p>Built-in token names all start with <code>brls/</code> (e.g. <code>brls/accent</code>, <code>brls/background</code>, <code>brls/sidebar/active_item</code>, <code>brls/button/primary_enabled_background</code>); the full list is in <code>library/lib/core/theme.cpp</code> and <code>style.cpp</code>. Add your own with <code>Style::addMetric("app/my_token", value)</code> / <code>Theme::addColor(...)</code> and reference them the same way.</p>

<h3 id="style-tokens">Style tokens (<code>@style/…</code>)</h3>
<p class="lede">Every numeric metric the built-in views use, extracted from <code>styleValues</code> in <code>style.cpp</code>. Many are documented as "unused by the library, here for users" — safe defaults you can reference for your own layouts (e.g. <code>brls/tab_frame/content_padding_sides</code>).</p>
{render_style_table()}

<h3 id="theme-tokens">Theme colors (<code>@theme/…</code>)</h3>
<p class="lede">Every color token, light-theme values shown (dark-theme values live alongside them in <code>theme.cpp</code>'s <code>darkThemeValues</code>).</p>
{render_theme_table()}

<h2 id="icons">Icons</h2>
<p>Borealis has two unrelated ways to show an icon — pick based on whether you have artwork or just want a glyph.</p>
<div class="two">
<div><h4>Bitmap icons (your own artwork)</h4><p>An ordinary <code>brls::Image</code> (or <code>AppletFrame::setIcon(path)</code>, which just sets a bound <code>Image</code>'s source and toggles its visibility). Use for app/title icons, avatars, anything that isn't a simple monochrome glyph.</p>{code(ICON_BITMAP_CPP)}</div>
<div><h4>Glyph icons (font-based)</h4><p>There is no dedicated icon widget. <code>FONT_MATERIAL_ICONS</code> and <code>FONT_SWITCH_ICONS</code> are registered as nanovg <em>fallback fonts</em> on top of the regular font at startup (see <code>desktop_font.cpp</code>). Any codepoint missing from the regular font — a Private-Use-Area codepoint that only exists in one of the icon fonts — renders from that fallback automatically. So a glyph icon is just a <code>brls::Label</code> whose text is that codepoint; no icon-specific API.</p>{code(ICON_GLYPH_CPP)}</div>
</div>
<p class="lede">Material Icons ligature names aren't enumerated anywhere in this codebase (the font is loaded but no code references specific glyphs), so there's no generated list for it here. The Switch icons font (<code>resources/font/switch_icons.ttf</code>, <code>FONT_SWITCH_ICONS</code>) is used for the controller-button glyphs in the hint bar — every codepoint the library actually maps, extracted from <code>Hint::getKeyIcon</code> in <code>hint.cpp</code>:</p>
{render_switch_icons_table()}
<p class="muted">A blank/empty cell in the Glyph column means the codepoint isn't covered by <code>switch_icons.ttf</code> in this checkout.</p>

<h2 id="reference">Component reference</h2>
<p class="lede">Generated from the headers and sources in <code>library/borealis/library</code>. For each class: the XML tag, its own XML attributes (types and allowed values), events, and the public C++ API with the doc comments found in the header.</p>
<div class="note">Attribute types — <code>number</code>: points, <code>percent</code>: <code>N%</code>, <code>auto</code>, <code>string</code>, <code>bool</code>, <code>color</code>: <code>#hex</code> or <code>@theme/</code>, <code>path</code>: <code>@res/</code>, <code>enum</code>: one of the listed values.</div>
{''.join(ref_sections)}
{('<h2>Other classes</h2><p class="muted">' + ', '.join(esc(u) for u in undocumented) + '</p>') if undocumented else ''}

<p class="muted" style="margin-top:60px">Generated by <code>docs/gen_docs.py</code> from submodule <code>library/borealis</code>. Screenshots captured from the desktop build of the demo app.</p>
</main>
</div>
"""

os.makedirs(OUT_DIR, exist_ok=True)
open(os.path.join(OUT_DIR, "index.html"), "w", encoding="utf-8").write(page)
json.dump(classes, open(os.path.join(OUT_DIR, "api.json"), "w"), indent=1)
print(f"{len(classes)} classes, {sum(len(c['attrs']) for c in classes.values())} attributes -> docs/index.html")
