/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 zenmastercat and Magia contributors */
/*
 * kbd_map.c - PC keyboard scancode set 1 (the i8042 translates set 2 to
 * set 1) to ASCII, US layout. Written from the IBM PC/AT scancode tables.
 * First draft by the local model (qwen3.6:27b), reviewed and tested.
 */

#include <magia/kbd.h>

/*
 * Scancode Set 1 Make Codes (0x00 - 0x7F)
 * US QWERTY Layout
 */

static const char map_plain[128] = {
    /* 0x00 - 0x0F */
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    /* 0x10 - 0x1F */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0, 'a', 's',
    /* 0x20 - 0x2F */
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    /* 0x30 - 0x3F */
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    /* 0x40 - 0x4F */
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    /* 0x50 - 0x5F */
    '2', '3', '0', '.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x60 - 0x6F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x70 - 0x7F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char map_shift[128] = {
    /* 0x00 - 0x0F */
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    /* 0x10 - 0x1F */
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0, 'A', 'S',
    /* 0x20 - 0x2F */
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    /* 0x30 - 0x3F */
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    /* 0x40 - 0x4F */
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    /* 0x50 - 0x5F */
    '2', '3', '0', '.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x60 - 0x6F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x70 - 0x7F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

int kbd_translate(struct kbd_state *st, uint8_t sc)
{
    /* Handle E0 prefix */
    if (sc == 0xe0) {
        st->e0 = true;
        return -1;
    }

    /* Handle E1 prefix (Pause) and controller responses/errors */
    if (sc == 0xe1 || sc == 0xfa || sc == 0xfe || sc == 0x00 || sc == 0xff) {
        return -1;
    }

    /* Handle Extended Codes (E0 prefix was set) */
    if (st->e0) {
        st->e0 = false; /* Clear e0 after processing the extended byte */
        
        switch (sc) {
            case 0x1c: /* Keypad Enter */
                return '\r';
            case 0x35: /* Keypad / */
                return '/';
            case 0x1d: /* Right Ctrl Make */
                st->ctrl = true;
                return -1;
            case 0x9d: /* Right Ctrl Break */
                st->ctrl = false;
                return -1;
            case 0x38: /* Right Alt Make */
                st->alt = true;
                return -1;
            case 0xb8: /* Right Alt Break */
                st->alt = false;
                return -1;
            default:
                /* Arrows and other extended codes return -1 */
                return -1;
        }
    }

    /* Handle Break Codes (bit 7 set) */
    if (sc & 0x80) {
        uint8_t base = sc & 0x7f;
        switch (base) {
            case 0x2a: /* Left Shift Break */
                st->shift_l = false;
                break;
            case 0x36: /* Right Shift Break */
                st->shift_r = false;
                break;
            case 0x1d: /* Left Ctrl Break */
                st->ctrl = false;
                break;
            case 0x38: /* Left Alt Break */
                st->alt = false;
                break;
            default:
                break;
        }
        return -1;
    }

    /* Handle Make Codes for Modifier Keys */
    switch (sc) {
        case 0x2a: /* Left Shift Make */
            st->shift_l = true;
            return -1;
        case 0x36: /* Right Shift Make */
            st->shift_r = true;
            return -1;
        case 0x1d: /* Left Ctrl Make */
            st->ctrl = true;
            return -1;
        case 0x38: /* Left Alt Make */
            st->alt = true;
            return -1;
        case 0x3a: /* Caps Lock Toggle */
            st->caps = !st->caps;
            return -1;
        default:
            break;
    }

    /* Handle Regular Keys */
    if (sc < 128) {
        const char *map;
        bool shift = st->shift_l || st->shift_r;
        
        /* Determine which map to use */
        if (shift) {
            map = map_shift;
        } else {
            map = map_plain;
        }

        char c = map[sc];

        /* If map entry is 0, no character produced */
        if (c == 0) {
            return -1;
        }

        /* Handle Caps Lock for letters only */
        if (st->caps) {
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            } else if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
        }

        /* Handle Ctrl for letters */
        if (st->ctrl) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                return c & 0x1f;
            }
        }

        return c;
    }

    return -1;
}

