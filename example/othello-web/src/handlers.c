#include "handlers.h"
#include "common.h"
#include "db.h"
#include "utils.h"
#include <cwist/sys/app/app.h>
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/net/http/query.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/core/template/template.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void root_handler(cwist_http_request *req, cwist_http_response *res) {
    int room_id = 1;
    const char *room_str = cwist_query_map_get(req->query_params, "room");
    if (room_str) {
        room_id = atoi(room_str);
    }

    int board[SIZE][SIZE];
    int turn, players;
    char status[32];
    char mode[16];
    get_game_state(req->db, room_id, board, &turn, status, &players, mode, NULL);

    cJSON *context = cJSON_CreateObject();
    cJSON_AddNumberToObject(context, "room_id", room_id);
    cJSON_AddStringToObject(context, "mode", mode);
    cJSON_AddStringToObject(context, "status", status);

    char turn_str[16];
    if (turn == 1) strcpy(turn_str, "Black");
    else if (turn == 2) strcpy(turn_str, "White");
    else strcpy(turn_str, "None");
    cJSON_AddStringToObject(context, "turn", turn_str);

    cJSON *board_json = cJSON_CreateArray();
    cJSON *row_json;
    for (int i = 0; i < SIZE; i++) {
        row_json = cJSON_CreateArray();
        for (int j = 0; j < SIZE; j++) {
            cJSON_AddItemToArray(row_json, cJSON_CreateNumber(board[i][j]));
        }
        cJSON_AddItemToArray(board_json, row_json);
    }
    cJSON_AddItemToObject(context, "board", board_json);

    cwist_sstring* rendered_html = cwist_template_render_file("public/index.html.tmpl", context);
    if (rendered_html) {
        cwist_sstring_assign(res->body, rendered_html->data);
        cwist_sstring_destroy(rendered_html);
    } else {
        res->status_code = 500;
        cwist_sstring_assign(res->body, "Failed to render template");
    }

    cJSON_Delete(context);
    cwist_http_header_add(&res->headers, "Content-Type", "text/html");
}

static int is_valid_move(int board[SIZE][SIZE], int r, int c, int p) {
    if (r < 0 || r >= SIZE || c < 0 || c >= SIZE || board[r][c] != 0) return 0;
    int opponent = (p == BLACK) ? WHITE : BLACK;
    int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + dr[i];
        int nc = c + dc[i];
        int count = 0;
        while(nr >= 0 && nr < SIZE && nc >= 0 && nc < SIZE && board[nr][nc] == opponent) {
            nr += dr[i]; nc += dc[i]; count++;
        }
        if (count > 0 && nr >= 0 && nr < SIZE && nc >= 0 && nc < SIZE && board[nr][nc] == p) {
            return 1;
        }
    }
    return 0;
}

static int has_valid_moves(int board[SIZE][SIZE], int p) {
    for(int r=0; r<SIZE; r++) {
        for(int c=0; c<SIZE; c++) {
            if(is_valid_move(board, r, c, p)) return 1;
        }
    }
    return 0;
}

static int count_pieces(int board[SIZE][SIZE]) {
    int count = 0;
    for(int r=0; r<SIZE; r++)
        for(int c=0; c<SIZE; c++)
            if(board[r][c] != 0) count++;
    return count;
}

static int get_room_id(cwist_http_request *req) {
    int room_id = 1;
    const char *room_str = cwist_query_map_get(req->query_params, "room");
    if (room_str && strlen(room_str) > 0) {
        room_id = atoi(room_str);
    }
    return room_id;
}

