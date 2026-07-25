/* Inference for Llama-2 Transformer model in pure C */
/**
 * Original author of this:
 * https://github.com/karpathy/llama2.c 
 * 
 * Slight modifications added to make it ESP32 friendly
 */

#include "llm.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <fcntl.h>
#include <setjmp.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_dsp.h"
#include "esp_attr.h"

#define MAP_FAILED NULL
#define munmap(ptr, length) custom_munmap(ptr)
#define close(fd) custom_close(fd)

// ── ESP32 port addition — exit() is fatal on ESP-IDF, not a normal process
// exit like on the desktop this was originally written for ──
// Upstream (github.com/karpathy/llama2.c) calls plain exit(EXIT_FAILURE) on
// any file-not-found/malloc-failure/bad-input error, which is safe on a
// desktop OS (it just ends the process). On ESP-IDF there is no "process" to
// end — exit() triggers abort() -> a full system panic -> the whole board
// reboots, for something as simple as a missing model file. Redefined here,
// same pattern this file already uses for close()/munmap() above, so a
// caller (llm_runner.c) can setjmp() once around a risky call and safely
// recover instead of losing the whole device.
jmp_buf g_llm_error_jmp;
#define exit(code) longjmp(g_llm_error_jmp, 1)

#define TASK_0_BIT (1 << 0)
#define TASK_1_BIT (1 << 1)
#define FORWARD_TASK_1 (1 << 2)
#define FORWARD_TASK_2 (1 << 3)
#define READY_BIT (1 << 3)
#define ALL_SYNC_BITS (TASK_0_BIT | TASK_1_BIT)
#define ALL_FORWARD_TASKS (FORWARD_TASK_1 | FORWARD_TASK_2)

typedef struct
{
    v4sf *xout;
    v4sf *x;
    v4sf *w;
    int start;
    int end;
    int n;
    int d;
    int task_num;
} MatMulTaskParams;

typedef struct
{
    RunState *s;
    TransformerWeights *w;
    Config *p;
    int pos;
    int start;
    int loff;
    int end;
    int dim;
    int kv_dim;
    int kv_mul;
    int hidden_dim;
    int head_size;
    int task_num;
} ForwardTaskParams;

EventGroupHandle_t xEventGroup;
EventGroupHandle_t ForwardEventGroup;

static const char *TAG = "LLM";
TaskHandle_t handle_forward_task = NULL;
TaskHandle_t matmul_task_2 = NULL;

// ESP32 port addition — same helper/rotating-buffer approach used
// elsewhere in this project (ram_test.c, llm_runner.c): formats a byte
// count as "X.X KB" (under 1MB) or "X.XX MB" (1MB+), so this file's own
// diagnostic logging (file size, free RAM) is human-readable too.
static const char *_fmt_bytes(long bytes) {
    static char buf[4][24];
    static int  idx = 0;
    idx = (idx + 1) % 4;
    if (bytes < 0) bytes = 0;
    if ((size_t)bytes < 1024u * 1024u) {
        snprintf(buf[idx], sizeof(buf[idx]), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf[idx], sizeof(buf[idx]), "%.2f MB", bytes / (1024.0 * 1024.0));
    }
    return buf[idx];
}

ForwardTaskParams *forward_params = NULL;
MatMulTaskParams *matmul_params = NULL;

SemaphoreHandle_t semaDataReady;
SemaphoreHandle_t semaForwardDataReady;

void matmul_task(void *params);
void forward_task(void *params);

void custom_munmap(void *ptr)
{
    free(ptr);
}

int custom_close(int fd)
{
    // Since there are no actual file descriptors to close, simply return 0 (success)
    return 0;
}

void chat(Transformer *transformer, Tokenizer *tokenizer, Sampler *sampler,
          char *cli_user_prompt, char *cli_system_prompt, int steps);

void malloc_run_state(RunState *s, Config *p)
{
    // we calloc instead of malloc to keep valgrind happy
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    s->x = calloc(p->dim, sizeof(v4sf));
    s->xb = calloc(p->dim, sizeof(v4sf));
    s->xb2 = calloc(p->dim, sizeof(v4sf));
    s->hb = calloc(p->hidden_dim, sizeof(v4sf));
    s->hb2 = calloc(p->hidden_dim, sizeof(v4sf));
    s->q = calloc(p->dim, sizeof(v4sf));
    s->key_cache = calloc(p->n_layers * p->seq_len * kv_dim, sizeof(v4sf));
    s->value_cache = calloc(p->n_layers * p->seq_len * kv_dim, sizeof(v4sf));
    s->att = calloc(p->n_heads * p->seq_len, sizeof(v4sf));
    s->logits = calloc(p->vocab_size, sizeof(v4sf));
    // ensure all mallocs went fine
    if (!s->x || !s->xb || !s->xb2 || !s->hb || !s->hb2 || !s->q || !s->key_cache || !s->value_cache || !s->att || !s->logits)
    {
        fprintf(stderr, "malloc failed!\n");
        exit(EXIT_FAILURE);
    }
}

void free_run_state(RunState *s)
{
    free(s->x);
    free(s->xb);
    free(s->xb2);
    free(s->hb);
    free(s->hb2);
    free(s->q);
    free(s->att);
    free(s->logits);
    free(s->key_cache);
    free(s->value_cache);
}

