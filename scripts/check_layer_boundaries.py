#!/usr/bin/env python3
"""Check source include boundaries without importing build or third-party tools.

Run from any directory: python scripts/check_layer_boundaries.py
Project includes are resolved relative to the source file and src first, then
through the project header index. Explicit Vulkan and Qt includes are also checked.
"""

from pathlib import Path
import re
import sys


SOURCE_ROOT = Path(__file__).resolve().parents[1] / "src"
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".cxx", ".mm"}
FORBIDDEN_LAYERS = {
    "core": {"shader", "asset", "scene", "editor", "render", "render_api", "ui"},
    "asset": {"shader", "editor", "render", "render_api", "ui"},
    "shader": {"app", "editor", "render", "render_api", "ui", "scene", "script"},
    "scene": {"shader", "editor", "render", "render_api", "ui"},
    "editor": {"render", "ui"},
    "script": {"render", "ui"},
    "ui": {"render"},
    "render": {"editor", "ui"},
    "render_api": {"shader", "editor", "render", "ui"},
    "command": {"editor", "render", "render_api", "ui"},
    "ipc": {"editor", "render", "render_api", "ui"},
    "cli": {"editor", "render", "render_api", "ui"},
    "platform": set(),
}
GPU_FREE_LAYERS = {"core", "asset", "shader", "scene", "editor", "script", "render_api", "command", "ipc", "cli", "ui"}
QT_FREE_LAYERS = (GPU_FREE_LAYERS - {"ui"}) | {"render"}
# Editor owns these contracts. Expose a header only to the consumer that needs it.
SHARED_EDITOR_HEADERS = {
    "editor/Input.hpp",
    "editor/events/AssetEvents.hpp",
    "editor/events/ConfigEvents.hpp",
    "editor/events/SceneEvents.hpp",
    "editor/events/ShaderEvents.hpp",
}
EDITOR_CONTRACTS = {
    "ui": SHARED_EDITOR_HEADERS | {
        "editor/events/InputEvents.hpp",
        "editor/events/OperationEvents.hpp",
        "editor/events/ProjectEvents.hpp",
        "editor/operations/HistoryView.hpp",
    },
    "script": SHARED_EDITOR_HEADERS | {
        "editor/events/GraphEvents.hpp",
        "editor/events/IEventQueue.hpp",
        "editor/events/ScriptEvents.hpp",
    },
}
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]+)[>"]', re.MULTILINE)
COMMENTS = re.compile(r'/\*.*?\*/|//[^\n]*', re.DOTALL)


def without_comments(text):
    """Preserve line numbers while excluding commented-out includes."""
    return COMMENTS.sub(lambda match: re.sub(r'[^\n]', ' ', match.group()), text)


def external_family(header):
    """Identify explicit toolkit header names, avoiding STL names like queue."""
    lowered = header.replace("\\", "/").lower()
    # This project API exposes Vulkan types too; CPU callers use NativeWindow.hpp.
    if lowered.rsplit("/", 1)[-1] == "platformsurface.hpp":
        return "Vulkan"
    if lowered.startswith("vulkan/") or lowered in {"vulkan.h", "vulkan.hpp"}:
        return "Vulkan"
    if header.startswith("Qt") or re.match(r'^Q[A-Z][A-Za-z0-9]*(?:/|\.|$)', header):
        return "Qt"
    if lowered.startswith(("qtcore/", "qtgui/", "qtwidgets/", "qtquick/", "qtqml/")):
        return "Qt"
    return None


def included_header(source, header, opener, headers_by_name):
    """Return the src-relative path, or just its layer for an ambiguous basename."""
    normalized = header.replace("\\", "/")
    for candidate in (source.parent / normalized, SOURCE_ROOT / normalized):
        resolved = candidate.resolve()
        if resolved.is_file() and resolved.is_relative_to(SOURCE_ROOT):
            return resolved.relative_to(SOURCE_ROOT)
    # Layer-qualified paths remain diagnosable even during a file move.
    first = normalized.split("/", 1)[0]
    if first in FORBIDDEN_LAYERS or first == "app":
        return Path(normalized)
    suffix = Path(normalized)
    matches = headers_by_name.get(suffix.name, [])
    if opener == "<":
        matches = [path for path in matches if path.parts[-len(suffix.parts):] == suffix.parts]
    if len(matches) == 1:
        return matches[0]
    layers = {path.parts[0] for path in matches}
    if len(layers) == 1:
        return Path(layers.pop())
    return None


def main():
    sources = sorted(path for path in SOURCE_ROOT.rglob("*") if path.suffix in SOURCE_SUFFIXES)
    headers_by_name = {}
    for path in sources:
        headers_by_name.setdefault(path.name, []).append(path.relative_to(SOURCE_ROOT))
    errors = []
    for source in sources:
        layer = source.relative_to(SOURCE_ROOT).parts[0]
        in_operations = source.is_relative_to(SOURCE_ROOT / "editor" / "operations")
        text = without_comments(source.read_text(encoding="utf-8-sig"))
        for match in INCLUDE.finditer(text):
            opener, header = match.groups()
            family = external_family(header)
            project_header = included_header(source, header, opener, headers_by_name)
            target = project_header.parts[0] if project_header else None
            reason = None
            if (not in_operations and target == "editor"
                    and Path(header.replace("\\", "/")).name == "OperationContext.hpp"):
                reason = "operation replay belongs to editor/operations"
            elif layer not in FORBIDDEN_LAYERS:
                continue
            elif target == "app" or target in FORBIDDEN_LAYERS[layer]:
                reason = f"{layer} must not include {target} internals"
            elif (target == "editor" and layer in EDITOR_CONTRACTS
                  and project_header.as_posix() not in EDITOR_CONTRACTS[layer]):
                reason = f"{layer} must use a public Editor contract"
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
