/**
 * @file test_shader_highlighter_python.cpp
 * @brief Syntax highlighting for the Python language added to CodeHighlighter.
 *
 * The Script Console feeds shell code to a CodeEditor backed by
 * CodeHighlighter, so the Python language must actually produce more than one
 * format per line and must not leak into the languages that already existed
 * (GLSL is exercised here). The checks read the rendered QTextLayout formats
 * directly, which is what the editor paints.
 */

#include <gtest/gtest.h>

#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextLayout>
#include <QVector>

#include "ui/utils/CodeHighlighter.h"

namespace neurus
{

namespace
{

// Number of distinct QTextCharFormats stored in a block's layout.
int DistinctFormats(const QTextBlock& block)
{
	QVector<QTextCharFormat> seen;
	for (const auto& range : block.layout()->formats()) {
		bool known = false;
		for (const auto& format : seen) {
			if (format == range.format) {
				known = true;
				break;
			}
		}
		if (!known)
			seen.append(range.format);
	}
	return seen.size();
}

} // namespace

TEST(CodeHighlighterPython, HighlightsPythonSource)
{
	QTextDocument document;
	CodeHighlighter highlighter(&document);
	highlighter.setLanguage(CodeHighlighter::Language::Python);

	document.setPlainText(QStringLiteral("def f():\n    return 1  # c\n"));
	highlighter.rehighlight();

	// First line: "def" (keyword) plus "f(" (function) at minimum.
	EXPECT_GE(DistinctFormats(document.firstBlock()), 2);
	// Second line: "return" (keyword), "1" (number), "# c" (comment).
	EXPECT_GE(DistinctFormats(document.firstBlock().next()), 2);
}

TEST(CodeHighlighterPython, DoesNotDisturbOtherLanguages)
{
	QTextDocument document;
	CodeHighlighter highlighter(&document);
	highlighter.setLanguage(CodeHighlighter::Language::GLSL);

	document.setPlainText(QStringLiteral("void main() {\n    gl_Position = vec4(0.0);\n}\n"));
	highlighter.rehighlight();

	EXPECT_GE(DistinctFormats(document.firstBlock()), 2);

	// Switching to Python must neither throw nor leave the document blank.
	EXPECT_NO_THROW(highlighter.setLanguage(CodeHighlighter::Language::Python));
	EXPECT_GE(DistinctFormats(document.firstBlock()), 1);

	// A GLSL document still highlights after Python was used.
	EXPECT_NO_THROW(highlighter.setLanguage(CodeHighlighter::Language::GLSL));
	document.setPlainText(QStringLiteral("void main() {}\n"));
	highlighter.rehighlight();
	EXPECT_GE(DistinctFormats(document.firstBlock()), 2);
}

} // namespace neurus
