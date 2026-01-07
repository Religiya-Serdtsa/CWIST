#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/net/http/http.h>
#include <cjson/cJSON.h>

#define PORT 8080
#define BUFFER_SIZE 4096

// Mock JSON Input (Simulating an incoming request body)
const char *MOCK_JSON_INPUT = "{"
    "\"System\": \"Solaris 2.5.1\","
    "\"Host\": \"sun-sparc-station\","
    "\"User\": \"yjlee\","
    "\"Shell\": \"/bin/csh\","
    "\"Uptime\": \"42 days\","
    "\"Load\": \"0.01, 0.05, 0.00\""
"}";

void generate_cde_html(cwist_sstring *html, cJSON *json) {
    // 1. Write HTML Header & CSS (CDE Retro Style)
    cwist_sstring_append(html, 
        "<!DOCTYPE html>\n<html>\n<head>\n"
        "<title>System Monitor</title>\n"
        "<style>\n"
        "  body { background-color: #5d97a6; font-family: 'Helvetica', sans-serif; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }\n"
        "  .cde-window {\n"
        "    background-color: #bebebe;\n"
        "    border-top: 2px solid #ffffff;\n"
        "    border-left: 2px solid #ffffff;\n"
        "    border-right: 2px solid #666666;\n"
        "    border-bottom: 2px solid #666666;\n"
        "    padding: 5px;\n"
        "    width: 400px;\n"
        "    box-shadow: 10px 10px 0px rgba(0,0,0,0.2);\n"
        "  }\n"
        "  .title-bar {\n"
        "    background-color: #4b6983;\n" /* CDE Active Title Color */
        "    color: white;\n"
        "    padding: 4px 8px;\n"
        "    font-weight: bold;\n"
        "    border-top: 1px solid #99b6d4;\n"
        "    border-left: 1px solid #99b6d4;\n"
        "    border-right: 1px solid #283745;\n"
        "    border-bottom: 1px solid #283745;\n"
        "    margin-bottom: 8px;\n"
        "    display: flex; justify-content: space-between;\n"
        "  }\n"
        "  .content-area {\n"
        "    border-top: 2px solid #666666;\n"
        "    border-left: 2px solid #666666;\n"
        "    border-right: 2px solid #ffffff;\n"
        "    border-bottom: 2px solid #ffffff;\n"
        "    padding: 10px;\n"
        "  }\n"
        "  table { width: 100%; border-collapse: collapse; font-size: 14px; }\n"
        "  td { padding: 4px; border: 1px solid transparent; }\n"
        "  tr:nth-child(odd) { background-color: #cccccc; }\n"
        "  .key { font-weight: bold; width: 40%; text-align: right; padding-right: 15px; color: #333; }\n"
        "  .value { font-family: 'Courier New', monospace; color: #000; }\n"
        "</style>\n</head>\n<body>\n");

    // 2. Start Window Structure
    cwist_sstring_append(html, 
        "<div class=\"cde-window\">\n"
        "  <div class=\"title-bar\"><span>Terminal Info</span><span>[X]</span></div>\n"
        "  <div class=\"content-area\">\n"
        "    <table>\n");

    // 3. Iterate JSON and generate Table Rows
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, json) {
        if (cJSON_IsString(item)) {
            cwist_sstring_append(html, "      <tr><td class=\"key\"> ");
            cwist_sstring_append(html, item->string); // JSON Key
            cwist_sstring_append(html, "</td><td class=\"value\"> ");
            cwist_sstring_append(html, item->valuestring); // JSON Value
            cwist_sstring_append(html, "</td></tr>\n");
        }
    }

    // 4. Close Tags
    cwist_sstring_append(html, 
        "    </table>\n"
        "  </div>\n"
        "</div>\n"
        "</body>\n</html>");
}

void handle_client(int client_fd, void *ctx) {
    (void)ctx;
    char buffer[BUFFER_SIZE];
    int read_len = read(client_fd, buffer, BUFFER_SIZE - 1);
    if (read_len < 0) {
        close(client_fd);
        return;
    }
    buffer[read_len] = '\0';
    
    // Log request (optional)
    printf("Received Request:\n%s\n----------------\n", buffer);

    // Prepare Response
    cwist_http_response *res = cwist_http_response_create();
    
    // Generate Body
    cJSON *json = cJSON_Parse(MOCK_JSON_INPUT);
    if (json) {
        generate_cde_html(res->body, json);
        cJSON_Delete(json);
        cwist_http_header_add(&res->headers, "Content-Type", "text/html");
        
        char len_str[32];
        if (res->body->data) {
            sprintf(len_str, "%zu", strlen(res->body->data));
            cwist_http_header_add(&res->headers, "Content-Length", len_str);
        }
    } else {
         res->status_code = CWIST_HTTP_INTERNAL_ERROR;
         cwist_sstring_assign(res->status_text, "Internal Server Error");
    }

    cwist_http_send_response(client_fd, res);
    cwist_http_response_destroy(res);
    close(client_fd);
}

int main() {
    int port = 8080, backlog = 128;
    const char *addr = "127.0.0.1";
    struct sockaddr_in sockv4;
    
    int server_fd =  cwist_make_socket_ipv4(&sockv4, addr, port, backlog);
    if (server_fd < 0) {
        printf("Failed to create server socket: %d\n", server_fd);
        return 1;
    }

    printf("Server listening on port %d\n", PORT);
    printf("Visit http://%s:%d to see the CDE JSON Viewer\n", addr, port);

    cwist_accept_socket(server_fd, (struct sockaddr*)&sockv4, handle_client, NULL);
    return 0;
}
