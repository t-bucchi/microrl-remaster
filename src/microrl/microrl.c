/**
 * \file            microrl.c
 * \brief           Micro Read Line library
 */

/*
 * Portion Copyright (c) 2011 Eugene SAMOYLOV
 * Portion Copyright (c) 2021 Dmitry KARASEV
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is part of MicroRL - Micro Read Line library for small and embedded devices.
 *
 * Authors:         Eugene SAMOYLOV aka Helius <ghelius@gmail.com>,
 *                  Dmitry KARASEV <karasevsdmitry@yandex.ru>
 * Version:         2.6.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "microrl.h"
#if MICRORL_CFG_USE_LIBC_STDIO
#include <stdio.h>
#endif /* MICRORL_CFG_USE_LIBC_STDIO */

#define IS_CONTROL_CHAR(x)                  \
    ((unsigned char)(x) <= MICRORL_ESC_ANSI_US || \
     (unsigned char)(x) == MICRORL_ESC_ANSI_DEL)

static int utf8_is_continuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

static size_t utf8_sequence_length(unsigned char first) {
    if (first <= 0x7F) {
        return 1;
    }
    if (first >= 0xC2 && first <= 0xDF) {
        return 2;
    }
    if (first >= 0xE0 && first <= 0xEF) {
        return 3;
    }
    if (first >= 0xF0 && first <= 0xF4) {
        return 4;
    }
    return 0;
}

static int utf8_sequence_is_valid(const unsigned char* bytes, size_t len) {
    if (len == 0 || utf8_sequence_length(bytes[0]) != len) {
        return 0;
    }

    for (size_t i = 1; i < len; ++i) {
        if (!utf8_is_continuation(bytes[i])) {
            return 0;
        }
    }

    if (len == 3) {
        if (bytes[0] == 0xE0 && bytes[1] < 0xA0) {
            return 0;
        }
        if (bytes[0] == 0xED && bytes[1] >= 0xA0) {
            return 0;
        }
    } else if (len == 4) {
        if (bytes[0] == 0xF0 && bytes[1] < 0x90) {
            return 0;
        }
        if (bytes[0] == 0xF4 && bytes[1] >= 0x90) {
            return 0;
        }
    }

    return 1;
}

static uint32_t utf8_decode(const unsigned char* bytes, size_t len) {
    if (len == 1) {
        return bytes[0];
    }

    uint32_t codepoint = bytes[0] & ((1U << (8 - len - 1)) - 1U);
    for (size_t i = 1; i < len; ++i) {
        codepoint = (codepoint << 6) | (bytes[i] & 0x3FU);
    }
    return codepoint;
}

static size_t utf8_next_char_length(const char* text, size_t pos, size_t len) {
    if (pos >= len) {
        return 0;
    }

    const unsigned char first = (unsigned char)text[pos];
    const size_t sequence_len = utf8_sequence_length(first);
    if (sequence_len == 0 || pos + sequence_len > len ||
        !utf8_sequence_is_valid((const unsigned char*)&text[pos], sequence_len)) {
        return 1;
    }
    return sequence_len;
}

static size_t utf8_previous_char_length(const char* text, size_t pos) {
    if (pos == 0) {
        return 0;
    }

    size_t start = pos - 1;
    while (start > 0 && utf8_is_continuation((unsigned char)text[start])) {
        --start;
    }

    const size_t sequence_len = pos - start;
    if (utf8_sequence_is_valid((const unsigned char*)&text[start], sequence_len)) {
        return sequence_len;
    }
    return 1;
}

