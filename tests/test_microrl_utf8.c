#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "microrl.h"

static char output[8192];
static size_t output_len;
static int execute_count;
static int executed_argc;
static char executed_argv[8][MICRORL_CFG_CMDLINE_LEN + 1];

static int capture_output(microrl_t* mrl, const char* text) {
    (void)mrl;
    const size_t len = strlen(text);
    assert(output_len + len < sizeof(output));
    memcpy(&output[output_len], text, len);
    output_len += len;
    output[output_len] = '\0';
    return (int)len;
}

static int capture_execute(microrl_t* mrl, int argc,
                           const char* const* argv) {
    (void)mrl;
    execute_count++;
    executed_argc = argc;
    assert(argc <= 8);
    for (int i = 0; i < argc; ++i) {
        assert(strlen(argv[i]) <= MICRORL_CFG_CMDLINE_LEN);
        strcpy(executed_argv[i], argv[i]);
    }
    return 0;
}

static void reset_capture(void) {
    output_len = 0;
    output[0] = '\0';
    execute_count = 0;
    executed_argc = 0;
    memset(executed_argv, 0, sizeof(executed_argv));
}

static void feed(microrl_t* mrl, const char* text) {
    assert(microrl_processing_input(mrl, text, strlen(text)) == microrlOK);
}

static void feed_byte(microrl_t* mrl, unsigned char byte) {
    assert(microrl_processing_input(mrl, &byte, 1) == microrlOK);
}

static void test_utf8_command_execution(void) {
    microrl_t mrl;
    char prompt[] = "> ";
    assert(microrl_init(&mrl, capture_output, capture_execute) == microrlOK);
    assert(microrl_set_prompt(&mrl, prompt) == microrlOK);

    feed(&mrl, "echo 日本語\r");
    assert(execute_count == 1);
    assert(executed_argc == 2);
    assert(strcmp(executed_argv[0], "echo") == 0);
    assert(strcmp(executed_argv[1], "日本語") == 0);
}

static void test_cursor_and_delete_use_codepoints(void) {
    microrl_t mrl;
    char prompt[] = "> ";
    reset_capture();
    assert(microrl_init(&mrl, capture_output, capture_execute) == microrlOK);
    assert(microrl_set_prompt(&mrl, prompt) == microrlOK);

    feed(&mrl, "A日本語B");
    assert(strcmp(mrl.cmdline_str, "A日本語B") == 0);

    feed(&mrl, "\033[D");
    assert(mrl.cursor == strlen("A日本語"));
    feed(&mrl, "\033[D");
    assert(mrl.cursor == strlen("A日本"));
    feed(&mrl, "\033[3~");
    assert(strcmp(mrl.cmdline_str, "A日本B") == 0);
    assert(mrl.cursor == strlen("A日本"));

    feed(&mrl, "\b");
    assert(strcmp(mrl.cmdline_str, "A日B") == 0);
    assert(mrl.cursor == strlen("A日"));

    assert(strstr(output, "\033[2D") != NULL);
}

static void test_split_utf8_input(void) {
    microrl_t mrl;
    char prompt[] = "> ";
    reset_capture();
    assert(microrl_init(&mrl, capture_output, capture_execute) == microrlOK);
    assert(microrl_set_prompt(&mrl, prompt) == microrlOK);

    feed_byte(&mrl, 0xE6);
    assert(mrl.cmdlen == 0);
    feed_byte(&mrl, 0x97);
    assert(mrl.cmdlen == 0);
    feed_byte(&mrl, 0xA5);
    assert(strcmp(mrl.cmdline_str, "日") == 0);
}

static void test_invalid_utf8_is_ignored(void) {
    microrl_t mrl;
    char prompt[] = "> ";
    reset_capture();
    assert(microrl_init(&mrl, capture_output, capture_execute) == microrlOK);
    assert(microrl_set_prompt(&mrl, prompt) == microrlOK);

    feed_byte(&mrl, 0xE3);
    feed_byte(&mrl, 0x81);
    feed_byte(&mrl, 'x');
    assert(strcmp(mrl.cmdline_str, "x") == 0);
}

static void test_command_line_limit_is_bytes(void) {
    microrl_t mrl;
    char prompt[] = "> ";
    const char japanese[] = "あ";
    reset_capture();
    assert(microrl_init(&mrl, capture_output, capture_execute) == microrlOK);
    assert(microrl_set_prompt(&mrl, prompt) == microrlOK);

    for (int i = 0; i < 21; ++i) {
        feed(&mrl, japanese);
    }
    assert(mrl.cmdlen == 63);

    unsigned char extra = 'x';
    assert(microrl_processing_input(&mrl, &extra, 1) == microrlOK);
    assert(mrl.cmdlen == 64);

    unsigned char overflow = 'y';
    assert(microrl_processing_input(&mrl, &overflow, 1) == microrlERRCLFULL);
    assert(mrl.cmdlen == 64);
}

int main(void) {
    test_utf8_command_execution();
    test_cursor_and_delete_use_codepoints();
    test_split_utf8_input();
    test_invalid_utf8_is_ignored();
    test_command_line_limit_is_bytes();
    puts("microrl UTF-8 tests passed");
    return 0;
}