void memory_map_weights(TransformerWeights *w, Config *p, v4sf *ptr, int shared_weights)
{
    int head_size = p->dim / p->n_heads;
    // make sure the multiplications below are done in 64bit to fit the parameter counts of 13B+ models
    unsigned long long n_layers = p->n_layers;
    w->token_embedding_table = ptr;
    ptr += p->vocab_size * p->dim;
    w->rms_att_weight = ptr;
    ptr += n_layers * p->dim;
    w->wq = ptr;
    ptr += n_layers * p->dim * (p->n_heads * head_size);
    w->wk = ptr;
    ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
    w->wv = ptr;
    ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
    w->wo = ptr;
    ptr += n_layers * (p->n_heads * head_size) * p->dim;
    w->rms_ffn_weight = ptr;
    ptr += n_layers * p->dim;
    w->w1 = ptr;
    ptr += n_layers * p->dim * p->hidden_dim;
    w->w2 = ptr;
    ptr += n_layers * p->hidden_dim * p->dim;
    w->w3 = ptr;
    ptr += n_layers * p->dim * p->hidden_dim;
    w->rms_final_weight = ptr;
    ptr += p->dim;
    ptr += p->seq_len * head_size / 2; // skip what used to be freq_cis_real (for RoPE)
    ptr += p->seq_len * head_size / 2; // skip what used to be freq_cis_imag (for RoPE)
    w->wcls = shared_weights ? w->token_embedding_table : ptr;
}

void read_checkpoint(char *checkpoint, Config *config, TransformerWeights *weights,
                     int *fd, v4sf **data, size_t *file_size)
{
    FILE *file = fopen(checkpoint, "rb");
    if (!file)
    {
        ESP_LOGE(TAG, "Couldn't open file %s", checkpoint);
        exit(EXIT_FAILURE);
    }
    // read in the config header
    if (fread(config, sizeof(Config), 1, file) != 1)
    {
        exit(EXIT_FAILURE);
    }
    // negative vocab size is hacky way of signaling unshared weights. bit yikes.
    int shared_weights = config->vocab_size > 0 ? 1 : 0;
    config->vocab_size = abs(config->vocab_size);
    ESP_LOGI(TAG, "Vocab size if %d", config->vocab_size);
    // TEMPORARY diagnostic (doc 131/133) — confirm the Config header itself
    // was parsed correctly. Expected for stories260K: dim=64 n_layers=5
    // n_heads=8 n_kv_heads=4 vocab=512 seq=512 (hidden_dim derived, not
    // officially published — see doc 123 — expect roughly ~172).
    ESP_LOGI(TAG, "[cfg] dim=%d hidden_dim=%d n_layers=%d n_heads=%d n_kv_heads=%d vocab=%d seq_len=%d",
             config->dim, config->hidden_dim, config->n_layers, config->n_heads,
             config->n_kv_heads, config->vocab_size, config->seq_len);
    // figure out the file size
    fseek(file, 0, SEEK_END); // move file pointer to end of file
    *file_size = ftell(file); // get the file size, in bytes
    fseek(file, 0, SEEK_SET); // move back to beginning for reading
    ESP_LOGI(TAG, "File size: %zu bytes (%s)", *file_size, _fmt_bytes((long)*file_size));
    ESP_LOGI(TAG, "Free ram available: %lu bytes (%s)", esp_get_free_heap_size(), _fmt_bytes((long)esp_get_free_heap_size()));
    *data = malloc(*file_size);
    if (*data == NULL)
    {
        ESP_LOGE(TAG, "Malloc operation failed");
        exit(EXIT_FAILURE);
    }
    // Read the entire file into memory
    size_t bytes_read = fread(*data, 1, *file_size, file);
    if (bytes_read != *file_size)
    {
        ESP_LOGE(TAG, "Failed to read file into memory");
        ESP_LOGE(TAG, "Bytes read %zu bytes (%s)", bytes_read, _fmt_bytes((long)bytes_read));
        exit(EXIT_FAILURE);
    }
    fclose(file);

    ESP_LOGI(TAG, "Successfully read LLM into memory");
    ESP_LOGI(TAG, "Free ram available: %lu bytes (%s)", esp_get_free_heap_size(), _fmt_bytes((long)esp_get_free_heap_size()));
    v4sf *weights_ptr = *data + sizeof(Config) / sizeof(v4sf);
    memory_map_weights(weights, config, weights_ptr, shared_weights);
    // TEMPORARY diagnostic (doc 131/133) — sanity-check the actual loaded
    // weight values. A real, trained model's embedding weights should be
    // small floats (roughly -1.0 to +1.0) — all-zero, all-identical, NaN,
    // or huge values here would mean the file wasn't loaded/mapped correctly.
    ESP_LOGI(TAG, "[wgt] token_embedding_table[0..7]: %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
             weights->token_embedding_table[0], weights->token_embedding_table[1],
             weights->token_embedding_table[2], weights->token_embedding_table[3],
             weights->token_embedding_table[4], weights->token_embedding_table[5],
             weights->token_embedding_table[6], weights->token_embedding_table[7]);
    ESP_LOGI(TAG, "[wgt] wcls[0..3] (classifier weights, first row): %.4f %.4f %.4f %.4f",
             weights->wcls[0], weights->wcls[1], weights->wcls[2], weights->wcls[3]);
    ESP_LOGI(TAG, "Successfully read checkpoint");
}

void build_transformer(Transformer *t, char *checkpoint_path)
{
    // read in the Config and the Weights from the checkpoint
    read_checkpoint(checkpoint_path, &t->config, &t->weights, &t->fd, &t->data, &t->file_size);
    // allocate the RunState buffers
    malloc_run_state(&t->state, &t->config);
    ESP_LOGI(TAG, "Transformer successfully built");

    // FreeRTos Tasks
    xEventGroup = xEventGroupCreate();
    ForwardEventGroup = xEventGroupCreate();
    semaDataReady = xSemaphoreCreateBinary();
    semaForwardDataReady = xSemaphoreCreateBinary();
    xSemaphoreGive(semaDataReady);
    xSemaphoreTake(semaDataReady, portMAX_DELAY);
    xSemaphoreGive(semaForwardDataReady);
    xSemaphoreTake(semaForwardDataReady, portMAX_DELAY);

    matmul_params = malloc(sizeof(MatMulTaskParams));
    forward_params = malloc(sizeof(ForwardTaskParams));
    xTaskCreatePinnedToCore(matmul_task, "MatMul2", 2048, matmul_params, 19, &matmul_task_2, 1);             // Run on Core 1
    xTaskCreatePinnedToCore(forward_task, "ForwardTask", 2048, forward_params, 19, &handle_forward_task, 1); // Run on Core 1
    ESP_LOGI(TAG, "Created FreeRTOS Tasks");
}

