/* fs：见 fs.h。Windows FindFirstFileW / POSIX dirent 双实现。
 * 产品代码要求五平台 CI 全绿，不走 gamepack 的 MinGW-only 路线。 */
#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

typedef struct {
    omnic_fs_entry *v;
    size_t n, cap;
} entry_vec;

static int vec_push(entry_vec *ev, const omnic_fs_entry *e) {
    if (ev->n == ev->cap) {
        size_t cap = ev->cap ? ev->cap * 2 : 1024;
        omnic_fs_entry *v = (omnic_fs_entry *)realloc(ev->v, cap * sizeof(*v));
        if (v == NULL) {
            omnic_set_error("fs: out of memory");
            return -1;
        }
        ev->v = v;
        ev->cap = cap;
    }
    ev->v[ev->n++] = *e;
    return 0;
}

#ifdef _WIN32

#include <windows.h>

/* UTF-8 → UTF-16；非法序列回退 CP_ACP（MinGW argv 是 ACP 编码的现实兜底）。
 * 返回含结尾 NUL 的字符数，失败 0。 */
static int utf8_to_w16(const char *s, wchar_t *out, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap);
    if (n > 0) return n;
    return MultiByteToWideChar(CP_ACP, 0, s, -1, out, cap);
}

static long long filetime_unix(const FILETIME *ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static int walk_dir(const wchar_t *dir, const char *rel, entry_vec *ev, size_t *skipped) {
    wchar_t wpat[OMNIC_FS_PATH_MAX];
    if (_snwprintf(wpat, OMNIC_FS_PATH_MAX, L"%s\\*", dir) >= OMNIC_FS_PATH_MAX) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        (*skipped)++;
        return 0;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char name[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL) == 0) {
            (*skipped)++;
            continue;
        }
        char child_rel[OMNIC_FS_PATH_MAX];
        if (rel[0] != '\0') {
            if (snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, name) >= (int)sizeof(child_rel)) {
                (*skipped)++;
                continue;
            }
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s", name);
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            (*skipped)++;
            continue;
        }
        omnic_fs_entry e;
        memset(&e, 0, sizeof(e));
        e.mtime = filetime_unix(&fd.ftLastWriteTime);
        snprintf(e.rel, sizeof(e.rel), "%s", child_rel);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            e.is_dir = 1;
            if (vec_push(ev, &e) != 0) break;
            wchar_t wchild[OMNIC_FS_PATH_MAX];
            if (_snwprintf(wchild, OMNIC_FS_PATH_MAX, L"%s\\%s", dir, fd.cFileName) < OMNIC_FS_PATH_MAX)
                walk_dir(wchild, child_rel, ev, skipped);
        } else {
            e.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (vec_push(ev, &e) != 0) break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

omnic_fs_entry *omnic_fs_walk(const char *root, size_t *n, size_t *skipped) {
    wchar_t wroot[OMNIC_FS_PATH_MAX];
    if (utf8_to_w16(root, wroot, OMNIC_FS_PATH_MAX) == 0) {
        omnic_set_error("fs: cannot convert path: %s", root);
        return NULL;
    }
    size_t len = wcslen(wroot);
    while (len > 0 && (wroot[len - 1] == L'/' || wroot[len - 1] == L'\\')) wroot[--len] = L'\0';
    if (len == 0) {
        omnic_set_error("fs: empty root path");
        return NULL;
    }
    entry_vec ev = {NULL, 0, 0};
    *skipped = 0;
    walk_dir(wroot, "", &ev, skipped);
    *n = ev.n;
    return ev.v;
}

omnic_status omnic_fs_mkdir_p(const char *path) {
    wchar_t wpath[OMNIC_FS_PATH_MAX];
    if (utf8_to_w16(path, wpath, OMNIC_FS_PATH_MAX) == 0) return OMNIC_E_USAGE;
    /* 按分隔符逐级建；盘符（含 ':' 的首段）跳过 */
    for (wchar_t *p = wpath; *p != L'\0'; p++) {
        if (*p != L'/' && *p != L'\\') continue;
        wchar_t c = *p;
        *p = L'\0';
        if (wcschr(wpath, L':') == NULL || wpath + 2 < p) {
            if (!CreateDirectoryW(wpath, NULL)) {
                DWORD e = GetLastError();
                if (e != ERROR_ALREADY_EXISTS) {
                    omnic_set_error("fs: mkdir failed (win32 %lu): %s", (unsigned long)e, path);
                    return OMNIC_E_SYSTEM;
                }
                DWORD at = GetFileAttributesW(wpath);
                if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
                    omnic_set_error("fs: not a directory: %s", path);
                    return OMNIC_E_SYSTEM;
                }
            }
        }
        *p = c;
    }
    if (!CreateDirectoryW(wpath, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            omnic_set_error("fs: mkdir failed (win32 %lu): %s", (unsigned long)e, path);
            return OMNIC_E_SYSTEM;
        }
    }
    return OMNIC_OK;
}

