#include "utils/ShaderHighlighter.h"

namespace neurus {

ShaderHighlighter::ShaderHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    // Keyword format
    m_keywordFmt.setForeground(QColor(0xD0, 0x60, 0x00)); // orange
    m_keywordFmt.setFontWeight(QFont::Bold);

    // Type format
    m_typeFmt.setForeground(QColor(0x00, 0x80, 0x80)); // teal
    m_typeFmt.setFontWeight(QFont::Bold);

    // Preprocessor format
    m_preprocessorFmt.setForeground(QColor(0x80, 0x50, 0x00)); // brown
    m_preprocessorFmt.setFontWeight(QFont::Bold);

    // Comment format
    m_commentFmt.setForeground(QColor(0x00, 0x80, 0x00)); // green
    m_commentFmt.setFontItalic(true);

    // String format
    m_stringFmt.setForeground(QColor(0x00, 0x60, 0xA0)); // blue

    // Number format
    m_numberFmt.setForeground(QColor(0x80, 0x00, 0x80)); // purple

    // Built-in format
    m_builtinFmt.setForeground(QColor(0x60, 0x00, 0xA0)); // violet
    m_builtinFmt.setFontItalic(true);

    // Function format
    m_functionFmt.setForeground(QColor(0x00, 0x40, 0x80)); // dark blue

    setLanguage(Language::GLSL);
}

void ShaderHighlighter::setLanguage(Language lang)
{
    m_rules.clear();
    switch (lang) {
        case Language::GLSL: applyGlsl(); break;
        case Language::Cpp:  applyCpp();  break;
        case Language::Json: applyJson(); break;
        case Language::Python: applyPython(); break;
    }
    m_multiLineStart = QRegularExpression("/\\*");
    m_multiLineEnd   = QRegularExpression("\\*/");
    rehighlight();
}

void ShaderHighlighter::highlightBlock(const QString& text)
{
    // Apply single-line rules
    for (const auto& rule : m_rules) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            auto match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }

    // Multi-line comments
    setCurrentBlockState(0);
    int startIndex = 0;
    int commentStart = -1;

    if (previousBlockState() != 1)
        startIndex = text.indexOf(m_multiLineStart);

    while (startIndex >= 0) {
        auto match = m_multiLineStart.match(text, startIndex);
        if (match.hasMatch()) {
            commentStart = match.capturedStart();
            auto endMatch = m_multiLineEnd.match(text, commentStart + match.capturedLength());
            if (endMatch.hasMatch()) {
                int commentLength = endMatch.capturedStart() + endMatch.capturedLength() - commentStart;
                setFormat(commentStart, commentLength, m_commentFmt);
                startIndex = commentStart + commentLength;
            } else {
                setCurrentBlockState(1);
                int commentLength = text.length() - commentStart;
                setFormat(commentStart, commentLength, m_commentFmt);
                break;
            }
        } else {
            break;
        }
    }

    if (previousBlockState() == 1) {
        commentStart = 0;
        auto endMatch = m_multiLineEnd.match(text);
        if (endMatch.hasMatch()) {
            int commentLength = endMatch.capturedStart() + endMatch.capturedLength();
            setFormat(0, commentLength, m_commentFmt);
        } else {
            setFormat(0, text.length(), m_commentFmt);
            setCurrentBlockState(1);
        }
    }
}

void ShaderHighlighter::applyGlsl()
{
    // Keywords
    QStringList keywords = {
        "void", "if", "else", "for", "while", "do", "return", "break", "continue",
        "struct", "uniform", "layout", "in", "out", "inout", "const", "switch", "case",
        "default", "discard", "flat", "smooth", "noperspective", "highp", "mediump", "lowp",
        "precision", "invariant", "attribute", "varying", "sampler2DShadow", "samplerCubeShadow"
    };
    for (const auto& kw : keywords)
        m_rules.append({QRegularExpression("\\b" + kw + "\\b"), m_keywordFmt});

    // Types
    QStringList types = {
        "float", "vec2", "vec3", "vec4", "mat2", "mat3", "mat4", "mat2x2", "mat2x3", "mat2x4",
        "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4",
        "int", "ivec2", "ivec3", "ivec4", "uint", "uvec2", "uvec3", "uvec4",
        "bool", "bvec2", "bvec3", "bvec4", "double", "dvec2", "dvec3", "dvec4",
        "sampler2D", "sampler3D", "samplerCube", "sampler2DArray", "samplerCubeArray",
        "isampler2D", "usampler2D", "sampler2DShadow", "image2D", "image3D"
    };
    for (const auto& t : types)
        m_rules.append({QRegularExpression("\\b" + t + "\\b"), m_typeFmt});

    // Preprocessor
    m_rules.append({QRegularExpression("^#\\s*\\w+.*"), m_preprocessorFmt});

    // Built-in variables
    QStringList builtins = {
        "gl_Position", "gl_PointSize", "gl_FragCoord", "gl_FragColor",
        "gl_FragDepth", "gl_FrontFacing", "gl_VertexID", "gl_InstanceID",
        "gl_SampleID", "gl_SamplePosition", "gl_ClipDistance"
    };
    for (const auto& b : builtins)
        m_rules.append({QRegularExpression("\\b" + b + "\\b"), m_builtinFmt});

    // Functions
    m_rules.append({QRegularExpression("\\b([a-zA-Z_]\\w*)\\s*\\("), m_functionFmt});

    // Numbers
    m_rules.append({QRegularExpression("\\b\\d+(\\.\\d+)?([eE][+-]?\\d+)?\\b"), m_numberFmt});

    // Single-line comments
    m_rules.append({QRegularExpression("//[^\n]*"), m_commentFmt});
}

