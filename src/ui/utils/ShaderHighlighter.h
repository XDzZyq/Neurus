#pragma once

#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QVector>
#include <QTextCharFormat>

namespace neurus {

class ShaderHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    enum class Language { GLSL, Cpp, Json, Python };

    explicit ShaderHighlighter(QTextDocument* parent = nullptr);
    ~ShaderHighlighter() override = default;

    ShaderHighlighter(const ShaderHighlighter&) = delete;
    ShaderHighlighter& operator=(const ShaderHighlighter&) = delete;

    void setLanguage(Language lang);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule { QRegularExpression pattern; QTextCharFormat format; };

    void applyGlsl();
    void applyCpp();
    void applyJson();
    void applyPython();

    QVector<Rule> m_rules;
    QRegularExpression m_multiLineStart;
    QRegularExpression m_multiLineEnd;

    QTextCharFormat m_keywordFmt;
    QTextCharFormat m_typeFmt;
    QTextCharFormat m_preprocessorFmt;
    QTextCharFormat m_commentFmt;
    QTextCharFormat m_stringFmt;
    QTextCharFormat m_numberFmt;
    QTextCharFormat m_builtinFmt;
    QTextCharFormat m_functionFmt;
};

} // namespace neurus
