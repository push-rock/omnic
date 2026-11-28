/* fs：见 fs.h。Windows FindFirstFileW / POSIX dirent 双实现。
 * 产品代码要求五平台 CI 全绿，不走 gamepack 的 MinGW-only 路线。 */
/* _DEFAULT_SOURCE 须先于任何系统头（features.h 首次包含即锁定）：
 * -std=c11 严格模式下 glibc 默认只暴露 ISO C，futimens/d_type 需显式开启。
 * Linux 的 statx（创建时间）只在 _GNU_SOURCE 下有声明。 */
#if !defined(_WIN32)
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#endif

#include "core/fs.h"

#include <errno.h>
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

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p != NULL) memcpy(p, s, n);
    return p;
}

/* 跳过分类：特殊文件 / 不可读 / 路径过长 */
enum { SKIP_SPECIAL, SKIP_UNREADABLE, SKIP_TOO_LONG };

static void note_skip(omnic_fs_skip_report *rep, int kind, const char *name) {
    if (rep == NULL) return;
    switch (kind) {
        case SKIP_SPECIAL: rep->special++; break;
        case SKIP_UNREADABLE: rep->unreadable++; break;
        default: rep->too_long++; break;
    }
    rep->total++;
    if (rep->n_first < OMNIC_FS_SKIP_SAMPLES) {
        char *dst = rep->first[rep->n_first++]; /* 截断存放：只为报错时认出是哪一条 */
        size_t l = strlen(name);
        if (l >= OMNIC_FS_SKIP_NAME) l = OMNIC_FS_SKIP_NAME - 1;
        memcpy(dst, name, l);
        dst[l] = '\0';
    }
}

void omnic_fs_entries_free(omnic_fs_entry *v, size_t n) {
    if (v == NULL) return;
    for (size_t i = 0; i < n; i++) {
        free(v[i].rel);
        free(v[i].link_target);
        free(v[i].xattr);
    }
    free(v);
}

/* 平台无关：扩展属性记录的编解码。读取端拿到的长度来自不可信的归档，
 * omnic_fs_xattr_next 对每条记录都做边界检查。 */
int omnic_fs_xattr_append(unsigned char **buf, size_t *len, const char *name, const void *val,
                          size_t val_len) {
    size_t nl = strlen(name);
    if (nl == 0 || nl > 0xFFFFu || val_len > 0xFFFFFFFFu) return -1;
    size_t need = 2 + nl + 4 + val_len;
    if (*len > (size_t)-1 - need) return -1;
    unsigned char *nb = (unsigned char *)realloc(*buf, *len + need);
    if (nb == NULL) return -1;
    unsigned char *p = nb + *len;
    p[0] = (unsigned char)nl;
    p[1] = (unsigned char)(nl >> 8);
    memcpy(p + 2, name, nl);
    p += 2 + nl;
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(val_len >> (8 * i));
    p += 4;
    if (val_len != 0) memcpy(p, val, val_len);
    *buf = nb;
    *len += need;
    return 0;
}

/* 遍历连续缓冲区：*pos 为游标。返回 1=取到一条，0=结束，-1=记录越界或畸形 */
int omnic_fs_xattr_next(const unsigned char *buf, size_t len, size_t *pos, const char **name,
                        size_t *name_len, const unsigned char **val, size_t *val_len) {
    size_t p = *pos;
    if (p == len) return 0;
    if (len < 2 || p > len - 2) return -1;
    size_t nl = (size_t)buf[p] | ((size_t)buf[p + 1] << 8);
    p += 2;
    if (nl == 0 || p > len - nl) return -1;
    *name = (const char *)(buf + p);
    *name_len = nl;
    p += nl;
    if (p > len - 4) return -1;
    size_t vl = (size_t)buf[p] | ((size_t)buf[p + 1] << 8) | ((size_t)buf[p + 2] << 16) |
                ((size_t)buf[p + 3] << 24);
    p += 4;
    if (vl > len - p) return -1;
    *val = buf + p;
    *val_len = vl;
    *pos = p + vl;
    return 1;
}

char *omnic_fs_default_archive_path(const char *root) {
    size_t len = strlen(root);
    /* 去尾分隔符（shell 补全给的正是尾斜杠）；盘符根的尾反斜杠要留 */
    while (len > 1 && (root[len - 1] == '/' || root[len - 1] == '\\') && root[len - 2] != ':')
        len--;
    char *out = (char *)malloc(len + sizeof(".omnic"));
    if (out == NULL) return NULL;
    memcpy(out, root, len);
    memcpy(out + len, ".omnic", sizeof(".omnic"));
    return out;
}

omnic_fs_entry *omnic_fs_walk_report(const char *root, size_t *n, omnic_fs_skip_report *rep) {
    return omnic_fs_walk_ex(root, n, rep, 0);
}

omnic_fs_entry *omnic_fs_walk(const char *root, size_t *n, size_t *skipped) {
    omnic_fs_skip_report rep;
    omnic_fs_entry *v = omnic_fs_walk_ex(root, n, &rep, 0);
    *skipped = rep.total;
    return v;
}

#ifdef _WIN32

#include <io.h>
#include <windows.h>

/* 叶子函数的宽路径缓冲区（非递归，余量与 \\?\ 前缀前的旧行为持平） */
#define WIN_PATH_MAX (OMNIC_FS_PATH_MAX * 2)

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#ifndef FILE_FLAG_OPEN_REPARSE_POINT
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 42, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_SET_REPARSE_POINT
#define FSCTL_SET_REPARSE_POINT CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 41, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif
#define WIN_TAG_MOUNT_POINT 0xA0000003u
#define WIN_TAG_SYMLINK 0xA000000Cu
#define WIN_REPARSE_BUF 16384 /* MAXIMUM_REPARSE_DATA_BUFFER_SIZE */

