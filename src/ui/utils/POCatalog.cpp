#include "ui/utils/POCatalog.h"

#include <QStringList>

namespace neurus::po {

namespace {

/**
 * @brief Unescapes a quoted PO field body (between the quotes).
 * @param line A line such as: msgid "A \"quoted\" string\n"
 * @return The decoded field content.
 */
QString ParseQuotedField(const QString& line)
{
	QString out;
	const int first = line.indexOf(QLatin1Char('"'));
	if (first < 0)
		return out;
	int i = first + 1;
	const int n = line.size();
	while (i < n && line.at(i) != QLatin1Char('"'))
	{
		if (line.at(i) == QLatin1Char('\\') && i + 1 < n)
		{
			switch (line.at(i + 1).unicode())
			{
			case 'n': out += QLatin1Char('\n'); break;
			case 't': out += QLatin1Char('\t'); break;
			case 'r': out += QLatin1Char('\r'); break;
			case '\\': out += QLatin1Char('\\'); break;
			case '"': out += QLatin1Char('"'); break;
			default:  out += line.at(i + 1); break;
			}
			i += 2;
		}
		else
		{
			out += line.at(i);
			++i;
		}
	}
	return out;
}

} // namespace

Stats Parse(const QByteArray& data, Catalog& out)
{
	// Ignored: a field whose payload must be dropped, continuation lines
	// included (msgid_plural, msgstr[1] and up).
	enum class Field { None, Context, Msgid, Msgstr, Ignored };
	Stats   stats;
	QString context, msgid, msgstr;
	Field   field = Field::None;

	auto flush = [&]() {
		if (!msgid.isEmpty())
		{
			++stats.total;
			if (!msgstr.isEmpty())
			{
				++stats.translated;
				out.insert({ context, msgid }, msgstr);
			}
		}
		context.clear();
		msgid.clear();
		msgstr.clear();
		field = Field::None;
	};

	const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
	for (const QString& rawLine : lines)
	{
		const QString line = rawLine.trimmed();
		if (line.isEmpty())
		{
			flush();
			continue;
		}
		if (line.startsWith(QLatin1Char('#')))
			continue;  // comments (incl. obsolete "#~" entries)

		if (line.startsWith(QLatin1Char('"')))  // continuation of previous field
		{
			const QString value = ParseQuotedField(line);
			if (field == Field::Context)
				context += value;
			else if (field == Field::Msgid)
				msgid += value;
			else if (field == Field::Msgstr)
				msgstr += value;
			continue;
		}

		if (line.startsWith(QLatin1String("msgctxt")))
		{
			if (field != Field::None)
				flush();  // msgctxt always opens a new entry.
			field = Field::Context;
			context = ParseQuotedField(line);
		}
		else if (line.startsWith(QLatin1String("msgid_plural")))
		{
			field = Field::Ignored;  // Must precede the msgid test (prefix).
		}
		else if (line.startsWith(QLatin1String("msgid")))
		{
			// A msgid straight after a msgctxt belongs to the same entry; any
			// other msgid opens a new one.
			if (field != Field::None && field != Field::Context)
				flush();
			field = Field::Msgid;
			msgid = ParseQuotedField(line);
		}
		else if (line.startsWith(QLatin1String("msgstr")))
		{
			// msgstr, or msgstr[0] — the singular. Higher indices are dropped.
			if (line.startsWith(QLatin1String("msgstr["))
			    && !line.startsWith(QLatin1String("msgstr[0]")))
			{
				field = Field::Ignored;
			}
			else
			{
				field = Field::Msgstr;
				msgstr = ParseQuotedField(line);
			}
		}
	}
	flush();

	return stats;
}

QString HeaderField(const QByteArray& data, QLatin1String name)
{
	// The header is the first entry (msgid ""), one "Field: value\n" per line.
	const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
	for (const QString& rawLine : lines)
	{
		const QString line = rawLine.trimmed();
		if (line.startsWith(QLatin1String("msgid"))
		    && !line.endsWith(QLatin1String("\"\"")))
			break;  // Past the header entry — give up.
		if (!line.startsWith(QLatin1Char('"')))
			continue;
		const QString field = ParseQuotedField(line);
		if (field.startsWith(name)
		    && field.mid(name.size()).startsWith(QLatin1Char(':')))
			return field.mid(name.size() + 1).trimmed();
	}
	return QString();
}

} // namespace neurus::po
