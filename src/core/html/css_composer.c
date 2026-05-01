/**
 * @file css_composer.c
 * @brief Dynamic CSS Synthesis and Color Mathematics Implementation.
 */

#include <cwist/core/html/css_composer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MIN3(a, b, c) ((a) < (b) ? ((a) < (c) ? (a) : (c)) : ((b) < (c) ? (b) : (c)))
#define MAX3(a, b, c) ((a) > (b) ? ((a) > (c) ? (a) : (c)) : ((b) > (c) ? (b) : (c)))

cwist_color_hsl cwist_color_rgb_to_hsl(cwist_color_rgb rgb) {
    cwist_color_hsl hsl = {0.0f, 0.0f, 0.0f};
    float r = rgb.r / 255.0f;
    float g = rgb.g / 255.0f;
    float b = rgb.b / 255.0f;

    float max = MAX3(r, g, b);
    float min = MIN3(r, g, b);
    hsl.l = (max + min) / 2.0f;

    if (max == min) {
        hsl.h = 0.0f;
        hsl.s = 0.0f;
    } else {
        float d = max - min;
        hsl.s = hsl.l > 0.5f ? d / (2.0f - max - min) : d / (max + min);

        if (max == r) {
            hsl.h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        } else if (max == g) {
            hsl.h = (b - r) / d + 2.0f;
        } else {
            hsl.h = (r - g) / d + 4.0f;
        }
        hsl.h /= 6.0f;
    }
    hsl.h *= 360.0f;
    return hsl;
}

static float hue2rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

cwist_color_rgb cwist_color_hsl_to_rgb(cwist_color_hsl hsl) {
    cwist_color_rgb rgb = {0, 0, 0};
    float h = hsl.h / 360.0f;
    float s = hsl.s;
    float l = hsl.l;

    if (s == 0.0f) {
        rgb.r = rgb.g = rgb.b = (unsigned char)(l * 255.0f);
    } else {
        float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        float p = 2.0f * l - q;
        rgb.r = (unsigned char)(roundf(hue2rgb(p, q, h + 1.0f / 3.0f) * 255.0f));
        rgb.g = (unsigned char)(roundf(hue2rgb(p, q, h) * 255.0f));
        rgb.b = (unsigned char)(roundf(hue2rgb(p, q, h - 1.0f / 3.0f) * 255.0f));
    }
    return rgb;
}

cwist_color_rgb cwist_color_hex_to_rgb(const char *hex) {
    cwist_color_rgb rgb = {0, 0, 0};
    if (!hex) return rgb;
    if (hex[0] == '#') hex++;
    if (strlen(hex) == 6) {
        int r, g, b;
        if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) == 3) {
            rgb.r = r; rgb.g = g; rgb.b = b;
        }
    }
    return rgb;
}

void cwist_color_rgb_to_hex(cwist_color_rgb rgb, char out_hex[8]) {
    if (out_hex) {
        snprintf(out_hex, 8, "#%02x%02x%02x", rgb.r, rgb.g, rgb.b);
    }
}

void cwist_css_config_init(cwist_css_config *cfg) {
    if (!cfg) return;
    cfg->primary_color = cwist_color_hex_to_rgb("#3B82F6"); // Default Tailwind Blue
    cfg->secondary_color = cwist_color_hex_to_rgb("#10B981"); // Default Tailwind Green
    cfg->roundness_px = 8.0f;
    cfg->spacing_base_px = 4.0f;
    cfg->is_dark_mode = false;
}

/**
 * @brief Interpolates a new color by altering lightness.
 */
static void get_shifted_hex(cwist_color_rgb base, float light_shift, char out[8]) {
    cwist_color_hsl hsl = cwist_color_rgb_to_hsl(base);
    hsl.l += light_shift;
    if (hsl.l > 1.0f) hsl.l = 1.0f;
    if (hsl.l < 0.0f) hsl.l = 0.0f;
    cwist_color_rgb shifted = cwist_color_hsl_to_rgb(hsl);
    cwist_color_rgb_to_hex(shifted, out);
}

