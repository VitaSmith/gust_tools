/*
  gust_pak - PAK archive unpacker for Gust (Koei/Tecmo) PC games
  Copyright © 2019-2025 VitaSmith
  Copyright © 2018 Yuri Hime (shizukachan)

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
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "utf8.h"
#include "util.h"
#include "parson.h"

#define A17_KEY_SIZE        20
#define A22_KEY_SIZE        32
#define MAX_KEY_SIZE        32
#define CURRENT_KEY_SIZE    (is_a22 ? A22_KEY_SIZE : A17_KEY_SIZE)
#define FILENAME_SIZE       128

#pragma pack(push, 1)
typedef struct {
    uint32_t version;
    uint32_t nb_files;
    uint32_t header_size;
    uint32_t flags;
} pak_header;

typedef struct {
    char     filename[FILENAME_SIZE];
    uint32_t size;
    uint8_t  key[A17_KEY_SIZE];
    uint32_t data_offset;
    uint32_t flags;
} pak_entry32;

typedef struct {
    char     filename[FILENAME_SIZE];
    uint32_t size;
    uint8_t  key[A17_KEY_SIZE];
    uint64_t data_offset;
    uint64_t flags;
} pak_entry64;

typedef struct {
    char     filename[FILENAME_SIZE];
    uint32_t size;
    uint8_t  key[A22_KEY_SIZE];
    uint32_t extra;
    uint64_t data_offset;
    uint64_t flags;
} pak_entry64_a22;
#pragma pack(pop)

#define MAX_PAK_ENTRY_SIZE  sizeof(pak_entry64_a22)
#define CURRENT_ENTRY_SIZE  (is_pak64 ? (is_a22 ? sizeof(pak_entry64_a22) : sizeof(pak_entry64)) : sizeof(pak_entry32))

//
// Per-game master keys that are used to decrypt data for A23 and later games.
//
// Note that the key below was derived directly from the PAK data rather than
// extracted from the game executable, where it also resides (and we will be
// happy to submit *FORMAL PROOF* of this, if legally challenged).
// As a result, because it was derived directly from the encrypted data, we
// did not have to circumvent any means of copy protection or breach the
// license agreement and therefore consider that there exist no legal barrier
// to publishing it in this source, per "clean room design" rules.
//
static char* master_key[][2] = {
    { "", "" },                                     // No master key
    { "A23", "dGGKXLHLuCJwv8aBc3YQX6X6sREVPchs" },  // A23 master key (Sophie 2)
    { "A24", "fyrixtT9AhA4v0cFahgMcgVwxFrry42A" },  // A24/A29 master key (Ryza 3/DX)
    { "A25", "vh0WESTtbeBuTwWusr4EVusMi4TbLmjQ" },  // A25 master key (Yumia)
    { "A26", "xpFLmgBWJSFFgMFg3XUgs7B7AgYJhyFJ" },  // A26 master key (Resleriana)
    { "A27", "EGkUyYzPidcRx5yTecqk5AYJeSzzc5BR" },  // A27 master key (Ryza 1 DX)
    { "A28", "ivZBFzn82RVuYnmgXfeFEgk33ALZcZDP" },  // A28 master key (Ryza 2 DX)
};
const char* mk = NULL;

static __inline void decode(uint8_t* a, uint8_t* k, uint32_t size, uint32_t key_size)
{
    // We may call decode() multiple times so make sure we preserve the original key
    uint8_t _k[MAX_KEY_SIZE];
    if (mk[0] != 0) {
        // XOR with the master key
        for (uint32_t i = 0; i < key_size; i++)
            _k[i] = k[i] ^ mk[i];
        k = _k;
    }
    for (uint32_t i = 0; i < size; i++)
        a[i] ^= k[i % key_size];
}

static char* key_to_string(uint8_t* key, uint32_t key_size)
{
    static char key_string[2 * MAX_KEY_SIZE + 1];
    for (size_t i = 0; i < key_size; i++) {
        key_string[2 * i] = ((key[i] >> 4) < 10) ? '0' + (key[i] >> 4) : 'a' + (key[i] >> 4) - 10;
        key_string[2 * i + 1] = ((key[i] & 0xf) < 10) ? '0' + (key[i] & 0xf) : 'a' + (key[i] & 0xf) - 10;
    }
    key_string[2 * key_size] = 0;
    return key_string;
}

static uint8_t* string_to_key(const char* str, uint32_t key_size)
{
    static uint8_t key[MAX_KEY_SIZE];
    for (size_t i = 0; i < key_size; i++) {
        key[i] = (str[2 * i] >= 'a') ? str[2 * i] - 'a' + 10 : str[2 * i] - '0';
        key[i] <<= 4;
        key[i] += (str[2 * i + 1] >= 'a') ? str[2 * i + 1] - 'a' + 10 : str[2 * i + 1] - '0';
    }
    return key;
}

uint32_t alphanum_score(const char* str, size_t len)
{
    uint32_t score = 0, num_chars = 1;
    for (uint32_t i = 0; i < len; i++) {
        char c = str[i];
        if (c == '.' || c == '_' || c == '-' || c == '\\' || c == '/' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            num_chars++;
            continue;
        }
        score += (c > 0x7E) ? 0x1000 : 0x10;
    }
    return score / num_chars;
}

// To handle either 32 or 64 bit PAK entries
#define entries32     ((pak_entry32*)entries)
#define entries64     ((pak_entry64*)entries)
#define entries64_a22 ((pak_entry64_a22*)entries)
#define entry(i, m) (is_pak64 ? (is_a22 ? (entries64_a22[i]).m : (entries64[i]).m) : (entries32[i]).m)
#define set_entry(i, m, v) do { if (is_pak64) { if (is_a22) (entries64_a22[i]).m = v; else (entries64[i]).m = v; } \
                                else (entries32[i]).m = (uint32_t)(v);} while(0)

int main_utf8(int argc, char** argv)
{
    int r = -1;
    FILE* file = NULL;
    uint8_t zero_key[MAX_KEY_SIZE] = { 0 }, *buf = NULL;
    char path[PATH_MAX];
    pak_header hdr = { 0 };
    void* entries = NULL;
    JSON_Value* json = NULL;
    bool is_pak64 = false, is_a22 = false, list_only = false, invalid_args = (argc < 2);

    for (int i = 1; i < argc - 1; i++) {
        char* cur = argv[i];
        if (strcmp(cur, "-l") == 0)
            list_only = true;
        else if (strcmp(cur, "-k") == 0 && i + 2 < argc)
            mk = argv[++i];
        else
            invalid_args = true;
    }

    if (invalid_args) {
        printf("%s %s (c) 2018-2025 Yuri Hime & VitaSmith\n\n"
            "Usage: %s [-l] [-k <GameKey>] <Gust PAK file>\n\n"
            "Extracts (.pak) or recreates (.json) a Gust .pak archive.\n\n",
            _appname(argv[0]), GUST_TOOLS_VERSION_STR, _appname(argv[0]));
        return 0;
    }

    if (is_directory(argv[argc - 1])) {
        fprintf(stderr, "ERROR: Directory packing is not supported.\n"
            "To recreate a .pak you need to use the corresponding .json file.\n");
    } else if (strstr(argv[argc - 1], ".json") != NULL) {
        if (list_only) {
            fprintf(stderr, "ERROR: Option -l is not supported when creating an archive\n");
            goto out;
        }
        json = json_parse_file_with_comments(argv[argc - 1]);
        if (json == NULL) {
            fprintf(stderr, "ERROR: Can't parse JSON data from '%s'\n", argv[argc - 1]);
            goto out;
        }
        const char* filename = json_object_get_string(json_object(json), "name");
        hdr.header_size = json_object_get_uint32(json_object(json), "header_size");
        if ((filename == NULL) || (hdr.header_size != sizeof(pak_header))) {
            fprintf(stderr, "ERROR: No filename/wrong header size\n");
            goto out;
        }
        hdr.version = json_object_get_uint32(json_object(json), "version");
        hdr.flags = json_object_get_uint32(json_object(json), "flags");
        hdr.nb_files = json_object_get_uint32(json_object(json), "nb_files");
        if (mk == NULL) {
            mk = json_object_get_string(json_object(json), "master_key");
            if (mk == NULL)
                mk = master_key[0][1];
        }
        is_pak64 = json_object_get_boolean(json_object(json), "64-bit");
        is_a22 = json_object_get_boolean(json_object(json), "a22-extensions");
        if (is_a22 && !is_pak64) {
            fprintf(stderr, "ERROR: A22 extensions can only be used on 64-bit PAKs\n");
            goto out;
        }
        snprintf(path, sizeof(path), "%s%c%s", _dirname(argv[argc - 1]), PATH_SEP, filename);
        printf("Creating '%s'...\n", path);
        create_backup(path);
        file = fopen_utf8(path, "wb+");
        if (file == NULL) {
            fprintf(stderr, "ERROR: Can't create file '%s'\n", path);
            goto out;
        }
        if (fwrite(&hdr, sizeof(pak_header), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't write PAK header\n");
            goto out;
        }
        entries = calloc(hdr.nb_files, CURRENT_ENTRY_SIZE);
        if (entries == NULL) {
            fprintf(stderr, "ERROR: Can't allocate entries\n");
            goto out;
        }
        // Write a dummy table for now
        if (fwrite(entries, CURRENT_ENTRY_SIZE, hdr.nb_files, file) != hdr.nb_files) {
            fprintf(stderr, "ERROR: Can't write initial PAK table\n");
            goto out;
        }
        uint64_t file_data_offset = ftell64(file);

        JSON_Array* json_files_array = json_object_get_array(json_object(json), "files");
        printf("OFFSET    SIZE     NAME\n");
        for (uint32_t i = 0; i < hdr.nb_files; i++) {
            JSON_Object* file_entry = json_array_get_object(json_files_array, i);
            uint8_t* key = string_to_key(json_object_get_string(file_entry, "key"), CURRENT_KEY_SIZE);
            filename = json_object_get_string(file_entry, "name");
            strncpy(entry(i, filename), filename, FILENAME_SIZE - 1);
            snprintf(path, sizeof(path), "%s%c%s", _dirname(argv[argc - 1]), PATH_SEP, filename);
            for (size_t n = 0; n < strlen(path); n++) {
                if (path[n] == '\\' || path[n] == '/')
                    path[n] = PATH_SEP;
            }
            set_entry(i, size, read_file(path, &buf));
            if (entry(i, size) == UINT32_MAX)
                goto out;
            bool encode_data = hdr.flags & 0x10;
            for (int j = 0; encode_data == false && j < CURRENT_KEY_SIZE; j++) {
                entry(i, key)[j] = key[j];
                if (key[j] != 0)
                    encode_data = true;
            }
            set_entry(i, data_offset, ftell64(file) - file_data_offset);
            uint64_t flags = json_object_get_uint64(file_entry, "flags");
            if (is_pak64)
                setbe64(is_a22 ? &(entries64_a22[i].flags) : &(entries64[i].flags), flags);
            else
                setbe32(&(entries32[i].flags), (uint32_t)flags);
            if (is_a22)
                setbe32(&(entries64_a22[i].extra), json_object_get_uint32(file_entry, "extra"));
            printf("%09" PRIx64 " %08x %s%c\n", entry(i, data_offset) + file_data_offset,
                entry(i, size), entry(i, filename), encode_data ? ' ' : '*');
            if (encode_data) {
                decode((uint8_t*)entry(i, filename), entry(i, key), FILENAME_SIZE, CURRENT_KEY_SIZE);
                decode(buf, entry(i, key), entry(i, size), CURRENT_KEY_SIZE);
            }
            if (fwrite(buf, 1, entry(i, size), file) != entry(i, size)) {
                fprintf(stderr, "ERROR: Can't write data for '%s'\n", path);
                goto out;
            }
            free(buf);
            buf = NULL;
        }
        fseek64(file, sizeof(pak_header), SEEK_SET);
        if (fwrite(entries, CURRENT_ENTRY_SIZE, hdr.nb_files, file) != hdr.nb_files) {
            fprintf(stderr, "ERROR: Can't write PAK table\n");
            goto out;
        }
        r = 0;
    } else {
        printf("%s '%s'...\n", list_only ? "Listing" : "Extracting", _basename(argv[argc - 1]));
        file = fopen_utf8(argv[argc - 1], "rb");
        if (file == NULL) {
            fprintf(stderr, "ERROR: Can't open PAK file '%s'", argv[argc - 1]);
            goto out;
        }

        if (fread(&hdr, sizeof(hdr), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't read hdr");
            goto out;
        }

        if ((hdr.version != 0x20000) || (hdr.header_size != sizeof(pak_header))) {
            fprintf(stderr, "ERROR: Signature doesn't match expected PAK file format.\n");
            goto out;
        }
        if (hdr.nb_files > 65536) {
            fprintf(stderr, "ERROR: Too many entries (%d).\n", hdr.nb_files);
            goto out;
        }

        entries = calloc(hdr.nb_files, MAX_PAK_ENTRY_SIZE);
        if (entries == NULL) {
            fprintf(stderr, "ERROR: Can't allocate entries\n");
            goto out;
        }

        if (fread(entries, MAX_PAK_ENTRY_SIZE, hdr.nb_files, file) != hdr.nb_files) {
            fprintf(stderr, "ERROR: Can't read PAK hdr\n");
            goto out;
        }

        // Detect if we are dealing with 32 or 64-bit pak entries by checking
        // the data_offsets at the expected 32 and 64-bit struct location and
        // adding the absolute value of the difference with last data_offset.
        // The sum that is closest to zero tells us if we are dealing with a
        // 32 or 64-bit PAK archive, as well as if it uses A22 extensions.
        uint64_t sum[3] = { 0, 0, 0 };
        uint32_t val[3], last[3] = { 0, 0, 0 };
        for (uint32_t i = 0; i < min(hdr.nb_files, 64); i++) {
            val[0] = ((pak_entry32*)entries)[i].data_offset;
            val[1] = (uint32_t)(((pak_entry64*)entries)[i].data_offset >> 32);
            val[2] = (uint32_t)(((pak_entry64_a22*)entries)[i].data_offset >> 32);
            for (int j = 0; j < 3; j++) {
                sum[j] += (val[j] > last[j]) ? val[j] - last[j] : last[j] - val[j];
                last[j] = val[j];
            }
        }
        is_pak64 = min(sum[0], min(sum[1], sum[2])) == min(sum[1], sum[2]);
        is_a22 = is_pak64 && (min(sum[1], sum[2]) == sum[2]);
        printf("Detected %s PAK format\n", is_pak64 ? (is_a22 ? "A22/64-bit" : "A18/64-bit") : "A17/32-bit");

        if (mk == NULL) {
            // Determine the master key that needs to be applied, if any
            char filename[FILENAME_SIZE];
            uint32_t weight[array_size(master_key)], best_score, best_weight, best_k, increment = 1;
            memset(weight, 0, array_size(master_key) * sizeof(uint32_t));
            // 128-255 entries should be enough for our detection
            if (hdr.nb_files > 0x80)
                increment = hdr.nb_files / (hdr.nb_files / 0x80);
            for (uint32_t i = 0; i < hdr.nb_files; i += increment) {
                bool decode_data = (hdr.flags & 0x10) || (memcmp(zero_key, entry(i, key), CURRENT_KEY_SIZE) != 0);
                if (decode_data) {
                    best_score = UINT32_MAX;
                    best_k = 0;
                    for (uint32_t k = 0; k < array_size(master_key); k++) {
                        mk = master_key[k][1];
                        memcpy(filename, entry(i, filename), FILENAME_SIZE);
                        decode((uint8_t*)filename, entry(i, key), FILENAME_SIZE, CURRENT_KEY_SIZE);
                        uint32_t score = alphanum_score(filename, strnlen(filename, 0x20));
                        if (score < best_score) {
                            best_score = score;
                            best_k = k;
                        }
                    }
                    weight[best_k]++;
                }
            }
            best_k = 0;
            best_weight = 0;
            for (uint32_t k = 0; k < array_size(master_key); k++) {
                if (weight[k] > best_weight) {
                    best_weight = weight[k];
                    best_k = k;
                }
            }
            mk = master_key[best_k][1];
            if (mk[0] != 0)
                printf("Using %s master key\n", master_key[best_k][0]);
            printf("\n");
        }

        // Store the data we'll need to reconstruct the archive to a JSON file
        json = json_value_init_object();
        json_object_set_string(json_object(json), "name", change_extension(_basename(argv[argc - 1]), ".pak"));
        json_object_set_number(json_object(json), "version", hdr.version);
        json_object_set_number(json_object(json), "header_size", hdr.header_size);
        json_object_set_number(json_object(json), "flags", hdr.flags);
        json_object_set_number(json_object(json), "nb_files", hdr.nb_files);
        json_object_set_boolean(json_object(json), "64-bit", is_pak64);
        if (is_a22)
            json_object_set_boolean(json_object(json), "a22-extensions", true);
        if (mk[0] != 0)
            json_object_set_string(json_object(json), "master_key", mk);

        uint64_t file_data_offset = sizeof(pak_header) + (uint64_t)hdr.nb_files * CURRENT_ENTRY_SIZE;
        JSON_Value* json_files_array = json_value_init_array();
        printf("OFFSET    SIZE     NAME\n");
        for (uint32_t i = 0; i < hdr.nb_files; i++) {
            bool decode_data = (hdr.flags & 0x10) || (memcmp(zero_key, entry(i, key), CURRENT_KEY_SIZE) != 0);
            if (decode_data) {
                decode((uint8_t*)entry(i, filename), entry(i, key), FILENAME_SIZE, CURRENT_KEY_SIZE);
                for (int j = 0; j < FILENAME_SIZE && entry(i, filename)[j] != 0; j++) {
                    char c = entry(i, filename)[j];
                    if (c == 0)
                        break;
                    if (c < 0x20 || c > 0x7e) {
                        fprintf(stderr, "ERROR: Failed to decode filename for entry %d\n", i);
                        if (i == 0)
                            fprintf(stderr, "This is most likely because this game needs a new key.\n");
                        goto out;
                    }
                }
            }
            printf("%09" PRIx64 " %08x %s%c\n", entry(i, data_offset) + file_data_offset,
                entry(i, size), entry(i, filename), decode_data ? ' ' : '*');
            if (list_only)
                continue;
            JSON_Value* json_file = json_value_init_object();
            json_object_set_string(json_object(json_file), "name", entry(i, filename));
            json_object_set_string(json_object(json_file), "key", key_to_string(entry(i, key), CURRENT_KEY_SIZE));
            uint64_t flags = is_pak64 ? (is_a22 ?
                getbe64(&entries64_a22[i].flags) : getbe64(&entries64[i].flags)): getbe32(&entries32[i].flags);
            if (flags != 0)
                json_object_set_number(json_object(json_file), "flags", (double)flags);
            if (is_a22 && (getbe32(&entries64_a22[i].extra) != 0))
                json_object_set_number(json_object(json_file), "extra", (double)getbe32(&entries64_a22[i].extra));

            json_array_append_value(json_array(json_files_array), json_file);
            snprintf(path, sizeof(path), "%s%c%s", _dirname(argv[argc - 1]), PATH_SEP, entry(i, filename));
            for (size_t n = 0; n < strlen(path); n++) {
                if (path[n] == '\\' || path[n] == '/')
                    path[n] = PATH_SEP;
            }
            if (!create_path(_dirname(path))) {
                fprintf(stderr, "ERROR: Can't create path '%s'\n", _dirname(path));
                goto out;
            }
            fseek64(file, entry(i, data_offset) + file_data_offset, SEEK_SET);
            buf = (uint8_t*)malloc(entry(i, size));
            if (buf == NULL) {
                fprintf(stderr, "ERROR: Can't allocate entries\n");
                goto out;
            }
            if (fread(buf, 1, entry(i, size), file) != entry(i, size)) {
                fprintf(stderr, "ERROR: Can't read archive\n");
                goto out;
            }
            if (decode_data)
                decode(buf, entry(i, key), entry(i, size), CURRENT_KEY_SIZE);
            if (!write_file(buf, entry(i, size), path, false))
                goto out;
            free(buf);
            buf = NULL;
        }

        if (!list_only) {
            json_object_set_value(json_object(json), "files", json_files_array);
            snprintf(path, sizeof(path), "%s%c%s", _dirname(argv[argc - 1]), PATH_SEP,
                change_extension(_basename(argv[argc - 1]), ".json"));
            printf("Creating '%s'\n", path);
            json_serialize_to_file_pretty(json, path);
        } else {
            json_value_free(json_files_array);
        }
        r = 0;
    }

out:
    json_value_free(json);
    free(buf);
    free(entries);
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