/* REPARSE_DATA_BUFFER：自带定义（各工具链把它放在不同头文件里），自然对齐 */
typedef struct {
    DWORD ReparseTag;
    WORD ReparseDataLength;
    WORD Reserved;
    union {
        struct {
            WORD SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } sym;
        struct {
            WORD SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength;
            WCHAR PathBuffer[1];
        } mnt;
    } u;
} win_reparse_data;

/* UTF-8 → UTF-16；非法序列回退 CP_ACP（MinGW argv 是 ACP 编码的现实兜底）。
 * 返回含结尾 NUL 的字符数，失败 0。 */
static int utf8_to_w16(const char *s, wchar_t *out, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap);
    if (n > 0) return n;
    return MultiByteToWideChar(CP_ACP, 0, s, -1, out, cap);
}

/* UTF-8 路径 → 不带前缀的绝对 UTF-16 路径（'/' 一并转成 '\'）。
 * 返回含结尾 NUL 的字符数，失败 0。 */
static int win_fullpath(const char *utf8, wchar_t *out, int cap) {
    wchar_t wraw[WIN_PATH_MAX];
    if (utf8_to_w16(utf8, wraw, WIN_PATH_MAX) == 0) return 0;
    for (wchar_t *p = wraw; *p != L'\0'; p++)
        if (*p == L'/') *p = L'\\';
    DWORD got = GetFullPathNameW(wraw, (DWORD)cap, out, NULL);
    return (got == 0 || got >= (DWORD)cap) ? 0 : (int)got + 1;
}

/* 再加 \\?\ 前缀：把 Win32 的路径上限从 MAX_PATH(260) 抬到缓冲区上限。前缀同时
 * 关闭系统的路径解析，所以相对路径与 '/' 必须先经 win_fullpath 消化掉。
 * 返回含结尾 NUL 的字符数，失败 0。 */
static int win_abspath(const char *utf8, wchar_t *out, int cap) {
    wchar_t wfull[WIN_PATH_MAX];
    int n = win_fullpath(utf8, wfull, WIN_PATH_MAX);
    if (n == 0) return 0;
    const wchar_t *pre = L"\\\\?\\", *src = wfull;
    if (wfull[0] == L'\\' && wfull[1] == L'\\') { /* UNC：\\server\share → \\?\UNC\server\share */
        pre = L"\\\\?\\UNC\\";
        src = wfull + 2;
    }
    int r = _snwprintf(out, (size_t)cap, L"%ls%ls", pre, src);
    return (r > 0 && r < cap) ? r + 1 : 0;
}

static long long filetime_unix(const FILETIME *ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static void unix_filetime(long long t, FILETIME *ft) {
    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)t * 10000000ULL + 116444736000000000ULL;
    ft->dwLowDateTime = u.LowPart;
    ft->dwHighDateTime = u.HighPart;
}

/* 读重解析点。成功时 *kind / *target（malloc，UTF-8）给出链接种类与目标；
 * E_USAGE = 不是可收录的链接（云文件占位、去重、应用别名等）；E_SYSTEM = 读不了。 */
static omnic_status read_link_w(const wchar_t *wpath, int *kind, char **target) {
    *kind = 0;
    *target = NULL;
    HANDLE h = CreateFileW(wpath, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) return OMNIC_E_SYSTEM;
    BYTE buf[WIN_REPARSE_BUF];
    DWORD got = 0;
    omnic_status st = OMNIC_E_SYSTEM;
    if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof(buf), &got, NULL) ||
        got < offsetof(win_reparse_data, u) + 8u) {
        CloseHandle(h);
        return st;
    }
    const win_reparse_data *rd = (const win_reparse_data *)buf;
    /* PathBuffer 的可用长度：ReparseDataLength 去掉各自的名字头（联接 8 字节，符号链接 12 字节） */
    size_t avail = 0;
    WORD sub_off = 0, sub_len = 0, prn_off = 0, prn_len = 0;
    const WCHAR *pathbuf = NULL;
    if (rd->ReparseTag == WIN_TAG_MOUNT_POINT && rd->ReparseDataLength >= 8) {
        *kind = OMNIC_FS_LINK_JUNCTION;
        pathbuf = rd->u.mnt.PathBuffer;
        sub_off = rd->u.mnt.SubstituteNameOffset;
        sub_len = rd->u.mnt.SubstituteNameLength;
        prn_off = rd->u.mnt.PrintNameOffset;
        prn_len = rd->u.mnt.PrintNameLength;
        avail = (size_t)rd->ReparseDataLength - 8u;
    } else if (rd->ReparseTag == WIN_TAG_SYMLINK && rd->ReparseDataLength >= 12) {
        DWORD at = GetFileAttributesW(wpath);
        *kind = (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY))
                    ? OMNIC_FS_LINK_DIR_SYMLINK
                    : OMNIC_FS_LINK_SYMLINK;
        pathbuf = rd->u.sym.PathBuffer;
        sub_off = rd->u.sym.SubstituteNameOffset;
        sub_len = rd->u.sym.SubstituteNameLength;
        prn_off = rd->u.sym.PrintNameOffset;
        prn_len = rd->u.sym.PrintNameLength;
        avail = (size_t)rd->ReparseDataLength - 12u;
    } else {
        CloseHandle(h);
        return OMNIC_E_USAGE; /* 不收录的重解析点 */
    }
    /* ReparseDataLength 与名字的偏移、长度都必须落在读回的数据内：畸形链接当损坏，不越界读 */
    st = OMNIC_E_DATA;
    if (got < offsetof(win_reparse_data, u) + (DWORD)rd->ReparseDataLength ||
        (sub_off & 1) != 0 || (sub_len & 1) != 0 || (prn_off & 1) != 0 || (prn_len & 1) != 0 ||
        (size_t)sub_off + sub_len > avail || (size_t)prn_off + prn_len > avail) {
        CloseHandle(h);
        return st;
    }
    const WCHAR *s = (prn_len != 0) ? pathbuf + prn_off / sizeof(WCHAR)
                                    : pathbuf + sub_off / sizeof(WCHAR);
    size_t n = (prn_len != 0) ? prn_len / sizeof(WCHAR) : sub_len / sizeof(WCHAR);
    if (prn_len == 0 && n >= 4 && wcsncmp(s, L"\\??\\", 4) == 0) { /* 退而取 SubstituteName */
        s += 4;
        n -= 4;
    }
    int need = WideCharToMultiByte(CP_UTF8, 0, s, (int)n, NULL, 0, NULL, NULL);
    char *t = (need >= 0) ? (char *)malloc((size_t)need + 1) : NULL;
    CloseHandle(h);
    if (t == NULL) return OMNIC_E_SYSTEM;
    WideCharToMultiByte(CP_UTF8, 0, s, (int)n, t, need, NULL, NULL);
    t[need] = '\0';
    *target = t;
    return OMNIC_OK;
}

