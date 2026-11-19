#!/usr/bin/env python3
"""Check source include boundaries without importing build or third-party tools.

Run from any directory: python scripts/check_layer_boundaries.py
Quoted includes are resolved relative to the source file and src first, then
through unique project header names. System includes are checked only for
explicit Vulkan/Qt names; ordinary standard-library headers are ignored.
"""

from pathlib import Path
import re
import sys


SOURCE_ROOT = Path(__file__).resolve().parents[1] / "src"
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".cxx", ".mm"}
FORBIDDEN_LAYERS = {
    "core": {"asset", "scene", "editor", "render", "render_api", "ui"},
    "asset": {"editor", "render", "render_api", "ui"},
    "scene": {"editor", "render", "render_api", "ui"},
    "editor": {"render", "ui"},
    "script": {"render", "ui"},
    "ui": {"render"},
    "render": {"editor", "ui"},
    "render_api": {"editor", "render", "ui"},
    "command": {"editor", "render", "render_api", "ui"},
    "ipc": {"editor", "render", "render_api", "ui"},
    "cli": {"editor", "render", "render_api", "ui"},
}
GPU_FREE_LAYERS = {"core", "asset", "scene", "editor", "script", "render_api", "command", "ipc", "cli"}
QT_FREE_LAYERS = GPU_FREE_LAYERS | {"render"}
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]+)[>"]', re.MULTILINE)
COMMENTS = re.compile(r'/\*.*?\*/|//[^\n]*', re.DOTALL)


def without_comments(text):
    """Preserve line numbers while excluding commented-out includes."""
    return COMMENTS.sub(lambda match: re.sub(r'[^\n]', ' ', match.group()), text)


def external_family(header):
    """Identify explicit toolkit header names, avoiding STL names like queue."""
    lowered = header.lower()
    if lowered.startswith("vulkan/") or lowered in {"vulkan.h", "vulkan.hpp"}:
        return "Vulkan"
    if header.startswith("Qt") or re.match(r'^Q[A-Z][A-Za-z0-9]*(?:/|\.|$)', header):
        return "Qt"
    if lowered.startswith(("qtcore/", "qtgui/", "qtwidgets/", "qtquick/", "qtqml/")):
        return "Qt"
    return None


def included_layer(source, header, opener, headers_by_name):
    """Resolve project includes, including local/relative and short headers."""
    normalized = header.replace("\\", "/")
    for candidate in (source.parent / normalized, SOURCE_ROOT / normalized):
        resolved = candidate.resolve()
        if resolved.is_file() and resolved.is_relative_to(SOURCE_ROOT):
            return resolved.relative_to(SOURCE_ROOT).parts[0]
    # Layer-qualified paths remain diagnosable even during a file move.
    first = normalized.split("/", 1)[0]
    if first in FORBIDDEN_LAYERS:
        return first
    if opener == '"':
        matches = headers_by_name.get(Path(normalized).name, [])
        layers = {path.relative_to(SOURCE_ROOT).parts[0] for path in matches}
        if len(layers) == 1:
            return layers.pop()
    return None


def main():
    sources = sorted(path for path in SOURCE_ROOT.rglob("*") if path.suffix in SOURCE_SUFFIXES)
    headers_by_name = {}
    for path in sources:
        headers_by_name.setdefault(path.name, []).append(path)
    errors = []
    for source in sources:
        layer = source.relative_to(SOURCE_ROOT).parts[0]
        if layer not in FORBIDDEN_LAYERS:
            continue
        text = without_comments(source.read_text(encoding="utf-8-sig"))
        for match in INCLUDE.finditer(text):
            opener, header = match.groups()
            family = external_family(header)
            target = included_layer(source, header, opener, headers_by_name)
            reason = None
            if target in FORBIDDEN_LAYERS[layer]:
                reason = f"{layer} must not include {target} internals"
            elif family == "Vulkan" and layer in GPU_FREE_LAYERS:
                reason = f"{layer} must remain Vulkan-free"
            elif family == "Qt" and layer in QT_FREE_LAYERS:
                reason = f"{layer} must remain Qt-free"
            if reason:
                line = text.count("\n", 0, match.start()) + 1
                relative = source.relative_to(SOURCE_ROOT.parent).as_posix()
                errors.append(f"{relative}:{line}: {reason}: {header}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Layer include boundaries: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