static int utf8_codepoint_width(uint32_t codepoint) {
    if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0)) {
        return 0;
    }

    if ((codepoint >= 0x1100 && codepoint <= 0x115F) ||
        (codepoint >= 0x2329 && codepoint <= 0x232A) ||
        (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
        (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||
        (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
        (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||
        (codepoint >= 0xFE30 && codepoint <= 0xFE6F) ||
        (codepoint >= 0xFF01 && codepoint <= 0xFF60) ||
        (codepoint >= 0xFFE0 && codepoint <= 0xFFE6) ||
        (codepoint >= 0x1F300 && codepoint <= 0x1FAFF)) {
        return 2;
    }

    return 1;
}

static size_t utf8_display_width(const char* text, size_t start, size_t end) {
    size_t width = 0;
    size_t pos = start;
    while (pos < end) {
        const size_t char_len = utf8_next_char_length(text, pos, end);
        const size_t safe_len = char_len == 0 ? 1 : char_len;
        width += (size_t)utf8_codepoint_width(
            utf8_decode((const unsigned char*)&text[pos], safe_len));
        pos += safe_len;
    }
    return width;
}

static void utf8_reset_pending(microrl_t* mrl) {
    mrl->utf8_pending_len = 0;
    mrl->utf8_pending_expected = 0;
}

/**
 * \brief           List of ANSI escape codes
 */
typedef enum {
    MICRORL_ESC_ANSI_NUL = 0x00,                    /*!< ^@ Null character */
    MICRORL_ESC_ANSI_SOH = 0x01,                    /*!< ^A Start of heading, = console interrupt */
    MICRORL_ESC_ANSI_STX = 0x02,                    /*!< ^B Start of text, maintenance mode on HP console */
    MICRORL_ESC_ANSI_ETX = 0x03,                    /*!< ^C End of text */
    MICRORL_ESC_ANSI_EOT = 0x04,                    /*!< ^D End of transmission, not the same as ETB */
    MICRORL_ESC_ANSI_ENQ = 0x05,                    /*!< ^E Enquiry, goes with ACK; old HP flow control */
    MICRORL_ESC_ANSI_ACK = 0x06,                    /*!< ^F Acknowledge, clears ENQ logon hand */
    MICRORL_ESC_ANSI_BEL = 0x07,                    /*!< ^G Bell, rings the bell... */
    MICRORL_ESC_ANSI_BS = 0x08,                     /*!< ^H Backspace, works on HP terminals/computers */
    MICRORL_ESC_ANSI_HT = 0x09,                     /*!< ^I Horizontal tab, move to next tab stop */
    MICRORL_ESC_ANSI_LF = 0x0A,                     /*!< ^J Line Feed */
    MICRORL_ESC_ANSI_VT = 0x0B,                     /*!< ^K Vertical tab */
    MICRORL_ESC_ANSI_FF = 0x0C,                     /*!< ^L Form Feed, page eject */
    MICRORL_ESC_ANSI_CR = 0x0D,                     /*!< ^M Carriage Return */
    MICRORL_ESC_ANSI_SO = 0x0E,                     /*!< ^N Shift Out, alternate character set */
    MICRORL_ESC_ANSI_SI = 0x0F,                     /*!< ^O Shift In, resume defaultn character set */
    MICRORL_ESC_ANSI_DLE = 0x10,                    /*!< ^P Data link escape */
    MICRORL_ESC_ANSI_DC1 = 0x11,                    /*!< ^Q XON, with XOFF to pause listings; "okay to send". */
    MICRORL_ESC_ANSI_DC2 = 0x12,                    /*!< ^R Device control 2, block-mode flow control */
    MICRORL_ESC_ANSI_DC3 = 0x13,                    /*!< ^S XOFF, with XON is TERM=18 flow control */
    MICRORL_ESC_ANSI_DC4 = 0x14,                    /*!< ^T Device control 4 */
    MICRORL_ESC_ANSI_NAK = 0x15,                    /*!< ^U Negative acknowledge */
    MICRORL_ESC_ANSI_SYN = 0x16,                    /*!< ^V Synchronous idle */
    MICRORL_ESC_ANSI_ETB = 0x17,                    /*!< ^W End transmission block, not the same as EOT */
    MICRORL_ESC_ANSI_CAN = 0x18,                    /*!< ^X Cancel line, MPE echoes !!! */
    MICRORL_ESC_ANSI_EM = 0x19,                     /*!< ^Y End of medium, Control-Y interrupt */
    MICRORL_ESC_ANSI_SUB = 0x1A,                    /*!< ^Z Substitute */
    MICRORL_ESC_ANSI_ESC = 0x1B,                    /*!< ^[ Escape, next character is not echoed */
    MICRORL_ESC_ANSI_FS = 0x1C,                     /*!< ^\ File separator */
    MICRORL_ESC_ANSI_GS = 0x1D,                     /*!< ^] Group separator */
    MICRORL_ESC_ANSI_RS = 0x1E,                     /*!< ^^ Record separator, block-mode terminator */
    MICRORL_ESC_ANSI_US = 0x1F,                     /*!< ^_ Unit separator */

    MICRORL_ESC_ANSI_DEL = 0x7F                     /*!< Delete (not a real control character...) */
} microrl_esc_ansi_t;

/**
 * \brief           History ring buffer memory status
 */
typedef enum {
    MICRORL_HIST_FULL = 0,                          /*!< History ring buffer is full */
    MICRORL_HIST_NOT_FULL                           /*!< History ring buffer is not full or empty */
} microrl_hist_status_t;

/**
 * \brief           Direction of history navigation
 */
typedef enum {
    MICRORL_HIST_DIR_NONE = 0,                      /*!< Current record in history ring buffer */
    MICRORL_HIST_DIR_UP,                            /*!< Previous record in history ring buffer */
    MICRORL_HIST_DIR_DOWN                           /*!< Next record in history ring buffer */
} microrl_hist_dir_t;

/**
 * \brief           Split command line to tokens array
 * \param[in]       mrl: \ref microrl_t working instance
 * \param[out]      tkn_str_arr: Tokens buffer stored split words
 * \param[out]      tkn_cnt_ptr: Number of split tokens
 * \param[in]       limit: Number of command line characters to split
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
static microrlr_t prv_cmdline_buf_split(microrl_t* mrl, const char** tkn_str_arr, uint8_t* tkn_cnt_ptr, size_t limit) {
    uint8_t num = 0;
    size_t src_pos = 0;
    size_t dst_pos = 0;
    char quote = '\0';
    char *src = mrl->cmdline_str;
    char *dst = mrl->token_buf;

    while (src_pos < limit && src[src_pos] != '\0') {
#if MICRORL_CFG_USE_QUOTING
        while (src_pos < limit && src[src_pos] == ' ') {
            ++src_pos;
        }
        if (src_pos >= limit || src[src_pos] == '\0') {
            break;
        }
#else
        if (src[src_pos] == ' ') {
            ++src_pos;
            continue;
        }
#endif

        if (num >= MICRORL_CFG_CMD_TOKEN_NMB) {
            return microrlERRTKNNUM;
        }
        tkn_str_arr[num++] = &dst[dst_pos];
        uint8_t in_token = 0;

        while (src_pos < limit && src[src_pos] != '\0') {
            char ch = src[src_pos];
#if MICRORL_CFG_USE_QUOTING
            if (quote != '\0') {
                in_token = 1;
                if (ch == '\\' && src_pos + 1 < limit &&
                    (src[src_pos + 1] == quote || src[src_pos + 1] == '\\')) {
                    dst[dst_pos++] = src[src_pos + 1];
                    src_pos += 2;
                } else if (ch == quote) {
                    quote = '\0';
                    ++src_pos;
                } else {
                    dst[dst_pos++] = ch;
                    ++src_pos;
                }
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
                in_token = 1;
                ++src_pos;
            } else if (ch == ' ') {
                break;
            } else if (ch == '\\' && src_pos + 1 < limit &&
                       (src[src_pos + 1] == '"' || src[src_pos + 1] == '\'' ||
                        src[src_pos + 1] == '\\')) {
                dst[dst_pos++] = src[src_pos + 1];
                src_pos += 2;
            } else {
                dst[dst_pos++] = ch;
                in_token = 1;
                ++src_pos;
            }
#else
            if (ch == ' ') {
                break;
            }
            dst[dst_pos++] = ch;
            in_token = 1;
            ++src_pos;
#endif /* MICRORL_CFG_USE_QUOTING */
        }

#if MICRORL_CFG_USE_QUOTING
        if (quote != '\0') {
            *tkn_cnt_ptr = 0;
            return microrlERRQUOTE;
        }
#endif /* MICRORL_CFG_USE_QUOTING */

        if (in_token) {
            dst[dst_pos++] = '\0';
        } else {
            --num;
        }

        while (src_pos < limit && src[src_pos] == ' ') {
            ++src_pos;
        }
    }

    dst[dst_pos] = '\0';

    *tkn_cnt_ptr = num;
    return microrlOK;
}

/**
 * \brief           Insert the passed text at the cursor position
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       text_str: Text to store on the command line
 * \param[in]       len: Length of text to store
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
static microrlr_t prv_cmdline_buf_insert_text(microrl_t* mrl, const char* text_str, size_t len) {
    if ((mrl->cmdlen + len) > (MICRORL_ARRAYSIZE(mrl->cmdline_str) - 1)) {
        return microrlERRCLFULL;
    }

#if MICRORL_CFG_USE_ECHO_OFF
    if ((mrl->echo != MICRORL_ECHO_ON) && (mrl->echo_off_pos == -1)) {
        mrl->echo_off_pos = mrl->cmdlen;
    }
#endif /* MICRORL_CFG_USE_ECHO_OFF */
    memmove(mrl->cmdline_str + mrl->cursor + len,
            mrl->cmdline_str + mrl->cursor,
            mrl->cmdlen - mrl->cursor);
    for (size_t i = 0; i < len; ++i) {
        mrl->cmdline_str[mrl->cursor + i] = text_str[i];
    }
    mrl->cursor += len;
    mrl->cmdlen += len;
    memset(&mrl->cmdline_str[mrl->cmdlen], 0x00, MICRORL_ARRAYSIZE(mrl->cmdline_str) - 1 - mrl->cmdlen);

    return microrlOK;
}

