/*
  gust_ebm - Ebm file processor for Gust (Koei/Tecmo) PC games
  Copyright © 2019-2022 VitaSmith

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "utf8.h"
#include "util.h"
#include "parson.h"

#define JSON_VERSION            2
#define MAX_STRING_SIZE         2048

/* An ebm structure is as follows:
    uint32_t type;              // always seems to be set to 2
    uint32_t voice_id;          // id of the voice for the speaking character
    uint32_t unknown1;          // ???
    uint32_t name_id;           // id of the name to use for the speaking character
    uint32_t extra_id;          // seems to be -1 for system messages
    uint32_t expr_id;           // serious = 0x09, surprise = 0x0a, happy = 0x0c, etc.
    uint32_t duration1[2];      // [OPTIONAL] Probably duration values. Used by Nelke, Sophie 2, etc
    uint32_t msg_id;            // sequential id of the message
    uint32_t unknown2;          // ???
    uint32_t msg_length;        // length of msg_string
    char     msg_string[];      // text message to display
    uint32_t duration2[]        // [OPTIONAL] Probably duration values. Used by NOA2, Ryza 2, Sophie 2
 */

uint32_t duration1_length[] = { 0, 2 };
uint32_t duration2_length[] = { 0, 1, 2 };

int main_utf8(int argc, char** argv)
{
    int r = -1;
    uint8_t* buf = NULL;
    char *ebm_message, path[PATH_MAX];
    FILE* file = NULL;
    JSON_Value* json = NULL;

    if (argc != 2) {
        printf("%s %s (c) 2019-2022 VitaSmith\n\n"
            "Usage: %s <file>\n\n"
            "Convert a .ebm file to or from an editable JSON file.\n\n",
            _appname(argv[0]), GUST_TOOLS_VERSION_STR, _appname(argv[0]));
        return 0;
    }

    if (strstr(argv[argc - 1], ".json") != NULL) {
        json = json_parse_file_with_comments(argv[argc - 1]);
        if (json == NULL) {
            fprintf(stderr, "ERROR: Can't parse JSON data from '%s'\n", argv[argc - 1]);
            goto out;
        }
        const uint32_t json_version = json_object_get_uint32(json_object(json), "json_version");
        if (json_version != JSON_VERSION) {
            fprintf(stderr, "ERROR: This utility is not compatible with the JSON file provided.\n"
                "You need to (re)extract the '.ebm' using this application.\n");
            goto out;
        }
        snprintf(path, sizeof(path), "%s%c%s", _dirname(argv[argc - 1]), PATH_SEP,
            json_object_get_string(json_object(json), "name"));
        printf("Creating '%s' from JSON...\n", path);
        create_backup(path);
        file = fopen_utf8(path, "wb");
        if (file == NULL) {
            fprintf(stderr, "ERROR: Cannot create file '%s'\n", path);
            goto out;
        }
        int32_t nb_messages = (int32_t)json_object_get_uint32(json_object(json), "nb_messages");
        if (fwrite(&nb_messages, sizeof(int32_t), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't write number of messages\n");
            goto out;
        }
        JSON_Array* json_messages = json_object_get_array(json_object(json), "messages");
        if (json_array_get_count(json_messages) != (size_t)abs(nb_messages)) {
            fprintf(stderr, "ERROR: Number of messages doesn't match the array size\n");
            goto out;
        }
        uint32_t ebm_header[11];
        assert(array_size(ebm_header) >= 9 + duration1_length[array_size(duration1_length) - 1]);
        assert(array_size(ebm_header) >= duration2_length[array_size(duration2_length) - 1]);
        for (size_t i = 0; i < (size_t)abs(nb_messages); i++) {
            JSON_Object* json_message = json_array_get_object(json_messages, i);
            memset(ebm_header, 0, sizeof(ebm_header));
            uint32_t j = 0;
            size_t x;
            ebm_header[j] = json_object_get_uint32(json_message, "type");
            ebm_header[++j] = json_object_get_uint32(json_message, "voice_id");
            ebm_header[++j] = json_object_get_uint32(json_message, "unknown1");
            ebm_header[++j] = json_object_get_uint32(json_message, "name_id");
            ebm_header[++j] = json_object_get_uint32(json_message, "extra_id");
            ebm_header[++j] = json_object_get_uint32(json_message, "expr_id");
            JSON_Array* json_duration_array = json_object_get_array(json_message, "duration1");
            for (x = 0; x < json_array_get_count(json_duration_array); x++)
                ebm_header[++j] = json_array_get_uint32(json_duration_array, x);
            ebm_header[++j] = json_object_get_uint32(json_message, "msg_id");
            ebm_header[++j] = json_object_get_uint32(json_message, "unknown2");
            const char* msg_string = json_object_get_string(json_message, "msg_string");
            ebm_header[++j] = (uint32_t)strlen(msg_string) + 1;
            if (fwrite(ebm_header, sizeof(uint32_t), j + 1, file) != j + 1) {
                fprintf(stderr, "ERROR: Can't write message header\n");
                goto out;
            }
            if (fwrite(msg_string, 1, ebm_header[j], file) != ebm_header[j]) {
                fprintf(stderr, "ERROR: Can't write message data\n");
                goto out;
            }
            json_duration_array = json_object_get_array(json_message, "duration2");
            for (x = 0; x < json_array_get_count(json_duration_array); x++)
                ebm_header[x] = json_array_get_uint32(json_duration_array, x);
            if (x != 0) {
                if (fwrite(ebm_header, sizeof(uint32_t), x, file) != x) {
                    fprintf(stderr, "ERROR: Can't write duration data\n");
                    goto out;
                }
            }
        }
        JSON_Array* json_extra_data = json_object_get_array(json_object(json), "extra_data");
        for (size_t i = 0; i < json_array_get_count(json_extra_data); i++) {
            uint32_t val = json_array_get_uint32(json_extra_data, i);
            if (fwrite(&val, sizeof(uint32_t), 1, file) != 1) {
                fprintf(stderr, "ERROR: Can't write extra data\n");
                goto out;
            }
        }
        r = 0;
    } else if (strstr(argv[argc - 1], ".ebm") != NULL) {
        printf("Converting '%s' to JSON...\n", _basename(argv[argc - 1]));
        uint32_t buf_size = read_file(argv[argc - 1], &buf);
        if (buf_size == UINT32_MAX)
            goto out;
        int32_t nb_messages = (int32_t)getle32(buf);
        if (buf_size < sizeof(uint32_t) + abs(nb_messages) * sizeof(ebm_message)) {
            fprintf(stderr, "ERROR: Invalid number of entries\n");
            goto out;
        }

        // Detect the length of the structure we will work with
        uint32_t d1, d2;
        for (d1 = 0; d1 < array_size(duration1_length); d1++) {
            for (d2 = 0; d2 < array_size(duration2_length); d2++) {
                uint32_t* p = (uint32_t*)&buf[sizeof(uint32_t)];
                bool good_candidate = true;
                for (size_t i = 0; i < (size_t)abs(nb_messages) && good_candidate; i++) {
                    uint32_t len = p[8 + duration1_length[d1]];
                    good_candidate = (len != 0 && len <= MAX_STRING_SIZE);
                    if (!good_candidate)
                        break;
                    char* str = (char*)&p[9 + duration1_length[d1]];
                    for (uint32_t j = 0; (j < len - 1) && good_candidate; j++)
                        good_candidate = (str[j] != 0);
                    p = (uint32_t*)(&str[len]);
                    p = &p[duration2_length[d2]];
                }
                if (good_candidate)
                    goto detected;

            }
        }
detected:
        if (d1 >= array_size(duration1_length) || d2 >= array_size(duration2_length)) {
            fprintf(stderr, "ERROR: Failed to detect EBM record structure (Unsupported?)\n");
            goto out;
        }
        JSON_Value* json_messages = NULL;
        JSON_Value* json_message = NULL;
        // Store the data we'll need to reconstruct the archive to a JSON file
        json = json_value_init_object();
        json_object_set_number(json_object(json), "json_version", JSON_VERSION);
        json_object_set_string(json_object(json), "name", _basename(argv[argc - 1]));
        json_object_set_number(json_object(json), "nb_messages", nb_messages & 0xffffffff);
        json_messages = json_value_init_array();
        uint32_t* ebm_header = (uint32_t*)&buf[sizeof(uint32_t)];
        for (size_t i = 0; i < (size_t)abs(nb_messages); i++) {
            uint32_t j = 0;
            json_message = json_value_init_object();
            json_object_set_number(json_object(json_message), "type", (double)ebm_header[j]);
            if (ebm_header[j] > 0x10)
                    fprintf(stderr, "WARNING: Unexpected header type 0x%08x\n", ebm_header[j]);
            json_object_set_number(json_object(json_message), "voice_id", (double)ebm_header[++j]);
            if (ebm_header[++j] != 0)
                json_object_set_number(json_object(json_message), "unknown1", (double)ebm_header[j]);
            json_object_set_number(json_object(json_message), "name_id", (double)ebm_header[++j]);
            if (ebm_header[++j] != 0)
                json_object_set_number(json_object(json_message), "extra_id", (double)ebm_header[j]);
            json_object_set_number(json_object(json_message), "expr_id", (double)ebm_header[++j]);
            if (duration1_length[d1] > 0) {
                JSON_Value* json_duration_array = json_value_init_array();
                for (uint32_t x = 0; x < duration1_length[d1]; x++)
                    json_array_append_number(json_array(json_duration_array), ebm_header[++j]);
                json_object_set_value(json_object(json_message), "duration1", json_duration_array);
            }
            json_object_set_number(json_object(json_message), "msg_id", (double)ebm_header[++j]);
            if (ebm_header[++j] != 0)
                json_object_set_number(json_object(json_message), "unknown2", (double)ebm_header[j]);
            // Don't store str_length since we'll reconstruct it
            uint32_t str_length = ebm_header[++j];
            if (str_length > MAX_STRING_SIZE) {
                fprintf(stderr, "ERROR: Unexpected string size\n");
                goto out;
            }
            char* str = (char*)&ebm_header[++j];
            json_object_set_string(json_object(json_message), "msg_string", str);
            ebm_header = (uint32_t*)&str[str_length];
            if (duration2_length[d2] > 0) {
                JSON_Value* json_duration_array = json_value_init_array();
                for (uint32_t x = 0; x < duration2_length[d2]; x++)
                    json_array_append_number(json_array(json_duration_array), *ebm_header++);
                json_object_set_value(json_object(json_message), "duration2", json_duration_array);
            }
            json_array_append_value(json_array(json_messages), json_message);
        }
        JSON_Value* json_extra_data = json_value_init_array();
        while ((uintptr_t)ebm_header < (uintptr_t)&buf[buf_size])
            json_array_append_number(json_array(json_extra_data), *ebm_header++);
        json_object_set_value(json_object(json), "messages", json_messages);
        if (json_array_get_count(json_array(json_extra_data)) > 0)
            json_object_set_value(json_object(json), "extra_data", json_extra_data);
        else
            json_value_free(json_extra_data);
        snprintf(path, sizeof(path), "%s%c%s", _dirname(argv[argc - 1]), PATH_SEP,
            change_extension(_basename(argv[argc - 1]), ".json"));
        printf("Creating '%s'\n", path);
        json_serialize_to_file_pretty(json, path);
        r = 0;
    } else {
        fprintf(stderr, "ERROR: You must specify a .ebm or .json file");
    }

out:
    json_value_free(json);
    free(buf);
    if (file != NULL)
        fclose(file);

    if (r != 0) {
        fflush(stdin);
        printf("\nPress any key to continue...");
        (void)getchar();
    }

    return r;
}

CALL_MAIN
