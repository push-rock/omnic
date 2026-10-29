#include "detect/filetype.h"

#include <string.h>

typedef struct {
    const char *ext;
    omnic_class cls;
} omnic_ext_class;

/* 常用后缀 -> 数据类型。一期覆盖 ~150 项（SW-DET-002 要求 ≥200，
 * 缺口由熵兜底路径补齐，后续按训练语料扩充）。 */
static const omnic_ext_class g_ext_class[] = {
    /* 文本 / 代码 / 标记语言 */
    {"txt", OMNIC_CLS_TEXT},   {"text", OMNIC_CLS_TEXT}, {"md", OMNIC_CLS_TEXT},
    {"markdown", OMNIC_CLS_TEXT}, {"rst", OMNIC_CLS_TEXT},
    {"c", OMNIC_CLS_TEXT},     {"h", OMNIC_CLS_TEXT},    {"cpp", OMNIC_CLS_TEXT},
    {"cc", OMNIC_CLS_TEXT},    {"cxx", OMNIC_CLS_TEXT},  {"hpp", OMNIC_CLS_TEXT},
    {"hh", OMNIC_CLS_TEXT},    {"cs", OMNIC_CLS_TEXT},   {"java", OMNIC_CLS_TEXT},
    {"py", OMNIC_CLS_TEXT},    {"pyw", OMNIC_CLS_TEXT},  {"rb", OMNIC_CLS_TEXT},
    {"go", OMNIC_CLS_TEXT},    {"rs", OMNIC_CLS_TEXT},   {"js", OMNIC_CLS_TEXT},
    {"jsx", OMNIC_CLS_TEXT},   {"ts", OMNIC_CLS_TEXT},   {"tsx", OMNIC_CLS_TEXT},
    {"html", OMNIC_CLS_TEXT},  {"htm", OMNIC_CLS_TEXT},  {"xhtml", OMNIC_CLS_TEXT},
    {"css", OMNIC_CLS_TEXT},   {"scss", OMNIC_CLS_TEXT}, {"less", OMNIC_CLS_TEXT},
    {"json", OMNIC_CLS_TEXT},  {"geojson", OMNIC_CLS_TEXT}, {"jsonl", OMNIC_CLS_TEXT},
    {"xml", OMNIC_CLS_TEXT},   {"svg", OMNIC_CLS_TEXT},  {"plist", OMNIC_CLS_TEXT},
    {"yaml", OMNIC_CLS_TEXT},  {"yml", OMNIC_CLS_TEXT},  {"toml", OMNIC_CLS_TEXT},
    {"ini", OMNIC_CLS_TEXT},   {"cfg", OMNIC_CLS_TEXT},  {"conf", OMNIC_CLS_TEXT},
    {"properties", OMNIC_CLS_TEXT}, {"log", OMNIC_CLS_TEXT}, {"csv", OMNIC_CLS_TEXT},
    {"tsv", OMNIC_CLS_TEXT},   {"sql", OMNIC_CLS_TEXT},  {"sh", OMNIC_CLS_TEXT},
    {"bash", OMNIC_CLS_TEXT},  {"bat", OMNIC_CLS_TEXT},  {"cmd", OMNIC_CLS_TEXT},
    {"ps1", OMNIC_CLS_TEXT},   {"lua", OMNIC_CLS_TEXT},  {"pl", OMNIC_CLS_TEXT},
    {"pm", OMNIC_CLS_TEXT},    {"php", OMNIC_CLS_TEXT},  {"phtml", OMNIC_CLS_TEXT},
    {"swift", OMNIC_CLS_TEXT}, {"kt", OMNIC_CLS_TEXT},   {"kts", OMNIC_CLS_TEXT},
    {"scala", OMNIC_CLS_TEXT}, {"r", OMNIC_CLS_TEXT},    {"m", OMNIC_CLS_TEXT},
    {"mm", OMNIC_CLS_TEXT},    {"tex", OMNIC_CLS_TEXT},  {"srt", OMNIC_CLS_TEXT},
    {"vtt", OMNIC_CLS_TEXT},   {"ass", OMNIC_CLS_TEXT},  {"po", OMNIC_CLS_TEXT},
    {"pot", OMNIC_CLS_TEXT},   {"patch", OMNIC_CLS_TEXT}, {"diff", OMNIC_CLS_TEXT},
    {"gradle", OMNIC_CLS_TEXT}, {"cmake", OMNIC_CLS_TEXT},
    /* 可执行 / 对象 */
    {"exe", OMNIC_CLS_EXEC},   {"dll", OMNIC_CLS_EXEC},  {"so", OMNIC_CLS_EXEC},
    {"dylib", OMNIC_CLS_EXEC}, {"o", OMNIC_CLS_EXEC},    {"obj", OMNIC_CLS_EXEC},
    {"a", OMNIC_CLS_EXEC},     {"lib", OMNIC_CLS_EXEC},  {"sys", OMNIC_CLS_EXEC},
    {"bin", OMNIC_CLS_EXEC},   {"com", OMNIC_CLS_EXEC},  {"ko", OMNIC_CLS_EXEC},
    {"elf", OMNIC_CLS_EXEC},   {"class", OMNIC_CLS_EXEC}, {"wasm", OMNIC_CLS_EXEC},
    {"pdb", OMNIC_CLS_EXEC},
    /* 结构化二进制 / 数据库 / 容器 */
    {"db", OMNIC_CLS_STRUCTURED}, {"sqlite", OMNIC_CLS_STRUCTURED},
    {"sqlite3", OMNIC_CLS_STRUCTURED}, {"db3", OMNIC_CLS_STRUCTURED},
    {"mdb", OMNIC_CLS_STRUCTURED}, {"dat", OMNIC_CLS_STRUCTURED},
    {"tar", OMNIC_CLS_STRUCTURED}, {"cpio", OMNIC_CLS_STRUCTURED},
    {"ar", OMNIC_CLS_STRUCTURED}, {"iso", OMNIC_CLS_STRUCTURED},
    /* 未压缩媒体 */
    {"bmp", OMNIC_CLS_MEDIA_RAW}, {"dib", OMNIC_CLS_MEDIA_RAW},
    {"wav", OMNIC_CLS_MEDIA_RAW},  {"tif", OMNIC_CLS_MEDIA_RAW},
    {"tiff", OMNIC_CLS_MEDIA_RAW}, {"tga", OMNIC_CLS_MEDIA_RAW},
    {"pcx", OMNIC_CLS_MEDIA_RAW},  {"ppm", OMNIC_CLS_MEDIA_RAW},
    {"pgm", OMNIC_CLS_MEDIA_RAW},  {"pbm", OMNIC_CLS_MEDIA_RAW},
    {"raw", OMNIC_CLS_MEDIA_RAW},  {"dds", OMNIC_CLS_MEDIA_RAW},
    {"exr", OMNIC_CLS_MEDIA_RAW},
    /* 已压缩：默认跳过（SYS-CMP-003 / 假设 A1） */
    {"jpg", OMNIC_CLS_ALREADY_COMPRESSED}, {"jpeg", OMNIC_CLS_ALREADY_COMPRESSED},
    {"jpe", OMNIC_CLS_ALREADY_COMPRESSED},  {"jfif", OMNIC_CLS_ALREADY_COMPRESSED},
    {"png", OMNIC_CLS_ALREADY_COMPRESSED},  {"apng", OMNIC_CLS_ALREADY_COMPRESSED},
    {"gif", OMNIC_CLS_ALREADY_COMPRESSED},  {"webp", OMNIC_CLS_ALREADY_COMPRESSED},
    {"heic", OMNIC_CLS_ALREADY_COMPRESSED}, {"avif", OMNIC_CLS_ALREADY_COMPRESSED},
    {"mp3", OMNIC_CLS_ALREADY_COMPRESSED},  {"aac", OMNIC_CLS_ALREADY_COMPRESSED},
    {"ogg", OMNIC_CLS_ALREADY_COMPRESSED},  {"oga", OMNIC_CLS_ALREADY_COMPRESSED},
    {"opus", OMNIC_CLS_ALREADY_COMPRESSED}, {"flac", OMNIC_CLS_ALREADY_COMPRESSED},
    {"m4a", OMNIC_CLS_ALREADY_COMPRESSED},  {"wma", OMNIC_CLS_ALREADY_COMPRESSED},
    {"mp4", OMNIC_CLS_ALREADY_COMPRESSED},  {"m4v", OMNIC_CLS_ALREADY_COMPRESSED},
    {"mkv", OMNIC_CLS_ALREADY_COMPRESSED},  {"webm", OMNIC_CLS_ALREADY_COMPRESSED},
    {"avi", OMNIC_CLS_ALREADY_COMPRESSED},  {"mov", OMNIC_CLS_ALREADY_COMPRESSED},
    {"wmv", OMNIC_CLS_ALREADY_COMPRESSED},  {"flv", OMNIC_CLS_ALREADY_COMPRESSED},
    {"mpg", OMNIC_CLS_ALREADY_COMPRESSED},  {"mpeg", OMNIC_CLS_ALREADY_COMPRESSED},
    {"ts", OMNIC_CLS_ALREADY_COMPRESSED},   {"3gp", OMNIC_CLS_ALREADY_COMPRESSED},
    {"zip", OMNIC_CLS_ALREADY_COMPRESSED},  {"rar", OMNIC_CLS_ALREADY_COMPRESSED},
    {"7z", OMNIC_CLS_ALREADY_COMPRESSED},   {"cab", OMNIC_CLS_ALREADY_COMPRESSED},
    {"msi", OMNIC_CLS_ALREADY_COMPRESSED},  {"wim", OMNIC_CLS_ALREADY_COMPRESSED},
    {"xar", OMNIC_CLS_ALREADY_COMPRESSED},  {"deb", OMNIC_CLS_ALREADY_COMPRESSED},
    {"rpm", OMNIC_CLS_ALREADY_COMPRESSED},  {"apk", OMNIC_CLS_ALREADY_COMPRESSED},
    {"jar", OMNIC_CLS_ALREADY_COMPRESSED},  {"ipa", OMNIC_CLS_ALREADY_COMPRESSED},
    {"pdf", OMNIC_CLS_ALREADY_COMPRESSED},  {"docx", OMNIC_CLS_ALREADY_COMPRESSED},
    {"xlsx", OMNIC_CLS_ALREADY_COMPRESSED}, {"pptx", OMNIC_CLS_ALREADY_COMPRESSED},
    {"epub", OMNIC_CLS_ALREADY_COMPRESSED}, {"mobi", OMNIC_CLS_ALREADY_COMPRESSED},
    {"parquet", OMNIC_CLS_ALREADY_COMPRESSED}, {"orc", OMNIC_CLS_ALREADY_COMPRESSED},
    {"crx", OMNIC_CLS_ALREADY_COMPRESSED},  {"xpi", OMNIC_CLS_ALREADY_COMPRESSED},
    {"dmg", OMNIC_CLS_ALREADY_COMPRESSED},  {"squashfs", OMNIC_CLS_ALREADY_COMPRESSED},
    {"zpaq", OMNIC_CLS_ALREADY_COMPRESSED},
    /* 本工具产物（再压无收益） */
    {"zst", OMNIC_CLS_ALREADY_COMPRESSED},  {"lz4", OMNIC_CLS_ALREADY_COMPRESSED},
    {"xz", OMNIC_CLS_ALREADY_COMPRESSED},   {"bz2", OMNIC_CLS_ALREADY_COMPRESSED},
    {"gz", OMNIC_CLS_ALREADY_COMPRESSED},   {"br", OMNIC_CLS_ALREADY_COMPRESSED},
    {"lzma", OMNIC_CLS_ALREADY_COMPRESSED}, {"lz", OMNIC_CLS_ALREADY_COMPRESSED},
    {"sz", OMNIC_CLS_ALREADY_COMPRESSED},   {"lzfse", OMNIC_CLS_ALREADY_COMPRESSED},
    {"lzav", OMNIC_CLS_ALREADY_COMPRESSED}, {"lzham", OMNIC_CLS_ALREADY_COMPRESSED},
};