/**
 * \brief           Remove len characters backwards at cursor
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       len: Number of chars to remove
 */
static void prv_cmdline_buf_backspace(microrl_t* mrl, size_t len) {
    if (mrl->cursor < len) {
        return;
    }

    memmove(mrl->cmdline_str + mrl->cursor - len,
            mrl->cmdline_str + mrl->cursor,
            mrl->cmdlen - mrl->cursor + len);
    mrl->cursor -= len;
    mrl->cmdline_str[mrl->cmdlen] = '\0';
    mrl->cmdlen -= len;
}

/**
 * \brief           Remove one characters forward at cursor
 * \param[in,out]   mrl: \ref microrl_t working instance
 */
static void prv_cmdline_buf_delete(microrl_t* mrl, size_t len) {
    if ((mrl->cmdlen == 0) || (mrl->cursor == mrl->cmdlen) || len == 0) {
        return;
    }

    if (mrl->cursor + len > mrl->cmdlen) {
        len = mrl->cmdlen - mrl->cursor;
    }
    memmove(mrl->cmdline_str + mrl->cursor,
            mrl->cmdline_str + mrl->cursor + len,
            mrl->cmdlen - mrl->cursor - len + 1);
    mrl->cmdlen -= len;
}

/**
 * \brief           Reset command line buffer and its position pointers
 * \param[out]      mrl: \ref microrl_t working instance
 */
MICRORL_CFG_STATIC_INLINE void prv_cmdline_buf_reset(microrl_t* mrl) {
    memset(mrl->cmdline_str, 0x00, sizeof(mrl->cmdline_str));
    mrl->cmdlen = 0;
    mrl->cursor = 0;
    utf8_reset_pending(mrl);
}

/**
 * \brief           Print prompt string in terminal
 * \param[in]       mrl: \ref microrl_t working instance
 */
MICRORL_CFG_STATIC_INLINE void prv_terminal_print_prompt(microrl_t* mrl) {
#if MICRORL_CFG_USE_PROMPT_COLOR
    mrl->out_fn(mrl, MICRORL_CFG_PROMPT_COLOR);
    mrl->out_fn(mrl, mrl->prompt_ptr);
    mrl->out_fn(mrl, MICRORL_COLOR_DEFAULT);
#else
    mrl->out_fn(mrl, mrl->prompt_ptr);
#endif
}

/**
 * \brief           Clear the last character in the terminal command line
 *                      and move the cursor to its position
 * \param[in]       mrl: \ref microrl_t working instance
 */
static char* prv_cursor_generate_move(char* str, int32_t offset);

MICRORL_CFG_STATIC_INLINE void prv_terminal_backspace(microrl_t* mrl, size_t width) {
    if (width == 0) {
        return;
    }

    char move[16];
    prv_cursor_generate_move(move, -(int32_t)width);
    mrl->out_fn(mrl, move);

    for (size_t i = 0; i < width; ++i) {
        mrl->out_fn(mrl, " ");
    }

    prv_cursor_generate_move(move, -(int32_t)width);
    mrl->out_fn(mrl, move);
}

/**
 * \brief           Print end line symbol defined in \ref MICRORL_CFG_END_LINE config
 * \param[in]       mrl: \ref microrl_t working instance
 */
MICRORL_CFG_STATIC_INLINE void prv_terminal_newline(microrl_t* mrl) {
    mrl->out_fn(mrl, MICRORL_CFG_END_LINE);
}

/**
 * \brief           Insert ESC sequence into the passed string to set the cursor
 *                      at the current position + offset (positive or negative)
 *                      in in the terminal command line.
 *                      The passed string must be at least 7 bytes long
 * \param[in]       str: The original string before moving the cursor
 * \param[in]       offset: Positive or negative interval to move cursor
 * \return          The original string after moving the cursor
 */
static char* prv_cursor_generate_move(char* str, int32_t offset) {
    char c = 'C';

    if (offset > 999) {
        offset = 999;
    }
    if (offset < -999) {
        offset = -999;
    }
    if (offset < 0) {
        offset = -offset;
        c = 'D';
    } else if (offset == 0) {
        *str = '\0';
        return str;
    }

#if MICRORL_CFG_USE_LIBC_STDIO
    str += sprintf(str, "\033[%d%c", offset, c);
#else
    *str++ = '\033';
    *str++ = '[';

    char tmp_str[4] = {0};
    size_t i = 0;

    while (offset > 0) {
        tmp_str[i++] = (offset % 10) + '0';
        offset /= 10;
    }

    for (size_t j = 0; j < i; ++j) {            /* Write reversed numerals to result */
        *str++ = tmp_str[i - j - 1];
    }

    *str++ = c;
    *str = '\0';
#endif /* MICRORL_CFG_USE_LIBC_STDIO */

    return str;
}

/**
 * \brief           Set cursor at current position + offset (positive or negative)
 *                      in terminal command line
 * \param[in]       mrl: \ref microrl_t working instance
 * \param[in]       offset: Positive or negative interval to move cursor
 */
static void prv_terminal_move_cursor(microrl_t* mrl, int32_t offset) {
    if (offset == 0) {
        return;
    }

    char str[16] = {0};
    prv_cursor_generate_move(str, offset);
    mrl->out_fn(mrl, str);
}

/**
 * \brief           Print command line buffer to specified position
 * \param[in]       mrl: \ref microrl_t working instance
 * \param[in]       pos: Start position from which the line will be printed
 * \param[in]       reset: Flag to reset the cursor position
 */
static void prv_terminal_print_line(microrl_t* mrl, int32_t pos, uint8_t reset) {
    if (pos < 0) {
        pos = 0;
    }
    if ((size_t)pos > mrl->cmdlen) {
        pos = (int32_t)mrl->cmdlen;
    }

    if (reset) {
        char str[16];
#if MICRORL_CFG_USE_CARRIAGE_RETURN
        mrl->out_fn(mrl, "\r");
        prv_cursor_generate_move(str, (int32_t)(mrl->prompt_size +
            utf8_display_width(mrl->cmdline_str, 0, (size_t)pos)));
#else
        prv_cursor_generate_move(str, -(int32_t)(MICRORL_ARRAYSIZE(mrl->cmdline_str) -
            1 + mrl->prompt_size + 2));
        mrl->out_fn(mrl, str);
        prv_cursor_generate_move(str, (int32_t)(mrl->prompt_size +
            utf8_display_width(mrl->cmdline_str, 0, (size_t)pos)));
#endif /* MICRORL_CFG_USE_CARRIAGE_RETURN */
        mrl->out_fn(mrl, str);
    }

    size_t i = (size_t)pos;
    while (i < mrl->cmdlen) {
        const size_t char_len = utf8_next_char_length(mrl->cmdline_str, i,
                                                       mrl->cmdlen);
        char text[5] = {0};

#if MICRORL_CFG_USE_ECHO_OFF
        if (((int32_t)i >= mrl->echo_off_pos) && (mrl->echo != MICRORL_ECHO_ON)) {
            text[0] = MICRORL_CFG_ECHO_OFF_MASK;
        } else
#endif /* MICRORL_CFG_USE_ECHO_OFF */
        {
            memcpy(text, &mrl->cmdline_str[i], char_len);
            text[char_len] = '\0';
        }
        mrl->out_fn(mrl, text);
        i += char_len;
    }

    mrl->out_fn(mrl, "\033[K");                /* Delete all past end of text */
    prv_terminal_move_cursor(mrl, -(int32_t)utf8_display_width(
        mrl->cmdline_str, mrl->cursor, mrl->cmdlen));
}

