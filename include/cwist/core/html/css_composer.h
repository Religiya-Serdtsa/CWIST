/**
 * @file css_composer.h
 * @brief Dynamic CSS Synthesis and Color Mathematics.
 */

#ifndef __CWIST_CSS_COMPOSER_H__
#define __CWIST_CSS_COMPOSER_H__

#include <cwist/core/sstring/sstring.h>
#include <stdbool.h>

/**
 * @brief RGB Color representation.
 */
typedef struct cwist_color_rgb {
    unsigned char r; ///< Red channel (0-255)
    unsigned char g; ///< Green channel (0-255)
    unsigned char b; ///< Blue channel (0-255)
} cwist_color_rgb;

/**
 * @brief HSL Color representation for dynamic manipulation.
 */
typedef struct cwist_color_hsl {
    float h; ///< Hue angle in degrees (0.0 to 360.0)
    float s; ///< Saturation percentage (0.0 to 1.0)
    float l; ///< Lightness percentage (0.0 to 1.0)
} cwist_color_hsl;

/**
 * @brief Dynamic CSS configuration parameters.
 * Contains base metrics used to extrapolate an entire design system.
 */
typedef struct cwist_css_config {
    cwist_color_rgb primary_color;   ///< Base primary brand color
    cwist_color_rgb secondary_color; ///< Secondary/Accent color
    float roundness_px;              ///< Base border radius in pixels (e.g. 8.0)
    float spacing_base_px;           ///< Base spacing unit in pixels (e.g. 4.0)
    bool is_dark_mode;               ///< Flag to trigger dark mode palette generation
} cwist_css_config;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Converts an RGB color to HSL color space.
 * @param rgb The RGB color to convert.
 * @return The corresponding HSL representation.
 */
cwist_color_hsl cwist_color_rgb_to_hsl(cwist_color_rgb rgb);

/**
 * @brief Converts an HSL color back to RGB color space.
 * @param hsl The HSL color to convert.
 * @return The corresponding RGB representation.
 */
cwist_color_rgb cwist_color_hsl_to_rgb(cwist_color_hsl hsl);

/**
 * @brief Parses a hex string (e.g., "#FF0000" or "FF0000") into an RGB struct.
 * @param hex The null-terminated hex string.
 * @return The parsed RGB color. Defaults to black on parse error.
 */
cwist_color_rgb cwist_color_hex_to_rgb(const char *hex);

/**
 * @brief Formats an RGB struct into a 7-character hex string (e.g., "#FFFFFF").
 * @param rgb The RGB color.
 * @param out_hex A character array of at least 8 bytes to hold the result.
 */
void cwist_color_rgb_to_hex(cwist_color_rgb rgb, char out_hex[8]);

/**
 * @brief Initializes a CSS configuration with sensible defaults.
 * @param cfg Pointer to the configuration struct to initialize.
 */
void cwist_css_config_init(cwist_css_config *cfg);

/**
 * @brief Generates a CSS string containing CSS Custom Properties (:root variables).
 * Extrapolates hover states, active states, and surface colors mathematically.
 * @param cfg The configuration parameters.
 * @return A dynamically allocated string containing the CSS rules. Must be destroyed.
 */
cwist_sstring *cwist_css_generate_variables(const cwist_css_config *cfg);

/**
 * @brief Generates a set of utility classes based on the design system configuration.
 * @param cfg The configuration parameters.
 * @return A dynamically allocated string containing the CSS classes. Must be destroyed.
 */
cwist_sstring *cwist_css_generate_utility_classes(const cwist_css_config *cfg);

/**
 * @brief High-level helper to generate a complete stylesheet (Variables + Utilities).
 * @param cfg The configuration parameters.
 * @return A dynamically allocated string containing the full stylesheet.
 */
cwist_sstring *cwist_css_generate_stylesheet(const cwist_css_config *cfg);

#ifdef __cplusplus
}
#endif

#endif
