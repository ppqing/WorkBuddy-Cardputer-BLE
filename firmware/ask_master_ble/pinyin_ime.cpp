#include "pinyin_ime.h"
#include <cstring>

int PinyinIME::cmpPinyin(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return *a - *b;
        a++; b++;
    }
    if (*a == 0) return 0;
    return *a - *b;
}

void PinyinIME::commitChar(char c) {
    if (_outLen < (int)sizeof(_out) - 4) {
        _out[_outLen++] = c;
        _out[_outLen] = '\0';
    }
}

void PinyinIME::commitStr(const char* s) {
    while (*s && _outLen < (int)sizeof(_out) - 4) {
        _out[_outLen++] = *s++;
    }
    _out[_outLen] = '\0';
}

void PinyinIME::addCandidate(const char* s) {
    if (!s || !*s || _candCount >= MAX_CANDS) return;
    int len = strlen(s);
    for (int i = 0; i < _candCount; i++) {
        if (_candLen[i] == len &&
            memcmp(_candBuf + _candOffset[i], s, len) == 0) {
            return;
        }
    }
    // +1 for the NUL terminator after each candidate, so candidate(i) returns
    // a NUL-terminated string.
    if (_candBufLen + len + 1 > (int)sizeof(_candBuf)) return;
    _candOffset[_candCount] = _candBufLen;
    _candLen[_candCount] = len;
    memcpy(_candBuf + _candBufLen, s, len);
    _candBufLen += len;
    _candBuf[_candBufLen] = '\0';
    _candBufLen++;  // advance past NUL
    _candCount++;
}

// Try to find an exact match for `py` in a sorted table of T entries.
// Returns a pointer to the matching entry's chars string, or nullptr.
template<typename Entry>
static const char* exactMatch(const Entry* table, size_t count, const char* py) {
    int lo = 0, hi = (int)count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(py, table[mid].pinyin);
        if (cmp < 0) hi = mid;
        else if (cmp > 0) lo = mid + 1;
        else return table[mid].chars;
    }
    return nullptr;
}

// Returns 0 if `a` is a prefix of (or equal to) `b`, <0 if a<b, >0 if a>b.
static int cmpPinyinPrefix(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return *a - *b;
        a++; b++;
    }
    if (*a == 0) return 0;
    return *a - *b;
}

// Collect prefix matches: entries whose pinyin starts with `py`.
template<typename Entry, typename Fn>
static void prefixMatches(const Entry* table, size_t count, const char* py, Fn fn) {
    int lo = 0, hi = (int)count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cmpPinyinPrefix(py, table[mid].pinyin) > 0) lo = mid + 1;
        else hi = mid;
    }
    for (int i = lo; i < (int)count; i++) {
        if (cmpPinyinPrefix(py, table[i].pinyin) != 0) break;
        if (!fn(table[i].chars)) return;
    }
}