#if MICRORL_CFG_USE_HISTORY || __DOXYGEN__

/**
 * \brief           Set the next record start position to the passed index
 * \param[in]       rbuf_ptr: Pointer to \ref microrl_hist_rbuf_t structure
 * \param[in,out]   idx_ptr: Pointer to the current record
 */
MICRORL_CFG_STATIC_INLINE void prv_hist_next_record(microrl_hist_rbuf_t* rbuf_ptr, size_t* idx_ptr) {
    while (rbuf_ptr->ring_buf[++(*idx_ptr)] != '\0') {
        if (*idx_ptr >= MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf)) {
            *idx_ptr -= MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf);
            if (rbuf_ptr->ring_buf[*idx_ptr] == '\0') {
                break;
            }
        }
    }
}

/**
 * \brief           Remove older record from ring buffer
 * \param[in,out]   rbuf_ptr: Pointer to \ref microrl_hist_rbuf_t structure
 */
static void prv_hist_erase_older(microrl_hist_rbuf_t* rbuf_ptr) {
    size_t new_pos = rbuf_ptr->head;
    prv_hist_next_record(rbuf_ptr, &new_pos);
    rbuf_ptr->head = new_pos;
}

/**
 * \brief           Check space in history buffer for new record
 * \param[in]       rbuf_ptr: Pointer to \ref microrl_hist_rbuf_t structure
 * \param[in]       len: Length of new record to save in history
 * \return          Member of \ref microrl_hist_status_t enumeration
 */
static microrl_hist_status_t prv_hist_is_space_for_new(microrl_hist_rbuf_t* rbuf_ptr, size_t len) {
    if (rbuf_ptr->tail >= rbuf_ptr->head) {
        if ((MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf) - rbuf_ptr->tail + rbuf_ptr->head - 1) > len) {
            return MICRORL_HIST_NOT_FULL;
        }
    } else {
        if ((rbuf_ptr->head - rbuf_ptr->tail - 1) > len) {
            return MICRORL_HIST_NOT_FULL;
        }
    }

    return MICRORL_HIST_FULL;
}

/**
 * \brief           Copy saved record to 'line_str' and return size of record
 * \param[in]       rbuf_ptr: Pointer to \ref microrl_hist_rbuf_t structure
 * \param[out]      line_str: Line to restore from history
 * \param[in]       dir: Record search direction, member of \ref microrl_hist_dir_t
 * \return          Size of restored line. `0` is returned, if history is empty
 */
static size_t prv_hist_restore_line(microrl_hist_rbuf_t* rbuf_ptr, char* line_str, microrl_hist_dir_t dir) {
    size_t cnt = 0;
    size_t i = rbuf_ptr->head;
    while (i != rbuf_ptr->tail) {               /* Count history records */
        prv_hist_next_record(rbuf_ptr, &i);
        ++cnt;
    }

    switch (dir) {
        case MICRORL_HIST_DIR_UP: {             /* Set navigation counter depending on the direction */
            if (cnt < rbuf_ptr->count) {
                return 0;                       /* Impossible state, return empty line */
            }
            if (cnt != rbuf_ptr->count) {
                ++rbuf_ptr->count;
            }
            break;
        }
        case MICRORL_HIST_DIR_DOWN: {
            if (rbuf_ptr->count == 0) {
                return 0;                       /* Empty line */
            }
            if (--rbuf_ptr->count == 0) {
                return 0;                       /* Empty line */
            }
            break;
        }
        default:
            break;
    }

    size_t idx = rbuf_ptr->head;
    size_t j = 0;
    while ((cnt - j++) != rbuf_ptr->count) {    /* Find record for 'rbuf_ptr->count' counter */
        prv_hist_next_record(rbuf_ptr, &idx);
    }

    ++idx;                                      /* Move position from `\0` marker */

    size_t rec_len = 0;
    size_t k = idx;
    while (rbuf_ptr->ring_buf[k++] != '\0') {   /* Calculating the length of the found record */
        if (k >= MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf)) {
            k -= MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf);
        }
        ++rec_len;
    }

    memset(line_str, 0x00, MICRORL_CFG_CMDLINE_LEN);    /* Placing the found record on the command line */
    if ((idx + rec_len) < MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf)) {
        memcpy(line_str, rbuf_ptr->ring_buf + idx, rec_len);
    } else {
        size_t part0 = MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf) - idx;
        memcpy(line_str, rbuf_ptr->ring_buf + idx, part0);
        memcpy(line_str + part0, rbuf_ptr->ring_buf, rec_len - part0);
    }

    return rec_len;
}

/**
 * \brief           Restore record to command line from history buffer
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       dir: Member of \ref microrl_hist_dir_t enumeration
 */
static void prv_hist_search(microrl_t* mrl, microrl_hist_dir_t dir) {
#if MICRORL_CFG_USE_ECHO_OFF
    if (mrl->echo != MICRORL_ECHO_ON) {
        return;
    }
#endif /* MICRORL_CFG_USE_ECHO_OFF */

    size_t len = prv_hist_restore_line(&mrl->ring_hist, mrl->cmdline_str, dir);
    memset(&mrl->cmdline_str[len], 0x00, MICRORL_ARRAYSIZE(mrl->cmdline_str) - 1 - len);
    mrl->cursor = mrl->cmdlen = len;
    prv_terminal_print_line(mrl, 0, 1);
}

/**
 * \brief           Put record to ring buffer
 * \param[in,out]   rbuf_ptr: Pointer to \ref microrl_hist_rbuf_t structure
 * \param[in]       line_str: Record to save in history
 * \param[in]       len: Record length
 */
