#ifndef _CTYPE_H
#define _CTYPE_H

// Complete ASCII-only ctype implementation for kernel use.
// This header provides the standard C character classification and
// conversion helpers. It is intentionally simple and locale-independent.

// Character classification — return nonzero if true, 0 if false.
static inline int isascii(int c) {
    return (unsigned int)c <= 0x7F;
}

static inline int isdigit(int c) {
    unsigned char uc = (unsigned char)c;
    return uc >= '0' && uc <= '9';
}

static inline int isupper(int c) {
    unsigned char uc = (unsigned char)c;
    return uc >= 'A' && uc <= 'Z';
}

static inline int islower(int c) {
    unsigned char uc = (unsigned char)c;
    return uc >= 'a' && uc <= 'z';
}

static inline int isalpha(int c) {
    return isupper(c) || islower(c);
}

static inline int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

static inline int isxdigit(int c) {
    unsigned char uc = (unsigned char)c;
    return isdigit(uc) || (uc >= 'A' && uc <= 'F') || (uc >= 'a' && uc <= 'f');
}

static inline int isspace(int c) {
    unsigned char uc = (unsigned char)c;
    return uc == ' '  || uc == '\t' || uc == '\n' ||
           uc == '\r' || uc == '\v' || uc == '\f';
}

static inline int isblank(int c) {
    unsigned char uc = (unsigned char)c;
    return uc == ' ' || uc == '\t';
}

static inline int isprint(int c) {
    unsigned char uc = (unsigned char)c;
    return uc >= 0x20 && uc <= 0x7E;
}

static inline int isgraph(int c) {
    unsigned char uc = (unsigned char)c;
    return uc >= 0x21 && uc <= 0x7E;
}

static inline int iscntrl(int c) {
    unsigned char uc = (unsigned char)c;
    return (uc <= 0x1F) || (uc == 0x7F);
}

static inline int ispunct(int c) {
    unsigned char uc = (unsigned char)c;
    return isprint(uc) && !isalnum(uc) && uc != ' ';
}

static inline int islower_or_upper(int c) {
    return islower(c) || isupper(c);
}

static inline int toupper(int c) {
    unsigned char uc = (unsigned char)c;
    if (islower(uc)) {
        return uc - 'a' + 'A';
    }
    return uc;
}

static inline int tolower(int c) {
    unsigned char uc = (unsigned char)c;
    if (isupper(uc)) {
        return uc - 'A' + 'a';
    }
    return uc;
}

#endif /* _CTYPE_H */