#include "app/AppArgs.h"

namespace neurus
{
namespace
{

bool TakeValue(const QStringList& arguments, int& index,
               const char* name, std::optional<std::string>& value, std::string& error)
{
	const QString argument = arguments[index];
	const QString option = QString::fromLatin1(name);
	if (argument == option)
	{
		if (index + 1 >= arguments.size())
		{
			error = std::string("Missing value for ") + name + ".";
			return true;
		}
		value = arguments[++index].toStdString();
		return true;
	}

	const QString prefix = option + QLatin1Char('=');
	if (argument.startsWith(prefix))
	{
		value = argument.mid(prefix.size()).toStdString();
		return true;
	}
	return false;
}

} // namespace

AppArgs AppArgs::Parse(const QStringList& arguments)
{
	AppArgs result;
	for (int index = 1; index < arguments.size(); ++index)
	{
		if (TakeValue(arguments, index, "--session", result.session, result.error))
		{
			if (!result.error.empty())
				break;
			continue;
		}
		if (TakeValue(arguments, index, "--project", result.project, result.error))
		{
			if (!result.error.empty())
				break;
			continue;
		}
		// Unknown arguments belong to Qt or to a future option; ignore them.
	}
	return result;
}

AppArgs AppArgs::Parse(int argc, char* argv[])
{
	QStringList arguments;
	arguments.reserve(argc);
	for (int i = 0; i < argc; ++i)
		arguments.push_back(QString::fromLocal8Bit(argv[i]));
	return Parse(arguments);
}

} // namespace neurus
