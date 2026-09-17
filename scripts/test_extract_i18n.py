#!/usr/bin/env python3
"""Unit tests for scripts/extract_i18n.py.

CI runs `extract_i18n.py --check` *before* the build and fails the job on a
stale or under-translated catalog, so a bug in this script does not produce a
bad translation — it produces a permanently red pipeline that no source change
can turn green. That makes the script itself worth testing.

The cases here pin the two properties that are easy to break and expensive to
notice:

- **Convergence.** Rendering a catalog and re-reading it must reach a fixed
  point: two consecutive runs, and the second must report `stale: False`.
  An entry class that parses to nothing but re-renders as something (which is
  what obsolete `#~` blocks used to do) makes the file oscillate forever.
- **Parser tolerance.** `.po` is a hand-editable, Poedit-editable format. Blank
  lines between entries are conventional, not required, and `msgid_plural` /
  `msgstr[N]` appear in any file a translator has touched with a real tool.

Run directly (`python3 scripts/test_extract_i18n.py`) or via unittest
discovery. No third-party dependencies.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import extract_i18n as ex  # noqa: E402


HEADER = ex.default_header("zz")


class ParsePoTest(unittest.TestCase):
    """Covers parse_po()'s entry segmentation and field dispatch."""

    def entries(self, text):
        """Returns {(ctx, msgid): entry} for every non-header entry."""
        return {e.key(): e for e in ex.parse_po(text) if e.msgid != ""}

    def test_obsolete_entries_keep_their_translation(self):
        # The regression: the `#~` branch used to `continue` before parsing its
        # payload, so obsolete entries round-tripped as empty and were dropped.
        text = (HEADER + '\n'
                'msgid "Live"\n'
                'msgstr "translated"\n'
                '\n'
                '#~ msgid "Removed"\n'
                '#~ msgstr "still translated"\n')
        got = self.entries(text)

        self.assertIn(("", "Removed"), got)
        self.assertEqual(got[("", "Removed")].msgstr, "still translated")
        self.assertTrue(got[("", "Removed")].obsolete)
        self.assertFalse(got[("", "Live")].obsolete)

    def test_obsolete_entries_keep_their_context(self):
        text = (HEADER + '\n'
                '#~ msgctxt "Dialog"\n'
                '#~ msgid "Gone"\n'
                '#~ msgstr "translated"\n')
        got = self.entries(text)

        self.assertEqual(list(got), [("Dialog", "Gone")])
        self.assertTrue(got[("Dialog", "Gone")].obsolete)

    def test_an_active_block_never_merges_with_an_obsolete_one(self):
        # No blank line between them: the only signal that these are two
        # entries is the `#~` prefix changing.
        text = (HEADER + '\n'
                'msgid "Live"\n'
                'msgstr "a"\n'
                '#~ msgid "Dead"\n'
                '#~ msgstr "b"\n')
        got = self.entries(text)

        self.assertEqual(got[("", "Live")].msgstr, "a")
        self.assertEqual(got[("", "Dead")].msgstr, "b")
        self.assertFalse(got[("", "Live")].obsolete)
        self.assertTrue(got[("", "Dead")].obsolete)

    def test_entries_without_blank_separators_are_all_kept(self):
        # Blank lines between entries are a convention, not part of the format.
        # Treating one as the only terminator made every key inherit the
        # previous entry's msgstr.
        text = (HEADER + '\n'
                'msgid "One"\n'
                'msgstr "1"\n'
                'msgid "Two"\n'
                'msgstr "2"\n'
                'msgctxt "Ctx"\n'
                'msgid "Three"\n'
                'msgstr "3"\n'
                'msgid "Four"\n'
                'msgstr "4"\n')
        got = self.entries(text)

        self.assertEqual(got[("", "One")].msgstr, "1")
        self.assertEqual(got[("", "Two")].msgstr, "2")
        self.assertEqual(got[("Ctx", "Three")].msgstr, "3")
        # The context must not leak past the entry that declared it.
        self.assertEqual(got[("", "Four")].msgstr, "4")

    def test_header_does_not_bleed_into_the_first_entry(self):
        # Same defect seen from the angle that actually bit: with no blank line
        # after the header, "First" used to resolve to the metadata block.
        text = (HEADER.rstrip("\n") + '\n'
                'msgid "First"\n'
                'msgstr "translated"\n')
        got = self.entries(text)

        self.assertEqual(got[("", "First")].msgstr, "translated")

    def test_plural_forms_do_not_corrupt_the_singular(self):
        # `msgid` is a prefix of `msgid_plural`, and `msgstr` of `msgstr[N]`, so
        # a naive startswith() test order swallows the plural fields.
        text = (HEADER + '\n'
                'msgid "%d file"\n'
                'msgid_plural "%d files"\n'
                'msgstr[0] "%d 个文件"\n'
                'msgstr[1] "%d 个文件们"\n'
                '\n'
                'msgid "After"\n'
                'msgstr "after"\n')
        got = self.entries(text)

        self.assertEqual(got[("", "%d file")].msgstr, "%d 个文件")
        self.assertEqual(got[("", "After")].msgstr, "after")
        self.assertNotIn(("", "%d files"), got)

    def test_continuation_lines_accumulate_on_the_right_field(self):
        text = (HEADER + '\n'
                'msgid ""\n'
                '"one "\n'
                '"two"\n'
                'msgstr ""\n'
                '"trans "\n'
                '"lated"\n')
        got = self.entries(text)

        self.assertEqual(got[("", "one two")].msgstr, "trans lated")

    def test_comments_are_ignored(self):
        text = (HEADER + '\n'
                '# translator note\n'
                '#. extracted comment\n'
                '#: src/ui/Foo.cpp:12\n'
                'msgid "Key"\n'
                'msgstr "value"\n')
        got = self.entries(text)

        self.assertEqual(got[("", "Key")].msgstr, "value")

    def test_escapes_are_decoded(self):
        text = (HEADER + '\n'
                'msgid "Tab\\there"\n'
                'msgstr "Quote\\"inside"\n')
        got = self.entries(text)

        self.assertEqual(got[("", "Tab\there")].msgstr, 'Quote"inside')


