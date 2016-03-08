/*
 * This file is distributed as part of MaxScale.  It is free
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


#include <rbr.h>
#include <mysql_utils.h>
#include <jansson.h>
#include <avro_schema.h>
#include <dbusers.h>

/**
 *
 * @param router
 * @param hdr
 * @param maphash
 * @param pos
 */
void handle_table_map_event(ROUTER_INSTANCE *router, REP_HEADER *hdr, uint64_t pos)
{
    uint8_t *buf = malloc(hdr->event_size);
    pread(router->binlog_fd, buf, hdr->event_size, pos + BINLOG_EVENT_HDR_LEN);
    uint8_t *ptr = buf;

    TABLE_MAP *map = table_map_alloc(ptr, router->event_type_hdr_lens[hdr->event_type]);

    if (map)
    {
        TABLE_MAP *old;

        if ((old = hashtable_fetch(router->table_maps, (void*) &map->id)) &&
            old->columns == map->columns &&
            memcmp(old->column_types, map->column_types,
                   MIN(old->columns, map->columns)) == 0)
        {
            table_map_free(map);
        }
        else
        {
            /** New definition for an old table */
            if (old)
            {
                hashtable_delete(router->table_maps, &map->id);
                char *oldschema = hashtable_fetch(router->schemas, &map->id);
                hashtable_delete(router->schemas, &map->id);
                table_map_free(old);
                free(oldschema);
            }
            char* newschema = json_schema_from_table_map(map);
            hashtable_add(router->table_maps, (void*) &map->id, map);
            hashtable_add(router->schemas, (void*) &map->id, newschema);
            save_avro_schema("/tmp", newschema, map);
            MXS_DEBUG("%s", newschema);
        }

        strncpy(map->gtid, router->current_gtid, GTID_MAX_LEN);
    }
    free(buf);
}

/**
 *
 * @param router
 * @param hdr
 * @param maphash
 * @param pos
 */
void handle_row_event(ROUTER_INSTANCE *router, REP_HEADER *hdr, HASHTABLE *maphash, uint64_t pos)
{
    uint8_t *buf = malloc(hdr->event_size - BINLOG_EVENT_HDR_LEN);
    ssize_t nread = pread(router->binlog_fd, buf, hdr->event_size - BINLOG_EVENT_HDR_LEN,
                          pos + BINLOG_EVENT_HDR_LEN);
    uint8_t *ptr = buf;
    uint8_t table_id_size = router->event_type_hdr_lens[hdr->event_type] == 6 ? 4 : 6;
    uint64_t table_id = 0;

    if (nread != hdr->event_size - BINLOG_EVENT_HDR_LEN)
    {
        MXS_ERROR("Failed to read event, read %ld bytes when expected %u.",
                  nread, hdr->event_size - BINLOG_EVENT_HDR_LEN);
    }

    memcpy(&table_id, ptr, table_id_size);
    ptr += table_id_size;

    uint16_t flags = 0;
    memcpy(&flags, ptr, 2);
    ptr += 2;

    if (table_id == TABLE_DUMMY_ID && flags & ROW_EVENT_END_STATEMENT)
    {
        /** This is an dummy event which should release all table maps. Right
         * now we just return without processing the rows. */
        free(buf);
        return;
    }

    if (hdr->event_type > DELETE_ROWS_EVENTv1)
    {
        /** Version 2 row event, skip extra data */
        uint16_t extra_len = 0;
        memcpy(&extra_len, ptr, 2);
        ptr += 2 + extra_len;
    }

    uint64_t ncolumns = leint_consume(&ptr);
    uint64_t col_present = 0;
    memcpy(&col_present, ptr, (ncolumns + 7) / 8);
    ptr += (ncolumns + 7) / 8;

    uint64_t col_update = 0;
    if (hdr->event_type == UPDATE_ROWS_EVENTv1 ||
        hdr->event_type == UPDATE_ROWS_EVENTv2)
    {
        memcpy(&col_update, ptr, (ncolumns + 7) / 8);
        ptr += (ncolumns + 7) / 8;
    }

    TABLE_MAP *map = hashtable_fetch(maphash, (void*) &table_id);
    if (map)
    {
        /** Right now the file is opened for every row event */
        avro_schema_t schema;
        char *schema_json = hashtable_fetch(router->schemas, &map->id);
        avro_schema_from_json_length(schema_json, strlen(schema_json), &schema);

        avro_file_writer_t writer;
        char outfile[PATH_MAX];
        snprintf(outfile, sizeof(outfile), "/tmp/%s.%s.%s.avro", map->database, map->table, map->version_string);

        if (access(outfile, F_OK) == 0)
        {
            avro_file_writer_open(outfile, &writer);
        }
        else
        {
            avro_file_writer_create(outfile, schema, &writer);
        }

        avro_value_iface_t  *writer_iface = avro_generic_class_from_schema(schema);
        avro_value_t record;
        avro_generic_value_new(writer_iface, &record);

        /** Each event has one or more rows in it. The number of rows is not known
         * beforehand so we must continue processing them until we reach the end
         * of the event. */
        while (ptr - buf < hdr->event_size - BINLOG_EVENT_HDR_LEN)
        {
            process_row_event(map, &record, &ptr, ncolumns, col_present, col_update);
            avro_file_writer_append_value(writer, &record);
        }
        avro_file_writer_flush(writer);
        avro_file_writer_close(writer);
        avro_value_decref(&record);
        avro_value_iface_decref(writer_iface);
        avro_schema_decref(schema);
    }
    free(buf);
}

