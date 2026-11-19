#define _POSIX_C_SOURCE 200809L
#include "package.h"

#include <ctype.h>
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const unsigned char *p, *end;
    const char *error;
} parser;

static void space(parser *p) {
    while (p->p < p->end && strchr(" \r\n\t", *p->p) && *p->p) ++p->p;
}

static int take(parser *p, int c) {
    space(p);
    if (p->p == p->end || *p->p != c) return 0;
    ++p->p;
    return 1;
}

static int word(parser *p, const char *literal) {
    size_t n = strlen(literal);
    space(p);
    if ((size_t)(p->end - p->p) < n || memcmp(p->p, literal, n)) return 0;
    p->p += n;
    return 1;
}

static int hex4(parser *p, unsigned *v) {
    *v = 0;
    for (int i = 0; i < 4; ++i) {
        if (p->p == p->end) return 0;
        unsigned c = *p->p++, n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else return 0;
        *v = (*v << 4) | n;
    }
    return 1;
}

static int utf8_valid(const unsigned char *s) {
    while (*s) {
        unsigned c = *s++, n, min;
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; min = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; min = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; min = 0x10000; c &= 7; }
        else return 0;
        while (n--) {
            if ((*s & 0xc0) != 0x80) return 0;
            c = (c << 6) | (*s++ & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    }
    return 1;
}

static int string(parser *p, char *out, size_t capacity) {
    size_t n = 0;
    if (!take(p, '"')) return 0;
    while (p->p < p->end) {
        unsigned c = *p->p++;
        if (c == '"') {
            out[n] = 0;
            return utf8_valid((const unsigned char *)out);
        }
        if (c < 32) return 0;
        if (c == '\\') {
            if (p->p == p->end) return 0;
            c = *p->p++;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                if (!hex4(p, &c) || !c) return 0;
                if (c >= 0xd800 && c <= 0xdbff) {
                    unsigned lo;
                    if (p->end - p->p < 6 || *p->p++ != '\\' || *p->p++ != 'u' ||
                        !hex4(p, &lo) || lo < 0xdc00 || lo > 0xdfff) return 0;
                    c = 0x10000 + ((c - 0xd800) << 10) + lo - 0xdc00;
                } else if (c >= 0xdc00 && c <= 0xdfff) return 0;
                unsigned char encoded[4];
                unsigned count = 0;
                if (c < 0x80) encoded[count++] = (unsigned char)c;
                else if (c < 0x800) {
                    encoded[count++] = (unsigned char)(0xc0 | (c >> 6));
                    encoded[count++] = (unsigned char)(0x80 | (c & 63));
                } else if (c < 0x10000) {
                    encoded[count++] = (unsigned char)(0xe0 | (c >> 12));
                    encoded[count++] = (unsigned char)(0x80 | ((c >> 6) & 63));
                    encoded[count++] = (unsigned char)(0x80 | (c & 63));
                } else {
                    encoded[count++] = (unsigned char)(0xf0 | (c >> 18));
                    encoded[count++] = (unsigned char)(0x80 | ((c >> 12) & 63));
                    encoded[count++] = (unsigned char)(0x80 | ((c >> 6) & 63));
                    encoded[count++] = (unsigned char)(0x80 | (c & 63));
                }
                if (n + count >= capacity) return 0;
                memcpy(out + n, encoded, count);
                n += count;
                continue;
            }
            default: return 0;
            }
        }
        if (n + 1 >= capacity) return 0;
        out[n++] = (char)c;
    }
    return 0;
}

static int portable_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

int app_id_valid(const char *s) {
    size_t n = strlen(s);
    if (!n || n > APP_ID_MAX || !isalnum((unsigned char)s[0])) return 0;
    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
              *s == '_' || *s == '-')) return 0;
    return 1;
}

int app_path_valid(const char *path) {
    size_t n = strlen(path);
    if (!n || n >= APP_PATH_MAX || *path == '/') return 0;
    const char *part = path;
    for (const char *p = path;; ++p) {
        if (!*p || *p == '/') {
            size_t len = (size_t)(p - part);
            if (!len || (len == 1 && *part == '.') ||
                (len == 2 && part[0] == '.' && part[1] == '.')) return 0;
            if (!*p) return 1;
            part = p + 1;
        } else if (!portable_char((unsigned char)*p)) return 0;
    }
}

