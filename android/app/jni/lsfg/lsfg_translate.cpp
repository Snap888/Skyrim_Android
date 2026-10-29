/* DXBC -> SPIR-V via in-tree DXVK DxbcModule. */
#include "lsfg_translate.h"
#include "lsfg_log.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <sstream>
#include <sys/stat.h>

#include "../dxvk/src/dxbc/dxbc_module.h"
#include "../dxvk/src/dxbc/dxbc_reader.h"
#include "../dxvk/src/dxbc/dxbc_modinfo.h"
#include "../dxvk/src/dxvk/dxvk_shader.h"

#define LOGI(...) lsfg_log("I", __VA_ARGS__)
#define LOGW(...) lsfg_log("W", __VA_ARGS__)
#define LOGE(...) lsfg_log("E", __VA_ARGS__)

static bool file_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 20 || (st.st_size % 4) != 0)
        return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t magic = 0;
    bool ok = fread(&magic, sizeof(magic), 1, f) == 1
           && magic == 0x07230203u;
    fclose(f);
    return ok;
}

static bool read_file(const char *path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return false; }
    out.resize((size_t)sz);
    if (fread(out.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); out.clear(); return false; }
    fclose(f);
    return true;
}

static bool write_bytes_atomic(const char *path, const void *data, size_t size) {
    char tmp_path[1024];
    int len = snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
    if (len < 0 || (size_t)len >= sizeof tmp_path) return false;

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    if (ok) ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        int rename_errno = errno;
        remove(tmp_path);
        LOGW("translate: cannot publish %s: %s", path, strerror(rename_errno));
        return false;
    }
    return true;
}

static bool write_spv(const char *path, const uint32_t *words, size_t dword_count) {
    if (!words || dword_count < 5) return false;
    if (words[0] != 0x07230203u) return false;
    return write_bytes_atomic(path, words, dword_count * sizeof(uint32_t));
}

static int translate_dxbc(const std::vector<uint8_t> &dxbc, std::vector<uint32_t> &spirv_out, std::string &err) {
    spirv_out.clear();
    try {
        dxvk::DxbcReader reader(reinterpret_cast<const char *>(dxbc.data()), dxbc.size());
        dxvk::DxbcModule module(reader);
        dxvk::DxbcModuleInfo info{};
        info.options = dxvk::DxbcOptions();
        info.tess = nullptr;
        info.xfb = nullptr;
        dxvk::Rc<dxvk::DxvkShader> shader = module.compile(info, "lsfg_cs");
        if (shader == nullptr) {
            err = "compile returned null";
            return -1;
        }
        std::ostringstream oss;
        shader->dump(oss);
        std::string bin = oss.str();
        if (bin.size() < 20 || (bin.size() % 4) != 0) {
            err = "bad spirv size from dump";
            return -1;
        }
        spirv_out.resize(bin.size() / 4);
        memcpy(spirv_out.data(), bin.data(), bin.size());
        if (spirv_out[0] != 0x07230203u) {
            err = "missing SPIR-V magic";
            return -1;
        }
        return 0;
    } catch (const dxvk::DxvkError &e) {
        err = e.message();
        return -1;
    } catch (const std::exception &e) {
        err = e.what();
        return -1;
    } catch (...) {
        err = "unknown exception";
        return -1;
    }
}