cwist_sstring *cwist_css_generate_variables(const cwist_css_config *cfg) {
    if (!cfg) return NULL;
    cwist_sstring *css = cwist_sstring_create();

    char p_hex[8], p_hover[8], p_active[8];
    char s_hex[8], s_hover[8], s_active[8];

    cwist_color_rgb_to_hex(cfg->primary_color, p_hex);
    get_shifted_hex(cfg->primary_color, cfg->is_dark_mode ? 0.1f : -0.1f, p_hover);
    get_shifted_hex(cfg->primary_color, cfg->is_dark_mode ? 0.15f : -0.15f, p_active);

    cwist_color_rgb_to_hex(cfg->secondary_color, s_hex);
    get_shifted_hex(cfg->secondary_color, cfg->is_dark_mode ? 0.1f : -0.1f, s_hover);
    get_shifted_hex(cfg->secondary_color, cfg->is_dark_mode ? 0.15f : -0.15f, s_active);

    char buf[1024];
    snprintf(buf, sizeof(buf),
             ":root {\n"
             "  --color-primary: %s;\n"
             "  --color-primary-hover: %s;\n"
             "  --color-primary-active: %s;\n"
             "  --color-secondary: %s;\n"
             "  --color-secondary-hover: %s;\n"
             "  --color-secondary-active: %s;\n"
             "  --bg-body: %s;\n"
             "  --bg-surface: %s;\n"
             "  --text-main: %s;\n"
             "  --text-muted: %s;\n"
             "  --radius-sm: %.1fpx;\n"
             "  --radius-md: %.1fpx;\n"
             "  --radius-lg: %.1fpx;\n"
             "  --space-1: %.1fpx;\n"
             "  --space-2: %.1fpx;\n"
             "  --space-4: %.1fpx;\n"
             "  --space-8: %.1fpx;\n"
             "}\n",
             p_hex, p_hover, p_active,
             s_hex, s_hover, s_active,
             cfg->is_dark_mode ? "#121212" : "#FFFFFF",
             cfg->is_dark_mode ? "#1E1E1E" : "#F3F4F6",
             cfg->is_dark_mode ? "#F9FAFB" : "#111827",
             cfg->is_dark_mode ? "#9CA3AF" : "#6B7280",
             cfg->roundness_px * 0.5f,
             cfg->roundness_px,
             cfg->roundness_px * 1.5f,
             cfg->spacing_base_px,
             cfg->spacing_base_px * 2.0f,
             cfg->spacing_base_px * 4.0f,
             cfg->spacing_base_px * 8.0f);

    cwist_sstring_append(css, buf);
    return css;
}

cwist_sstring *cwist_css_generate_utility_classes(const cwist_css_config *cfg) {
    (void)cfg; // Currently relying on generated variables
    cwist_sstring *css = cwist_sstring_create();

    const char *utils = 
        "/* Typography */\n"
        ".text-primary { color: var(--color-primary); }\n"
        ".text-main { color: var(--text-main); }\n"
        ".text-muted { color: var(--text-muted); }\n"
        "\n"
        "/* Backgrounds & Surfaces */\n"
        ".bg-body { background-color: var(--bg-body); }\n"
        ".bg-surface { background-color: var(--bg-surface); }\n"
        ".bg-primary { background-color: var(--color-primary); color: #fff; }\n"
        ".bg-primary:hover { background-color: var(--color-primary-hover); }\n"
        ".bg-primary:active { background-color: var(--color-primary-active); }\n"
        "\n"
        "/* Components */\n"
        ".btn {\n"
        "  display: inline-flex;\n"
        "  align-items: center;\n"
        "  justify-content: center;\n"
        "  padding: var(--space-2) var(--space-4);\n"
        "  border-radius: var(--radius-md);\n"
        "  font-weight: 500;\n"
        "  transition: background-color 0.2s, color 0.2s;\n"
        "  cursor: pointer;\n"
        "  border: none;\n"
        "}\n"
        "\n"
        ".card {\n"
        "  background-color: var(--bg-surface);\n"
        "  border-radius: var(--radius-lg);\n"
        "  padding: var(--space-4);\n"
        "  box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1);\n"
        "}\n";

    cwist_sstring_append(css, utils);
    return css;
}

cwist_sstring *cwist_css_generate_stylesheet(const cwist_css_config *cfg) {
    if (!cfg) return NULL;
    cwist_sstring *final_css = cwist_sstring_create();
    
    cwist_sstring *vars = cwist_css_generate_variables(cfg);
    cwist_sstring *utils = cwist_css_generate_utility_classes(cfg);

    if (vars) cwist_sstring_append(final_css, vars->data);
    if (utils) cwist_sstring_append(final_css, utils->data);

    if (vars) cwist_sstring_destroy(vars);
    if (utils) cwist_sstring_destroy(utils);

    return final_css;
}