static void walk_dir(const wchar_t *dir, const char *rel, entry_vec *ev,
                     omnic_fs_skip_report *rep, unsigned want) {
    wchar_t wpat[OMNIC_FS_PATH_MAX];
    if (_snwprintf(wpat, OMNIC_FS_PATH_MAX, L"%ls\\*", dir) < 0) {
        note_skip(rep, SKIP_TOO_LONG, rel);
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        note_skip(rep, SKIP_UNREADABLE, rel);
        return;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char name[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL) == 0) {
            note_skip(rep, SKIP_UNREADABLE, rel);
            continue;
        }
        char child_rel[OMNIC_FS_PATH_MAX];
        int rl = (rel[0] != '\0') ? snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, name)
                                  : snprintf(child_rel, sizeof(child_rel), "%s", name);
        if (rl < 0 || rl >= (int)sizeof(child_rel)) {
            note_skip(rep, SKIP_TOO_LONG, rel);
            continue;
        }
        wchar_t wchild[OMNIC_FS_PATH_MAX];
        if (_snwprintf(wchild, OMNIC_FS_PATH_MAX, L"%ls\\%ls", dir, fd.cFileName) < 0) {
            note_skip(rep, SKIP_TOO_LONG, child_rel);
            continue;
        }
        /* 符号链接与目录联接收录；其余重解析点（云文件占位、去重、应用别名…）不收录 */
        int link_kind = 0;
        char *target = NULL;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
            read_link_w(wchild, &link_kind, &target) != OMNIC_OK) {
            note_skip(rep, SKIP_SPECIAL, child_rel);
            continue;
        }
        omnic_fs_entry e;
        memset(&e, 0, sizeof(e));
        e.rel = dup_str(child_rel);
        if (e.rel == NULL) {
            omnic_set_error("fs: out of memory");
            free(target);
            break;
        }
        e.mtime = filetime_unix(&fd.ftLastWriteTime);
        if ((want & OMNIC_FS_WANT_CTIME) != 0 &&
            (fd.ftCreationTime.dwLowDateTime != 0 || fd.ftCreationTime.dwHighDateTime != 0))
            e.ctime = filetime_unix(&fd.ftCreationTime);
        e.attrs = fd.dwFileAttributes & OMNIC_FS_ATTR_MASK;
        if (link_kind != 0) {
            e.size = strlen(target);
            e.link_kind = link_kind;
            e.link_target = target;
        } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            e.is_dir = 1;
        } else {
            e.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
        if (vec_push(ev, &e) != 0) {
            free(e.rel);
            free(target);
            break;
        }
        if (e.is_dir) walk_dir(wchild, child_rel, ev, rep, want);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

omnic_fs_entry *omnic_fs_walk_ex(const char *root, size_t *n, omnic_fs_skip_report *rep,
                                 unsigned want) {
    wchar_t wroot[OMNIC_FS_PATH_MAX];
    if (rep != NULL) memset(rep, 0, sizeof(*rep));
    *n = 0;
    if (win_abspath(root, wroot, OMNIC_FS_PATH_MAX) == 0) {
        omnic_set_error("fs: cannot convert path: %s", root);
        return NULL;
    }
    size_t len = wcslen(wroot);
    /* 去尾分隔符；盘符根的尾反斜杠必须保留（\\?\C: 指设备，不是目录） */
    while (len > 1 && wroot[len - 1] == L'\\' && wroot[len - 2] != L':') wroot[--len] = L'\0';
    entry_vec ev = {NULL, 0, 0};
    walk_dir(wroot, "", &ev, rep, want);
    *n = ev.n;
    return ev.v;
}

omnic_status omnic_fs_mkdir_p(const char *path) {
    wchar_t wpath[WIN_PATH_MAX];
    if (win_abspath(path, wpath, WIN_PATH_MAX) == 0) return OMNIC_E_USAGE;
    /* 逐级建；起点跳过 \\?\ 前缀与盘符根 / UNC 的主机共享段——它们不是可创建的目录分量 */
    const wchar_t *start = wpath;
    if (wcsncmp(wpath, L"\\\\?\\UNC\\", 8) == 0) {
        start = wpath + 8;
        for (int seen = 0; *start != L'\0'; start++) {
            if (*start == L'\\' && ++seen == 2) {
                start++;
                break;
            }
        }
    } else if (wpath[5] == L':') {
        const wchar_t *sep = wcschr(wpath + 4, L'\\'); /* \?\C:\ 之后才是可创建的目录分量 */
        start = (sep != NULL) ? sep + 1 : wpath + wcslen(wpath);
    }
    for (wchar_t *p = (wchar_t *)start; *p != L'\0'; p++) {
        if (*p != L'\\') continue;
        wchar_t c = *p;
        *p = L'\0';
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

omnic_status omnic_fs_set_mtime_stream(FILE *f, long long mtime) {
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    if (h == INVALID_HANDLE_VALUE) return OMNIC_E_SYSTEM;
    FILETIME ft;
    unix_filetime(mtime, &ft);
    return SetFileTime(h, NULL, &ft, &ft) ? OMNIC_OK : OMNIC_E_SYSTEM;
}

FILE *omnic_fs_fopen(const char *utf8_path, const char *mode) {
    wchar_t wpath[WIN_PATH_MAX], wmode[8];
    if (win_abspath(utf8_path, wpath, WIN_PATH_MAX) == 0) return NULL;
    int i;
    for (i = 0; mode[i] != '\0' && i < 7; i++) wmode[i] = (wchar_t)(unsigned char)mode[i];
    wmode[i] = L'\0';
    return _wfopen(wpath, wmode);
}

omnic_status omnic_fs_file_id(const char *path, unsigned long long *dev, unsigned long long *ino) {
    *dev = 0;
    *ino = 0;
    wchar_t wpath[WIN_PATH_MAX];
    if (win_abspath(path, wpath, WIN_PATH_MAX) == 0) return OMNIC_E_USAGE;
    HANDLE h = CreateFileW(wpath, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return OMNIC_E_SYSTEM;
    BY_HANDLE_FILE_INFORMATION info;
    omnic_status st = OMNIC_E_SYSTEM;
    if (GetFileInformationByHandle(h, &info)) {
        *dev = info.dwVolumeSerialNumber; /* ReFS 的 128 位文件号放不下时得 0：按未知处理 */
        *ino = ((unsigned long long)info.nFileIndexHigh << 32) | info.nFileIndexLow;
        st = OMNIC_OK;
    }
    CloseHandle(h);
    return st;
}

omnic_status omnic_fs_write_file_meta(const char *path, const void *data, size_t n,
                                      const omnic_fs_meta *m, int force, int *opened) {
    wchar_t wpath[WIN_PATH_MAX];
    if (win_abspath(path, wpath, WIN_PATH_MAX) == 0) return OMNIC_E_USAGE;
    if (opened != NULL) *opened = 0;
    HANDLE h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        /* -f：只读的既存文件拒绝 CREATE_ALWAYS——强制删除后重建 */
        if (!force || omnic_fs_remove_force(path) != OMNIC_OK) return OMNIC_E_SYSTEM;
        h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return OMNIC_E_SYSTEM;
    }
    if (opened != NULL) *opened = 1;
    const uint8_t *p = (const uint8_t *)data;
    size_t left = n;
    while (left > 0) {
        DWORD chunk = (left > 0x40000000u) ? 0x40000000u : (DWORD)left;
        DWORD wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, NULL) || wrote != chunk) {
            CloseHandle(h);
            return OMNIC_E_SYSTEM;
        }
        p += wrote;
        left -= wrote;
    }
    if (m != NULL && (m->mtime != 0 || m->ctime != 0 || m->attrs != 0)) {
        /* 时间与属性一次调用；失败不视作写失败（与旧语义一致）。FILE_BASIC_INFO 的时间
         * 是 LARGE_INTEGER，与 FILETIME 同值域、同纪元。 */
        FILE_BASIC_INFO bi;
        FILETIME ft;
        memset(&bi, 0, sizeof(bi));
        if (m->mtime != 0) {
            unix_filetime(m->mtime, &ft);
            bi.LastWriteTime.QuadPart = (LONGLONG)ft.dwLowDateTime | ((LONGLONG)ft.dwHighDateTime << 32);
            bi.LastAccessTime = bi.LastWriteTime;
        }
        if (m->ctime != 0) {
            unix_filetime(m->ctime, &ft);
            bi.CreationTime.QuadPart = (LONGLONG)ft.dwLowDateTime | ((LONGLONG)ft.dwHighDateTime << 32);
        }
        unsigned a = m->attrs & OMNIC_FS_ATTR_MASK;
        bi.FileAttributes = a | FILE_ATTRIBUTE_ARCHIVE; /* 新建文件本来带归档位，别把它清掉 */
        SetFileInformationByHandle(h, FileBasicInfo, &bi, sizeof(bi));
    }
    CloseHandle(h);
    return OMNIC_OK;
}

omnic_status omnic_fs_set_meta(const char *path, const omnic_fs_meta *m) {
    wchar_t wpath[WIN_PATH_MAX];
    if (win_abspath(path, wpath, WIN_PATH_MAX) == 0) return OMNIC_E_USAGE;
    DWORD at = GetFileAttributesW(wpath);
    if (at == INVALID_FILE_ATTRIBUTES) return OMNIC_E_SYSTEM;
    DWORD nw = (at & ~OMNIC_FS_ATTR_MASK) | (m->attrs & OMNIC_FS_ATTR_MASK); /* mode 在 Windows 上无对应 */
    if (nw == at) return OMNIC_OK;
    return SetFileAttributesW(wpath, nw) ? OMNIC_OK : OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_make_symlink(const char *path, const char *target, int is_dir,
                                   int *unprivileged) {
    if (unprivileged != NULL) *unprivileged = 0;
    wchar_t wplain[WIN_PATH_MAX], wt[WIN_PATH_MAX];
    if (win_fullpath(path, wplain, WIN_PATH_MAX) == 0 || utf8_to_w16(target, wt, WIN_PATH_MAX) == 0)
        return OMNIC_E_USAGE;
    DWORD flags = (is_dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    /* CreateSymbolicLinkW 不认 \\?\ 前缀：先走普通绝对路径，只有长路径才换前缀再试 */
    if (CreateSymbolicLinkW(wplain, wt, flags)) return OMNIC_OK;
    DWORD e = GetLastError();
    if (e == ERROR_PRIVILEGE_NOT_HELD) {
        if (unprivileged != NULL) *unprivileged = 1;
        return OMNIC_E_SYSTEM;
    }
    if (e == ERROR_INVALID_PARAMETER) { /* 旧系统不认免特权标志 */
        flags &= ~SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (CreateSymbolicLinkW(wplain, wt, flags)) return OMNIC_OK;
        return OMNIC_E_SYSTEM;
    }
    wchar_t wpath[WIN_PATH_MAX];
    if (wcslen(wplain) >= MAX_PATH - 12 && win_abspath(path, wpath, WIN_PATH_MAX) != 0 &&
        CreateSymbolicLinkW(wpath, wt, flags))
        return OMNIC_OK;
    SetLastError(e);
    return OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_make_junction(const char *path, const char *target) {
    wchar_t wpath[WIN_PATH_MAX], wt[WIN_PATH_MAX], sub[WIN_PATH_MAX];
    if (win_abspath(path, wpath, WIN_PATH_MAX) == 0 || utf8_to_w16(target, wt, WIN_PATH_MAX) == 0)
        return OMNIC_E_USAGE;
    /* 联接的目标必须是绝对路径；替换名用 NT 对象管理器形式（\??\C:\…，UNC 为 \??\UNC\…） */
    int r = (wt[0] == L'\\' && wt[1] == L'\\')
                ? _snwprintf(sub, WIN_PATH_MAX, L"\\??\\UNC\\%ls", wt + 2)
                : _snwprintf(sub, WIN_PATH_MAX, L"\\??\\%ls", wt);
    if (r <= 0 || r >= WIN_PATH_MAX || (wt[1] != L':' && wt[0] != L'\\')) return OMNIC_E_USAGE;
    if (!CreateDirectoryW(wpath, NULL)) return OMNIC_E_SYSTEM;
    HANDLE h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(wpath);
        return OMNIC_E_SYSTEM;
    }
    size_t sub_w = wcslen(sub), prn_w = wcslen(wt);
    BYTE buf[WIN_REPARSE_BUF];
    win_reparse_data *rd = (win_reparse_data *)buf;
    size_t names_w = sub_w + 1 + prn_w + 1;
    if (offsetof(win_reparse_data, u) + 8 + names_w * sizeof(WCHAR) > sizeof(buf))
        return OMNIC_E_USAGE; /* 目标路径长过重解析数据上限（16 KiB） */
    memset(buf, 0, sizeof(buf));
    rd->ReparseTag = WIN_TAG_MOUNT_POINT;
    rd->u.mnt.SubstituteNameLength = (WORD)(sub_w * sizeof(WCHAR));
    rd->u.mnt.PrintNameOffset = (WORD)((sub_w + 1) * sizeof(WCHAR));
    rd->u.mnt.PrintNameLength = (WORD)(prn_w * sizeof(WCHAR));
    rd->ReparseDataLength = (WORD)(8 + names_w * sizeof(WCHAR));
    memcpy(rd->u.mnt.PathBuffer, sub, (sub_w + 1) * sizeof(WCHAR));
    memcpy(rd->u.mnt.PathBuffer + sub_w + 1, wt, (prn_w + 1) * sizeof(WCHAR));
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf,
                              (DWORD)(offsetof(win_reparse_data, u) + rd->ReparseDataLength), NULL, 0,
                              &ret, NULL);
    CloseHandle(h);
    if (!ok) {
        RemoveDirectoryW(wpath); /* 不留一个空目录冒充链接 */
        return OMNIC_E_SYSTEM;
    }
    return OMNIC_OK;
}

omnic_status omnic_fs_make_hardlink(const char *existing, const char *path) {
    wchar_t we[WIN_PATH_MAX], wp[WIN_PATH_MAX];
    if (win_abspath(existing, we, WIN_PATH_MAX) == 0 || win_abspath(path, wp, WIN_PATH_MAX) == 0)
        return OMNIC_E_USAGE;
    return CreateHardLinkW(wp, we, NULL) ? OMNIC_OK : OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_remove_force(const char *path) {
    wchar_t wpath[WIN_PATH_MAX];
    if (win_abspath(path, wpath, WIN_PATH_MAX) == 0) return OMNIC_E_USAGE;
    DWORD at = GetFileAttributesW(wpath); /* 不跟随链接：取到的是链接自身的属性 */
    if (at == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? OMNIC_OK : OMNIC_E_SYSTEM;
    }
    if (at & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(at & FILE_ATTRIBUTE_REPARSE_POINT)) return OMNIC_E_SYSTEM; /* 真实目录不归这里删 */
        return RemoveDirectoryW(wpath) ? OMNIC_OK : OMNIC_E_SYSTEM;      /* 只删链接本身 */
    }
    if ((at & FILE_ATTRIBUTE_READONLY) && !SetFileAttributesW(wpath, at & ~FILE_ATTRIBUTE_READONLY))
        return OMNIC_E_SYSTEM;
    return DeleteFileW(wpath) ? OMNIC_OK : OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_replace(const char *from, const char *to) {
    wchar_t wf[WIN_PATH_MAX], wt[WIN_PATH_MAX];
    if (win_abspath(from, wf, WIN_PATH_MAX) == 0 || win_abspath(to, wt, WIN_PATH_MAX) == 0)
        return OMNIC_E_USAGE;
    return MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? OMNIC_OK
                                                                                    : OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_remove(const char *path) {
    wchar_t wp[WIN_PATH_MAX];
    if (win_abspath(path, wp, WIN_PATH_MAX) == 0) return OMNIC_E_USAGE;
    if (DeleteFileW(wp)) return OMNIC_OK;
    return GetLastError() == ERROR_FILE_NOT_FOUND ? OMNIC_OK : OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_rmdir(const char *path) {
    wchar_t wp[WIN_PATH_MAX];
    if (win_abspath(path, wp, WIN_PATH_MAX) == 0) return OMNIC_E_USAGE;
    if (RemoveDirectoryW(wp)) return OMNIC_OK;
    return GetLastError() == ERROR_FILE_NOT_FOUND ? OMNIC_OK : OMNIC_E_SYSTEM;
}

char *omnic_fs_abspath(const char *path) {
    wchar_t wfull[OMNIC_FS_PATH_MAX];
    char *out = NULL;
    if (win_fullpath(path, wfull, OMNIC_FS_PATH_MAX) != 0) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wfull, -1, NULL, 0, NULL, NULL);
        out = (need > 0) ? (char *)malloc((size_t)need) : NULL;
        if (out != NULL &&
            WideCharToMultiByte(CP_UTF8, 0, wfull, -1, out, need, NULL, NULL) != need) {
            free(out);
            out = NULL;
        }
    }
    if (out == NULL) return NULL;
    for (char *p = out; *p != '\0'; p++)
        if (*p == '\\') *p = '/';
    return out;
}

int omnic_fs_path_inside(const char *root_abs, const char *path_abs) {
    size_t rn = strlen(root_abs);
    while (rn > 0 && root_abs[rn - 1] == '/') rn--; /* 剥尾斜杠 */
    if (rn == 0) return 1;
    if (_strnicmp(path_abs, root_abs, rn) != 0) return 0; /* Windows 卷不敏感 */
    return path_abs[rn] == '/';
}

/* Windows 上没有 POSIX 意义的属主与扩展属性：一律"没有/不能"，归档里就不会出现这些数据 */
int omnic_fs_can_chown(void) { return 0; }

omnic_status omnic_fs_set_owner(const char *path, unsigned uid, unsigned gid) {
    (void)path; (void)uid; (void)gid;
    return OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_get_xattrs(const char *path, unsigned char **buf, size_t *len) {
    (void)path;
    *buf = NULL;
    *len = 0;
    return OMNIC_OK;
}

omnic_status omnic_fs_set_xattr(const char *path, const char *name, const void *val,
                                size_t val_len) {
    (void)path;
    (void)name;
    (void)val;
    (void)val_len;
    return OMNIC_E_SYSTEM;
}

#else /* POSIX */

#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <utime.h>
#include <time.h>
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#define OMNIC_HAVE_XATTR 1
#endif
#ifdef OMNIC_HAVE_XATTR
#include <sys/xattr.h>
#endif

/* 创建时间：BSD/macOS 有 st_birthtime（macOS 上是映射到 st_birthtimespec.tv_sec 的宏），
 * Linux 得走 statx（glibc ≥ 2.28、musl）。拿不到就当未知。 */
static long long birthtime_of(const char *path, const struct stat *st) {
    (void)path;
    (void)st;
#if defined(st_birthtime)
    return (long long)st->st_birthtime;
#elif defined(__linux__) && defined(STATX_BTIME)
    struct statx sx;
    memset(&sx, 0, sizeof(sx));
    if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BTIME, &sx) == 0 &&
        (sx.stx_mask & STATX_BTIME) != 0)
        return (long long)sx.stx_btime.tv_sec;
#endif
    return 0;
}

static char *read_link_alloc(const char *path) {
    size_t cap = 256;
    for (;;) {
        char *buf = (char *)malloc(cap);
        if (buf == NULL) return NULL;
        ssize_t r = readlink(path, buf, cap);
        if (r < 0) {
            free(buf);
            return NULL;
        }
        if ((size_t)r < cap) {
            buf[r] = '\0';
            return buf;
        }
        free(buf); /* 装不下：翻倍再读（目标长度上限 1 MiB） */
        if (cap >= (1u << 20)) return NULL;
        cap *= 2;
    }
}

/* ---- 扩展属性（xattr）与属主：POSIX 专有 ----
 * listxattr 在两边语义不同：Linux 返回"名字串的总长"，macOS 返回"名字串总长 + 每个名字
 * 一个 NUL"；llistxattr(…, NULL, 0) 两边都只给长度。名字一律以 NUL 分隔，逐个 lgetxattr 取值。
 * 读一个属性失败不致命：跳过它，剩下的照存。 */

int omnic_fs_can_chown(void) { return geteuid() == 0 ? 1 : 0; }

omnic_status omnic_fs_set_owner(const char *path, unsigned uid, unsigned gid) {
    if (lchown(path, (uid_t)uid, (gid_t)gid) == 0) return OMNIC_OK;
    return OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_get_xattrs(const char *path, unsigned char **buf, size_t *len) {
    *buf = NULL;
    *len = 0;
#ifdef OMNIC_HAVE_XATTR
    ssize_t sz = llistxattr(path, NULL, 0);
    if (sz < 0) return (errno == ENOTSUP) ? OMNIC_OK : OMNIC_E_SYSTEM; /* 文件系统不支持 = 没有 */
    if (sz == 0) return OMNIC_OK;
    char *names = (char *)malloc((size_t)sz + 1);
    if (names == NULL) return OMNIC_E_SYSTEM;
    ssize_t got = llistxattr(path, names, (size_t)sz);
    if (got != sz) { /* 两次调用之间属性变了：按当下没有处理，不存半截 */
        free(names);
        return OMNIC_OK;
    }
    names[sz] = '\0';
    unsigned char *acc = NULL;
    size_t acc_len = 0;
    for (const char *nm = names; nm < names + sz; nm += strlen(nm) + 1) {
        if (*nm == '\0') continue;
        ssize_t vsz = lgetxattr(path, nm, NULL, 0);
        if (vsz < 0) continue;
        void *val = malloc(vsz > 0 ? (size_t)vsz : 1);
        if (val == NULL) { free(names); free(acc); return OMNIC_E_SYSTEM; }
        if (vsz > 0 && lgetxattr(path, nm, val, (size_t)vsz) != vsz) { free(val); continue; }
        if (omnic_fs_xattr_append(&acc, &acc_len, nm, val, (size_t)vsz) != 0) {
            free(val); free(names); free(acc);
            return OMNIC_E_SYSTEM;
        }
        free(val);
    }
    free(names);
    *buf = acc;
    *len = acc_len;
#else
    (void)path;
#endif
    return OMNIC_OK;
}

omnic_status omnic_fs_set_xattr(const char *path, const char *name, const void *val,
                                size_t val_len) {
#ifdef OMNIC_HAVE_XATTR
    return lsetxattr(path, name, val, val_len, 0) == 0 ? OMNIC_OK : OMNIC_E_SYSTEM;
#else
    (void)path; (void)name; (void)val; (void)val_len;
    return OMNIC_E_SYSTEM;
#endif
}

static void walk_dir(const char *dir, const char *rel, entry_vec *ev, omnic_fs_skip_report *rep,
                     unsigned want) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        note_skip(rep, SKIP_UNREADABLE, rel);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child_path[OMNIC_FS_PATH_MAX], child_rel[OMNIC_FS_PATH_MAX];
        int rr = (rel[0] != '\0') ? snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name)
                                  : snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        int rp = snprintf(child_path, sizeof(child_path), "%s/%s", dir, ent->d_name);
        if (rr < 0 || rr >= (int)sizeof(child_rel) || rp < 0 || rp >= (int)sizeof(child_path)) {
            note_skip(rep, SKIP_TOO_LONG, rel);
            continue;
        }
        struct stat st;
        if (lstat(child_path, &st) != 0) {
            note_skip(rep, SKIP_UNREADABLE, child_rel);
            continue;
        }
        omnic_fs_entry e;
        memset(&e, 0, sizeof(e));
        if (S_ISLNK(st.st_mode)) {
            e.link_target = read_link_alloc(child_path);
            if (e.link_target == NULL) {
                note_skip(rep, SKIP_UNREADABLE, child_rel);
                continue;
            }
            e.size = strlen(e.link_target);
            e.link_kind = OMNIC_FS_LINK_SYMLINK; /* 目标现存且是目录才算目录链接 */
            struct stat tst;
            if (stat(child_path, &tst) == 0 && S_ISDIR(tst.st_mode))
                e.link_kind = OMNIC_FS_LINK_DIR_SYMLINK;
        } else if (S_ISDIR(st.st_mode)) {
            e.is_dir = 1;
        } else if (S_ISREG(st.st_mode)) {
            e.size = (unsigned long long)st.st_size;
            e.dev = (unsigned long long)st.st_dev;
            e.ino = (unsigned long long)st.st_ino;
        } else {
            note_skip(rep, SKIP_SPECIAL, child_rel); /* fifo/socket/dev */
            continue;
        }
        e.rel = dup_str(child_rel);
        if (e.rel == NULL) {
            omnic_set_error("fs: out of memory");
            free(e.link_target);
            break;
        }
        e.mtime = (long long)st.st_mtime;
        if (!S_ISLNK(st.st_mode)) { /* 链接自身的权限位各平台语义不一，不存 */
            e.mode = (unsigned)(st.st_mode & 07777u);
            e.has_mode = 1;
        }
        if ((want & OMNIC_FS_WANT_OWNER) != 0) {
            e.uid = (unsigned)st.st_uid;
            e.gid = (unsigned)st.st_gid;
            e.has_owner = 1;
        }
        if ((want & OMNIC_FS_WANT_XATTR) != 0 &&
            omnic_fs_get_xattrs(child_path, &e.xattr, &e.xattr_len) != OMNIC_OK) {
            free(e.xattr); /* 读不出来就不存：不影响条目本身 */
            e.xattr = NULL;
            e.xattr_len = 0;
        }
        if ((want & OMNIC_FS_WANT_CTIME) != 0) e.ctime = birthtime_of(child_path, &st);
        if (vec_push(ev, &e) != 0) {
            free(e.rel);
            free(e.link_target);
            free(e.xattr);
            break;
        }
        if (e.is_dir) walk_dir(child_path, child_rel, ev, rep, want);
    }
    closedir(d);
}

