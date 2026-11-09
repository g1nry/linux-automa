#include <fileward/glob.h>

#include <stdio.h>

int main(void) {
    static const struct {
        const char *pattern;
        const char *path;
        int expected;
    } cases[] = {
        {"*.txt", "a.txt", 1}, {"*.txt", "sub/a.txt", 0},
        {"**/*.txt", "a.txt", 1}, {"**/*.txt", "sub/deep/a.txt", 1},
        {"docs/**/*.md", "docs/readme.md", 1}, {"docs/**/*.md", "other/readme.md", 0},
        {"a?.txt", "ab.txt", 1}, {"a?.txt", "a/.txt", 0},
        {"[a-c].txt", "b.txt", 1}, {"[!a-c].txt", "z.txt", 1},
        {"[!a-c].txt", "b.txt", 0}, {"[abc", "a", 0},
        {"\\*.txt", "*.txt", 1}, {"", "", 1}, {"*", "", 1},
        {"[!a]", "", 0}, {"[!a]", "/", 0}, {"[/]", "/", 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (glob_match(cases[i].pattern, cases[i].path) != cases[i].expected) {
            fprintf(stderr, "glob failed: pattern '%s', path '%s'\n", cases[i].pattern, cases[i].path);
            return 1;
        }
    }
    puts("glob tests passed");
    return 0;
}