void free_transformer(Transformer *t)
{
    // close the memory mapping
    if (t->data != MAP_FAILED)
    {
        munmap(t->data, t->file_size);
    }
    if (t->fd != -1)
    {
        close(t->fd);
    }
    // free the RunState buffers
    free_run_state(&t->state);
}

// ----------------------------------------------------------------------------
// neural net blocks; the dynamics of the Transformer

void rmsnorm(v4sf *o, v4sf *x, v4sf *weight, int size)
{
    // calculate sum of squares
    v4sf ss = 0.0f;
    for (int j = 0; j < size; j++)
    {
        ss += x[j] * x[j];
    }
    ss /= size;
    ss += 1e-5f;
    ss = 1.0f / sqrtf(ss);
    // normalize and scale
    for (int j = 0; j < size; j++)
    {
        o[j] = weight[j] * (ss * x[j]);
    }
}

void softmax(v4sf *x, int size)
{
    // find max value (for numerical stability)
    v4sf max_val = x[0];
    for (int i = 1; i < size; i++)
    {
        if (x[i] > max_val)
        {
            max_val = x[i];
        }
    }
    // exp and sum
    v4sf sum = 0.0f;
    for (int i = 0; i < size; i++)
    {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }
    // normalize
    for (int i = 0; i < size; i++)
    {
        x[i] /= sum;
    }
}

void matmul_task(void *params)
{
    const TickType_t xDelay = 1 / portTICK_PERIOD_MS;
    MatMulTaskParams *p = (MatMulTaskParams *)params;
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    char *tName = pcTaskGetName(current_task);
    // ESP_LOGI(TAG, "Created Task %s", tName);
    // ESP32 port note — an earlier version of this fix added a periodic
    // vTaskDelay() directly inside this loop as a second safety net
    // alongside generate()'s own per-token yield. Removed: this loop is
    // part of a tightly-timed handshake with the calling task (a semaphore
    // hand-off + xEventGroupSync rendezvous, repeated ~36 times per
    // token) — inserting an unpredictable pause here desynced the two
    // sides' notion of "which round we're on" and caused a real hang on
    // hardware. generate()'s own yield (outside this handshake entirely,
    // only running once a full token's exchange has fully completed) is
    // the safe place for this — see doc 130.
    for (;;)
    {
        if (xSemaphoreTake(semaDataReady, portMAX_DELAY) == pdTRUE)
        {
            //   ESP_LOGI(TAG, "Started Task %s", tName);
            for (int i = p->start; i < p->end; i++)
            {
                v4sf val = 0.0f;
                v4sf *row = &p->w[i * p->n]; // Pointer to the start of the current row in matrix w
                dsps_dotprod_f32_aes3(row, p->x, &val, p->n);
                p->xout[i] = val;
            }
            //    ESP_LOGI(TAG, "Completed task %s", tName);
            xSemaphoreGive(semaDataReady);
            xEventGroupSync(xEventGroup, p->task_num, ALL_SYNC_BITS, portMAX_DELAY);
        }
    }
}

void forward_task(void *params)
{
    const TickType_t xDelay = 1 / portTICK_PERIOD_MS;
    ForwardTaskParams *t_params = (ForwardTaskParams *)params;
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    char *tName = pcTaskGetName(current_task);
    // ESP_LOGI(TAG, "Created Task %s", tName);
    // ESP32 port note — see matmul_task's own identical comment: a periodic
    // vTaskDelay() was tried here too and removed for the same reason (it
    // sits inside a tightly-timed handshake with the calling task and
    // caused a real hang). See doc 130.
    for (;;)
    {
        if (xSemaphoreTake(semaForwardDataReady, portMAX_DELAY) == pdTRUE)
        {
            //   ESP_LOGI(TAG, "Started Task %s", tName);
            int h;
            // #pragma omp parallel for private(h)
            for (h = t_params->start; h < t_params->end; h++)
            {
                // get the query vector for this head
                v4sf *q = t_params->s->q + h * t_params->head_size;
                // attention scores for this head
                v4sf *att = t_params->s->att + h * t_params->p->seq_len;
                // iterate over all timesteps, including the current one
                for (int t = 0; t <= t_params->pos; t++)
                {
                    // get the key vector for this head and at this timestep
                    v4sf *k = t_params->s->key_cache + t_params->loff + t * t_params->kv_dim + (h / t_params->kv_mul) * t_params->head_size;
                    // calculate the attention score as the dot product of q and k
                    v4sf score = 0.0f;
                    for (int i = 0; i < t_params->head_size; i++)
                    {
                        score += q[i] * k[i];
                    }
                    score /= sqrtf(t_params->head_size);
                    // save the score to the attention buffer
                    att[t] = score;
                }

                // softmax the scores to get attention weights, from 0..pos inclusively
                softmax(att, t_params->pos + 1);

                // weighted sum of the values, store back into xb
                v4sf *xb = t_params->s->xb + h * t_params->head_size;
                memset(xb, 0, t_params->head_size * sizeof(v4sf));
                for (int t = 0; t <= t_params->pos; t++)
                {
                    // get the value vector for this head and at this timestep
                    v4sf *v = t_params->s->value_cache + t_params->loff + t * t_params->kv_dim + (h / t_params->kv_mul) * t_params->head_size;
                    // get the attention weight for this timestep
                    v4sf a = att[t];
                    // accumulate the weighted value into xb
                    for (int i = 0; i < t_params->head_size; i++)
                    {
                        xb[i] += a * v[i];
                    }
                }
            }
            //   ESP_LOGI(TAG, "Completed task %s", tName);
            xSemaphoreGive(semaForwardDataReady);
            xEventGroupSync(ForwardEventGroup, t_params->task_num, ALL_FORWARD_TASKS, portMAX_DELAY);
        }
    }
}