class RoundTripTest(unittest.TestCase):
    """Covers render_po/parse_po as a fixed point, via update_catalog()."""

    def setUp(self):
        self._dir = tempfile.TemporaryDirectory()
        self.po = os.path.join(self._dir.name, "zz.po")
        self.addCleanup(self._dir.cleanup)

    def update(self, keys, write=True):
        return ex.update_catalog(self.po, sorted(keys), verbose=False, write=write)

    def translate_all(self, value="译"):
        """Fills in every empty msgstr in the catalog, the way a human would."""
        with open(self.po, encoding="utf-8") as f:
            text = f.read()
        entries = ex.parse_po(text)
        header = ex.load_header(text, "zz")
        active, obsolete = [], []
        for e in entries:
            if e.msgid == "":
                continue
            if not e.msgstr:
                e.msgstr = value
            (obsolete if e.obsolete else active).append(e)
        with open(self.po, "w", encoding="utf-8") as f:
            f.write(ex.render_po(header, active, obsolete))

    def test_a_fresh_catalog_converges_on_the_second_run(self):
        keys = [("", "Alpha"), ("Dialog", "Beta")]

        self.assertTrue(self.update(keys)["stale"])    # created
        self.assertFalse(self.update(keys)["stale"])   # fixed point

    def test_a_fully_translated_catalog_is_not_stale(self):
        keys = [("", "Alpha"), ("Dialog", "Beta")]
        self.update(keys)
        self.translate_all()

        stats = self.update(keys)
        self.assertFalse(stats["stale"])
        self.assertEqual(stats["missing"], 0)
        self.assertEqual(stats["coverage"], 100.0)

    def test_removing_a_key_converges_instead_of_oscillating(self):
        # The CI-killer. An obsolete entry that re-renders but parses to nothing
        # makes new_text != old_text on *every* run, so --check reports the
        # catalog stale forever and no source change can turn the job green.
        keys = [("", "Alpha"), ("", "Beta")]
        self.update(keys)
        self.translate_all()

        reduced = [("", "Alpha")]
        self.assertTrue(self.update(reduced)["stale"])         # Beta retired
        self.assertFalse(self.update(reduced)["stale"])         # settled
        self.assertFalse(self.update(reduced)["stale"])         # and stays

    def test_a_readded_key_recovers_its_obsolete_translation(self):
        keys = [("", "Alpha"), ("Dialog", "Beta")]
        self.update(keys)
        self.translate_all("保留")
        self.update([("", "Alpha")])  # Beta becomes obsolete

        stats = self.update(keys)  # ... and comes back
        self.assertEqual(stats["missing"], 0,
                         "a re-added key must keep the translation parked in "
                         "its #~ entry, not come back empty")
        self.assertEqual(stats["obsolete"], 0)

    def test_check_mode_never_writes(self):
        keys = [("", "Alpha")]
        self.update(keys)
        with open(self.po, encoding="utf-8") as f:
            before = f.read()

        self.assertTrue(self.update([("", "Alpha"), ("", "Gamma")],
                                    write=False)["stale"])
        with open(self.po, encoding="utf-8") as f:
            self.assertEqual(f.read(), before)

    def test_the_header_survives_a_round_trip(self):
        self.update([("", "Alpha")])
        with open(self.po, encoding="utf-8") as f:
            entries = ex.parse_po(f.read())

        header = next(e for e in entries if e.msgid == "")
        self.assertIn("Content-Type: text/plain; charset=UTF-8", header.msgstr)
        self.assertIn("Language: zz", header.msgstr)