omnic_fs_entry *omnic_fs_walk_ex(const char *root, size_t *n, omnic_fs_skip_report *rep,
                                 unsigned want) {
    entry_vec ev = {NULL, 0, 0};
    if (rep != NULL) memset(rep, 0, sizeof(*rep));
    *n = 0;
    walk_dir(root, "", &ev, rep, want);
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

omnic_status omnic_fs_set_mtime_stream(FILE *f, long long mtime) {
    struct timespec ts[2];
    ts[0].tv_sec = (time_t)mtime;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = (time_t)mtime;
    ts[1].tv_nsec = 0;
    return futimens(fileno(f), ts) == 0 ? OMNIC_OK : OMNIC_E_SYSTEM;
}

FILE *omnic_fs_fopen(const char *utf8_path, const char *mode) {
    return fopen(utf8_path, mode);
}

omnic_status omnic_fs_file_id(const char *path, unsigned long long *dev, unsigned long long *ino) {
    struct stat st;
    if (stat(path, &st) != 0) return OMNIC_E_SYSTEM;
    *dev = (unsigned long long)st.st_dev;
    *ino = (unsigned long long)st.st_ino;
    return OMNIC_OK;
}

omnic_status omnic_fs_write_file_meta(const char *path, const void *data, size_t n,
                                      const omnic_fs_meta *m, int force, int *opened) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        /* -f：只读的既存文件拒绝 O_TRUNC——删除后重建 */
        if (!force || unlink(path) != 0) return OMNIC_E_SYSTEM;
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return OMNIC_E_SYSTEM;
    }
    if (opened != NULL) *opened = 1;
    const uint8_t *p = (const uint8_t *)data;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) {
            close(fd);
            return OMNIC_E_SYSTEM;
        }
        p += w;
        left -= (size_t)w;
    }
    if (m != NULL && m->mtime != 0) { /* mtime 失败不视作写失败（与旧语义一致） */
        struct timespec ts[2];
        ts[0].tv_sec = (time_t)m->mtime;
        ts[0].tv_nsec = 0;
        ts[1] = ts[0];
        futimens(fd, ts);
    }
    if (m != NULL && m->mode != 0) fchmod(fd, (mode_t)m->mode); /* 写完再设：只读位不能挡住自己 */
    close(fd);
    return OMNIC_OK;
}