void matmul(v4sf *xout, v4sf *x, v4sf *w, int n, int d)
{
    // d is the number of rows
    // n is the number of columns
    // d X n

    // TEMPORARY DIAGNOSTIC (doc 131/133) — the dual-core split (half the
    // rows computed here inline, half offloaded to matmul_task on Core 1
    // via the semaphore/event-group handshake below) is disabled. Every
    // row is now computed inline, single-core, sequentially. This isolates
    // whether the token-511 collapse is caused by the split/cross-core
    // handshake itself, or lives elsewhere (logits/weights/state).
    // matmul_task is still created but never fed, so it just sits asleep,
    // harmless. REVERT this whole function to restore the original
    // dual-core split once diagnosed.
    for (int i = 0; i < d; i++)
    {
        v4sf val = 0.0f;
        v4sf *row = &w[i * n]; // Pointer to the start of the current row in matrix w
        dsps_dotprod_f32_aes3(row, x, &val, n);
        xout[i] = val;
    }
}

v4sf *forward(Transformer *transformer, int token, int pos)
{
    ESP_LOGD(TAG, "ram available: %lu", esp_get_free_heap_size());

    // a few convenience variables
    Config *p = &transformer->config;
    TransformerWeights *w = &transformer->weights;
    RunState *s = &transformer->state;
    v4sf *x = s->x;
    int dim = p->dim;
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    int kv_mul = p->n_heads / p->n_kv_heads; // integer multiplier of the kv sharing in multiquery
    int hidden_dim = p->hidden_dim;
    int head_size = dim / p->n_heads;

    // copy the token embedding into x
    v4sf *content_row = w->token_embedding_table + token * dim;
    ESP_LOGD(TAG, "Content row: %f", *content_row);
    memcpy(x, content_row, dim * sizeof(*x));

    // forward all the layers
    for (unsigned long long l = 0; l < p->n_layers; l++)
    {
        ESP_LOGD(TAG, "X: %f, Weights %f", *x, *w->rms_att_weight);
        // attention rmsnorm
        rmsnorm(s->xb, x, w->rms_att_weight + l * dim, dim);

        // key and value point to the kv cache
        int loff = l * p->seq_len * kv_dim; // kv cache layer offset for convenience
        s->k = s->key_cache + loff + pos * kv_dim;
        s->v = s->value_cache + loff + pos * kv_dim;

        // qkv matmuls for this position
        matmul(s->q, s->xb, w->wq + l * dim * dim, dim, dim);
        matmul(s->k, s->xb, w->wk + l * dim * kv_dim, dim, kv_dim);
        matmul(s->v, s->xb, w->wv + l * dim * kv_dim, dim, kv_dim);

        // RoPE relative positional encoding: complex-valued rotate q and k in each head
        for (int i = 0; i < dim; i += 2)
        {
            int head_dim = i % head_size;
            v4sf freq = 1.0f / powf(10000.0f, head_dim / (v4sf)head_size);
            v4sf val = pos * freq;
            v4sf fcr = cosf(val);
            v4sf fci = sinf(val);
            int rotn = i < kv_dim ? 2 : 1; // how many vectors? 2 = q & k, 1 = q only
            for (int v = 0; v < rotn; v++)
            {
                v4sf *vec = v == 0 ? s->q : s->k; // the vector to rotate (query or key)
                v4sf v0 = vec[i];
                v4sf v1 = vec[i + 1];
                vec[i] = v0 * fcr - v1 * fci;
                vec[i + 1] = v0 * fci + v1 * fcr;
            }
        }
        // TEMPORARY DIAGNOSTIC (doc 131/133) — same as matmul()'s own
        // change above: the dual-core split (half the heads here inline,
        // half offloaded to forward_task on Core 1) is disabled. The loop
        // below now covers ALL heads (0..n_heads), not just the first
        // half, so no offloaded work is skipped — forward_task is still
        // created but never fed, harmless. REVERT to restore the
        // original split once diagnosed.

        // multihead attention. iterate over all heads
        int h;
        // #pragma omp parallel for private(h)
        for (h = 0; h < p->n_heads; h++)
        {
            // get the query vector for this head
            v4sf *q = s->q + h * head_size;
            // attention scores for this head
            v4sf *att = s->att + h * p->seq_len;
            // iterate over all timesteps, including the current one
            for (int t = 0; t <= pos; t++)
            {
                // get the key vector for this head and at this timestep
                v4sf *k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                // calculate the attention score as the dot product of q and k
                v4sf score = 0.0f;
                for (int i = 0; i < head_size; i++)
                {
                    score += q[i] * k[i];
                }
                score /= sqrtf(head_size);
                // save the score to the attention buffer
                att[t] = score;
            }

            // softmax the scores to get attention weights, from 0..pos inclusively
            softmax(att, pos + 1);

            // weighted sum of the values, store back into xb
            v4sf *xb = s->xb + h * head_size;
            memset(xb, 0, head_size * sizeof(v4sf));
            for (int t = 0; t <= pos; t++)
            {
                // get the value vector for this head and at this timestep
                v4sf *v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                // get the attention weight for this timestep
                v4sf a = att[t];
                // accumulate the weighted value into xb
                for (int i = 0; i < head_size; i++)
                {
                    xb[i] += a * v[i];
                }
            }
        }
        // TEMPORARY DIAGNOSTIC (doc 131/133) — this used to be gated behind
        // xSemaphoreTake(semaForwardDataReady, ...)/xEventGroupSync(...),
        // waiting for forward_task's offloaded half. Since nothing feeds
        // that semaphore anymore (all heads are computed inline above),
        // that wait would now block forever — removed, and this code
        // (which was always the real, needed computation, just previously
        // wrapped in that wait) now just runs unconditionally instead.
        {
            // final matmul to get the output of the attention
            matmul(s->xb2, s->xb, w->wo + l * dim * dim, dim, dim);

            // residual connection back into x
            for (int i = 0; i < dim; i++)
            {
                x[i] += s->xb2[i];
            }

            // ffn rmsnorm
            rmsnorm(s->xb, x, w->rms_ffn_weight + l * dim, dim);

            // Now for FFN in PyTorch we have: self.w2(F.silu(self.w1(x)) * self.w3(x))
            // first calculate self.w1(x) and self.w3(x)
            matmul(s->hb, s->xb, w->w1 + l * dim * hidden_dim, dim, hidden_dim);
            matmul(s->hb2, s->xb, w->w3 + l * dim * hidden_dim, dim, hidden_dim);

            // SwiGLU non-linearity
            for (int i = 0; i < hidden_dim; i++)
            {
                v4sf val = s->hb[i];
                // silu(x)=x*σ(x), where σ(x) is the logistic sigmoid
                val *= (1.0f / (1.0f + expf(-val)));
                // elementwise multiply with w3(x)
                val *= s->hb2[i];
                s->hb[i] = val;
            }

            // final matmul to get the output of the ffn
            matmul(s->xb, s->hb, w->w2 + l * dim * hidden_dim, hidden_dim, dim);

            // residual connection
            for (int i = 0; i < dim; i++)
            {
                x[i] += s->xb[i];
            }
        }
    }

    // final rmsnorm
    rmsnorm(x, x, w->rms_final_weight, dim);

    // classifier into logits
    matmul(s->logits, x, w->wcls, p->dim, p->vocab_size);
    return s->logits;
}