void join_handler(cwist_http_request *req, cwist_http_response *res) {
    int room_id = get_room_id(req);
    int pid;
    char mode[16];
    const char *requested_mode = cwist_query_map_get(req->query_params, "mode");

    if (db_join_game(req->db, room_id, requested_mode, &pid, mode) < 0) {
         res->status_code = CWIST_HTTP_FORBIDDEN;
         cwist_sstring_assign(res->body, "{\"error\": \"Room full\"}");
         return;
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "player_id", pid);
    cJSON_AddNumberToObject(json, "room_id", room_id);
    cJSON_AddStringToObject(json, "mode", mode);
    
    char *str = cJSON_PrintUnformatted(json);
    cwist_sstring_assign(res->body, str);
    free(str);
    cJSON_Delete(json);
    
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

void state_handler(cwist_http_request *req, cwist_http_response *res) {
    int room_id = get_room_id(req);
    int board[SIZE][SIZE];
    int turn, players;
    char status[32];
    char mode[16];
    get_game_state(req->db, room_id, board, &turn, status, &players, mode, NULL);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", status);
    cJSON_AddNumberToObject(json, "turn", turn);
    cJSON_AddStringToObject(json, "mode", mode);
    cJSON_AddNumberToObject(json, "room_id", room_id);
    
    cJSON *board_arr = cJSON_CreateArray();
    for(int r=0; r<SIZE; r++) {
        cJSON *row_arr = cJSON_CreateArray();
        for(int c=0; c<SIZE; c++) {
            cJSON_AddItemToArray(row_arr, cJSON_CreateNumber(board[r][c]));
        }
        cJSON_AddItemToArray(board_arr, row_arr);
    }
    cJSON_AddItemToObject(json, "board", board_arr);
    
    char *str = cJSON_PrintUnformatted(json);
    cwist_sstring_assign(res->body, str);
    free(str);
    cJSON_Delete(json);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

void move_handler(cwist_http_request *req, cwist_http_response *res) {
    int room_id = get_room_id(req);
    cJSON *json = cJSON_Parse(req->body->data);
    if (!json) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        return;
    }

    int r = cJSON_GetObjectItem(json, "r")->valueint;
    int c = cJSON_GetObjectItem(json, "c")->valueint;
    int p = cJSON_GetObjectItem(json, "player")->valueint;
    
    int board[SIZE][SIZE];
    int turn, players;
    char status[32];
    char mode[16];
    get_game_state(req->db, room_id, board, &turn, status, &players, mode, NULL);

    if (strcmp(status, "active") == 0 && p == turn) {
        int pieces = count_pieces(board);
        int is_reversi_setup = (strcmp(mode, "reversi") == 0 && pieces < 4);
        
        if (is_reversi_setup) {
            if (r < 3 || r > 4 || c < 3 || c > 4 || board[r][c] != 0) {
                res->status_code = CWIST_HTTP_BAD_REQUEST;
                cJSON_Delete(json);
                return;
            }
        } else if (!is_valid_move(board, r, c, p)) {
            res->status_code = CWIST_HTTP_BAD_REQUEST;
            cJSON_Delete(json);
            return;
        }

        // Apply Move
        board[r][c] = p;
        int opponent = (p == BLACK) ? WHITE : BLACK;
        
        if (!is_reversi_setup) {
            int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
            int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + dr[i], nc = c + dc[i];
                int r_temp = nr, c_temp = nc, count = 0;
                while(r_temp >= 0 && r_temp < SIZE && c_temp >= 0 && c_temp < SIZE && board[r_temp][c_temp] == opponent) {
                    r_temp += dr[i]; c_temp += dc[i]; count++;
                }
                if (count > 0 && r_temp >= 0 && r_temp < SIZE && board[r_temp][c_temp] == p) {
                    int rr = r + dr[i], cc = c + dc[i];
                    while(board[rr][cc] == opponent) {
                        board[rr][cc] = p; rr += dr[i]; cc += dc[i];
                    }
                }
            }
        }
        
        // Determine next turn
        if (is_reversi_setup && count_pieces(board) < 4) {
            turn = opponent;
        } else {
            if (has_valid_moves(board, opponent)) {
                turn = opponent;
            } else if (!has_valid_moves(board, p)) {
                strcpy(status, "finished");
            }
        }

        update_game_state(req->db, room_id, board, turn, status, players, mode);
        cwist_sstring_assign(res->body, "{\"status\":\"ok\"}");
    } else {
         res->status_code = CWIST_HTTP_FORBIDDEN;
    }
    cJSON_Delete(json);
}