#!/usr/bin/env python3
"""Regression checks for the dependency-free layer-isolation CI gate."""

import contextlib
import io
from pathlib import Path
import tempfile
import unittest

import check_layer_boundaries as checker


class LayerBoundariesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="neurus-layer-check-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "src"
        original_root = checker.SOURCE_ROOT
        self.addCleanup(setattr, checker, "SOURCE_ROOT", original_root)
        checker.SOURCE_ROOT = self.root
        self.write("platform/NativeWindow.hpp", "using NativeWindowHandle = void*;\n")
        self.write("platform/PlatformSurface.hpp", "#include <vulkan/vulkan_raii.hpp>\n")

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def check(self, text, layer="ui"):
        self.write(layer + "/Consumer.hpp", text)
        output = io.StringIO()
        errors = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = checker.main()
        return result, errors.getvalue()

    def test_ui_native_handle_stays_vulkan_free(self):
        result, errors = self.check('#include "platform/NativeWindow.hpp"\n')
        self.assertEqual(result, 0, errors)

    def test_ui_qt_includes_are_allowed(self):
        result, errors = self.check("#include <QWidget>\n")
        self.assertEqual(result, 0, errors)

    def test_ui_vulkan_include_is_rejected(self):
        result, errors = self.check("#include <vulkan/vulkan_raii.hpp>\n")
        self.assertEqual(result, 1)
        self.assertIn("src/ui/Consumer.hpp:1: ui must remain Vulkan-free", errors)

    def test_ui_surface_api_is_rejected_for_all_include_spellings(self):
        for header in ["platform/PlatformSurface.hpp", "../platform/PlatformSurface.hpp", "PlatformSurface.hpp"]:
            with self.subTest(header=header):
                result, errors = self.check('#include "' + header + '"\n')
                self.assertEqual(result, 1)
                self.assertIn("ui must remain Vulkan-free", errors)

    def test_cpu_surface_api_is_rejected(self):
        result, errors = self.check('#include "platform/PlatformSurface.hpp"\n', "editor")
        self.assertEqual(result, 1)
        self.assertIn("editor must remain Vulkan-free", errors)

    def test_renderer_surface_api_is_allowed(self):
        result, errors = self.check('#include "platform/PlatformSurface.hpp"\n', "render")
        self.assertEqual(result, 0, errors)

    def test_editor_qt_include_is_rejected(self):
        result, errors = self.check("#include <QWidget>\n", "editor")
        self.assertEqual(result, 1)
        self.assertIn("editor must remain Qt-free", errors)

    def test_only_operations_include_the_replay_context(self):
        self.write("editor/operations/OperationContext.hpp", "// replay API\n")
        for layer in ["editor/controllers", "app"]:
            for header in ["editor/operations/OperationContext.hpp", "OperationContext.hpp"]:
                with self.subTest(layer=layer, header=header):
                    result, errors = self.check(f'#include "{header}"\n', layer)
                    self.assertEqual(result, 1)
                    self.assertIn(f"src/{layer}/Consumer.hpp:1:", errors)
                    self.assertIn("operation replay belongs to editor/operations", errors)

    def test_operation_implementation_can_include_its_local_context(self):
        self.write("editor/operations/OperationContext.hpp", "// replay API\n")
        result, errors = self.check('#include "OperationContext.hpp"\n', "editor/operations")
        self.assertEqual(result, 0, errors)

    def test_controller_can_submit_history_or_call_an_unrelated_apply(self):
        result, errors = self.check(
            '#include "editor/operations/GraphOperations.hpp"\n'
            'void Update(Filter& filter) { filter.Apply(); }\n',
            "editor/controllers")
        self.assertEqual(result, 0, errors)

    def test_forbidden_layer_include_is_rejected(self):
        result, errors = self.check('#include "editor/Editor.hpp"\n', "render")
        self.assertEqual(result, 1)
        self.assertIn("render must not include editor internals", errors)

    def test_commented_include_is_ignored_and_lines_are_preserved(self):
        result, errors = self.check('// #include <QWidget>\n/* #include <vulkan/vulkan.hpp> */\n')
        self.assertEqual(result, 0, errors)
        result, errors = self.check('// comment\n#include <vulkan/vulkan.hpp>\n')
        self.assertEqual(result, 1)
        self.assertIn("Consumer.hpp:2:", errors)


if __name__ == "__main__":
    unittest.main()