// ----------------------------------------------------------------------------
// The Byte Pair Encoding (BPE) Tokenizer that translates strings <-> tokens

int compare_tokens(const void *a, const void *b)
{
    return strcmp(((TokenIndex *)a)->str, ((TokenIndex *)b)->str);
}

void build_tokenizer(Tokenizer *t, char *tokenizer_path, int vocab_size)
{
    // i should have written the vocab_size into the tokenizer file... sigh
    ESP_LOGI(TAG, "Vocab size is %d\n", vocab_size);
    t->vocab_size = vocab_size;
    // malloc space to hold the scores and the strings
    t->vocab = (char **)malloc(vocab_size * sizeof(char *));
    t->vocab_scores = (v4sf *)malloc(vocab_size * sizeof(v4sf));
    t->sorted_vocab = NULL; // initialized lazily
    for (int i = 0; i < 256; i++)
    {
        t->byte_pieces[i * 2] = (unsigned char)i;
        t->byte_pieces[i * 2 + 1] = '\0';
    }
    // read in the file
    FILE *file = fopen(tokenizer_path, "rb");
    if (!file)
    {
        ESP_LOGE(TAG, "couldn't load %s", tokenizer_path);
        exit(EXIT_FAILURE);
    }
    ESP_LOGI(TAG, "Opened Tokenizer File");
    if (fread(&t->max_token_length, sizeof(int), 1, file) != 1)
    {
        ESP_LOGE(TAG, "failed read");
        exit(EXIT_FAILURE);
    }
    int len;
    for (int i = 0; i < vocab_size; i++)
    {
        if (fread(t->vocab_scores + i, sizeof(v4sf), 1, file) != 1)
        {
            ESP_LOGE(TAG, "failed read vocab scores");
            exit(EXIT_FAILURE);
        }
        if (fread(&len, sizeof(int), 1, file) != 1)
        {
            ESP_LOGE(TAG, "failed read len");
            exit(EXIT_FAILURE);
        }
        t->vocab[i] = (char *)malloc(len + 1);
        if (fread(t->vocab[i], len, 1, file) != 1)
        {
            ESP_LOGE(TAG, "failed read vocab");
            exit(EXIT_FAILURE);
        }
        t->vocab[i][len] = '\0'; // add the string terminating token
    }
    fclose(file);
    ESP_LOGI(TAG, "Tokenizer successfully built");
}

void free_tokenizer(Tokenizer *t)
{
    for (int i = 0; i < t->vocab_size; i++)
    {
        free(t->vocab[i]);
    }
    free(t->vocab);
    free(t->vocab_scores);
    free(t->sorted_vocab);
}

char *decode(Tokenizer *t, int prev_token, int token)
{
    char *piece = t->vocab[token];
    // following BOS (1) token, sentencepiece decoder strips any leading whitespace (see PR #89)
    if (prev_token == 1 && piece[0] == ' ')
    {
        piece++;
    }
    // careful, some tokens designate raw bytes, and look like e.g. '<0x01>'
    // parse this and convert and return the actual byte
    unsigned char byte_val;
    if (sscanf(piece, "<0x%02hhX>", &byte_val) == 1)
    {
        piece = (char *)t->byte_pieces + byte_val * 2;
    }
    return piece;
}

