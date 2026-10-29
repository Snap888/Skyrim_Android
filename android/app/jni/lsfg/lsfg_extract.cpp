/* Minimal PE RT_RCDATA extractor for Lossless.dll (no pe-parse). */
#include "lsfg_extract.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "lsfg_log.h"
#define LOGI(...) lsfg_log("I", __VA_ARGS__)
#define LOGW(...) lsfg_log("W", __VA_ARGS__)
#define LOGE(...) lsfg_log("E", __VA_ARGS__)

#pragma pack(push, 1)
struct DosHdr {
    uint16_t e_magic;
    uint16_t pad[29];
    int32_t  e_lfanew;
};
struct FileHdr {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};
struct DataDir {
    uint32_t VirtualAddress;
    uint32_t Size;
};
struct OptHdr32 {
    uint16_t Magic;
    uint8_t  skip[90];
    uint32_t NumberOfRvaAndSizes;
    DataDir  DataDirectory[16];
};
struct OptHdr64 {
    uint16_t Magic;
    uint8_t  skip[106];
    uint32_t NumberOfRvaAndSizes;
    DataDir  DataDirectory[16];
};
struct SecHdr {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
};
struct ResDir {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint16_t NumberOfNamedEntries;
    uint16_t NumberOfIdEntries;
};
struct ResEntry {
    uint32_t NameOrId;
    uint32_t Offset;
};
struct ResData {
    uint32_t OffsetToData;
    uint32_t Size;
    uint32_t CodePage;
    uint32_t Reserved;
};
#pragma pack(pop)

enum { RT_RCDATA = 10, IMAGE_DIRECTORY_ENTRY_RESOURCE = 2 };

static int mkdir_p(const char *path) {
    char tmp[768];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    if (n == 0) return -1;
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

static uint8_t *map_file(const char *path, size_t *out_sz) {
    *out_sz = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 256 * 1024 * 1024) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_sz = (size_t)sz;
    return buf;
}

static int rva_to_off(const SecHdr *secs, int nsec, uint32_t rva, uint32_t *off) {
    for (int i = 0; i < nsec; i++) {
        uint32_t va = secs[i].VirtualAddress;
        uint32_t vs = secs[i].VirtualSize ? secs[i].VirtualSize : secs[i].SizeOfRawData;
        if (rva >= va && rva < va + vs) {
            *off = secs[i].PointerToRawData + (rva - va);
            return 0;
        }
    }
    return -1;
}

static const char *blob_kind(const uint8_t *p, size_t n) {
    if (n >= 4 && p[0] == 'D' && p[1] == 'X' && p[2] == 'B' && p[3] == 'C')
        return "dxbc";
    if (n >= 4) {
        uint32_t m = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        if (m == 0x07230203u) return "spirv";
    }
    return "raw";
}