omnic_status omnic_fs_set_mtime(const char *path, long long mtime) {
    wchar_t wpath[OMNIC_FS_PATH_MAX];
    if (utf8_to_w16(path, wpath, OMNIC_FS_PATH_MAX) == 0) return OMNIC_E_USAGE;
    HANDLE h = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL); /* BACKUP_SEMANTICS 兼目录 */
    if (h == INVALID_HANDLE_VALUE) return OMNIC_E_SYSTEM;
    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)mtime * 10000000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    BOOL ok = SetFileTime(h, NULL, &ft, &ft);
    CloseHandle(h);
    return ok ? OMNIC_OK : OMNIC_E_SYSTEM;
}

FILE *omnic_fs_fopen(const char *utf8_path, const char *mode) {
    wchar_t wpath[OMNIC_FS_PATH_MAX * 2], wmode[8];
    if (utf8_to_w16(utf8_path, wpath, OMNIC_FS_PATH_MAX * 2) == 0) return NULL;
    int i;
    for (i = 0; mode[i] != '\0' && i < 7; i++) wmode[i] = (wchar_t)(unsigned char)mode[i];
    wmode[i] = L'\0';
    return _wfopen(wpath, wmode);
}

#else /* POSIX */

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>

static int walk_dir(const char *dir, const char *rel, entry_vec *ev, size_t *skipped) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        (*skipped)++;
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child_path[OMNIC_FS_PATH_MAX], child_rel[OMNIC_FS_PATH_MAX];
        if (rel[0] != '\0')
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name);
        else
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        if (snprintf(child_path, sizeof(child_path), "%s/%s", dir, ent->d_name) >= (int)sizeof(child_path)) {
            (*skipped)++;
            continue;
        }
        struct stat st;
        if (lstat(child_path, &st) != 0) {
            (*skipped)++;
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            (*skipped)++;
            continue;
        }
        omnic_fs_entry e;
        memset(&e, 0, sizeof(e));
        snprintf(e.rel, sizeof(e.rel), "%s", child_rel);
        e.mtime = (long long)st.st_mtime;
        if (S_ISDIR(st.st_mode)) {
            e.is_dir = 1;
            if (vec_push(ev, &e) != 0) break;
            walk_dir(child_path, child_rel, ev, skipped);
        } else if (S_ISREG(st.st_mode)) {
            e.size = (unsigned long long)st.st_size;
            if (vec_push(ev, &e) != 0) break;
        } else {
            (*skipped)++; /* fifo/socket/dev */
        }
    }
    closedir(d);
    return 0;
}

omnic_fs_entry *omnic_fs_walk(const char *root, size_t *n, size_t *skipped) {
    entry_vec ev = {NULL, 0, 0};
    *skipped = 0;
    walk_dir(root, "", &ev, skipped);
    *n = ev.n;
    return ev.v;
}

omnic_status omnic_fs_mkdir_p(const char *path) {
    char buf[OMNIC_FS_PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) return OMNIC_E_USAGE;
    for (char *p = buf + 1; *p != '\0'; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
            omnic_set_error("fs: mkdir failed: %s", buf);
            return OMNIC_E_SYSTEM;
        }
        *p = '/';
    }
    if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
        omnic_set_error("fs: mkdir failed: %s", buf);
        return OMNIC_E_SYSTEM;
    }
    /* 终点若非目录（如已是文件）则报错 */
    struct stat st;
    if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
        omnic_set_error("fs: not a directory: %s", buf);
        return OMNIC_E_SYSTEM;
    }
    return OMNIC_OK;
}

omnic_status omnic_fs_set_mtime(const char *path, long long mtime) {
    struct utimbuf t;
    t.actime = (time_t)mtime;
    t.modtime = (time_t)mtime;
    return utime(path, &t) == 0 ? OMNIC_OK : OMNIC_E_SYSTEM;
}

FILE *omnic_fs_fopen(const char *utf8_path, const char *mode) {
    return fopen(utf8_path, mode);
}

#endif /* _WIN32 */
