#include <fileward/glob.h>

#include <stddef.h>

static int match_internal(const char *pattern, const char *text) {
    if (*pattern == '\0') {
        return *text == '\0';
    }

    if (pattern[0] == '*' && pattern[1] == '*') {
        const char *rest = pattern + 2;
        if (*rest == '/') {
            rest++;
        }

        const char *t = text;
        do {
            if (match_internal(rest, t)) {
                return 1;
            }
            if (*t == '\0') {
                break;
            }
            t++;
        } while (1);

        return 0;
    }

    if (*pattern == '*') {
        const char *t = text;
        do {
            if (match_internal(pattern + 1, t)) {
                return 1;
            }
            if (*t == '/' || *t == '\0') {
                break;
            }
            t++;
        } while (1);
        return 0;
    }

    if (*pattern == '?') {
        if (*text == '\0' || *text == '/') {
            return 0;
        }
        return match_internal(pattern + 1, text + 1);
    }

    if (*pattern == '\\' && pattern[1] != '\0') {
        if (*text != pattern[1]) {
            return 0;
        }
        return match_internal(pattern + 2, text + 1);
    }

    if (*pattern == '[') {
        int negate = 0;
        const char *pat = pattern + 1;
        if (*pat == '!') {
            negate = 1;
            pat++;
        }

        int matched = 0;
        while (*pat != '\0' && *pat != ']') {
            if (pat[1] == '-' && pat[2] != ']' && pat[2] != '\0') {
                char start = *pat;
                char end = pat[2];
                if (*text >= start && *text <= end) {
                    matched = 1;
                }
                pat += 3;
            } else {
                if (*pat == *text) {
                    matched = 1;
                }
                pat++;
            }
        }

        if (*pat != ']') {
            return 0;
        }

        if (matched == negate) {
            return 0;
        }

        return match_internal(pat + 1, text + 1);
    }

    if (*pattern == *text) {
        return match_internal(pattern + 1, text + 1);
    }

    return 0;
}

int glob_match(const char *pattern, const char *text) {
    if (pattern == NULL || text == NULL) {
        return 0;
    }
    return match_internal(pattern, text);
}
