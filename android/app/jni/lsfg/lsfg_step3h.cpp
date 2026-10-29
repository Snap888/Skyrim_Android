/* Step 3h-a: pin LSFG shader resource IDs to roles (from lsfg-vk v1.0.0 Extract::nameIdxTable).
 * Does not run the pass graph yet — only validates cache contains known RCDATA ids
 * and writes manifest_roles.txt for later steps. */
#include "lsfg_log.h"
#include "lsfg_host.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

struct Role {
    const char *name;
    uint32_t id;
    int perf; /* 1 = performance-mode (p_*) variant preferred when performance=1 */
};

/* Source: PancakeTAS/lsfg-vk v1.0.0 src/extract/extract.cpp nameIdxTable */
static const Role k_roles[] = {
    { "mipmaps",    255, 0 },
    { "generate",   256, 0 },

    { "gamma[0]",   257, 0 },
    { "gamma[1]",   259, 0 },
    { "gamma[2]",   260, 0 },
    { "gamma[3]",   261, 0 },
    { "gamma[4]",   262, 0 },

    { "delta[0]",   257, 0 },
    { "delta[1]",   263, 0 },
    { "delta[2]",   264, 0 },
    { "delta[3]",   265, 0 },
    { "delta[4]",   266, 0 },
    { "delta[5]",   258, 0 },
    { "delta[6]",   271, 0 },
    { "delta[7]",   272, 0 },
    { "delta[8]",   273, 0 },
    { "delta[9]",   274, 0 },

    { "alpha[0]",   267, 0 },
    { "alpha[1]",   268, 0 },
    { "alpha[2]",   269, 0 },
    { "alpha[3]",   270, 0 },

    { "beta[0]",    275, 0 },
    { "beta[1]",    276, 0 },
    { "beta[2]",    277, 0 },
    { "beta[3]",    278, 0 },
    { "beta[4]",    279, 0 },

    { "p_mipmaps",  255, 1 },
    { "p_generate", 256, 1 },

    { "p_gamma[0]", 280, 1 },
    { "p_gamma[1]", 282, 1 },
    { "p_gamma[2]", 283, 1 },
    { "p_gamma[3]", 284, 1 },
    { "p_gamma[4]", 285, 1 },

    { "p_delta[0]", 280, 1 },
    { "p_delta[1]", 286, 1 },
    { "p_delta[2]", 287, 1 },
    { "p_delta[3]", 288, 1 },
    { "p_delta[4]", 289, 1 },
    { "p_delta[5]", 281, 1 },
    { "p_delta[6]", 294, 1 },
    { "p_delta[7]", 295, 1 },
    { "p_delta[8]", 296, 1 },
    { "p_delta[9]", 297, 1 },

    { "p_alpha[0]", 290, 1 },
    { "p_alpha[1]", 291, 1 },
    { "p_alpha[2]", 292, 1 },
    { "p_alpha[3]", 293, 1 },

    { "p_beta[0]",  298, 1 },
    { "p_beta[1]",  299, 1 },
    { "p_beta[2]",  300, 1 },
    { "p_beta[3]",  301, 1 },
    { "p_beta[4]",  302, 1 },
};

enum {
    LSFG_ID_MIPMAPS  = 255,
    LSFG_ID_GENERATE = 256,
};

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int has_spv(const char *cache, uint32_t id) {
    char path[700];
    snprintf(path, sizeof path, "%s/%u.spv", cache, id);
    if (file_exists(path)) return 1;
    snprintf(path, sizeof path, "%s/rcdata_%u.bin", cache, id);
    return file_exists(path);
}

static uint32_t g_id_generate = LSFG_ID_GENERATE;
static uint32_t g_id_mipmaps  = LSFG_ID_MIPMAPS;
static int g_perf_mode;
static int g_roles_ok;
static int g_roles_missing;

extern "C" uint32_t lsfg_role_generate_id(void) { return g_id_generate; }
extern "C" uint32_t lsfg_role_mipmaps_id(void)  { return g_id_mipmaps; }
extern "C" int      lsfg_role_perf_mode(void)   { return g_perf_mode; }
extern "C" int      lsfg_roles_ready(void)      { return g_roles_ok; }

extern "C" void lsfg_step3h_pin_roles(const char *cache_dir, int performance) {
    if (!cache_dir || !*cache_dir) {
        lsfg_log("W", "step3h-a: no cache dir");
        return;
    }

    g_perf_mode = performance ? 1 : 0;
    g_id_generate = LSFG_ID_GENERATE;
    g_id_mipmaps  = LSFG_ID_MIPMAPS;

    lsfg_log("I", "=== LSFG step 3h-a: pin shader RCDATA ids (lsfg-vk table) ===");
    lsfg_log("I", "step3h-a: model=%s generate=%u mipmaps=%u",
             g_perf_mode ? "performance(3.1P)" : "quality(3.1)",
             g_id_generate, g_id_mipmaps);

    char out_path[600];
    snprintf(out_path, sizeof out_path, "%s/manifest_roles.txt", cache_dir);
    FILE *out = fopen(out_path, "w");
    if (out)
        fprintf(out, "# role id present  (lsfg-vk v1.0.0 nameIdxTable)\n");

    int present = 0, missing = 0;
    int need_present = 0, need_missing = 0;

    for (size_t i = 0; i < sizeof(k_roles) / sizeof(k_roles[0]); i++) {
        const Role *r = &k_roles[i];
        int in_model = g_perf_mode ? (r->perf == 1 || r->id == 255 || r->id == 256)
                                   : (r->perf == 0);
        int ok = has_spv(cache_dir, r->id);
        if (ok) present++;
        else missing++;

        if (in_model) {
            if (ok) need_present++;
            else need_missing++;
        }

        if (out)
            fprintf(out, "%s %u %s%s\n",
                    r->name, r->id,
                    ok ? "ok" : "MISSING",
                    in_model ? " *" : "");

        if (r->id == LSFG_ID_GENERATE || r->id == LSFG_ID_MIPMAPS ||
            (in_model && (strncmp(r->name, "gamma", 5) == 0 ||
                          strncmp(r->name, "p_gamma", 7) == 0 ||
                          strcmp(r->name, "generate") == 0 ||
                          strcmp(r->name, "p_generate") == 0))) {
            if (!ok)
                lsfg_log("W", "step3h-a: MISSING %s id=%u", r->name, r->id);
        }
    }

    if (out) {
        fprintf(out, "# summary present=%d missing=%d model_ok=%d model_missing=%d\n",
                present, missing, need_present, need_missing);
        fclose(out);
    }

    g_roles_missing = need_missing;
    g_roles_ok = (has_spv(cache_dir, g_id_generate) && has_spv(cache_dir, g_id_mipmaps));

    lsfg_log("I", "step3h-a: table entries=%zu file_ok=%d file_miss=%d model_ok=%d model_miss=%d",
             sizeof(k_roles) / sizeof(k_roles[0]), present, missing, need_present, need_missing);
    lsfg_log("I", "step3h-a: roles %s (generate=%u mipmaps=%u) -> %s",
             g_roles_ok ? "PINNED" : "INCOMPLETE",
             g_id_generate, g_id_mipmaps, out_path);
    lsfg_log("I", "NEXT step 3h-b: build pass graph mipmaps->flow(alpha/beta/gamma/delta)->generate");
}

extern "C" void lsfg_step3h_install(void) {
    lsfg_log("I", "LSFG: step3h-a role table ready (lsfg-vk RCDATA map)");
}