void PinyinIME::resolveCandidates() {
    _candCount = 0;
    _candBufLen = 0;
    _candBuf[0] = '\0';
    if (_pyLen == 0) return;

    // 1. Single-character exact match for the full composing string.
    //    E.g. typing "ni" → "你" as the first candidate.
    //    This takes priority over phrases so short pinyin like "ni" doesn't
    //    get drowned by phrase prefix matches like "nianchan"(年产).
    {
        int lo = 0, hi = (int)PINYIN_ENTRY_COUNT;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            int cmp = strcmp(_py, pinyinTable[mid].pinyin);
            if (cmp < 0) hi = mid;
            else if (cmp > 0) lo = mid + 1;
            else {
                const char* chars = pinyinTable[mid].chars;
                const char* p = chars;
                while (*p && _candCount < MAX_CANDS) {
                    int len = 1;
                    unsigned char c0 = (unsigned char)*p;
                    if (c0 >= 0xF0) len = 4;
                    else if (c0 >= 0xE0) len = 3;
                    else if (c0 >= 0xC0) len = 2;
                    char one[5];
                    memcpy(one, p, len);
                    one[len] = '\0';
                    addCandidate(one);
                    p += len;
                }
                break;
            }
        }
    }

    // 2. Exact phrase match (e.g. "nihao" → "你好")
    {
        int lo = 0, hi = (int)PHRASE_ENTRY_COUNT;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            int cmp = strcmp(_py, phraseTable[mid].pinyin);
            if (cmp < 0) hi = mid;
            else if (cmp > 0) lo = mid + 1;
            else { addCandidate(phraseTable[mid].phrases); break; }
        }
    }

    // 3. Phrase prefix matches (e.g. "ni" → "你好", "你们", ...)
    {
        int lo = 0, hi = (int)PHRASE_ENTRY_COUNT;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (cmpPinyinPrefix(_py, phraseTable[mid].pinyin) > 0) lo = mid + 1;
            else hi = mid;
        }
        for (int i = lo; i < (int)PHRASE_ENTRY_COUNT && _candCount < MAX_CANDS; i++) {
            if (cmpPinyinPrefix(_py, phraseTable[i].pinyin) != 0) break;
            if (strcmp(phraseTable[i].pinyin, _py) == 0) continue;  // exact already added
            addCandidate(phraseTable[i].phrases);
        }
    }

    // 4. Single-character candidates for the longest matching syllable suffix.
    //    Only used when the full composing string isn't a valid syllable
    //    (e.g. "nihao" → try "hao" → 好).
    if (_candCount == 0) {
        for (int start = _pyLen > 6 ? _pyLen - 6 : 0; start <= _pyLen - 1 && _candCount < MAX_CANDS; start++) {
            const char* suffix = _py + start;
            int lo = 0, hi = (int)PINYIN_ENTRY_COUNT;
            const char* chars = nullptr;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                int cmp = strcmp(suffix, pinyinTable[mid].pinyin);
                if (cmp < 0) hi = mid;
                else if (cmp > 0) lo = mid + 1;
                else { chars = pinyinTable[mid].chars; break; }
            }
            if (chars) {
                const char* p = chars;
                while (*p && _candCount < MAX_CANDS) {
                    int len = 1;
                    unsigned char c0 = (unsigned char)*p;
                    if (c0 >= 0xF0) len = 4;
                    else if (c0 >= 0xE0) len = 3;
                    else if (c0 >= 0xC0) len = 2;
                    char one[5];
                    memcpy(one, p, len);
                    one[len] = '\0';
                    addCandidate(one);
                    p += len;
                }
                break;
            }
        }
    }
}

void PinyinIME::select(int i) {
    if (i < 0 || i >= _candCount) return;
    // Only copy _candLen[i] bytes — commitStr would run to the next NUL,
    // but candidates are packed back-to-back in _candBuf without separators.
    int len = _candLen[i];
    if (_outLen + len < (int)sizeof(_out) - 1) {
        memcpy(_out + _outLen, _candBuf + _candOffset[i], len);
        _outLen += len;
        _out[_outLen] = '\0';
    }

    _pyLen = 0;
    _py[0] = '\0';
    _candCount = 0;
    _candBufLen = 0;
    _candBuf[0] = '\0';
}

PinyinIME::Action PinyinIME::handleKey(char c, bool del, bool enter) {
    if (del) {
        if (_pyLen > 0) {
            _pyLen--;
            _py[_pyLen] = '\0';
            resolveCandidates();
            return Redraw;
        }
        if (_outLen > 0) {
            int back = 1;
            while (back < 4 && _outLen - back >= 0 &&
                   ((unsigned char)_out[_outLen - back] & 0xC0) == 0x80) {
                back++;
            }
            _outLen -= back;
            if (_outLen < 0) _outLen = 0;
            _out[_outLen] = '\0';
            return Redraw;
        }
        return None;
    }

    if (enter) {
        if (_pyLen > 0) {
            commitStr(_py);
            _pyLen = 0;
            _py[0] = '\0';
            _candCount = 0;
        }
        return Send;
    }

    if (!_pinyinMode) {
        commitChar(c);
        return Redraw;
    }

    if (c >= 'a' && c <= 'z') {
        if (_pyLen < MAX_PINYIN) {
            _py[_pyLen++] = c;
            _py[_pyLen] = '\0';
            resolveCandidates();
        }
        return Redraw;
    }

    if (c >= '1' && c <= '9') {
        select(c - '1');
        return Redraw;
    }

    if (_pyLen > 0) {
        commitStr(_py);
        _pyLen = 0;
        _py[0] = '\0';
        _candCount = 0;
    }
    commitChar(c);
    return Redraw;
}