int app_manifest_parse(const char *text, size_t length, app_manifest *out,
                       char *error, size_t error_size) {
    parser p = {(const unsigned char *)text, (const unsigned char *)text + length, NULL};
    const char *why = "invalid JSON or manifest field";
    unsigned seen = 0;
    memset(out, 0, sizeof(*out));
    if (!length || length > APP_MANIFEST_MAX || !take(&p, '{')) goto bad;
    for (;;) {
        char key[32];
        if (!string(&p, key, sizeof(key)) || !take(&p, ':')) goto bad;
        unsigned field;
        if (!strcmp(key, "schema")) field = 0;
        else if (!strcmp(key, "id")) field = 1;
        else if (!strcmp(key, "name")) field = 2;
        else if (!strcmp(key, "version")) field = 3;
        else if (!strcmp(key, "entry")) field = 4;
        else if (!strcmp(key, "args")) field = 5;
        else if (!strcmp(key, "display")) field = 6;
        else { why = "unknown manifest field"; goto bad; }
        if (seen & (1u << field)) { why = "duplicate manifest field"; goto bad; }
        seen |= 1u << field;
        if (field == 0) {
            if (!take(&p, '1')) { why = "unsupported schema"; goto bad; }
        } else if (field == 6) {
            if (word(&p, "true")) out->display = 1;
            else if (!word(&p, "false")) { why = "display must be true or false"; goto bad; }
        } else if (field == 5) {
            if (!take(&p, '[')) goto bad;
            if (!take(&p, ']')) {
                do {
                    if (out->argc == APP_ARGS_MAX ||
                        !string(&p, out->args[out->argc++], APP_ARG_MAX)) goto bad;
                } while (take(&p, ','));
                if (!take(&p, ']')) goto bad;
            }
        } else {
            char *dst = field == 1 ? out->id : field == 2 ? out->name :
                        field == 3 ? out->version : out->entry;
            size_t cap = field == 1 ? sizeof(out->id) : field == 2 ? sizeof(out->name) :
                         field == 3 ? sizeof(out->version) : sizeof(out->entry);
            if (!string(&p, dst, cap) || !*dst) goto bad;
        }
        if (take(&p, '}')) break;
        if (!take(&p, ',')) goto bad;
    }
    space(&p);
    if (p.p != p.end || (seen & 31u) != 31u) goto bad;
    if (!app_id_valid(out->id)) { why = "unsafe app id"; goto bad; }
    if (!app_path_valid(out->entry)) { why = "unsafe entry path"; goto bad; }
    for (const unsigned char *s = (const unsigned char *)out->name; *s; ++s)
        if (*s < 32 || *s == 127) { why = "control character in app name"; goto bad; }
    for (const unsigned char *s = (const unsigned char *)out->version; *s; ++s)
        if (!portable_char(*s) && *s != '+') { why = "invalid version"; goto bad; }
    return 0;
bad:
    snprintf(error, error_size, "%s", why);
    return -1;
}

int app_elf_validate(int fd, char *error, size_t error_size) {
    Elf64_Ehdr eh;
    struct stat st;
    const char *why = "invalid or incompatible static ELF";
#ifdef SD_APPS_HOST_TEST
#if defined(__x86_64__)
    const unsigned machine = EM_X86_64;
#elif defined(__aarch64__)
    const unsigned machine = EM_AARCH64;
#else
#error Unsupported test host architecture
#endif
#else
    const unsigned machine = EM_AARCH64;
#endif
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < (off_t)sizeof(eh) ||
        pread(fd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh)) goto bad;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_ident[EI_VERSION] != EV_CURRENT ||
        (eh.e_ident[EI_OSABI] != ELFOSABI_NONE && eh.e_ident[EI_OSABI] != ELFOSABI_LINUX) ||
        eh.e_machine != machine || eh.e_version != EV_CURRENT || eh.e_type != ET_EXEC ||
        eh.e_ehsize != sizeof(eh) || eh.e_phentsize != sizeof(Elf64_Phdr) ||
        !eh.e_phnum || eh.e_phnum > 128 || eh.e_phoff > (uint64_t)st.st_size ||
        (uint64_t)eh.e_phnum * sizeof(Elf64_Phdr) > (uint64_t)st.st_size - eh.e_phoff) goto bad;
    int entry_ok = 0;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        if (pread(fd, &ph, sizeof(ph), (off_t)(eh.e_phoff + i * sizeof(ph))) !=
            (ssize_t)sizeof(ph)) goto bad;
        if (ph.p_type == PT_INTERP || ph.p_type == PT_DYNAMIC) {
            why = "dynamic executables are unsupported; build a static executable";
            goto bad;
        }
        if (ph.p_type == PT_LOAD) {
            if (ph.p_offset > (uint64_t)st.st_size ||
                ph.p_filesz > (uint64_t)st.st_size - ph.p_offset ||
                ph.p_filesz > ph.p_memsz || ph.p_vaddr > UINT64_MAX - ph.p_memsz) goto bad;
            if ((ph.p_flags & PF_X) && eh.e_entry >= ph.p_vaddr &&
                eh.e_entry - ph.p_vaddr < ph.p_filesz) entry_ok = 1;
        }
    }
    if (!entry_ok) goto bad;
    return 0;
bad:
    snprintf(error, error_size, "%s", why);
    return -1;
}
