#pragma once

/**
 * @brief Decode a URL-encoded string in place.
 * 
 * Converts URL-encoded characters (like %20 for space, + for space)
 * to their normal ASCII representation. The string is modified in place.
 * 
 * @param str Pointer to null-terminated string to decode (modified in place)
 * 
 * @note Useful for processing form data from HTTP POST requests.
 */
static inline void url_decode(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            int hi = src[1], lo = src[2];
            hi = (hi >= 'A') ? (hi & ~0x20) - 'A' + 10 : hi - '0';
            lo = (lo >= 'A') ? (lo & ~0x20) - 'A' + 10 : lo - '0';
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}