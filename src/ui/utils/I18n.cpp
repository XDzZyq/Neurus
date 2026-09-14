#include "ui/utils/I18n.h"

#include "core/Log.h"
#include "ui/utils/POCatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>

namespace neurus {


I18n& I18n::instance()
{
	static I18n s_instance;
	return s_instance;
}

void I18n::setLanguage(const QString& code)
{
	QString c = code.isEmpty() ? QStringLiteral("en") : code;
	// "auto" means "follow the system UI language".
	if (c == QLatin1String("auto"))
		c = systemLanguage();
	if (c == m_language)
		return;
	m_language = c;
	loadCatalog(c);
	emit languageChanged();
}

QString I18n::translate(const char* key) const
{
	return translateCtx(key, "");
}

QString I18n::translateCtx(const char* key, const char* context) const
{
	if (!key)
		return QString();
	const auto it = m_dict.constFind({ QString::fromUtf8(context ? context : ""),
	                                   QString::fromUtf8(key) });
	if (it != m_dict.constEnd())
		return it.value();
	return QString::fromUtf8(key);  // English fallback.
}

QList<I18n::LanguageInfo> I18n::supportedLanguages()
{
	// English is implicit: the msgids ARE the English strings, so it has no
	// catalog. Every other language is discovered from the embedded catalogs,
	// making a new res/i18n/<code>.po the only edit needed to add a language.
	QList<LanguageInfo> langs{ { QStringLiteral("en"), QStringLiteral("English") } };

	const QFileInfoList files = QDir(QStringLiteral(":/i18n"))
	                                .entryInfoList({ QStringLiteral("*.po") },
	                                               QDir::Files, QDir::Name);
	for (const QFileInfo& info : files)
	{
		const QString code = info.completeBaseName();
		if (code == QLatin1String("en"))
			continue;

		QString name;
		QFile file(info.filePath());
		if (file.open(QIODevice::ReadOnly))
		{
			name = po::HeaderField(file.readAll(), QLatin1String("X-Language-Name"));
			file.close();
		}
		if (name.isEmpty())
			name = QLocale(code).nativeLanguageName();
		if (name.isEmpty())
			name = code;

		langs.append({ code, name });
	}
	return langs;
}

QString I18n::systemLanguage()
{
	const QLocale sys = QLocale::system();
	if (sys.script() == QLocale::SimplifiedHanScript)
		return QStringLiteral("zh_CN");
	return QStringLiteral("en");
}

void I18n::loadCatalog(const QString& code)
{
	m_dict.clear();
	if (code == QLatin1String("en"))
		return;  // English is the built-in fallback — msgids ARE the strings.

	QFile file(QStringLiteral(":/i18n/%1.po").arg(code));
	if (!file.open(QIODevice::ReadOnly))
	{
		NEURUS_LOG("[I18n] no catalog for language '"
		           << code.toStdString() << "', falling back to English");
		return;
	}
	const QByteArray data = file.readAll();
	file.close();

	const po::Stats stats = po::Parse(data, m_dict);

	if (stats.total == 0)
	{
		NEURUS_ERR("[I18n] catalog :/i18n/" << code.toStdString()
		           << ".po is empty or unparsable, falling back to English");
		return;
	}

	NEURUS_LOG("[I18n] loaded " << stats.translated << "/" << stats.total
	           << " strings for '" << code.toStdString() << "' ("
	           << (stats.total - stats.translated) << " untranslated)");
}

} // namespace neurus
