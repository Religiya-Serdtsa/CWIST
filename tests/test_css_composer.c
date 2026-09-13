#include <cwist/core/html/css_composer.h>
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

void test_hex_parsing_6digit(void) {
    cwist_color_rgb white = cwist_color_hex_to_rgb("#FFFFFF");
    assert(white.r == 255 && white.g == 255 && white.b == 255);

    cwist_color_rgb black = cwist_color_hex_to_rgb("000000");
    assert(black.r == 0 && black.g == 0 && black.b == 0);

    cwist_color_rgb custom = cwist_color_hex_to_rgb("#3B82F6");
    assert(custom.r == 0x3B && custom.g == 0x82 && custom.b == 0xF6);

    char hex_out[8];
    cwist_color_rgb_to_hex(custom, hex_out);
    assert(strcasecmp(hex_out, "#3B82F6") == 0);
    printf("Passed test_hex_parsing_6digit\n");
}

void test_hex_parsing_3digit(void) {
    /* 3-digit hex shorthand: #fff -> #ffffff */
    cwist_color_rgb white = cwist_color_hex_to_rgb("#fff");
    assert(white.r == 255 && white.g == 255 && white.b == 255);

    /* 3-digit without '#' prefix */
    cwist_color_rgb red = cwist_color_hex_to_rgb("f00");
    assert(red.r == 255 && red.g == 0 && red.b == 0);

    cwist_color_rgb custom = cwist_color_hex_to_rgb("#123");
    assert(custom.r == 0x11 && custom.g == 0x22 && custom.b == 0x33);

    char hex_out[8];
    cwist_color_rgb_to_hex(custom, hex_out);
    assert(strcasecmp(hex_out, "#112233") == 0);
    printf("Passed test_hex_parsing_3digit\n");
}

void test_hex_parsing_invalid(void) {
    /* NULL pointer */
    cwist_color_rgb c = cwist_color_hex_to_rgb(NULL);
    assert(c.r == 0 && c.g == 0 && c.b == 0);

    /* Empty string */
    c = cwist_color_hex_to_rgb("");
    assert(c.r == 0 && c.g == 0 && c.b == 0);

    /* Invalid lengths */
    c = cwist_color_hex_to_rgb("#12");
    assert(c.r == 0 && c.g == 0 && c.b == 0);

    c = cwist_color_hex_to_rgb("#12345");
    assert(c.r == 0 && c.g == 0 && c.b == 0);

    /* Invalid hex characters */
    c = cwist_color_hex_to_rgb("#12zz45");
    assert(c.r == 0 && c.g == 0 && c.b == 0);

    c = cwist_color_hex_to_rgb("#xyz");
    assert(c.r == 0 && c.g == 0 && c.b == 0);
    printf("Passed test_hex_parsing_invalid\n");
}

void test_css_generation(void) {
    cwist_css_config cfg;
    cwist_css_config_init(&cfg);
    assert(cfg.primary_color.r == 0x3B && cfg.primary_color.g == 0x82 && cfg.primary_color.b == 0xF6);

    cwist_sstring *stylesheet = cwist_css_generate_stylesheet(&cfg);
    assert(stylesheet != NULL && stylesheet->data != NULL);
    assert(strstr(stylesheet->data, ":root") != NULL);
    assert(strstr(stylesheet->data, "--color-primary:") != NULL);
    assert(strstr(stylesheet->data, ".btn") != NULL);

    cwist_sstring_destroy(stylesheet);
    printf("Passed test_css_generation\n");
}

int main(void) {
    test_hex_parsing_6digit();
    test_hex_parsing_3digit();
    test_hex_parsing_invalid();
    test_css_generation();
    printf("All CSS composer tests passed!\n");
    return 0;
}
