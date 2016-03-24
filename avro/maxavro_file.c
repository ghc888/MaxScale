/*
 * This file is distributed as part of the MariaDB Corporation MaxScale.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright MariaDB Corporation Ab 2016
 */

#include "maxavro.h"
#include <errno.h>
#include <string.h>
#include <log_manager.h>


static bool maxavro_read_sync(FILE *file, uint8_t* sync)
{
    return fread(sync, 1, SYNC_MARKER_SIZE, file) == SYNC_MARKER_SIZE;
}

bool maxavro_verify_block(MAXAVRO_FILE *file)
{
    char sync[SYNC_MARKER_SIZE];
    int rc = fread(sync, 1, SYNC_MARKER_SIZE, file->file);
    if (rc != SYNC_MARKER_SIZE)
    {
        if (rc == -1)
        {
            MXS_ERROR("Failed to read file: %d %s", errno, strerror(errno));
        }
        else
        {
            MXS_ERROR("Short read when reading sync marker. Read %d bytes instead of %d",
                      rc, SYNC_MARKER_SIZE);
        }
        return false;
    }

    if (memcmp(file->sync, sync, SYNC_MARKER_SIZE))
    {
        MXS_ERROR("Sync marker mismatch.");
        return false;
    }

    /** Increment block count */
    file->blocks_read++;
    file->bytes_read += file->block_size;
    return true;
}

bool maxavro_read_datablock_start(MAXAVRO_FILE* file)
{
    uint64_t records, bytes;
    bool rval = maxavro_read_integer(file, &records) && maxavro_read_integer(file, &bytes);

    if (rval)
    {
        file->block_size = bytes;
        file->records_in_block = records;
        file->records_read_from_block = 0;
        file->block_start_pos = ftell(file->file);
    }
    else if (maxavro_get_error(file) != MAXAVRO_ERR_NONE)
    {
        MXS_ERROR("Failed to read data block start.");
    }
    return rval;
}

/** The header metadata is encoded as an Avro map with @c bytes encoded
 * key-value pairs. A @c bytes value is written as a length encoded string
 * where the length of the value is stored as a @c long followed by the
 * actual data. */
static char* read_schema(MAXAVRO_FILE* file)
{
    char *rval = NULL;
    MAXAVRO_MAP* head = maxavro_map_read(file);
    MAXAVRO_MAP* map = head;

    while (map)
    {
        if (strcmp(map->key, "avro.schema") == 0)
        {
            rval = strdup(map->value);
            break;
        }
        map = map->next;
    }

    if (rval == NULL)
    {
        MXS_ERROR("No schema found from Avro header.");
    }

    maxavro_map_free(head);
    return rval;
}

/**
 * @brief Open an avro file
 *
 * This function performs checks on the file header and creates an internal
 * representation of the file's schema. This schema can be accessed for more
 * information about the fields.
 * @param filename File to open
 * @return Pointer to opened file or NULL if an error occurred
 */
MAXAVRO_FILE* maxavro_file_open(const char* filename)
{
    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        MXS_ERROR("Failed to open file '%s': %d, %s", filename, errno, strerror(errno));
        return NULL;
    }

    char magic[AVRO_MAGIC_SIZE];

    if (fread(magic, 1, AVRO_MAGIC_SIZE, file) != AVRO_MAGIC_SIZE)
    {
        fclose(file);
        MXS_ERROR("Failed to read file magic marker from '%s'", filename);
        return NULL;
    }

    if (memcmp(magic, avro_magic, AVRO_MAGIC_SIZE) != 0)
    {
        fclose(file);
        MXS_ERROR("Error: Avro magic marker bytes are not correct.");
        return NULL;
    }

    MAXAVRO_FILE* avrofile = calloc(1, sizeof(MAXAVRO_FILE));

    if (avrofile)
    {
        avrofile->file = file;
        avrofile->filename = strdup(filename);
        char *schema = read_schema(avrofile);
        avrofile->schema = schema ? maxavro_schema_alloc(schema) : NULL;
        avrofile->last_error = MAXAVRO_ERR_NONE;

        if (!schema || !avrofile->schema ||
            !maxavro_read_sync(file, avrofile->sync) ||
            !maxavro_read_datablock_start(avrofile))
        {
            MXS_ERROR("Failed to initialize avrofile.");
            free(avrofile->schema);
            free(avrofile);
            avrofile = NULL;
        }
        free(schema);
    }
    else
    {
        fclose(file);
        free(avrofile);
        avrofile = NULL;
    }

    return avrofile;
}

/**
 * @brief Return the last error from the file
 * @param file File to check
 * @return The last error or MAXAVRO_ERR_NONE if no errors have occurred
 */
enum maxavro_error maxavro_get_error(MAXAVRO_FILE *file)
{
    return file->last_error;
}

/**
 * @brief Get the error string for this file
 * @param file File to check
 * @return Error in string form
 */
const char* maxavro_get_error_string(MAXAVRO_FILE *file)
{
    switch (file->last_error)
    {
        case MAXAVRO_ERR_IO:
            return "MAXAVRO_ERR_IO";

        case MAXAVRO_ERR_MEMORY:
            return "MAXAVRO_ERR_MEMORY";

        case MAXAVRO_ERR_VALUE_OVERFLOW:
            return "MAXAVRO_ERR_VALUE_OVERFLOW";

        case MAXAVRO_ERR_NONE:
            return "MAXAVRO_ERR_NONE";

        default:
            return "UNKNOWN ERROR";
    }
}

/**
 * @brief Close an avro file
 * @param file File to close
 */
void maxavro_file_close(MAXAVRO_FILE *file)
{
    fclose(file->file);
    free(file->filename);
    maxavro_schema_free(file->schema);
    free(file);
}
