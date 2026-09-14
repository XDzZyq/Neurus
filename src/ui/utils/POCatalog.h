/**
 * @file POCatalog.h
 * @brief GNU gettext .po parsing — the file-format half of I18n.
 *
 * Split out of I18n so the format handling can be tested directly: it is pure
 * text processing with no singleton, no Qt resources and no active-language
 * state, and the interesting cases (entries without blank separators, plural
 * forms) are awkward to reach through a catalog embedded in the binary.
 *
 * Only the subset Neurus needs is supported: msgctxt / msgid / msgstr with
 * quoted continuation lines. Plural forms are recognised solely so they cannot
 * corrupt the singular entry (see Parse).
 *
 * @note UI Layer utility — Qt only, no Vulkan, no Renderer.
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QLatin1String>
#include <QPair>
#include <QString>

namespace neurus::po {

/** @brief (context, msgid) -> translation; an empty context is the default. */
using Catalog = QHash<QPair<QString, QString>, QString>;

/** @brief Entry tally from one Parse() call, for the load-time coverage log. */
struct Stats
{
	int total = 0;       ///< Entries with a non-empty msgid (header excluded).
	int translated = 0;  ///< Of those, the ones with a non-empty msgstr.
};

/**
 * @brief Parses a .po file into @p out, keeping only translated entries.
 *
 * Untranslated entries (empty msgstr) and the metadata header (msgid "") are
 * counted but not stored — I18n falls back to the English key for them.
 *
 * An entry ends at the next msgctxt, at the next msgid that does not belong to
 * a msgctxt just seen, or at a blank line. Blank lines are conventional rather
 * than required, so treating them as the only terminator made an entry inherit
 * the previous one's msgstr — with the header at the top of every file, the
 * first real key resolved to the header text.
 *
 * Plural forms are dropped, not merged: msgid_plural is ignored (it must be
 * tested before msgid, of which it is a prefix), msgstr[0] is taken as the
 * translation, and higher indices are ignored. Neurus has no plural keys, but
 * hand-editing and Poedit both emit them.
 *
 * @param data Raw .po file bytes (UTF-8).
 * @param out  Catalog to insert into (not cleared).
 * @return Entry counts (see Stats).
 */
Stats Parse(const QByteArray& data, Catalog& out);

/**
 * @brief Reads one field out of a catalog's PO header entry.
 * @param data Raw .po file bytes (UTF-8).
 * @param name Field name, e.g. "X-Language-Name".
 * @return The trimmed field value, or an empty string if absent.
 */
QString HeaderField(const QByteArray& data, QLatin1String name);

} // namespace neurus::po
