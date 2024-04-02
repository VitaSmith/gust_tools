/*
  gust_elixir - Archive unpacker for Gust (Koei/Tecmo) .elixir[.gz] files
  Copyright © 2019-2021 VitaSmith

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
#ifndef _WIN32
#include <unistd.h>
#define _unlink unlink
#endif

#include "utf8.h"
#include "util.h"
#include "parson.h"

#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_MALLOC
#include "miniz_tinfl.h"
#include "miniz_tdef.h"

#define JSON_VERSION            1
#define EARC_MAGIC              0x45415243  // 'EARC'
#define DEFAULT_CHUNK_SIZE      0x4000
#define REPORT_URL              "https://github.com/VitaSmith/gust_tools/issues"

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t filename_size;
    uint32_t payload_size;
    uint32_t header_size;
    uint32_t table_size;
    uint32_t nb_files;
    uint32_t flags;             // Can be 0x0 or 0xA
} lxr_header;

typedef struct {
    uint32_t offset;
    uint32_t size;
    char     filename[0x20];    // May be extended by (filename_size * 0x10)
} lxr_entry;
#pragma pack(pop)

int32_t decompress_mem_to_mem(void* pOut_buf, size_t out_buf_len, const void* pSrc_buf, size_t src_buf_len, int flags)
{
    tinfl_decompressor decomp;
    tinfl_status status;
    tinfl_init(&decomp);
    status = tinfl_decompress(&decomp, (const mz_uint8*)pSrc_buf, &src_buf_len, (mz_uint8*)pOut_buf,
        (mz_uint8*)pOut_buf, &out_buf_len, (flags & ~TINFL_FLAG_HAS_MORE_INPUT) | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    switch (status) {
    case TINFL_STATUS_DONE:
        return (int32_t)out_buf_len;
    case TINFL_STATUS_HAS_MORE_OUTPUT:
        return -2;
    default:
        return -1;
    }
}

int main_utf8(int argc, char** argv)
{
    int r = -1;
    char path[256];
    uint8_t *buf = NULL, *zbuf = NULL;
    uint32_t zsize, lxr_entry_size = sizeof(lxr_entry);
    FILE *file = NULL, *dst = NULL;
    JSON_Value* json = NULL;
    tdefl_compressor* compressor = NULL;
    lxr_entry* table = NULL;
    bool list_only = (argc == 3) && (argv[1][0] == '-') && (argv[1][1] == 'l');
    bool decompress_only = (argc == 3) && (argv[1][0] == '-') && (argv[1][1] == 'd');

    if ((argc != 2) && !list_only && !decompress_only) {
        printf("%s %s (c) 2019-2021 VitaSmith\n\n"
            "Usage: %s [-d] [-l] <elixir[.gz]> file>\n\n"
            "Extracts (file) or recreates (directory) a Gust .elixir archive.\n\n"
            "Note: A backup (.bak) of the original is automatically created, when the target\n"
            "is being overwritten for the first time.\n",
            _appname(argv[0]), GUST_TOOLS_VERSION_STR, _appname(argv[0]));
        return 0;
    }

    if (is_directory(argv[argc - 1])) {
        if (list_only) {
            fprintf(stderr, "ERROR: Option -l is not supported when creating an archive\n");
            goto out;
        }
        if (decompress_only) {
            fprintf(stderr, "ERROR: Option -d is not supported when creating an archive\n");
            goto out;
        }
        snprintf(path, sizeof(path), "%s%celixir.json", argv[argc - 1], PATH_SEP);
        if (!is_file(path)) {
            fprintf(stderr, "ERROR: '%s' does not exist\n", path);
            goto out;
        }
        json = json_parse_file_with_comments(path);
        if (json == NULL) {
            fprintf(stderr, "ERROR: Can't parse JSON data from '%s'\n", path);
            goto out;
        }
        //const uint32_t json_version = json_object_get_uint32(json_object(json), "json_version");
        //if (json_version != JSON_VERSION) {
        //    fprintf(stderr, "ERROR: This utility is not compatible with the JSON file provided.\n"
        //        "You need to (re)extract the original archive using this application.\n");
        //    goto out;
        //}
        const char* filename = json_object_get_string(json_object(json), "name");
        if (filename == NULL)
            goto out;
        printf("Creating '%s'...\n", filename);
        create_backup(filename);
        // Work with a temporary file if we're going to compress it
        if (json_object_get_boolean(json_object(json), "compressed"))
            snprintf(path, sizeof(path), "%s.tmp", filename);
        else
            strncpy(path, filename, sizeof(path));
        path[sizeof(path) - 1] = 0;
        file = fopen_utf8(path, "wb+");
        if (file == NULL) {
            fprintf(stderr, "ERROR: Can't create file '%s'\n", path);
            goto out;
        }
        lxr_header hdr = { 0 };
        hdr.magic = EARC_MAGIC;
        hdr.header_size = (uint32_t)sizeof(lxr_header);
        hdr.flags = json_object_get_uint32(json_object(json), "flags");
        JSON_Array* json_files_array = json_object_get_array(json_object(json), "files");
        hdr.nb_files = (uint32_t)json_array_get_count(json_files_array);
        if (hdr.nb_files == 0) {
            fprintf(stderr, "ERROR: JSON files array is either missing or empty\n");
            goto out;
        }
        uint32_t max_filename_length = 0x30;
        for (uint32_t i = 0; i < hdr.nb_files; i++)
            max_filename_length = max(max_filename_length,
                (uint32_t)strlen(json_array_get_string(json_files_array, i) + 1));
        hdr.filename_size = (max_filename_length - 0x20 + 0x0f) / 0x10;
        lxr_entry_size += hdr.filename_size * 0x10;
        hdr.table_size = hdr.nb_files * lxr_entry_size;
        if (fwrite(&hdr, sizeof(hdr), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't write header\n");
            goto out;
        }
        table = (lxr_entry*)calloc(hdr.nb_files, lxr_entry_size);
        // Allocate the space in file - we'll update it later on
        if (fwrite(table, lxr_entry_size, hdr.nb_files, file) != hdr.nb_files) {
            fprintf(stderr, "ERROR: Can't write header table\n");
            goto out;
        }
        printf("OFFSET   SIZE     NAME\n");
        lxr_entry* entry = table;
        const char* entry_name;
        for (uint32_t i = 0; i < hdr.nb_files; i++) {
            entry->size = 0;
            entry->offset = ftell(file);
            entry_name = json_array_get_string(json_files_array, i);
            snprintf(path, sizeof(path), "%s%c%s", _basename(argv[argc - 1]), PATH_SEP, entry_name);
            if (strcmp(entry_name, "dummy") != 0) {
                entry->size = read_file(path, &buf);
                if (entry->size == UINT32_MAX)
                    goto out;
            }
            strncpy(entry->filename, json_array_get_string(json_files_array, i),
                0x20 + ((size_t)hdr.filename_size * 0x10));
            printf("%08x %08x %s\n", entry->offset, entry->size, path);
            if ((entry->size != 0) && (fwrite(buf, 1, entry->size, file) != entry->size)) {
                fprintf(stderr, "ERROR: Can't add file data\n");
                goto out;
            }
            free(buf);
            buf = NULL;
            entry = (lxr_entry*) &((uint8_t*)entry)[lxr_entry_size];
        }
        hdr.payload_size = ftell(file) - hdr.header_size - hdr.table_size;
        fseek(file, 2 * sizeof(uint32_t), SEEK_SET);
        if (fwrite(&hdr.payload_size, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't update file size\n");
            goto out;
        }
        fseek(file, hdr.header_size, SEEK_SET);
        if (fwrite(table, lxr_entry_size, hdr.nb_files, file) != hdr.nb_files) {
            fprintf(stderr, "ERROR: Can't write header table\n");
            goto out;
        }

        if (json_object_get_boolean(json_object(json), "compressed")) {
            printf("Compressing...\n");
            compressor = (tdefl_compressor*)calloc(1, sizeof(tdefl_compressor));
            if (compressor == NULL)
                goto out;
            dst = fopen_utf8(filename, "wb");
            if (dst == NULL) {
                fprintf(stderr, "ERROR: Can't create compressed file\n");
                goto out;
            }
            fseek(file, 0, SEEK_SET);
            buf = (uint8_t*)malloc(DEFAULT_CHUNK_SIZE);
            zbuf = (uint8_t*)malloc(DEFAULT_CHUNK_SIZE);
            while (1) {
                size_t written = DEFAULT_CHUNK_SIZE;
                size_t read = fread(buf, 1, DEFAULT_CHUNK_SIZE, file);
                if (read == 0)
                    break;
                tdefl_status status = tdefl_init(compressor, NULL, NULL,
                    TDEFL_WRITE_ZLIB_HEADER | TDEFL_COMPUTE_ADLER32 | 256);
                if (status != TDEFL_STATUS_OKAY) {
                    fprintf(stderr, "ERROR: Can't init compressor\n");
                    goto out;
                }
                status = tdefl_compress(compressor, buf, &read, zbuf, &written, TDEFL_FINISH);
                if (status != TDEFL_STATUS_DONE) {
                    fprintf(stderr, "ERROR: Can't compress data\n");
                    goto out;
                }
                if (fwrite(&written, sizeof(uint32_t), 1, dst) != 1) {
                    fprintf(stderr, "ERROR: Can't write compressed stream size\n");
                    goto out;
                }
                if (fwrite(zbuf, 1, written, dst) != written) {
                    fprintf(stderr, "ERROR: Can't write compressed data\n");
                    goto out;
                }
            }
            uint32_t end_marker = 0;
            if (fwrite(&end_marker, sizeof(uint32_t), 1, dst) != 1) {
                fprintf(stderr, "ERROR: Can't write end marker\n");
                goto out;
            }
            fclose(file);
            file = NULL;
            snprintf(path, sizeof(path), "%s.tmp", filename);
            _unlink(path);
        }

        r = 0;
    } else {
        printf("%s '%s'...\n", list_only ? "Listing" :
            (decompress_only ? "Decompressing" : "Extracting"), _basename(argv[argc - 1]));
        char* elixir_pos = strstr(argv[argc - 1], ".elixir");
        if (elixir_pos == NULL) {
            fprintf(stderr, "ERROR: File should have a '.elixir[.gz]' extension\n");
            goto out;
        }
        char* gz_pos = strstr(argv[argc - 1], ".gz");

        file = fopen_utf8(argv[argc - 1], "rb");
        if (file == NULL) {
            fprintf(stderr, "ERROR: Can't open elixir file '%s'", argv[argc - 1]);
            goto out;
        }

        // Some elixir.gz files are actually uncompressed versions
        if (fread(&zsize, sizeof(zsize), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't read from elixir file '%s'", argv[argc - 1]);
            goto out;
        }
        if ((zsize == EARC_MAGIC) && (gz_pos != NULL))
            gz_pos = NULL;

        fseek(file, 0L, SEEK_END);
        size_t file_size = ftell(file);
        fseek(file, 0L, SEEK_SET);

        if (gz_pos != NULL) {
            file_size *= 2;
            buf = (uint8_t *)malloc(file_size);
            if (buf == NULL)
                goto out;
            size_t pos = 0;
            int32_t s = 0;
            while (1) {
                uint32_t file_pos = (uint32_t)ftell(file);
                if (fread(&zsize, sizeof(uint32_t), 1, file) != 1) {
                    fprintf(stderr, "ERROR: Can't read compressed stream size at position %08x\n", file_pos);
                    goto out;
                }
                if (zsize == 0)
                    break;
                zbuf = (uint8_t*)malloc(zsize);
                if (zbuf == NULL)
                    goto out;
                if (fread(zbuf, 1, zsize, file) != zsize) {
                    fprintf(stderr, "ERROR: Can't read compressed stream at position %08x\n", file_pos);
                    goto out;
                }
increase_buf_size:
                // Elixirs are inflated using a constant chunk size which simplifies overflow handling
                if ((s == -2) || (pos + DEFAULT_CHUNK_SIZE > file_size)) {
                    file_size *= 2;
                    uint8_t* old_buf = buf;
                    buf = (uint8_t *)realloc(buf, file_size);
                    if (buf == NULL) {
                        fprintf(stderr, "ERROR: Can't increase buffer size\n");
                        buf = old_buf;
                        goto out;
                    }
                }
                s = decompress_mem_to_mem(&buf[pos], file_size - pos, zbuf, zsize,
                    TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32);
                if (s == -2)
                    goto increase_buf_size;
                if (s <= 0) {
                    fprintf(stderr, "ERROR: Can't decompress stream at position %08x\n", file_pos);
                    goto out;
                }
                free(zbuf);
                zbuf = NULL;
                pos += s;
            } while (zsize != 0);
            file_size = pos;
            if (decompress_only) {
                *gz_pos = 0;
                dst = fopen(argv[argc - 1], "wb");
                if (dst == NULL) {
                    fprintf(stderr, "ERROR: Can't create file '%s'\n", argv[argc - 1]);
                    goto out;
                }
                if (fwrite(buf, 1, file_size, dst) != file_size) {
                    fprintf(stderr, "ERROR: Can't write file '%s'\n", argv[argc - 1]);
                    fclose(dst);
                    goto out;
                }
                printf("%08x %s\n", (uint32_t)file_size, _basename(argv[argc - 1]));
                r = 0;
                goto out;
            }
        } else {
            buf = (uint8_t*)malloc(file_size);
            if (buf == NULL)
                goto out;
            if (fread(buf, 1, file_size, file) != file_size) {
                fprintf(stderr, "ERROR: Can't read uncompressed data");
                goto out;
            }
        }

        // Now that we have an uncompressed .elixir file, extract the files
        json = json_value_init_object();
        json_object_set_number(json_object(json), "json_version", JSON_VERSION);
        json_object_set_string(json_object(json), "name", _basename(argv[argc - 1]));
        if (gz_pos != NULL)
            json_object_set_boolean(json_object(json), "compressed", (gz_pos != NULL));

        *elixir_pos = 0;
        if (!list_only && !create_path(argv[argc - 1]))
            goto out;

        lxr_header* hdr = (lxr_header*)buf;
        if (hdr->magic != EARC_MAGIC) {
            fprintf(stderr, "ERROR: Not an elixir file (bad magic)\n");
            goto out;
        }
        if (hdr->filename_size > 0x100) {
            fprintf(stderr, "ERROR: filename_size is too large (0x%08X)\n", hdr->filename_size);
            goto out;
        }
        if (hdr->header_size != 0x1C) {
            fprintf(stderr, "ERROR: Unexpected header size (0x%08X)\n", hdr->header_size);
            fprintf(stderr, "Please report this error to %s.\n", REPORT_URL);
            goto out;
        }
        lxr_entry_size += hdr->filename_size * 0x10;
        json_object_set_number(json_object(json), "flags", hdr->flags);

        if (hdr->nb_files * lxr_entry_size != hdr->table_size) {
            fprintf(stderr, "ERROR: Table size mismatch\n");
            goto out;
        }
        if (sizeof(lxr_header) + hdr->table_size + hdr->payload_size != file_size) {
            fprintf(stderr, "ERROR: File size mismatch\n");
            goto out;
        }

        JSON_Value* json_files_array = json_value_init_array();
        printf("OFFSET   SIZE     NAME\n");
        for (uint32_t i = 0; i < hdr->nb_files; i++) {
            lxr_entry* entry = (lxr_entry*)&buf[sizeof(lxr_header) + (size_t)i * lxr_entry_size];
            assert(entry->offset + entry->size <= (uint32_t)file_size);
            char* filename = (char*)calloc(0x20 + hdr->filename_size * 0x10 + 1, 1);
            if (filename == NULL)
                goto out;
            memcpy(filename, entry->filename, 0x20 + hdr->filename_size * 0x10);
            json_array_append_string(json_array(json_files_array), filename);
            snprintf(path, sizeof(path), "%s%c%s", argv[argc - 1], PATH_SEP, filename);
            free(filename);
            printf("%08x %08x %s\n", entry->offset, entry->size, path);
            if (list_only)
                continue;
            // No need to extract data for dummy entries
            if ((entry->size == 0) && (strcmp(entry->filename, "dummy") == 0))
                continue;
            if (!write_file(&buf[entry->offset], entry->size, path, false))
                goto out;
        }

        json_object_set_value(json_object(json), "files", json_files_array);
        snprintf(path, sizeof(path), "%s%celixir.json", argv[argc - 1], PATH_SEP);
        if (!list_only)
            json_serialize_to_file_pretty(json, path);

        r = 0;
    }

out:
    json_value_free(json);
    free(buf);
    free(zbuf);
    free(table);
    free(compressor);
    if (file != NULL)
        fclose(file);
    if (dst != NULL)
        fclose(dst);

    if (r != 0) {
        fflush(stdin);
        printf("\nPress any key to continue...");
        (void)getchar();
    }

    return r;
}

CALL_MAIN