class DockKeyScrapeTest(unittest.TestCase):
    """Covers _function_body(), which scopes the UIPanel.h dock-title scrape."""

    # PanelIdFor() sits right above DefaultNameKey() and has an identical switch
    # shape, but returns serialization ids. Scraping both would put "dock.*"
    # into the catalog as if it were display text.
    SRC = '''
	static const char* PanelIdFor(PanelType type)
	{
		switch (type)
		{
		case PanelType::Viewport: return "dock.viewport";
		case PanelType::Log:      return "dock.log";
		}
		return "dock.unknown";
	}

	static const char* DefaultNameKey(PanelType type)
	{
		switch (type)
		{
		case PanelType::Viewport: return "Viewport";
		case PanelType::Log:      return "Log";
		}
		return "Unknown";
	}
'''

    def test_only_the_named_function_body_is_scraped(self):
        body = ex._function_body(self.SRC, "DefaultNameKey")
        found = [m.group(1) for m in ex._DOCK_KEY_RE.finditer(body)]

        self.assertEqual(found, ["Viewport", "Log"])
        self.assertNotIn("dock.viewport", body)

    def test_a_call_site_is_not_mistaken_for_the_definition(self):
        # UIPanel's ctor mentions DefaultNameKey(type) in a member-init list;
        # requiring a `{` straight after the parameter list skips it.
        src = ('\t, m_nameKey(nameKey && nameKey[0] ? nameKey '
               ': DefaultNameKey(type)) {}\n') + self.SRC
        body = ex._function_body(src, "DefaultNameKey")

        self.assertIn('return "Viewport";', body)

    def test_a_missing_function_fails_closed(self):
        self.assertEqual(ex._function_body(self.SRC, "NoSuchFunction"), "")


class LiveCatalogTest(unittest.TestCase):
    """Sanity-checks the catalogs actually in the repo."""

    def test_every_shipped_catalog_is_a_fixed_point(self):
        keys = ex.find_translation_keys()
        po_dir = os.path.join(ex.ROOT, "res", "i18n")
        names = sorted(n for n in os.listdir(po_dir) if n.endswith(".po"))
        self.assertTrue(names, "no catalogs found in res/i18n")

        for name in names:
            with self.subTest(catalog=name):
                stats = ex.update_catalog(os.path.join(po_dir, name), keys,
                                          verbose=False, write=False)
                self.assertFalse(stats["stale"],
                                 "run `python3 scripts/extract_i18n.py`")
                self.assertEqual(stats["missing"], 0,
                                 "%d key(s) untranslated" % stats["missing"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
