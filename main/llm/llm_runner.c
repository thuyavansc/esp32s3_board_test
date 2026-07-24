// ================================================================
// llm_runner.c — see llm_runner.h. Serial-console adapter around
// llm.c's ported inference engine — no display coupling of any kind.
// ================================================================
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "config.h"
#include "llm.h"
#include "llm_runner.h"

static const char *TAG = "llm";

static bool s_mounted     = false;
static bool s_model_ready = false;
static int  s_steps       = LLM_DEFAULT_STEPS;

static Transformer s_transformer;
static Tokenizer   s_tokenizer;
static Sampler     s_sampler;

static void _on_generate_complete(float tok_s) {
    ESP_LOGI(TAG, "Generation complete — %.2f tok/s", tok_s);
}

static void _show_help(void) {
    printf("\n  llm run <prompt text>   Generate text continuing from <prompt text>\n");
    printf("                          (first call loads the model — takes a few seconds)\n");
    printf("  llm steps <n>           Set tokens-to-generate for future 'llm run' (default %d)\n", LLM_DEFAULT_STEPS);
    printf("  llm help                This message\n\n");
}

static bool _ensure_model_loaded(void) {
    if (s_model_ready) return true;

    if (!s_mounted) {
        ESP_LOGE(TAG, "LLM partition not mounted — cannot load model");
        return false;
    }

    ESP_LOGI(TAG, "Loading model (first use) — %s", LLM_MODEL_PATH);
    build_transformer(&s_transformer, LLM_MODEL_PATH);
    build_tokenizer(&s_tokenizer, LLM_TOKENIZER_PATH, s_transformer.config.vocab_size);
    build_sampler(&s_sampler, s_transformer.config.vocab_size,
                  LLM_TEMPERATURE, LLM_TOPP, (unsigned long long)time(NULL));

    s_model_ready = true;
    ESP_LOGI(TAG, "Model ready — vocab_size=%d seq_len=%d",
             s_transformer.config.vocab_size, s_transformer.config.seq_len);
    return true;
}

void llm_runner_init(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = LLM_MOUNT_POINT,
        .partition_label        = "llm",
        .max_files              = 2,
        .format_if_mount_failed = false,   // pre-flashed image — a mount failure here is a real error, not "first boot"
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount 'llm' partition at %s (%s) — was the SPIFFS image actually flashed?",
                 LLM_MOUNT_POINT, esp_err_to_name(ret));
        return;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("llm", &total, &used);
    ESP_LOGI(TAG, "LLM partition mounted — %u/%u bytes used", (unsigned)used, (unsigned)total);
    s_mounted = true;
}

bool llm_runner_process_command(const char *line) {
    while (*line == ' ') line++;
    if (strncasecmp(line, "llm", 3) != 0) return false;

    const char *p = line + 3;
    while (*p == ' ') p++;

    if (*p == '\0' || strncasecmp(p, "help", 4) == 0) { _show_help(); return true; }

    if (strncasecmp(p, "steps", 5) == 0) {
        p += 5;
        while (*p == ' ') p++;
        int n = atoi(p);
        if (n <= 0) {
            printf("Usage: llm steps <n>\n");
        } else {
            s_steps = n;
            ESP_LOGI(TAG, "Steps set to %d for future 'llm run'", s_steps);
        }
        return true;
    }

    if (strncasecmp(p, "run", 3) == 0) {
        p += 3;
        while (*p == ' ') p++;

        if (!_ensure_model_loaded()) {
            printf("Model failed to load — see log above.\n");
            return true;
        }

        const char *prompt = (*p == '\0') ? NULL : p;
        ESP_LOGI(TAG, "Generating (%d steps)%s%s ...", s_steps,
                 prompt ? " — prompt: " : " — no prompt", prompt ? prompt : "");
        generate(&s_transformer, &s_tokenizer, &s_sampler, (char *)prompt, s_steps, &_on_generate_complete);
        return true;
    }

    printf("Unknown 'llm' subcommand. Try 'llm help'.\n");
    return true;
}
