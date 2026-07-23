#include "config.h"

#if ENABLE_MINI_COMMAND

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "esp_log.h"
#include "mini_command.h"

static const char *TAG = "game";

static bool s_active = false;
static int  s_target = 0;
static int  s_tries   = 0;

static void _start(void) {
    s_target = (int)(esp_random() % 10) + 1; // 1..10
    s_tries  = 0;
    s_active = true;
    printf("\nI'm thinking of a number 1-10. Type 'guess <n>' to play.\n> ");
}

static void _guess(int n) {
    if (!s_active) {
        printf("No game running — type 'game' to start one.\n> ");
        return;
    }
    s_tries++;
    if (n == s_target) {
        printf("Correct! It was %d — you got it in %d tries.\n> ", s_target, s_tries);
        s_active = false;
    } else if (n < s_target) {
        printf("Higher! (try %d)\n> ", s_tries);
    } else {
        printf("Lower! (try %d)\n> ", s_tries);
    }
}

bool mini_command_process(const char *line) {
    while (*line == ' ') line++;

    if (strcmp(line, "game") == 0) {
        ESP_LOGI(TAG, "New round started");
        _start();
        return true;
    }
    if (strncmp(line, "guess", 5) == 0) {
        const char *p = line + 5;
        while (*p == ' ') p++;
        int n = atoi(p);
        _guess(n);
        return true;
    }
    return false;
}

#else // !ENABLE_MINI_COMMAND

#include "mini_command.h"
bool mini_command_process(const char *line) { (void)line; return false; }

#endif // ENABLE_MINI_COMMAND