void safe_printf(char *piece)
{
    // piece might be a raw byte token, and we only want to print printable chars or whitespace
    // because some of the other bytes can be various control codes, backspace, etc.
    if (piece == NULL)
    {
        return;
    }
    if (piece[0] == '\0')
    {
        return;
    }
    if (piece[1] == '\0')
    {
        unsigned char byte_val = piece[0];
        if (!(isprint(byte_val) || isspace(byte_val)))
        {
            return; // bad byte, don't print it
        }
    }
    printf("%s", piece);
}

int str_lookup(char *str, TokenIndex *sorted_vocab, int vocab_size)
{
    // efficiently find the perfect match for str in vocab, return its index or -1 if not found
    TokenIndex tok = {.str = str}; // acts as the key to search for
    TokenIndex *res = bsearch(&tok, sorted_vocab, vocab_size, sizeof(TokenIndex), compare_tokens);
    return res != NULL ? res->id : -1;
}

void encode(Tokenizer *t, char *text, int8_t bos, int8_t eos, int *tokens, int *n_tokens)
{
    // encode the string text (input) into an upper-bound preallocated tokens[] array
    // bos != 0 means prepend the BOS token (=1), eos != 0 means append the EOS token (=2)
    if (text == NULL)
    {
        ESP_LOGE(TAG, "cannot encode NULL text");
        exit(EXIT_FAILURE);
    }

    if (t->sorted_vocab == NULL)
    {
        // lazily malloc and sort the vocabulary
        t->sorted_vocab = malloc(t->vocab_size * sizeof(TokenIndex));
        for (int i = 0; i < t->vocab_size; i++)
        {
            t->sorted_vocab[i].str = t->vocab[i];
            t->sorted_vocab[i].id = i;
        }
        qsort(t->sorted_vocab, t->vocab_size, sizeof(TokenIndex), compare_tokens);
    }

    // create a temporary buffer that will store merge candidates of always two consecutive tokens
    // *2 for concat, +1 for null terminator +2 for UTF8 (in case max_token_length is 1)
    char *str_buffer = malloc((t->max_token_length * 2 + 1 + 2) * sizeof(char));
    size_t str_len = 0;

    // start at 0 tokens
    *n_tokens = 0;

    // add optional BOS (=1) token, if desired
    if (bos)
        tokens[(*n_tokens)++] = 1;

    // add_dummy_prefix is true by default
    // so prepend a dummy prefix token to the input string, but only if text != ""
    // TODO: pretty sure this isn't correct in the general case but I don't have the
    // energy to read more of the sentencepiece code to figure out what it's doing
    if (text[0] != '\0')
    {
        int dummy_prefix = str_lookup(" ", t->sorted_vocab, t->vocab_size);
        tokens[(*n_tokens)++] = dummy_prefix;
    }

    // Okay UTF-8 time. This will get messy. Here is the reference from Wikipedia:
    // Code point ↔ UTF-8 conversion
    // First code point	Last code point	Byte 1	Byte 2	Byte 3	Byte 4
    // U+0000	U+007F	    0xxxxxxx
    // U+0080	U+07FF	    110xxxxx	10xxxxxx
    // U+0800	U+FFFF	    1110xxxx	10xxxxxx	10xxxxxx
    // U+10000	U+10FFFF    11110xxx	10xxxxxx	10xxxxxx	10xxxxxx

    // process the raw (UTF-8) byte sequence of the input string
    for (char *c = text; *c != '\0'; c++)
    {

        // reset buffer if the current byte is ASCII or a leading byte
        // 0xC0 is 11000000, so (*c & 0xC0) keeps the first 2 bits and zeros the rest
        // 0x80 is 10000000
        // in UTF-8, all continuation bytes start with "10" in first two bits
        // so in English this is: "if this byte is not a continuation byte"
        if ((*c & 0xC0) != 0x80)
        {
            // this byte must be either a leading byte (11...) or an ASCII char (0x...)
            // => reset our location, as we're starting a new UTF-8 codepoint
            str_len = 0;
        }

        // append the current byte to the buffer
        str_buffer[str_len++] = *c; // ++ is post-increment, incremented after this line
        str_buffer[str_len] = '\0';

        // while the next character is a continuation byte, continue appending
        // but if there are too many of them, just stop to avoid overruning str_buffer size.
        if ((*(c + 1) & 0xC0) == 0x80 && str_len < 4)
        {
            continue;
        }

        // ok c+1 is not a continuation byte, so we've read in a full codepoint
        int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);

        if (id != -1)
        {
            // we found this codepoint in vocab, add it as a token
            tokens[(*n_tokens)++] = id;
        }
        else
        {
            // byte_fallback encoding: just encode each byte as a token
            // +3 is here because the first 3 vocab elements are <unk>, <s>, </s>
            // so the individual bytes only start at index 3
            for (int i = 0; i < str_len; i++)
            {
                tokens[(*n_tokens)++] = (unsigned char)str_buffer[i] + 3;
            }
        }
        str_len = 0; // protect against a sequence of stray UTF8 continuation bytes
    }

    // merge the best consecutive pair each iteration, according the scores in vocab_scores
    while (1)
    {
        v4sf best_score = -1e10;
        int best_id = -1;
        int best_idx = -1;

        for (int i = 0; i < (*n_tokens - 1); i++)
        {
            // check if we can merge the pair (tokens[i], tokens[i+1])
            sprintf(str_buffer, "%s%s", t->vocab[tokens[i]], t->vocab[tokens[i + 1]]);
            int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);
            if (id != -1 && t->vocab_scores[id] > best_score)
            {
                // this merge pair exists in vocab! record its score and position
                best_score = t->vocab_scores[id];
                best_id = id;
                best_idx = i;
            }
        }

        if (best_idx == -1)
        {
            break; // we couldn't find any more pairs to merge, so we're done
        }

        // merge the consecutive pair (best_idx, best_idx+1) into new token best_id
        tokens[best_idx] = best_id;
        // delete token at position best_idx+1, shift the entire sequence back 1
        for (int i = best_idx + 1; i < (*n_tokens - 1); i++)
        {
            tokens[i] = tokens[i + 1];
        }
        (*n_tokens)--; // token length decreased
    }

    // add optional EOS (=2) token, if desired
    if (eos)
        tokens[(*n_tokens)++] = 2;

    free(str_buffer);
}