/* Run by ktest 031. */

int kbd_map_selftest(void)
{
    int failures = 0;
    struct kbd_state st = { false, false, false, false, false, false };

    /* Helper macro to check result */
    #define CHECK(cond, msg) do { if (!(cond)) failures++; } while (0)

    /* 1. 0x1e -> 'a' */
    st = (struct kbd_state){0};
    CHECK(kbd_translate(&st, 0x1e) == 'a', "0x1e should be 'a'");

    /* 2. shift down (0x2a), 0x1e -> 'A' */
    st = (struct kbd_state){0};
    kbd_translate(&st, 0x2a); /* Shift down */
    CHECK(kbd_translate(&st, 0x1e) == 'A', "Shift+0x1e should be 'A'");

    /* 3. shift up (0xaa), 0x1e -> 'a' */
    kbd_translate(&st, 0xaa); /* Shift up */
    CHECK(kbd_translate(&st, 0x1e) == 'a', "After shift up, 0x1e should be 'a'");

    /* 4. caps (0x3a) then 0x1e -> 'A' */
    st = (struct kbd_state){0};
    kbd_translate(&st, 0x3a); /* Caps on */
    CHECK(kbd_translate(&st, 0x1e) == 'A', "Caps+0x1e should be 'A'");

    /* 5. caps (0x3a) then 0x02 -> '1' (numbers unaffected) */
    CHECK(kbd_translate(&st, 0x02) == '1', "Caps+0x02 should be '1'");

    /* 6. caps again restores */
    kbd_translate(&st, 0x3a); /* Caps off */
    CHECK(kbd_translate(&st, 0x1e) == 'a', "Caps off, 0x1e should be 'a'");

    /* 7. ctrl (0x1d) + 0x2e -> 3 (Ctrl-C) */
    st = (struct kbd_state){0};
    kbd_translate(&st, 0x1d); /* Ctrl down */
    CHECK(kbd_translate(&st, 0x2e) == 3, "Ctrl+0x2e should be 3");

    /* 8. ctrl release 0x9d */
    kbd_translate(&st, 0x9d); /* Ctrl up */
    CHECK(kbd_translate(&st, 0x2e) == 'c', "After Ctrl up, 0x2e should be 'c'");

    /* 9. 0x1c -> '\r' */
    st = (struct kbd_state){0};
    CHECK(kbd_translate(&st, 0x1c) == '\r', "0x1c should be '\\r'");

    /* 10. 0xe0 0x1c -> '\r' */
    st = (struct kbd_state){0};
    kbd_translate(&st, 0xe0);
    CHECK(kbd_translate(&st, 0x1c) == '\r', "E0+0x1c should be '\\r'");

    /* 11. 0xe0 0x48 (up arrow) -> -1 */
    st = (struct kbd_state){0};
    kbd_translate(&st, 0xe0);
    CHECK(kbd_translate(&st, 0x48) == -1, "E0+0x48 should be -1");

    /* 12. 0x9e (break of a) -> -1 */
    st = (struct kbd_state){0};
    CHECK(kbd_translate(&st, 0x9e) == -1, "0x9e should be -1");

    /* 13. 0xfa -> -1 */
    st = (struct kbd_state){0};
    CHECK(kbd_translate(&st, 0xfa) == -1, "0xfa should be -1");

    /* 14. 0x0e -> '\b' */
    st = (struct kbd_state){0};
    CHECK(kbd_translate(&st, 0x0e) == '\b', "0x0e should be '\\b'");

    /* 15. shift + 0x0d -> '+' */
    st = (struct kbd_state){0};
    kbd_translate(&st, 0x2a); /* Shift down */
    CHECK(kbd_translate(&st, 0x0d) == '+', "Shift+0x0d should be '+'");

    return failures;
}