static void prv_hist_save_line(microrl_hist_rbuf_t* rbuf_ptr, char* line_str, size_t len) {
    if (len > (MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf) - 1)) {
        return;
    }

    /* Don't save the same line as the last record */
    char last_record_str[MICRORL_CFG_CMDLINE_LEN + 1];
    prv_hist_restore_line(rbuf_ptr, last_record_str,
                              rbuf_ptr->count == 1 ? MICRORL_HIST_DIR_NONE : MICRORL_HIST_DIR_UP);
    if (strcmp(line_str, last_record_str) == 0) {
        rbuf_ptr->count = 0;
        return;
    }

    /* Freeing up space for saving in the ring buffer */
    while (prv_hist_is_space_for_new(rbuf_ptr, len) == MICRORL_HIST_FULL) {
        prv_hist_erase_older(rbuf_ptr);
    }

    if (len < (MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf) - rbuf_ptr->tail - 1)) {  /* Store record */
        memcpy(rbuf_ptr->ring_buf + rbuf_ptr->tail + 1, line_str, len);
    } else {
        size_t part_len = MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf) - rbuf_ptr->tail - 1;
        memcpy(rbuf_ptr->ring_buf + rbuf_ptr->tail + 1, line_str, part_len);
        memcpy(rbuf_ptr->ring_buf, line_str + part_len, len - part_len);
    }

    rbuf_ptr->tail = rbuf_ptr->tail + len + 1;  /* Update position pointer and navigation counter */
    if (rbuf_ptr->tail >= MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf)) {
        rbuf_ptr->tail -= MICRORL_ARRAYSIZE(rbuf_ptr->ring_buf);
    }
    rbuf_ptr->ring_buf[rbuf_ptr->tail] = 0;
    rbuf_ptr->count = 0;
}

#endif /* MICRORL_CFG_USE_HISTORY || __DOXYGEN__ */

#if MICRORL_CFG_USE_ESC_SEQ || __DOXYGEN__
/**
 * \brief           Handle ANSI escape code sequences
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       ch: Input character
 * \return          '1' if full escape sequence is processed, '0' otherwise
 */
static uint8_t prv_escape_process(microrl_t* mrl, char ch) {
    if (ch == '[') {
        mrl->esc_code = MICRORL_ESC_BRACKET;
        return 0;
    } else if (ch == MICRORL_ESC_ANSI_DEL) {    /* Alt+BACKSPACE */
        prv_cmdline_buf_delete(mrl, utf8_next_char_length(mrl->cmdline_str,
                                                           mrl->cursor,
                                                           mrl->cmdlen));
        prv_terminal_print_line(mrl, mrl->cursor, 0);
        return 1;
    } else if (mrl->esc_code == MICRORL_ESC_BRACKET) {
        if (ch == 'A') {                        /* UP */
#if MICRORL_CFG_USE_HISTORY
            prv_hist_search(mrl, MICRORL_HIST_DIR_UP);
#endif /* MICRORL_CFG_USE_HISTORY */
            return 1;
        } else if (ch == 'B') {                 /* DOWN */
#if MICRORL_CFG_USE_HISTORY
            prv_hist_search(mrl, MICRORL_HIST_DIR_DOWN);
#endif /* MICRORL_CFG_USE_HISTORY */
            return 1;
        } else if (ch == 'C') {                 /* RIGHT */
            if (mrl->cursor < mrl->cmdlen) {
                const size_t char_len = utf8_next_char_length(mrl->cmdline_str,
                                                               mrl->cursor,
                                                               mrl->cmdlen);
                prv_terminal_move_cursor(mrl, (int32_t)utf8_display_width(
                    mrl->cmdline_str, mrl->cursor, mrl->cursor + char_len));
                mrl->cursor += char_len;
            }
            return 1;
        } else if (ch == 'D') {                 /* LEFT */
            if (mrl->cursor > 0) {
                const size_t char_len = utf8_previous_char_length(mrl->cmdline_str,
                                                                   mrl->cursor);
                prv_terminal_move_cursor(mrl, -(int32_t)utf8_display_width(
                    mrl->cmdline_str, mrl->cursor - char_len, mrl->cursor));
                mrl->cursor -= char_len;
            }
            return 1;
        } else if (ch == '7' || ch == '1') {
            mrl->esc_code = MICRORL_ESC_HOME;
            return 0;
        } else if (ch == '8' || ch == '4') {
            mrl->esc_code = MICRORL_ESC_END;
            return 0;
        } else if (ch == '3') {
            mrl->esc_code = MICRORL_ESC_DEL;
            return 0;
        }
    } else if (ch == '~') {
        if (mrl->esc_code == MICRORL_ESC_HOME) {/* HOME */
            prv_terminal_move_cursor(mrl, -(int32_t)utf8_display_width(
                mrl->cmdline_str, 0, mrl->cursor));
            mrl->cursor = 0;
            return 1;
        } else if (mrl->esc_code == MICRORL_ESC_END) {  /* END */
            prv_terminal_move_cursor(mrl, (int32_t)utf8_display_width(
                mrl->cmdline_str, mrl->cursor, mrl->cmdlen));
            mrl->cursor = mrl->cmdlen;
            return 1;
        } else if (mrl->esc_code == MICRORL_ESC_DEL) {  /* DELETE */
            prv_cmdline_buf_delete(mrl, utf8_next_char_length(mrl->cmdline_str,
                                                               mrl->cursor,
                                                               mrl->cmdlen));
            prv_terminal_print_line(mrl, mrl->cursor, 0);
            return 1;
        }
    }

    return 1;                                   /* Unknown escape sequence, stop processing */
}
#endif /* MICRORL_CFG_USE_ESC_SEQ || __DOXYGEN__ */

/**
 * \brief           Processing input string from command line and calling execute_fn() callback
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
static microrlr_t prv_handle_newline(microrl_t* mrl) {
    const char* tkn_str_arr[MICRORL_CFG_CMD_TOKEN_NMB] = {0};
    uint8_t tkn_cnt = 0;
    microrlr_t status = microrlOK;

    prv_terminal_newline(mrl);

    if (mrl->cmdlen == 0) {
        goto exit;
    }

#if MICRORL_CFG_USE_HISTORY
#if MICRORL_CFG_USE_ECHO_OFF
    if (mrl->echo == MICRORL_ECHO_ON) {
#endif /* MICRORL_CFG_USE_ECHO_OFF */
        prv_hist_save_line(&mrl->ring_hist, mrl->cmdline_str, mrl->cmdlen);
#if MICRORL_CFG_USE_ECHO_OFF
    }
#endif /* MICRORL_CFG_USE_ECHO_OFF */
#endif /* MICRORL_CFG_USE_HISTORY */

#if MICRORL_CFG_USE_ECHO_OFF
    if (mrl->echo == MICRORL_ECHO_ONCE && mrl->cmdline_str[mrl->echo_off_pos] != '\0') {
        microrl_set_echo(mrl, MICRORL_ECHO_ON);
        mrl->echo_off_pos = -1;
    }
