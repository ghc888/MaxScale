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
#include <string.h>
#include <unistd.h>
#include <sys/types.h>


/** Encoding values in-memory */
uint64_t maxavro_encode_integer(uint8_t* buffer, uint64_t val);
uint64_t maxavro_encode_string(uint8_t* dest, const char* str);
uint64_t maxavro_encode_float(uint8_t* dest, float val);
uint64_t maxavro_encode_double(uint8_t* dest, double val);

/** Writing values straight to disk*/
bool maxavro_write_integer(FILE *file, uint64_t val);
bool maxavro_write_string(FILE *file, const char* str);
bool maxavro_write_float(FILE *file, float val);
bool maxavro_write_double(FILE *file, double val);

MAXAVRO_DATABLOCK* maxavro_datablock_allocate(MAXAVRO_FILE *file, size_t buffersize)
{
    MAXAVRO_DATABLOCK *datablock = malloc(sizeof(MAXAVRO_DATABLOCK));

    if (datablock && (datablock->buffer = malloc(buffersize)))
    {
        datablock->buffersize = buffersize;
        datablock->avrofile = file;
        datablock->datasize = 0;
        datablock->records = 0;
    }

    return datablock;
}

void maxavro_datablock_free(MAXAVRO_DATABLOCK* block)
{
    if (block)
    {
        free(block->buffer);
        free(block);
    }
}

bool maxavro_datablock_finalize(MAXAVRO_DATABLOCK* block)
{
    bool rval = true;
    FILE *file = block->avrofile->file;

    /** Store the current position so we can truncate the file if a write fails */
    long pos = ftell(file);

    if (!maxavro_write_integer(file, block->records) ||
        !maxavro_write_integer(file, block->datasize) ||
        fwrite(block->buffer, 1, block->datasize, file) != block->datasize ||
        fwrite(block->avrofile->sync, 1, SYNC_MARKER_SIZE, file) != SYNC_MARKER_SIZE)
    {
        int fd = fileno(file);
        ftruncate(fd, pos);
        fseek(file, 0, SEEK_END);
        rval = false;
    }
    else
    {
        /** The current block is successfully written, reset datablock for
         * a new write. */
        block->buffersize = 0;
        block->records = 0;
    }
    return rval;
}

static bool reallocate_datablock(MAXAVRO_DATABLOCK *block)
{
    void *tmp = realloc(block->buffer, block->buffersize * 2);
    if (tmp == NULL)
    {
        return false;
    }

    block->buffer = tmp;
    block->buffersize *= 2;
    return true;
}

bool maxavro_datablock_add_integer(MAXAVRO_DATABLOCK *block, uint64_t val)
{
    if (block->datasize + 9 >= block->buffersize && !reallocate_datablock(block))
    {
        return false;
    }

    uint64_t added = maxavro_encode_integer(block->buffer + block->datasize, val);
    block->datasize += added;
    return true;
}

bool maxavro_datablock_add_string(MAXAVRO_DATABLOCK *block, const char* str)
{
    if (block->datasize + 9 + strlen(str) >= block->buffersize && !reallocate_datablock(block))
    {
        return false;
    }

    uint64_t added = maxavro_encode_string(block->buffer + block->datasize, str);
    block->datasize += added;
    return true;
}

bool maxavro_datablock_add_float(MAXAVRO_DATABLOCK *block, float val)
{
    if (block->datasize + sizeof(val) >= block->buffersize && !reallocate_datablock(block))
    {
        return false;
    }

    uint64_t added = maxavro_encode_float(block->buffer + block->datasize, val);
    block->datasize += added;
    return true;
}

bool maxavro_datablock_add_double(MAXAVRO_DATABLOCK *block, double val)
{
    if (block->datasize + sizeof(val) >= block->buffersize && !reallocate_datablock(block))
    {
        return false;
    }

    uint64_t added = maxavro_encode_double(block->buffer + block->datasize, val);
    block->datasize += added;
    return true;
}