omnic_status omnic_fs_set_meta(const char *path, const omnic_fs_meta *m) {
    if (m->mode == 0) return OMNIC_OK; /* attrs 在 POSIX 上无对应 */
    return chmod(path, (mode_t)m->mode) == 0 ? OMNIC_OK : OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_make_symlink(const char *path, const char *target, int is_dir,
                                   int *unprivileged) {
    (void)is_dir;
    if (unprivileged != NULL) *unprivileged = 0;
    return symlink(target, path) == 0 ? OMNIC_OK : OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_make_junction(const char *path, const char *target) {
    return symlink(target, path) == 0 ? OMNIC_OK : OMNIC_E_SYSTEM; /* 目录联接在 POSIX 上就是目录链接 */
}

omnic_status omnic_fs_make_hardlink(const char *existing, const char *path) {
    return link(existing, path) == 0 ? OMNIC_OK : OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_remove_force(const char *path) {
    if (unlink(path) == 0) return OMNIC_OK;
    if (errno == ENOENT) return OMNIC_OK;
    if (errno == EISDIR && rmdir(path) == 0) return OMNIC_OK; /* 空目录 */
    return OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_replace(const char *from, const char *to) {
    return rename(from, to) == 0 ? OMNIC_OK : OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_remove(const char *path) {
    return (unlink(path) == 0 || errno == ENOENT) ? OMNIC_OK : OMNIC_E_SYSTEM;
}

omnic_status omnic_fs_rmdir(const char *path) {
    return (rmdir(path) == 0 || errno == ENOENT) ? OMNIC_OK : OMNIC_E_SYSTEM;
}

char *omnic_fs_abspath(const char *path) {
    char buf[OMNIC_FS_PATH_MAX];
    if (realpath(path, buf) != NULL) {
        char *out = (char *)malloc(strlen(buf) + 1);
        if (out != NULL) strcpy(out, buf);
        return out;
    }
    /* 目标尚不存在（pack 输出）：cwd 拼接 */
    if (path[0] == '/') {
        char *out = (char *)malloc(strlen(path) + 1);
        if (out != NULL) strcpy(out, path);
        return out;
    }
    char cwd[OMNIC_FS_PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) return NULL;
    if (snprintf(buf, sizeof(buf), "%s/%s", cwd, path) >= (int)sizeof(buf)) return NULL;
    char *out = (char *)malloc(strlen(buf) + 1);
    if (out != NULL) strcpy(out, buf);
    return out;
}

int omnic_fs_path_inside(const char *root_abs, const char *path_abs) {
    size_t rn = strlen(root_abs);
    while (rn > 0 && root_abs[rn - 1] == '/') rn--;
    if (rn == 0) return 1;
    if (strncmp(path_abs, root_abs, rn) != 0) return 0;
    return path_abs[rn] == '/';
}

#endif /* _WIN32 */