#endif /* MICRORL_CFG_USE_ECHO_OFF */

    status = prv_cmdline_buf_split(mrl, tkn_str_arr, &tkn_cnt, mrl->cmdlen);
    if (status == microrlOK) {
#if MICRORL_CFG_USE_COMMAND_HOOKS
        int exec_status = 0;

        MICRORL_PRE_COMMAND_HOOK(mrl, tkn_cnt, tkn_str_arr);

        exec_status = mrl->exec_fn(mrl, tkn_cnt, tkn_str_arr);

        MICRORL_POST_COMMAND_HOOK(mrl, exec_status, tkn_cnt, tkn_str_arr);
#else
        mrl->exec_fn(mrl, tkn_cnt, tkn_str_arr);
#endif /* MICRORL_CFG_USE_COMMAND_HOOKS */
    } else if (status == microrlERRTKNNUM) {
        mrl->out_fn(mrl, "ERROR: too many tokens");
        prv_terminal_newline(mrl);
    } else if (status == microrlERRQUOTE) {
        mrl->out_fn(mrl, "ERROR: unclosed quote");
        prv_terminal_newline(mrl);
    }

exit:
    prv_terminal_print_prompt(mrl);
    prv_cmdline_buf_reset(mrl);

    return status;
}

#if MICRORL_CFG_USE_COMPLETE || __DOXYGEN__

/**
 * \brief           Calculate total length of all completion tokens
 * \param[in]       arr: Completion tokens array
 * \return          Total length of all completion tokens
 */
static size_t prv_complete_total_len(const char* const * argv) {
    size_t i;
    const char* short_str = (const char*)argv[0];
    size_t shortlen = strlen(short_str);

    for (i = 0; argv[i] != NULL; ++i) {
        if (strlen(argv[i]) < shortlen) {
            short_str = argv[i];
            shortlen = strlen(short_str);
        }
    }

    for (i = 0; i < shortlen; ++i) {
        for (size_t j = 0; argv[j] != NULL; ++j) {
            if (short_str[i] != argv[j][i]) {
                return i;
            }
        }
    }

    return i;
}

/**
 * \brief           Auto-complete activities to complete input in
 *                      command line
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
static microrlr_t prv_complete_get_input(microrl_t* mrl) {
    /* Skip completion when echo is OFF */
#if MICRORL_CFG_USE_ECHO_OFF
    if (mrl->echo != MICRORL_ECHO_ON) {
        return microrlOK;
    }
#endif /* MICRORL_CFG_USE_ECHO_OFF */

    uint8_t tkn_cnt = 0;
    const char* tkn_str_arr[MICRORL_CFG_CMD_TOKEN_NMB] = {0};
    char** cmplt_tkn_arr;

    if (prv_cmdline_buf_split(mrl, tkn_str_arr, &tkn_cnt, mrl->cursor) != microrlOK) {
        return microrlERRCPLT;
    }

    if (tkn_cnt == 0) {
        tkn_str_arr[tkn_cnt++] = "";
        tkn_str_arr[tkn_cnt] = NULL;
    }

    if (mrl->cursor > 0 && mrl->cmdline_str[mrl->cursor - 1] == ' ') {
        /* Last char is whitespace */
        if (tkn_cnt >= MICRORL_CFG_CMD_TOKEN_NMB) {
            return microrlERRCPLT;
        }
        tkn_str_arr[tkn_cnt++] = "";
        tkn_str_arr[tkn_cnt] = NULL;
    }

    cmplt_tkn_arr = mrl->get_completion_fn(mrl, tkn_cnt, tkn_str_arr);
    if (cmplt_tkn_arr[0] == NULL) {
        return microrlERRCPLT;
    }

    size_t i = 0;
    size_t len;
    size_t pos = mrl->cursor;

    if (cmplt_tkn_arr[1] == NULL) {
        len = strlen(cmplt_tkn_arr[0]);
    } else {
        len = prv_complete_total_len((const char* const *)cmplt_tkn_arr);
        prv_terminal_newline(mrl);
        while (cmplt_tkn_arr[i] != NULL) {
            mrl->out_fn(mrl, cmplt_tkn_arr[i]);
            mrl->out_fn(mrl, " ");
            ++i;
        }
        prv_terminal_newline(mrl);
        prv_terminal_print_prompt(mrl);
        pos = 0;
    }

    if (len != 0) {
        prv_cmdline_buf_insert_text(mrl, cmplt_tkn_arr[0] + strlen(tkn_str_arr[tkn_cnt - 1]),
                                    len - strlen(tkn_str_arr[tkn_cnt - 1]));
    }

    /* Insert end space if completion is performed */
    if (cmplt_tkn_arr[1] == NULL) {
        prv_cmdline_buf_insert_text(mrl, " ", 1);
    }

    prv_terminal_print_line(mrl, pos, 0);

    return microrlOK;
}

#endif /* MICRORL_CFG_USE_COMPLETE || __DOXYGEN__ */

/**
 * \brief           Initialize microRL instance
 * \param[in,out]   mrl: microRL working instance
 * \param[in]       out_fn: Output function used for print operation
 * \brief           exec_fn: Command execute callback function
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
microrlr_t microrl_init(microrl_t* mrl, microrl_output_fn out_fn, microrl_exec_fn exec_fn) {
    if (mrl == NULL || out_fn == NULL || exec_fn == NULL) {
        return microrlERRPAR;
    }

    memset(mrl, 0x00, sizeof(microrl_t));
    mrl->out_fn = out_fn;
    mrl->exec_fn = exec_fn;
    microrl_set_prompt(mrl, MICRORL_CFG_PROMPT_STRING);

#if MICRORL_CFG_PROMPT_ON_INIT
    prv_terminal_print_prompt(mrl);
#endif /* MICRORL_CFG_PROMPT_ON_INIT */

#if MICRORL_CFG_USE_ECHO_OFF
    mrl->echo = MICRORL_ECHO_ON;
    mrl->echo_off_pos = -1;
#endif /* MICRORL_CFG_USE_ECHO_OFF */

    return microrlOK;
}

/**
 * \brief           Set pointer to command execute callback, that called when user press 'Enter'
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       exec_fn: Command execute callback
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
microrlr_t microrl_set_execute_callback(microrl_t* mrl, microrl_exec_fn exec_fn) {
    if (mrl == NULL || exec_fn == NULL) {
        return microrlERRPAR;
    }

    mrl->exec_fn = exec_fn;

    return microrlOK;
}

#if MICRORL_CFG_USE_COMPLETE || __DOXYGEN__
/**
 * \brief           Set pointer to input complition callback, that called when user press 'Tab'
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       get_completion_fn: Auto-complete input string callback
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
microrlr_t microrl_set_complete_callback(microrl_t* mrl, microrl_get_compl_fn get_completion_fn) {
    if (mrl == NULL || get_completion_fn == NULL) {
        return microrlERRPAR;
    }

    mrl->get_completion_fn = get_completion_fn;

    return microrlOK;
}
#endif /* MICRORL_CFG_USE_COMPLETE || __DOXYGEN__ */