// ----------------------------------------------------------------------------
// The Sampler, which takes logits and returns a sampled token
// sampling can be done in a few ways: greedy argmax, sampling, top-p sampling

int sample_argmax(v4sf *probabilities, int n)
{
    // return the index that has the highest probability
    int max_i = 0;
    v4sf max_p = probabilities[0];
    for (int i = 1; i < n; i++)
    {
        if (probabilities[i] > max_p)
        {
            max_i = i;
            max_p = probabilities[i];
        }
    }
    return max_i;
}

int sample_mult(v4sf *probabilities, int n, v4sf coin)
{
    // sample index from probabilities (they must sum to 1!)
    // coin is a random number in [0, 1), usually from random_f32()
    v4sf cdf = 0.0f;
    for (int i = 0; i < n; i++)
    {
        cdf += probabilities[i];
        if (coin < cdf)
        {
            return i;
        }
    }
    return n - 1; // in case of rounding errors
}

int compare(const void *a, const void *b)
{
    ProbIndex *a_ = (ProbIndex *)a;
    ProbIndex *b_ = (ProbIndex *)b;
    if (a_->prob > b_->prob)
        return -1;
    if (a_->prob < b_->prob)
        return 1;
    return 0;
}

int sample_topp(v4sf *probabilities, int n, v4sf topp, ProbIndex *probindex, v4sf coin)
{
    // top-p sampling (or "nucleus sampling") samples from the smallest set of
    // tokens that exceed probability topp. This way we never sample tokens that
    // have very low probabilities and are less likely to go "off the rails".
    // coin is a random number in [0, 1), usually from random_f32()

    int n0 = 0;
    // quicksort indices in descending order of probabilities
    // values smaller than (1 - topp) / (n - 1) cannot be part of the result
    // so for efficiency we crop these out as candidates before sorting
    const v4sf cutoff = (1.0f - topp) / (n - 1);
    for (int i = 0; i < n; i++)
    {
        if (probabilities[i] >= cutoff)
        {
            probindex[n0].index = i;
            probindex[n0].prob = probabilities[i];
            n0++;
        }
    }
    qsort(probindex, n0, sizeof(ProbIndex), compare);

    // truncate the list where cumulative probability exceeds topp
    v4sf cumulative_prob = 0.0f;
    int last_idx = n0 - 1; // in case of rounding errors consider all elements
    for (int i = 0; i < n0; i++)
    {
        cumulative_prob += probindex[i].prob;
        if (cumulative_prob > topp)
        {
            last_idx = i;
            break; // we've exceeded topp by including last_idx
        }
    }

    // sample from the truncated list
    v4sf r = coin * cumulative_prob;
    v4sf cdf = 0.0f;
    for (int i = 0; i <= last_idx; i++)
    {
        cdf += probindex[i].prob;
        if (r < cdf)
        {
            return probindex[i].index;
        }
    }
    return probindex[last_idx].index; // in case of rounding errors
}

void build_sampler(Sampler *sampler, int vocab_size, v4sf temperature, v4sf topp, unsigned long long rng_seed)
{
    sampler->vocab_size = vocab_size;
    sampler->temperature = temperature;
    sampler->topp = topp;
    sampler->rng_state = rng_seed;
    // buffer only used with nucleus sampling; may not need but it's ~small
    sampler->probindex = malloc(sampler->vocab_size * sizeof(ProbIndex));
    ESP_LOGI(TAG, "Sampler Successfully built");
}

void free_sampler(Sampler *sampler)
{
    free(sampler->probindex);
}

unsigned int random_u32(unsigned long long *state)
{
    // xorshift rng: https://en.wikipedia.org/wiki/Xorshift#xorshift.2A
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    return (*state * 0x2545F4914F6CDD1Dull) >> 32;
}
v4sf random_f32(unsigned long long *state)
{ // random v4sf32 in [0,1)
    return (random_u32(state) >> 8) / 16777216.0f;
}

int sample(Sampler *sampler, v4sf *logits)
{
    // sample the token given the logits and some hyperparameters
    int next;
    // TEMPORARY diagnostic (doc 131/133 continued) — logits[] independently
    // scanned in generate() right before this call disagrees with what this
    // function returns. This proves either (a) temperature isn't really
    // 0.0f at runtime so the argmax branch below isn't even taken, or
    // (b) sample_argmax() itself returns something other than the true max
    // despite identical logic to the independent scan. Logged here, at the
    // exact point of use, to tell those two apart.
    ESP_LOGI(TAG, "[smp] temperature=%.6f vocab_size=%d", sampler->temperature, sampler->vocab_size);
    if (sampler->temperature == 0.0f)
    {
        // greedy argmax sampling: take the token with the highest probability
        next = sample_argmax(logits, sampler->vocab_size);
        ESP_LOGI(TAG, "[smp] sample_argmax returned=%d", next);
    }
    else
    {
        // apply the temperature to the logits
        for (int q = 0; q < sampler->vocab_size; q++)
        {
            logits[q] /= sampler->temperature;
        }
        // apply softmax to the logits to get the probabilities for next token
        softmax(logits, sampler->vocab_size);
        // flip a (v4sf) coin (this is our source of entropy for sampling)
        v4sf coin = random_f32(&sampler->rng_state);
        // we sample from this distribution to get the next token
        if (sampler->topp <= 0 || sampler->topp >= 1)
        {
            // simply sample from the predicted probability distribution
            next = sample_mult(logits, sampler->vocab_size, coin);
        }
        else
        {
            // top-p (nucleus) sampling, clamping the least likely tokens to zero
            next = sample_topp(logits, sampler->vocab_size, sampler->topp, sampler->probindex, coin);
        }
    }
    return next;
}

