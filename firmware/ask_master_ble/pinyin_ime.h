#ifndef PINYIN_IME_H
#define PINYIN_IME_H

#include <Arduino.h>
#include "pinyin_dict.h"
#include "pinyin_phrases.h"

// A pinyin input method for the Cardputer keyboard with phrase support.
//
// As the user types pinyin letters, the IME builds a composing string (e.g.
// "nihao"). It then offers two kinds of candidates:
//   1. Phrase matches from phraseTable (e.g. "nihao" → "你好")
//   2. Single-character matches from pinyinTable for the last syllable
//      (e.g. after selecting "你", typing "hao" offers "好")
//
// Candidates are stored as a list of (pointer, length) pairs into a flat
// buffer, so a phrase candidate and a single-char candidate can coexist.
class PinyinIME {
public:
    static constexpr int MAX_PINYIN = 12;       // "zhuangguo..." up to 12 letters
    static constexpr int MAX_CANDS = 10;        // candidates shown on screen
    static constexpr int MAX_RETURN = 80;       // composed output buffer
    static constexpr int CAND_BUF_SIZE = 160;   // flat buffer for candidate text

    PinyinIME() { reset(); }

    void reset();

    enum Action { None, Redraw, Send };
    Action handleKey(char c, bool del, bool enter);

    const char* text() const { return _out; }
    const char* composing() const { return _py; }

    // Returns a NUL-terminated string for candidate i, or nullptr if i is out
    // of range. The pointer is valid until the next handleKey/select call.
    const char* candidate(int i) const;

    int candidateCount() const { return _candCount; }

    // Pick candidate index i (0-based). Appends the chosen text to the output
    // and clears the composing syllable.
    void select(int i);

    bool pinyinMode() const { return _pinyinMode; }
    void toggleMode() { _pinyinMode = !_pinyinMode; _pyLen = 0; }

private:
    void commitChar(char c);
    void commitStr(const char* s);
    void resolveCandidates();
    void addCandidate(const char* s);
    static int cmpPinyin(const char* a, const char* b);
    // Extract the i-th UTF-8 character from a NUL-terminated string.
    static const char* utf8CharAt(const char* s, int i, int& outLen);

    char _py[MAX_PINYIN + 1];
    int _pyLen = 0;

    // Flat buffer holding all candidate text back-to-back.
    char _candBuf[CAND_BUF_SIZE];
    int _candBufLen = 0;
    // Offsets of each candidate within _candBuf.
    int _candOffset[MAX_CANDS];
    int _candLen[MAX_CANDS];     // byte length of each candidate
    int _candCount = 0;

    char _out[MAX_RETURN + 8];
    int _outLen = 0;

    bool _pinyinMode = true;
};

inline const char* PinyinIME::candidate(int i) const {
    if (i < 0 || i >= _candCount) return nullptr;
    return _candBuf + _candOffset[i];
}

inline void PinyinIME::reset() {
    _pyLen = 0;
    _py[0] = '\0';
    _candBufLen = 0;
    _candBuf[0] = '\0';
    _candCount = 0;
    _outLen = 0;
    _out[0] = '\0';
    _pinyinMode = true;
}

#endif  // PINYIN_IME_H