#if MICRORL_CFG_USE_CTRL_C || __DOXYGEN__
/**
 * \brief           Set callback for Ctrl+C terminal signal
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       sigint_fn: Ctrl+C terminal signal callback
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
microrlr_t microrl_set_sigint_callback(microrl_t* mrl, microrl_sigint_fn sigint_fn) {
    if (mrl == NULL || sigint_fn == NULL) {
        return microrlERRPAR;
    }

    mrl->sigint_fn = sigint_fn;

    return microrlOK;
}
#endif /* MICRORL_CFG_USE_CTRL_C || __DOXYGEN__ */


#if MICRORL_CFG_USE_PROMPT_COLOR
/**
 * \brief           Calculate prompt size excluding ANSI escape sequences
 * \param[in]       prompt_str: Pointer to prompt string
 * \return          Size of prompt string excluding ANSI escape sequences
 */
static size_t prv_calculate_prompt_size(const char* prompt_str) {
    size_t size = 0;
    const char* ptr = prompt_str;

    while (*ptr != '\0') {
        if (*ptr == '\033') {                 // Check for ANSI escape code
            while (*ptr != 'm' && *ptr != '\0') {
                ++ptr;                        // Skip ANSI escape code
            }
            if (*ptr == 'm') {
                ++ptr;                        // Move past 'm'
            }
        } else {
            const size_t char_len = utf8_next_char_length(ptr, 0, strlen(ptr));
            size += utf8_display_width(ptr, 0, char_len);
            ptr += char_len;
        }
    }

    return size;
}
#endif

/**
 * \brief           Set prompt string
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       prompt_str: Pointer to prompt string to set
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
microrlr_t  microrl_set_prompt(microrl_t* mrl, char* prompt_str) {
    if (mrl == NULL || prompt_str == NULL) {
        return microrlERRPAR;
    }

    mrl->prompt_ptr = prompt_str;
#if MICRORL_CFG_USE_PROMPT_COLOR
    mrl->prompt_size = prv_calculate_prompt_size(prompt_str);
#else
    mrl->prompt_size = strlen(prompt_str);
#endif

    return microrlOK;
}

#if MICRORL_CFG_USE_ECHO_OFF || __DOXYGEN__
/**
 * \brief           Set echo mode used to mask user input
 *
 * Use \ref MICRORL_ECHO_ONCE to disable echo for password input, echo mode will enabled after user press 'Enter'.
 * Use \ref MICRORL_ECHO_ON or \ref MICRORL_ECHO_OFF to turn on or off the echo manually.
 *
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       echo: Member of \ref microrl_echo_t enumeration
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
microrlr_t microrl_set_echo(microrl_t* mrl, microrl_echo_t echo) {
    if (mrl == NULL) {
        return microrlERRPAR;
    }

    mrl->echo = echo;

    return microrlOK;
}
#endif /* MICRORL_CFG_USE_ECHO_OFF || __DOXYGEN__ */

/**
 * \brief           Process ANSI control key
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       ch: Input char to process
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
static microrlr_t prv_control_char_process(microrl_t* mrl, char ch) {
    switch (ch) {
        case MICRORL_ESC_ANSI_HT: {
#if MICRORL_CFG_USE_COMPLETE
            if (mrl->get_completion_fn == NULL) {
                return microrlERRPAR;
            }
            if (prv_complete_get_input(mrl) != microrlOK) {
                return microrlERRCPLT;
            }
#endif /* MICRORL_CFG_USE_COMPLETE */
            break;
        }
        case MICRORL_ESC_ANSI_ESC: {
#if MICRORL_CFG_USE_ESC_SEQ
            mrl->escape = 1;
#endif /* MICRORL_CFG_USE_ESC_SEQ */
            break;
        }
        case MICRORL_ESC_ANSI_NAK: { /* ^U */
            if (mrl->cursor > 0) {
                prv_cmdline_buf_backspace(mrl, mrl->cursor);
            }
            prv_terminal_print_line(mrl, 0, 1);
            break;
        }
        case MICRORL_ESC_ANSI_VT: { /* ^K */
            mrl->out_fn(mrl, "\033[K");
            mrl->cmdlen = mrl->cursor;
            break;
        }
        case MICRORL_ESC_ANSI_ENQ: { /* ^E */
            prv_terminal_move_cursor(mrl, (int32_t)utf8_display_width(
                mrl->cmdline_str, mrl->cursor, mrl->cmdlen));
            mrl->cursor = mrl->cmdlen;
            break;
        }
        case MICRORL_ESC_ANSI_SOH: { /* ^A */
            prv_terminal_move_cursor(mrl, -(int32_t)utf8_display_width(
                mrl->cmdline_str, 0, mrl->cursor));
            mrl->cursor = 0;
            break;
        }
        case MICRORL_ESC_ANSI_ACK: { /* ^F */
            if (mrl->cursor < mrl->cmdlen) {
                const size_t char_len = utf8_next_char_length(mrl->cmdline_str,
                                                               mrl->cursor,
                                                               mrl->cmdlen);
                prv_terminal_move_cursor(mrl, (int32_t)utf8_display_width(
                    mrl->cmdline_str, mrl->cursor, mrl->cursor + char_len));
                mrl->cursor += char_len;
            }
            break;
        }
        case MICRORL_ESC_ANSI_STX: { /* ^B */
            if (mrl->cursor != 0) {
                const size_t char_len = utf8_previous_char_length(mrl->cmdline_str,
                                                                   mrl->cursor);
                prv_terminal_move_cursor(mrl, -(int32_t)utf8_display_width(
                    mrl->cmdline_str, mrl->cursor - char_len, mrl->cursor));
                mrl->cursor -= char_len;
            }
            break;
        }
        case MICRORL_ESC_ANSI_DLE: { /* ^P */
#if MICRORL_CFG_USE_HISTORY
            prv_hist_search(mrl, MICRORL_HIST_DIR_UP);
#endif /* MICRORL_CFG_USE_HISTORY */
            break;
        }
        case MICRORL_ESC_ANSI_SO: { /* ^N */
#if MICRORL_CFG_USE_HISTORY
            prv_hist_search(mrl, MICRORL_HIST_DIR_DOWN);
#endif /* MICRORL_CFG_USE_HISTORY */
            break;
        }
        case MICRORL_ESC_ANSI_DEL:  /* Backspace */
        case MICRORL_ESC_ANSI_BS: { /* ^H */
            if (mrl->cursor > 0) {
                const size_t char_len = utf8_previous_char_length(mrl->cmdline_str,
                                                                   mrl->cursor);
                const size_t char_width = utf8_display_width(
                    mrl->cmdline_str, mrl->cursor - char_len, mrl->cursor);
                prv_cmdline_buf_backspace(mrl, char_len);
                if (mrl->cursor == mrl->cmdlen) {
                    prv_terminal_backspace(mrl, char_width);
                } else {
                    prv_terminal_print_line(mrl, mrl->cursor, 1);
                }
            }
            break;
        }
        case MICRORL_ESC_ANSI_EOT: { /* ^D */
            prv_cmdline_buf_delete(mrl, utf8_next_char_length(mrl->cmdline_str,
                                                               mrl->cursor,
                                                               mrl->cmdlen));
            prv_terminal_print_line(mrl, mrl->cursor, 0);
            break;
        }
        case MICRORL_ESC_ANSI_DC2: { /* ^R */
            prv_terminal_newline(mrl);
            prv_terminal_print_prompt(mrl);
            prv_terminal_print_line(mrl, 0, 0);
            break;
        }
        case MICRORL_ESC_ANSI_ETX: {
#if MICRORL_CFG_USE_CTRL_C
            if (mrl->sigint_fn == NULL) {
                return microrlERRPAR;
            }
            mrl->sigint_fn(mrl);
#endif /* MICRORL_CFG_USE_CTRL_C */
            break;
        }
        case MICRORL_ESC_ANSI_FF: { /* ^L */
            microrl_clear_terminal(mrl);
            break;
        }
        default:
            break;
    }

    return microrlOK;
}