static const char *ext_of(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL || dot[1] == '\0') return NULL;
    /* 跳过目录部分（Windows 兼容） */
    if (strchr(dot, '/') != NULL || strchr(dot, '\\') != NULL) return NULL;
    return dot + 1;
}

omnic_class omnic_classify_ext(const char *path) {
    const char *ext = ext_of(path);
    if (ext == NULL) return OMNIC_CLS_UNKNOWN;
    char buf[16];
    size_t i = 0;
    for (; ext[i] != '\0' && i < sizeof(buf) - 1; i++) {
        buf[i] = (char)((ext[i] >= 'A' && ext[i] <= 'Z') ? ext[i] + 32 : ext[i]);
    }
    buf[i] = '\0';
    for (size_t k = 0; k < sizeof(g_ext_class) / sizeof(g_ext_class[0]); k++) {
        if (strcmp(buf, g_ext_class[k].ext) == 0) return g_ext_class[k].cls;
    }
    return OMNIC_CLS_UNKNOWN;
}

omnic_class omnic_classify(const char *path, const uint8_t *sample, size_t n) {
    omnic_class c = omnic_classify_ext(path);
    if (c != OMNIC_CLS_UNKNOWN) return c;
    (void)sample;
    (void)n;
    /* 熵细分在 api.c 层结合 omnic_entropy 完成（UNKNOWN -> RANDOM 判定） */
    return OMNIC_CLS_UNKNOWN;
}