/**
 * @brief Extract the values from a single row  in a row event
 *
 * The newer v1 and v2 row event types have extra information in the row event.
 * This could be processed to see the complete before and after image of the row
 * in question.
 * @param map
 * @param ptr
 * @param ncolumns
 * @param columns_present
 * @param col_update
 */
void process_row_event(TABLE_MAP *map, avro_value_t *record, uint8_t **orig_ptr, long ncolumns,
                       uint64_t columns_present, uint64_t columns_update)
{
    char rstr[2048];
    uint8_t *ptr = *orig_ptr;
    int npresent = 0;
    avro_value_t field;
    sprintf(rstr, "Row event for table %s.%s: %lu columns. ", map->database,
            map->table, ncolumns);

    /** Skip the nul-bitmap */
    ptr += (ncolumns + 7) / 8;

    for (long i = 0; i < map->columns && npresent < ncolumns; i++)
    {
        char colname[128];
        sprintf(colname, "column_%ld", i + 1);
        avro_value_get_by_name(record, colname, &field, NULL);

        if (columns_present & (1 << i))
        {
            npresent++;
            if (column_is_string_type(map->column_types[i]))
            {
                size_t sz;
                char *str = lestr_consume(&ptr, &sz);
                avro_value_set_string_len(&field, str, sz);
                strcat(rstr, "S: ");
                strncat(rstr, str, sz);
                strcat(rstr, " ");

            }
            else
            {
                uint64_t lval = 0;
                ptr += extract_field_value(ptr, map->column_types[i], &lval);
                char buf[200];
                snprintf(buf, sizeof(buf), "I: %lu ", lval);
                strcat(rstr, buf);

                if (is_temporal_value(map->column_types[i]))
                {
                    struct tm tm;
                    unpack_temporal_value(map->column_types[i], lval, &tm);
                    format_temporal_value(buf, sizeof(buf), map->column_types[i], &tm);
                    avro_value_set_string(&field, buf);
                    MXS_DEBUG("%s: %s",
                              table_type_to_string(map->column_types[i]), buf);
                }
                else
                {
                    sprintf(buf, "%lu", lval);
                    avro_value_set_string(&field, buf);
                }
            }
        }
    }

    if (columns_update != 0)
    {
        /** Skip the nul-bitmap for the update rows */
        ptr += (ncolumns + 7) / 8;

        for (long i = 0; i < map->columns && npresent < ncolumns; i++)
        {

            if (columns_update & (1 << i))
            {
                if (column_is_string_type(map->column_types[i]))
                {
                    char *str = lestr_consume_dup(&ptr);
                    free(str);
                }
                else
                {
                    uint64_t lval = 0;
                    ptr += extract_field_value(ptr, map->column_types[i], &lval);
                    if (is_temporal_value(map->column_types[i]))
                    {
                        struct tm tm;
                        unpack_temporal_value(map->column_types[i], lval, &tm);
                    }
                }
            }
        }
    }

    MXS_NOTICE("%s", rstr);
    *orig_ptr = ptr;
}

/**
 * Extract the table definition from a CREATE TABLE statement
 * @param sql
 * @param size
 * @return
 */
static const char* get_table_definition(const char *sql, int* size)
{
    const char *rval = NULL;
    const char *ptr = sql;
    const char *end = strchr(sql, '\0');
    while (ptr < end && *ptr != '(')
    {
        ptr++;
    }

    /** We assume at least the parentheses are in the statement */
    if (ptr < end - 2)
    {
        int depth = 0;
        const char *start = ptr + 1;
        while (ptr < end)
        {
            switch (*ptr)
            {
                case '(':
                    depth++;
                    break;

                case ')':
                    depth--;
                    break;

                default:
                    break;
            }

            /** We found the last closing parenthesis */
            if (depth < 0)
            {
                *size = ptr - start;
                rval = start;
            }
        }
    }

    return rval;
}

/**
 *
 * @param sql
 * @return
 * TODO: NULL return value checks
 */
TABLE_CREATE* hande_create_table_event(const char* sql)
{
    return NULL;
    int stmt_len = 0;
    char table[MYSQL_TABLE_MAXLEN];
    char db[MYSQL_DATABASE_MAXLEN];
    /** Extract the table definition so we can get the column names from it */

    const char* statement_sql = get_table_definition(sql, &stmt_len);
    MXS_NOTICE("Create table statement: %.*s", stmt_len, statement_sql);
    TABLE_CREATE *rval = NULL;
    const char *nameptr = statement_sql;
    int i = 0;

    /** Process columns in groups of 8 */
    size_t names_size = 8;
    char **names = malloc(sizeof(char*) * names_size);

    while (nameptr)
    {
        if (i >= names_size)
        {
            char **tmp = realloc(names, (names_size + 8) * sizeof(char*));
            if (tmp)
            {
                names = tmp;
                names_size += 8;
            }
        }

        nameptr++;
        while (isspace(*nameptr))
        {
            nameptr++;
        }
        char colname[64 + 1];
        char *end = strchr(nameptr, ' ');
        if (end)
        {
            sprintf(colname, "%.*s", (int)(end - nameptr), nameptr);
            names[i++] = strdup(colname);
            MXS_NOTICE("Column name: %s", colname);
        }

        nameptr = strchr(nameptr, ',');
    }

    /** We have appear to have a valid CREATE TABLE statement */
    if (i > 0)
    {
        rval = malloc(sizeof(TABLE_CREATE));
        rval->column_names = names;
        rval->columns = i;
        rval->database = db;
        rval->table = table;
        rval->gtid[0] = '\0'; // GTID not yet implemented
    }

    return rval;
}