#ifndef CWIST_TEMPLATE_H
#define CWIST_TEMPLATE_H

#include <cwist/core/sstring/sstring.h>
#include <cjson/cJSON.h>

/**
 * @brief Renders a template string with the given cJSON context.
 *
 * This function parses a template string and replaces placeholders with values
 * from the provided cJSON context.
 *
 * It supports:
 * - Variable replacement: {{ key }}
 * - Simple conditionals: {% if key %}...{% endif %}
 * - Looping over arrays: {% for item in array %}...{{ item.key }}...{% endfor %}
 *
 * @param template_str The template string to render.
 * @param context The cJSON object containing the data for the template.
 * @return A new cwist_sstring containing the rendered output. The caller is
 *         responsible for destroying the returned string. Returns NULL on failure.
 */
cwist_sstring* cwist_template_render(const char *template_str, const cJSON *context);

/**
 * @brief Renders a template from a file with the given cJSON context.
 *
 * Reads a template from the specified file path and renders it using the
 * provided cJSON context.
 *
 * @param file_path The path to the template file.
 * @param context The cJSON object containing the data for the template.
 * @return A new cwist_sstring containing the rendered output. The caller is
 *         responsible for destroying the returned string. Returns NULL if the
 *         file cannot be read or on rendering failure.
 */
cwist_sstring* cwist_template_render_file(const char *file_path, const cJSON *context);

#endif // CWIST_TEMPLATE_H
