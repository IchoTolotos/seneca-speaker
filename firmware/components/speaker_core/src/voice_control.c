#include "voice_control.h"
#include <string.h>

bool sc_barge_in_feed(sc_barge_in_t *s, const int16_t *pcm, size_t count,
                      uint64_t now, bool speaking, bool muted)
{
    if (!s) return false;
    if (!speaking || muted || !pcm || !count || count > 16000) {
        s->above = false;
        return false;
    }
    uint64_t energy = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t v = pcm[i];
        energy += (uint64_t)(v * v);
    }
    // 1475 / 32768 ~= 0.045, matching the existing browser client monitor.
    if (energy < (uint64_t)1475 * 1475 * count) {
        s->above = false;
        return false;
    }
    if (!s->above) { s->above = true; s->above_since_ms = now; }
    if (now - s->above_since_ms < 120 ||
        (s->interrupted && now - s->last_interrupt_ms < 800)) return false;
    s->above = false;
    s->interrupted = true;
    s->last_interrupt_ms = now;
    return true;
}

bool sc_voice_is_hangup(const char *text)
{
    if (!text) return false;
    char words[128];
    size_t used = 0;
    bool space = false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        unsigned char ch = *p;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch >= 'a' && ch <= 'z') {
            if (used + (space ? 1 : 0) + 1 >= sizeof words) return false;
            if (space) words[used++] = ' ';
            words[used++] = (char)ch;
            space = false;
        } else if (ch >= 0x80 || (ch >= '0' && ch <= '9')) {
            return false; // never remove arbitrary words/numbers and accidentally create a command
        } else if (used) {
            space = true;
        }
    }
    words[used] = 0;
    // Same spoken phrase; accept the common German ASR spellings too.
    return !strcmp(words, "seneca stop") || !strcmp(words, "seneca stopp") ||
           !strcmp(words, "seneka stop") || !strcmp(words, "seneka stopp");
}

void sc_voice_stop_update(sc_voice_stop_t *s, const char *text, uint64_t now)
{
    if (!s || !text) return;
    if (sc_voice_is_hangup(text)) {
        s->candidate = true;
        s->confirm_at_ms = now + 500;
        s->addressed = false;
        return;
    }
    // Some transcription events contain individual word fragments rather than the
    // whole utterance. The short-lived prefix still requires the spoken Seneca.
    char word[32];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        unsigned char ch = *p;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch >= 'a' && ch <= 'z') {
            if (n + 1 >= sizeof word) { s->candidate = s->addressed = false; return; }
            word[n++] = (char)ch;
        } else if (ch >= 0x80 || (ch >= '0' && ch <= '9')) {
            s->candidate = s->addressed = false; return;
        } else if (*p && n) {
            // A second word is not a single-word prefix/stop fragment.
            const unsigned char *q = p;
            while (*q && !((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z'))) ++q;
            if (*q) { s->candidate = s->addressed = false; return; }
        }
    }
    word[n] = 0;
    if (!strcmp(word, "seneca") || !strcmp(word, "seneka")) {
        s->addressed = true;
        s->prefix_until_ms = now + 3000;
        s->candidate = false;
    } else if ((!strcmp(word, "stop") || !strcmp(word, "stopp")) &&
               s->addressed && now <= s->prefix_until_ms) {
        s->candidate = true;
        s->confirm_at_ms = now + 500;
        s->addressed = false;
    } else if (n) {
        s->candidate = s->addressed = false;
    }
}

bool sc_voice_stop_poll(sc_voice_stop_t *s, uint64_t now)
{
    if (!s || !s->candidate || now < s->confirm_at_ms) return false;
    s->candidate = s->addressed = false;
    return true;
}