/**
 * \brief           Print character in terminal
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \param[in]       ch: Character to print
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
static microrlr_t prv_insert_text(microrl_t* mrl, const char* text, size_t len) {
    const size_t start = mrl->cursor;
    if (prv_cmdline_buf_insert_text(mrl, text, len) != microrlOK) {
        return microrlERRCLFULL;
    }

    if (mrl->cursor == mrl->cmdlen) {
#if MICRORL_CFG_USE_ECHO_OFF
        if (((int32_t)mrl->cursor >= mrl->echo_off_pos) && (mrl->echo != MICRORL_ECHO_ON)) {
            mrl->out_fn(mrl, "*");
        } else
#endif /* MICRORL_CFG_USE_ECHO_OFF */
        {
            char output[5] = {0};
            memcpy(output, text, len);
            mrl->out_fn(mrl, output);
        }
    } else {
        prv_terminal_print_line(mrl, (int32_t)start, 0);
    }

    return microrlOK;
}

static microrlr_t prv_insert_char(microrl_t* mrl, char ch) {
    return prv_insert_text(mrl, &ch, 1);
}

/**
 * \brief           Processing command line input
 * \param[in]       mrl: \ref microrl_t working instance
 * \param[in]       data_ptr: Input data to process
 * \param[in]       len: Length of data for input
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
static microrlr_t prv_process_input_byte(microrl_t* mrl, unsigned char byte) {
    char ch = (char)byte;

#if MICRORL_CFG_USE_ESC_SEQ
    if (mrl->escape) {
        if (prv_escape_process(mrl, ch)) {
            mrl->escape = 0;
        }
        return microrlOK;
    }
#endif /* MICRORL_CFG_USE_ESC_SEQ */

    if (mrl->utf8_pending_len != 0) {
        if (utf8_is_continuation(byte)) {
            mrl->utf8_pending[mrl->utf8_pending_len++] = byte;
            if (mrl->utf8_pending_len < mrl->utf8_pending_expected) {
                return microrlOK;
            }

            if (utf8_sequence_is_valid(mrl->utf8_pending,
                                       mrl->utf8_pending_expected)) {
                microrlr_t result = prv_insert_text(
                    mrl, (const char*)mrl->utf8_pending,
                    mrl->utf8_pending_expected);
                utf8_reset_pending(mrl);
                return result;
            }
            utf8_reset_pending(mrl);
            return microrlOK;
        }
        utf8_reset_pending(mrl);
    }

    if (byte >= 0x80) {
        const size_t expected = utf8_sequence_length(byte);
        if (expected > 1 && expected <= sizeof(mrl->utf8_pending)) {
            mrl->utf8_pending[0] = byte;
            mrl->utf8_pending_len = 1;
            mrl->utf8_pending_expected = (uint8_t)expected;
        }
        return microrlOK;
    }

    if ((ch == MICRORL_ESC_ANSI_CR) || (ch == MICRORL_ESC_ANSI_LF)) {
        /* Only trigger a newline if `ch` doesn't follow its companion. */
        if (mrl->last_endl == (ch == MICRORL_ESC_ANSI_CR ?
                               MICRORL_ESC_ANSI_LF : MICRORL_ESC_ANSI_CR)) {
            mrl->last_endl = 0;
        } else {
            mrl->last_endl = ch;
            if (prv_handle_newline(mrl) != microrlOK) {
                return microrlERRTKNNUM;
            }
        }
        return microrlOK;
    }
    mrl->last_endl = 0;

    if (IS_CONTROL_CHAR(ch)) {
        return prv_control_char_process(mrl, ch);
    }
    if ((ch == ' ') && (mrl->cmdlen == 0)) {
        return microrlOK;
    }
    return prv_insert_char(mrl, ch);
}

microrlr_t microrl_processing_input(microrl_t* mrl, const void* data_ptr, size_t len) {
    if (mrl == NULL || data_ptr == NULL || len == 0) {
        return microrlERRPAR;
    }

    const unsigned char* buf_ptr = (const unsigned char*)data_ptr;
    while (len-- != 0) {
        microrlr_t result = prv_process_input_byte(mrl, *buf_ptr++);
        if (result != microrlOK) {
            return result;
        }
    }

    return microrlOK;
}

/**
 * \brief           Get current version number of the MicroRL library.
 *                      Semantic versioning is used for numbering
 * \see             Semantic versioning: https://semver.org
 * \return          MicroRL library version number combined into an uint32_t:
 *                      - [23:16] Major version
 *                      - [15:8]  Minor version
 *                      - [7:0]   Patch version
 */
uint32_t microrl_get_version(void) {
    return ((MICRORL_VERSION_MAJOR << 16) | (MICRORL_VERSION_MINOR << 8) | (MICRORL_VERSION_PATCH));
}


/**
 * \brief           Clear terminal screen and print prompt
 * \param[in,out]   mrl: \ref microrl_t working instance
 * \return          \ref microrlOK on success, member of \ref microrlr_t enumeration otherwise
 */
microrlr_t  microrl_clear_terminal(microrl_t* mrl){
    if (mrl == NULL) {
        return microrlERRPAR;
    }

    mrl->out_fn(mrl, "\033[2J");        /* Clear screen */
    mrl->out_fn(mrl, "\033[H");         /* Move cursor to home position */
    prv_terminal_print_prompt(mrl);
    prv_terminal_print_line(mrl, 0, 0);

    return microrlOK;
}
