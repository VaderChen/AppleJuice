#pragma once

/* A／W 入口先截成單一碼元；宿主和客體使用相同分類。 */
static inline unsigned path_character_class(unsigned ch)
{
    if (ch < 32 || ch == '/' || ch == '<' || ch == '>' || ch == '|') return 0;
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) return ~0U;
    switch (ch) {
    case '?': return 1;
    case '*': return 2;
    case '.': return 4;
    case '\\': return 8;
    case ':': return 16;
    case ';': return 32;
    case ',': return 64;
    case ' ': return 128;
    case '"': return 512;
    default: return 256;
    }
}

static inline unsigned path_character_type(unsigned ch)
{
    if (ch < 32 || ch == '/' || ch == '"' || ch == '<' || ch == '>' || ch == '|') return 0;
    if (ch == '*' || ch == '?') return 4;
    if (ch == '\\' || ch == ':') return 8;
    /* 短檔名沿用 Win32 的奇數碼元規則，分號例外。 */
    if (ch >= 126 || (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z') || ((ch & 1U) && ch != ';') ||
        ch == '$' || ch == '&' || ch == '(' || ch == '.' || ch == '@' || ch == '^' || ch == 96)
        return 3;
    return 1;
}
