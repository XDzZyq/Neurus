#include "app/AppArgs.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace neurus
{
namespace
{

bool TakeValue(const std::vector<std::string>& arguments, std::size_t& index,
               const char* name, std::optional<std::string>& value, std::string& error)
{
	const std::string& argument = arguments[index];
	const std::string option = name;
	if (argument == option)
	{
		if (index + 1 >= arguments.size())
		{
			error = std::string("Missing value for ") + name + ".";
			return true;
		}
		value = arguments[++index];
		return true;
	}

	const std::string prefix = option + "=";
	if (argument.rfind(prefix, 0) == 0)
	{
		value = argument.substr(prefix.size());
		return true;
	}
	return false;
}

/**
 * @brief Consumes a repeatable, comma-separated list option.
 *
 * Each occurrence appends its items, so `--preload a --preload b,c` yields
 * `{a, b, c}`. Whitespace around items is trimmed; empty items are skipped.
 */
bool TakeListValue(const std::vector<std::string>& arguments, std::size_t& index,
                   const char* name, std::vector<std::string>& values, std::string& error)
{
	const std::string& argument = arguments[index];
	const std::string option = name;
	std::string value;
	if (argument == option)
	{
		if (index + 1 >= arguments.size())
		{
			error = std::string("Missing value for ") + name + ".";
			return true;
		}
		value = arguments[++index];
	}
	else
	{
		const std::string prefix = option + "=";
		if (argument.rfind(prefix, 0) != 0)
		{
			return false;
		}
		value = argument.substr(prefix.size());
	}

	std::size_t added = 0;
	std::size_t start = 0;
	while (start <= value.size())
	{
		const std::size_t comma = value.find(',', start);
		const std::size_t length =
			comma == std::string::npos ? std::string::npos : comma - start;
		std::string item = value.substr(start, length);

		const std::size_t first = item.find_first_not_of(" \t");
		const std::size_t last = item.find_last_not_of(" \t");
		if (first != std::string::npos)
		{
			item = item.substr(first, last - first + 1);
			values.push_back(std::move(item));
			++added;
		}

		if (comma == std::string::npos)
		{
			break;
		}
		start = comma + 1;
	}

	if (added == 0)
	{
		error = std::string("Missing value for ") + name + ".";
	}
	return true;
}

/**
 * @brief Parses a `--width`/`--height` token strictly.
 *
 * Digits only, then a full-token `std::from_chars` and a range check. `std::stoul`
 * would accept a numeric prefix ("1280px") and wrap "-1" into ULONG_MAX, either
 * of which would reach the renderer as an unintended extent.
 *
 * @return True when @p text is a plain positive integer within the 32-bit bound.
 */
bool ParseExtent(const std::string& text, uint32_t& value)
{
	if (text.empty())
		return false;
	for (const char ch : text)
	{
		if (ch < '0' || ch > '9')
			return false;
	}

	uint32_t parsed = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
	if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
		return false;

	// 16384 is beyond any current display and still allocatable; the bound exists
	// to reject a nonsense token, not to describe a device.
	if (parsed == 0 || parsed > 16384)
		return false;

	value = parsed;
	return true;
}

} // namespace

AppArgs AppArgs::Parse(const std::vector<std::string>& arguments)
{
	AppArgs result;
	for (std::size_t index = 1; index < arguments.size(); ++index)
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
		if (TakeListValue(arguments, index, "--preload", result.preload, result.error))
		{
			if (!result.error.empty())
				break;
			continue;
		}
		std::optional<std::string> sizeText;
		if (TakeValue(arguments, index, "--width", sizeText, result.error))
		{
			if (result.error.empty() && !ParseExtent(*sizeText, result.width))
				result.error = "Invalid value for --width.";
			if (!result.error.empty())
				break;
			continue;
		}
		sizeText.reset();
		if (TakeValue(arguments, index, "--height", sizeText, result.error))
		{
			if (result.error.empty() && !ParseExtent(*sizeText, result.height))
				result.error = "Invalid value for --height.";
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
	std::vector<std::string> arguments;
	arguments.reserve(static_cast<std::size_t>(argc));
	for (int i = 0; i < argc; ++i)
		arguments.emplace_back(argv[i] ? argv[i] : "");
	return Parse(arguments);
}

} // namespace neurus