void ShaderHighlighter::applyCpp()
{
    QStringList keywords = {"if","else","for","while","do","return","break","continue",
        "switch","case","default","class","struct","enum","namespace","template","typename",
        "public","private","protected","virtual","override","final","static","const","volatile",
        "using","typedef","sizeof","new","delete","try","catch","throw","nullptr","auto"};
    for (const auto& kw : keywords)
        m_rules.append({QRegularExpression("\\b" + kw + "\\b"), m_keywordFmt});

    QStringList types = {"int","float","double","char","bool","void","long","short","unsigned","signed",
        "size_t","uint32_t","int32_t","string","vector","map","shared_ptr","unique_ptr"};
    for (const auto& t : types)
        m_rules.append({QRegularExpression("\\b" + t + "\\b"), m_typeFmt});

    m_rules.append({QRegularExpression("^#\\s*\\w+.*"), m_preprocessorFmt});
    m_rules.append({QRegularExpression("\\b\\d+(\\.\\d+)?([eE][+-]?\\d+)?\\b"), m_numberFmt});
    m_rules.append({QRegularExpression("\"[^\"]*\""), m_stringFmt});
    m_rules.append({QRegularExpression("//[^\n]*"), m_commentFmt});
}

void ShaderHighlighter::applyJson()
{
    m_rules.append({QRegularExpression("\"[^\"]*\""), m_stringFmt});
    m_rules.append({QRegularExpression("\\b(true|false|null)\\b"), m_keywordFmt});
    m_rules.append({QRegularExpression("\\b-?\\d+(\\.\\d+)?([eE][+-]?\\d+)?\\b"), m_numberFmt});
}

void ShaderHighlighter::applyPython()
{
    // Function names / calls first, so keywords and builtins can override a
    // call-shaped keyword like "if (" further down the rule list.
    m_rules.append({QRegularExpression("\\b([a-zA-Z_]\\w*)\\s*\\("), m_functionFmt});

    // Keywords
    QStringList keywords = {
        "def", "class", "if", "elif", "else", "for", "while", "import", "from",
        "return", "yield", "with", "as", "try", "except", "finally", "lambda",
        "async", "await", "in", "is", "not", "and", "or", "pass", "break",
        "continue", "global", "nonlocal", "assert", "del", "raise"
    };
    for (const auto& kw : keywords)
        m_rules.append({QRegularExpression("\\b" + kw + "\\b"), m_keywordFmt});

    // Built-in functions and types
    QStringList builtins = {
        "print", "len", "range", "list", "dict", "set", "tuple", "str", "int",
        "float", "bool", "type", "isinstance", "enumerate", "zip", "open", "repr",
        "abs", "min", "max", "sum", "sorted", "reversed", "any", "all", "map",
        "filter", "iter", "next", "super", "hasattr", "getattr", "setattr",
        "round", "pow", "divmod", "callable", "format", "input", "id", "hash"
    };
    for (const auto& b : builtins)
        m_rules.append({QRegularExpression("\\b" + b + "\\b"), m_builtinFmt});

    // Literals
    m_rules.append({QRegularExpression("\\b(True|False|None)\\b"), m_keywordFmt});

    // Numbers
    m_rules.append({QRegularExpression("\\b\\d+(\\.\\d+)?([eE][+-]?\\d+)?\\b"), m_numberFmt});

    // Strings: single-quoted first, then triple-quoted so the latter wins.
    m_rules.append({QRegularExpression("'[^'\\n]*'"), m_stringFmt});
    m_rules.append({QRegularExpression("\"[^\"\\n]*\""), m_stringFmt});
    m_rules.append({QRegularExpression("'''[\\s\\S]*?'''"), m_stringFmt});
    m_rules.append({QRegularExpression("\"\"\"[\\s\\S]*?\"\"\""), m_stringFmt});

    // Comments: '#' to end of line
    m_rules.append({QRegularExpression("#[^\\n]*"), m_commentFmt});
}

} // namespace neurus