static int write_blob(const char *cache_dir, uint32_t id, const uint8_t *data, size_t size, FILE *manifest) {
    char path[900];
    snprintf(path, sizeof path, "%s/rcdata_%u.bin", cache_dir, id);
    FILE *out = fopen(path, "wb");
    if (!out) {
        LOGE("extract: cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    if (fwrite(data, 1, size, out) != size) {
        fclose(out);
        LOGE("extract: short write %s", path);
        return -1;
    }
    fclose(out);
    const char *kind = blob_kind(data, size);
    fprintf(manifest, "%u %zu %s\n", id, size, kind);
    LOGI("extract: id=%u size=%zu kind=%s", id, size, kind);
    return 0;
}

static int walk_res(const uint8_t *file, size_t fsz,
                    const SecHdr *secs, int nsec,
                    uint32_t rsrc_rva, uint32_t dir_rva,
                    int level, uint32_t name_id,
                    const char *cache_dir, FILE *manifest, int *count) {
    uint32_t off = 0;
    if (rva_to_off(secs, nsec, dir_rva, &off) != 0) return -1;
    if (off + sizeof(ResDir) > fsz) return -1;
    const ResDir *dir = (const ResDir *)(file + off);
    uint32_t nent = (uint32_t)dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
    if (off + sizeof(ResDir) + nent * sizeof(ResEntry) > fsz) return -1;
    const ResEntry *ents = (const ResEntry *)(file + off + sizeof(ResDir));

    for (uint32_t i = 0; i < nent; i++) {
        uint32_t id = ents[i].NameOrId;
        int is_dir = (ents[i].Offset & 0x80000000u) != 0;
        uint32_t next = ents[i].Offset & 0x7FFFFFFFu;
        uint32_t next_rva = rsrc_rva + next;

        if (level == 0) {
            if (id & 0x80000000u) continue;
            if (id != (uint32_t)RT_RCDATA) continue;
            if (!is_dir) continue;
            if (walk_res(file, fsz, secs, nsec, rsrc_rva, next_rva, 1, 0, cache_dir, manifest, count) != 0)
                return -1;
        } else if (level == 1) {
            if (id & 0x80000000u) continue;
            if (!is_dir) continue;
            if (walk_res(file, fsz, secs, nsec, rsrc_rva, next_rva, 2, id, cache_dir, manifest, count) != 0)
                return -1;
        } else {
            if (is_dir) continue;
            uint32_t doff = 0;
            if (rva_to_off(secs, nsec, next_rva, &doff) != 0) return -1;
            if (doff + sizeof(ResData) > fsz) return -1;
            const ResData *rd = (const ResData *)(file + doff);
            uint32_t data_off = 0;
            if (rva_to_off(secs, nsec, rd->OffsetToData, &data_off) != 0) return -1;
            if (rd->Size == 0 || data_off + rd->Size > fsz) {
                LOGW("extract: skip id=%u bad size/rva", name_id);
                continue;
            }
            if (write_blob(cache_dir, name_id, file + data_off, rd->Size, manifest) == 0)
                (*count)++;
        }
    }
    return 0;
}

bool lsfg_extract_cache_ready(const char *cache_dir) {
    if (!cache_dir || !*cache_dir) return false;
    char path[800];
    snprintf(path, sizeof path, "%s/manifest.txt", cache_dir);
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    int count = 0;
    bool valid = true;
    while (fgets(line, sizeof line, f)) {
        unsigned id = 0;
        unsigned long expected_size = 0;
        char kind[32] = {0};
        if (sscanf(line, "%u %lu %31s", &id, &expected_size, kind) != 3) {
            valid = false;
            break;
        }

        char blob_path[900];
        snprintf(blob_path, sizeof blob_path, "%s/rcdata_%u.bin", cache_dir, id);
        struct stat st;
        if (stat(blob_path, &st) != 0 || st.st_size <= 0
            || (unsigned long)st.st_size != expected_size) {
            valid = false;
            break;
        }
        count++;
    }
    if (ferror(f)) valid = false;
    fclose(f);
    return valid && count > 0;
}

int lsfg_extract_dll(const char *dll_path, const char *cache_dir) {
    if (!dll_path || !*dll_path || !cache_dir || !*cache_dir) return -1;

    if (lsfg_extract_cache_ready(cache_dir)) {
        LOGI("extract: cache already present at %s", cache_dir);
        char path[800];
        snprintf(path, sizeof path, "%s/manifest.txt", cache_dir);
        FILE *f = fopen(path, "r");
        int n = 0;
        char line[128];
        while (f && fgets(line, sizeof line, f)) n++;
        if (f) fclose(f);
        return n;
    }

    if (mkdir_p(cache_dir) != 0) {
        LOGE("extract: mkdir %s failed: %s", cache_dir, strerror(errno));
        return -1;
    }

    size_t fsz = 0;
    uint8_t *file = map_file(dll_path, &fsz);
    if (!file) {
        LOGE("extract: cannot read DLL %s", dll_path);
        return -1;
    }

    int result = -1;
    int count = 0;
    FILE *manifest = NULL;

    do {
        if (fsz < sizeof(DosHdr)) { LOGE("extract: file too small"); break; }
        const DosHdr *dos = (const DosHdr *)file;
        if (dos->e_magic != 0x5A4D) { LOGE("extract: not MZ"); break; }
        if (dos->e_lfanew <= 0 || (size_t)dos->e_lfanew + 4 + sizeof(FileHdr) > fsz) {
            LOGE("extract: bad e_lfanew"); break;
        }
        if (memcmp(file + dos->e_lfanew, "PE\0\0", 4) != 0) { LOGE("extract: not PE"); break; }

        const FileHdr *fh = (const FileHdr *)(file + dos->e_lfanew + 4);
        size_t opt_off = (size_t)dos->e_lfanew + 4 + sizeof(FileHdr);
        if (opt_off + 2 > fsz) break;
        uint16_t opt_magic = *(const uint16_t *)(file + opt_off);

        DataDir rsrc_dir;
        rsrc_dir.VirtualAddress = 0;
        rsrc_dir.Size = 0;
        if (opt_magic == 0x10b) {
            if (opt_off + sizeof(OptHdr32) > fsz) break;
            const OptHdr32 *opt = (const OptHdr32 *)(file + opt_off);
            if (opt->NumberOfRvaAndSizes > (uint32_t)IMAGE_DIRECTORY_ENTRY_RESOURCE)
                rsrc_dir = opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        } else if (opt_magic == 0x20b) {
            if (opt_off + sizeof(OptHdr64) > fsz) break;
            const OptHdr64 *opt = (const OptHdr64 *)(file + opt_off);
            if (opt->NumberOfRvaAndSizes > (uint32_t)IMAGE_DIRECTORY_ENTRY_RESOURCE)
                rsrc_dir = opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        } else {
            LOGE("extract: unknown optional magic %04x", opt_magic);
            break;
        }

        if (rsrc_dir.VirtualAddress == 0 || rsrc_dir.Size == 0) {
            LOGE("extract: no resource directory");
            break;
        }

        size_t sec_off = opt_off + fh->SizeOfOptionalHeader;
        if (sec_off + (size_t)fh->NumberOfSections * sizeof(SecHdr) > fsz) break;
        const SecHdr *secs = (const SecHdr *)(file + sec_off);
        int nsec = fh->NumberOfSections;

        char man_path[800];
        char man_tmp_path[840];
        snprintf(man_path, sizeof man_path, "%s/manifest.txt", cache_dir);
        snprintf(man_tmp_path, sizeof man_tmp_path, "%s/manifest.txt.tmp", cache_dir);
        manifest = fopen(man_tmp_path, "w");
        if (!manifest) {
            LOGE("extract: cannot write temporary manifest");
            break;
        }

        if (walk_res(file, fsz, secs, nsec, rsrc_dir.VirtualAddress, rsrc_dir.VirtualAddress,
                     0, 0, cache_dir, manifest, &count) != 0) {
            LOGE("extract: resource walk failed");
            break;
        }

        if (fflush(manifest) != 0) {
            LOGE("extract: failed flushing temporary manifest");
            break;
        }
        LOGI("extract: done, %d RCDATA blob(s) -> %s", count, cache_dir);
        result = count;
    } while (0);

    int manifest_close_ok = 1;
    if (manifest) {
        manifest_close_ok = (fclose(manifest) == 0);
        manifest = NULL;
    }
    char man_path2[800];
    char man_tmp_path2[840];
    snprintf(man_path2, sizeof man_path2, "%s/manifest.txt", cache_dir);
    snprintf(man_tmp_path2, sizeof man_tmp_path2, "%s/manifest.txt.tmp", cache_dir);
    if (result >= 0 && manifest_close_ok) {
        if (rename(man_tmp_path2, man_path2) != 0) {
            LOGE("extract: cannot publish manifest: %s", strerror(errno));
            result = -1;
        }
    } else {
        result = -1;
    }
    free(file);
    if (result < 0) {
        remove(man_tmp_path2);
        remove(man_path2);
    }
    return result;
}