int lsfg_translate_cache(const char *cache_dir) {
    if (!cache_dir || !*cache_dir) return -1;

    char man_path[800];
    snprintf(man_path, sizeof man_path, "%s/manifest.txt", cache_dir);
    FILE *man = fopen(man_path, "r");
    if (!man) {
        LOGE("translate: no manifest at %s", man_path);
        return -1;
    }

    int total = 0;
    char line[256];
    while (fgets(line, sizeof line, man)) {
        unsigned id; unsigned long sz; char kind[32];
        if (sscanf(line, "%u %lu %31s", &id, &sz, kind) == 3) total++;
    }
    rewind(man);

    char out_man_path[800];
    char out_man_tmp[840];
    snprintf(out_man_path, sizeof out_man_path, "%s/manifest_spirv.txt", cache_dir);
    snprintf(out_man_tmp, sizeof out_man_tmp, "%s/manifest_spirv.txt.tmp", cache_dir);
    FILE *out_man = fopen(out_man_tmp, "w");
    if (!out_man) {
        fclose(man);
        LOGE("translate: cannot write temporary manifest %s", out_man_tmp);
        return -1;
    }

    LOGI("=== LSFG step 2: DXBC -> SPIR-V (%d resources) ===", total);
    int ok = 0, fail = 0, skip = 0, cached = 0, done = 0;
    while (fgets(line, sizeof line, man)) {
        unsigned id = 0;
        unsigned long size = 0;
        char kind[32] = {0};
        if (sscanf(line, "%u %lu %31s", &id, &size, kind) != 3) continue;
        done++;

        char bin_path[900];
        snprintf(bin_path, sizeof bin_path, "%s/rcdata_%u.bin", cache_dir, id);
        char spv_path[900];
        snprintf(spv_path, sizeof spv_path, "%s/%u.spv", cache_dir, id);

        if (file_exists(spv_path)) {
            fprintf(out_man, "%u 0 ok cached\n", id);
            cached++;
            ok++;
            if (done == 1 || done == total || (done % 25) == 0)
                LOGI("translate progress %d/%d (cached %d)", done, total, cached);
            continue;
        }

        if (!strcmp(kind, "spirv")) {
            std::vector<uint8_t> data;
            if (!read_file(bin_path, data) || data.size() < 4) {
                LOGW("translate: id=%u spirv read fail", id);
                fail++;
                continue;
            }
            uint32_t magic = 0;
            if (data.size() >= sizeof(magic))
                memcpy(&magic, data.data(), sizeof(magic));
            if ((data.size() % sizeof(uint32_t)) != 0
                || data.size() < 5 * sizeof(uint32_t)
                || magic != 0x07230203u
                || !write_bytes_atomic(spv_path, data.data(), data.size())) {
                LOGW("translate: id=%u spirv copy/validation fail", id);
                fail++;
                continue;
            }
            fprintf(out_man, "%u %zu ok spirv-copy\n", id, data.size() / 4);
            ok++;
            continue;
        }

        if (strcmp(kind, "dxbc") != 0) {
            fprintf(out_man, "%u 0 skip %s\n", id, kind);
            skip++;
            continue;
        }

        std::vector<uint8_t> dxbc;
        if (!read_file(bin_path, dxbc)) {
            LOGW("translate: id=%u cannot read %s", id, bin_path);
            fail++;
            continue;
        }

        std::vector<uint32_t> spirv;
        std::string err;
        if (translate_dxbc(dxbc, spirv, err) != 0) {
            LOGW("translate: id=%u DXBC failed: %s", id, err.c_str());
            fprintf(out_man, "%u 0 fail %s\n", id, err.c_str());
            fail++;
            continue;
        }

        if (!write_spv(spv_path, spirv.data(), spirv.size())) {
            LOGW("translate: id=%u write %s failed", id, spv_path);
            fail++;
            continue;
        }
        fprintf(out_man, "%u %zu ok dxbc\n", id, spirv.size());
        ok++;
        if (done == 1 || done == total || (done % 10) == 0)
            LOGI("translate progress %d/%d ok=%d fail=%d (last id=%u %zu dwords)",
                 done, total, ok, fail, id, spirv.size());
    }

    fclose(man);
    int manifest_close_ok = (fclose(out_man) == 0);
    if (!manifest_close_ok) {
        remove(out_man_tmp);
        LOGE("translate: failed closing temporary manifest %s", out_man_tmp);
        return -1;
    }
    if (rename(out_man_tmp, out_man_path) != 0) {
        int rename_errno = errno;
        remove(out_man_tmp);
        LOGE("translate: cannot publish manifest %s: %s",
             out_man_path, strerror(rename_errno));
        return -1;
    }
    LOGI("translate done: ok=%d (cached=%d) fail=%d skip=%d -> %s",
         ok, cached, fail, skip, out_man_path);
    return ok;
}