// ----------------------------------------------------------------------------
// utilities: time

long time_in_ms()
{
    // return time in milliseconds, for benchmarking the model speed
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    return time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

// ----------------------------------------------------------------------------
// generation loop

void generate(Transformer *transformer, Tokenizer *tokenizer, Sampler *sampler, char *prompt, int steps, generated_complete_cb cb_done)
{
    char *empty_prompt = "";
    if (prompt == NULL)
    {
        prompt = empty_prompt;
    }

    // encode the (string) prompt into tokens sequence
    int num_prompt_tokens = 0;
    int *prompt_tokens = (int *)malloc((strlen(prompt) + 3) * sizeof(int)); // +3 for '\0', ?BOS, ?EOS
    encode(tokenizer, prompt, 1, 0, prompt_tokens, &num_prompt_tokens);
    if (num_prompt_tokens < 1)
    {
        ESP_LOGE(TAG, "something is wrong, expected at least 1 prompt token");
        exit(EXIT_FAILURE);
    }

    // start the main loop
    long start = 0;               // used to time our code, only initialized after first iteration
    int next;                     // will store the next token in the sequence
    int token = prompt_tokens[0]; // kick off with the first token in the prompt
    int pos = 0;                  // position in the sequence
    // ESP32 port addition — this loop (and everything it calls: forward(),
    // matmul(), the offloaded matmul_task/forward_task work on Core 1) never
    // calls vTaskDelay()/yields anywhere, upstream's own design (it was
    // written for a desktop, where this doesn't matter). On ESP-IDF, that
    // means Core 1's own idle task never gets scheduled for the ENTIRE
    // generation run, tripping the 5-second task watchdog
    // (CONFIG_ESP_TASK_WDT_TIMEOUT_S) — confirmed for real on hardware, see
    // doc 128. A brief, deliberate pause once every ~500ms (10x safety
    // margin under that 5s timeout) is enough to let it run.
    TickType_t last_yield = xTaskGetTickCount();
    while (pos < steps)
    {
        // forward the transformer to get logits for the next token
        v4sf *logits = forward(transformer, token, pos);

        // TEMPORARY diagnostic (doc 131/133) — raw logits BEFORE sample()
        // touches them (sample() divides by temperature + applies softmax
        // in place when temperature != 0 — logged here to see the true,
        // untouched forward-pass output). Also finds the actual raw max
        // across the whole array, independent of sample_argmax(), as a
        // cross-check.
        {
            int raw_max_i = 0;
            v4sf raw_max_v = logits[0];
            for (int qi = 1; qi < sampler->vocab_size; qi++) {
                if (logits[qi] > raw_max_v) { raw_max_v = logits[qi]; raw_max_i = qi; }
            }
            ESP_LOGI(TAG, "[logits] pos=%d [0]=%.4f [255]=%.4f [256]=%.4f [511]=%.4f raw_max_i=%d raw_max_v=%.4f",
                     pos, logits[0], logits[255], logits[256], logits[511], raw_max_i, raw_max_v);
        }

        // advance the state machine
        if (pos < num_prompt_tokens - 1)
        {
            // if we are still processing the input prompt, force the next prompt token
            next = prompt_tokens[pos + 1];
        }
        else
        {
            // otherwise sample the next token from the logits
            next = sample(sampler, logits);
        }
        pos++;

        // data-dependent terminating condition: the BOS (=1) token delimits sequences
        if (next == 1)
        {
            break;
        }

        // print the token as string, decode it with the Tokenizer object
        char *piece = decode(tokenizer, token, next);
        // TEMPORARY diagnostic — remove once the blank-output issue is
        // understood. Prints the raw predicted token id plus the decoded
        // piece's length and first byte (hex), so we can see whether the
        // model is predicting the same token repeatedly, or different
        // tokens that all happen to decode to something blank-looking.
        ESP_LOGI(TAG, "[dbg] pos=%d next_token_id=%d piece_len=%d piece_byte0=0x%02X",
                 pos, next, (int)strlen(piece), (unsigned)(unsigned char)piece[0]);
        safe_printf(piece); // same as printf("%s", piece), but skips "unsafe" bytes
        fflush(stdout);
        token = next;

        // init the timer here because the first iteration can be slower
        if (start == 0)
        {
            start = time_in_ms();
        }

        // ESP32 port addition — see the comment above this loop's start.
        TickType_t now = xTaskGetTickCount();
        if ((now - last_yield) >= pdMS_TO_TICKS(500))
        {
            last_yield = now;
            vTaskDelay(1);
        }
    }
    printf("\n");

    // report achieved tok/s (pos-1 because the timer starts after first iteration)
    if (pos > 1)
    {
        long end = time_in_ms();
        float tks = (pos - 1) / (double)(end - start) * 1000;
        fprintf(stderr, "achieved tok/s: %f\n", tks);
        cb_done(tks);
    }

    free(prompt_tokens);
    ESP_LOGI(TAG, "Generate complete");
}

void read_stdin(const char *guide, char *buffer, size_t bufsize)
{
    // read a line from stdin, up to but not including \n
    printf("%s", guide);
    if (fgets(buffer, bufsize, stdin) != NULL)
    {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n')
        {
            buffer[len - 1] = '\0'; // strip newline
        }
    }
}
