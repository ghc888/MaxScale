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

/**
 * @file avro_file.c - File operations for the Avro router
 *
 * This file contains functions that handle the low level file operations for
 * the Avro router. The handling of Avro data files is done via the Avro C API
 * but the handling of MySQL format binary logs is done manually.
 *
 * Parts of this file have been copied from blr_file.c and modified for other
 * uses.
 *
 * @verbatim
 * Revision History
 *
 * Date         Who             Description
 * 25/02/2016   Markus Mäkelä   Initial implementation
 *
 * @endverbatim
 */

#include <binlog_common.h>
#include <sys/stat.h>
#include <avrorouter.h>
#include <log_manager.h>
#include <fcntl.h>
#include <maxscale_pcre2.h>
#include <ini.h>
#include <dirent.h>
#include <stdlib.h>

static const char *statefile_section = "avro-conversion";
static const char *ddl_list_name = "table-ddl.list";
void handle_query_event(AVRO_INSTANCE *router, REP_HEADER *hdr,
                        int *pending_transaction, uint8_t *ptr);
bool is_create_table_statement(AVRO_INSTANCE *router, char* ptr, size_t len);
void avro_flush_all_tables(AVRO_INSTANCE *router);
void avro_notify_client(AVRO_CLIENT *client);

/**
 * Prepare an existing binlog file to be appened to.
 *
 * @param router    The router instance
 * @param file      The binlog file name
 */
bool avro_open_binlog(const char *binlogdir, const char *file, int *dest)
{
    char path[PATH_MAX + 1] = "";
    int fd;

    snprintf(path, sizeof(path), "%s/%s", binlogdir, file);

    if ((fd = open(path, O_RDWR | O_APPEND, 0666)) == -1)
    {
        MXS_ERROR("Failed to open binlog file %s.", path);
        return false;
    }

    if (lseek(fd, BINLOG_MAGIC_SIZE, SEEK_SET) < 4)
    {
        /* If for any reason the file's length is between 1 and 3 bytes
         * then report an error. */
        MXS_ERROR("Binlog file %s has an invalid length.", path);
        close(fd);
        return false;
    }

    *dest = fd;
    return true;
}

/**
 * Close a binlog file
 * @param fd Binlog file descriptor
 */
void avro_close_binlog(int fd)
{
    close(fd);
}

/**
 * @brief Allocate an Avro table
 *
 * Create an Aro table and prepare it for writing.
 * @param filepath Path to the created file
 * @param json_schema The schema of the table in JSON format
 */
AVRO_TABLE* avro_table_alloc(const char* filepath, const char* json_schema)
{
    AVRO_TABLE *table = calloc(1, sizeof(AVRO_TABLE));
    if (table)
    {
        if (avro_schema_from_json_length(json_schema, strlen(json_schema),
                                         &table->avro_schema))
        {
            MXS_ERROR("Avro error: %s", avro_strerror());
            free(table);
            return NULL;
        }
        int rc = 0;
        if (access(filepath, F_OK) == 0)
        {
            rc = avro_file_writer_open(filepath, &table->avro_file);
        }
        else
        {
            rc = avro_file_writer_create(filepath, table->avro_schema, &table->avro_file);
        }
        if (rc)
        {
            MXS_ERROR("Avro error: %s", avro_strerror());
            avro_schema_decref(table->avro_schema);
            free(table);
            return NULL;
        }

        if ((table->avro_writer_iface = avro_generic_class_from_schema(table->avro_schema)) == NULL)
        {
            MXS_ERROR("Avro error: %s", avro_strerror());
            avro_schema_decref(table->avro_schema);
            avro_file_writer_close(table->avro_file);
            free(table);
            return NULL;
        }
        table->json_schema = strdup(json_schema);
        table->filename = strdup(filepath);
    }
    return table;
}

/**
 * @brief Write a new ini file with current conversion status
 *
 * The file is stored in the cache directory as 'avro-conversion.ini'.
 * @param router Avro router instance
 * @return True if the file was written successfully to disk
 *
 */
bool avro_save_conversion_state(AVRO_INSTANCE *router)
{
    FILE *config_file;
    char filename[PATH_MAX + 1];
    char err_msg[STRERROR_BUFLEN];

    snprintf(filename, sizeof(filename), "%s/avro-conversion.ini.tmp", router->avrodir);

    /* open file for writing */
    config_file = fopen(filename, "wb");
    if (config_file == NULL)
    {
        MXS_ERROR("Failed to open file '%s': %d, %s", filename,
                  errno, strerror_r(errno, err_msg, sizeof(err_msg)));
        return false;
    }

    fprintf(config_file, "[%s]\n", statefile_section);
    fprintf(config_file, "position=%lu\n", router->current_pos);
    fprintf(config_file, "gtid=%lu-%lu-%lu:%lu\n", router->gtid.domain,
            router->gtid.server_id, router->gtid.seq, router->gtid.event_num);
    fprintf(config_file, "file=%s\n", router->binlog_name);
    fclose(config_file);

    /* rename tmp file to right filename */
    char newname[PATH_MAX + 1];
    snprintf(newname, sizeof(newname), "%s/avro-conversion.ini", router->avrodir);
    int rc = rename(filename, newname);

    if (rc == -1)
    {
        MXS_ERROR("Failed to rename file '%s' to '%s': %d, %s", filename, newname,
                  errno, strerror_r(errno, err_msg, sizeof(err_msg)));
        return false;
    }

    return true;
}

/**
 * @brief Callback for the @c ini_parse of the stored conversion position
 *
 * @param data User provided data
 * @param section Section name
 * @param key Parameter name
 * @param value Parameter value
 * @return 1 if the parsing should continue, 0 if an error was detected
 */
static int conv_state_handler(void* data, const char* section, const char* key, const char* value)
{
    AVRO_INSTANCE *router = (AVRO_INSTANCE*) data;

    if (strcmp(section, statefile_section) == 0)
    {
        if (strcmp(key, "gtid") == 0)
        {
            char tempval[strlen(value) + 1];
            memcpy(tempval, value, sizeof(tempval));
            char *saved, *domain = strtok_r(tempval, ":-\n", &saved);
            char *serv_id = strtok_r(NULL, ":-\n", &saved);
            char *seq = strtok_r(NULL, ":-\n", &saved);
            char *subseq = strtok_r(NULL, ":-\n", &saved);
            if (domain && serv_id && seq)
            {
                router->gtid.domain = strtol(domain, NULL, 10);
                router->gtid.server_id = strtol(serv_id, NULL, 10);
                router->gtid.seq = strtol(seq, NULL, 10);
                router->gtid.event_num = strtol(subseq, NULL, 10);
            }
        }
        else if (strcmp(key, "position") == 0)
        {
            router->current_pos = strtol(value, NULL, 10);
        }
        else if (strcmp(key, "file") == 0)
        {
            strncpy(router->binlog_name, value, sizeof(router->binlog_name));
        }
        else
        {
            return 0;
        }
    }

    return 1;
}

/**
 * @brief Load a stored conversion state from file
 *
 * @param router Avro router instance
 * @return True if the stored state was loaded successfully
 */
bool avro_load_conversion_state(AVRO_INSTANCE *router)
{
    char filename[PATH_MAX + 1];
    bool rval = false;

    snprintf(filename, sizeof(filename), "%s/avro-conversion.ini", router->avrodir);

    /** No stored state, this is the first time the router is started */
    if (access(filename, F_OK) == -1)
    {
        return true;
    }

    int rc = ini_parse(filename, conv_state_handler, router);

    switch (rc)
    {
        case 0:
            rval = true;
            MXS_NOTICE("Loaded stored binary log conversion state: File: [%s] Position: [%ld] GTID: [%lu-%lu-%lu:%lu]",
                       router->binlog_name, router->current_pos, router->gtid.domain,
                       router->gtid.server_id, router->gtid.seq, router->gtid.event_num);
            break;

        case -1:
            MXS_ERROR("Failed to open file '%s'. ", filename);
            break;

        case -2:
            MXS_ERROR("Failed to allocate enough memory when parsing file '%s'. ", filename);
            break;

        default:
            MXS_ERROR("Failed to parse stored conversion state '%s', error "
                      "on line %d. ", filename, rc);
            break;
    }

    return rval;
}

/**
 * @brief Free an AVRO_TABLE
 *
 * @param table Table to free
 * @return Always NULL
 */
void* avro_table_free(AVRO_TABLE *table)
{
    if (table)
    {
        avro_file_writer_flush(table->avro_file);
        avro_file_writer_close(table->avro_file);
        avro_value_iface_decref(table->avro_writer_iface);
        avro_schema_decref(table->avro_schema);
        free(table->json_schema);
        free(table->filename);
    }
    return NULL;
}

/**
 * @brief Rotate to next file if it exists
 *
 * @param router Avro router instance
 * @param pos Current position, used for logging
 * @param stop_seen If a stop event was seen when processing current file
 * @return AVRO_OK if the next file exists, AVRO_LAST_FILE if this is the last
 * available file.
 */
static avro_binlog_end_t rotate_to_next_file_if_exists(AVRO_INSTANCE* router, uint64_t pos, bool stop_seen)
{
    avro_binlog_end_t rval = AVRO_LAST_FILE;

    if (binlog_next_file_exists(router->binlogdir, router->binlog_name))
    {
        char next_binlog[BINLOG_FNAMELEN + 1];
        snprintf(next_binlog, sizeof(next_binlog),
                 BINLOG_NAMEFMT, router->fileroot,
                 blr_file_get_next_binlogname(router->binlog_name));

        if (stop_seen)
        {
            MXS_NOTICE("End of binlog file [%s] at %lu with a "
                       "close event. Rotating to next binlog file [%s].",
                       router->binlog_name, pos, next_binlog);
        }
        else
        {
            MXS_NOTICE("End of binlog file [%s] at %lu with no "
                       "close or rotate event. Rotating to next binlog file [%s].",
                       router->binlog_name, pos, next_binlog);
        }

        rval = AVRO_OK;
        strncpy(router->binlog_name, next_binlog, sizeof(router->binlog_name));
        router->binlog_position = 4;
        router->current_pos = 4;
    }
    else if (stop_seen)
    {
        MXS_NOTICE("End of binlog file [%s] at %lu with a close event. "
                   "Next binlog file does not exist, pausing file conversion.",
                   router->binlog_name, pos);
    }

    return rval;
}

/**
 * @brief Rotate to a specific file
 *
 * This rotates the current binlog file being processed to a specific file.
 * Currently this is only used to rotate to files that rotate events point to.
 * @param router Avro router instance
 * @param pos Current position, only used for logging
 * @param next_binlog The next file to rotate to
 */
static void rotate_to_file(AVRO_INSTANCE* router, uint64_t pos, const char *next_binlog)
{
    /** Binlog file is processed, prepare for next one */
    MXS_NOTICE("End of binlog file [%s] at %lu. Rotating to file [%s].",
               router->binlog_name, pos, next_binlog);
    strncpy(router->binlog_name, next_binlog, sizeof(router->binlog_name));
    router->binlog_position = 4;
    router->current_pos = 4;
}

/**
 * @brief Read the replication event payload
 *
 * @param router Avro router instance
 * @param hdr Replication header
 * @param pos Starting position of the event header
 * @return The event data or NULL if an error occurred
 */
static GWBUF* read_event_data(AVRO_INSTANCE *router, REP_HEADER* hdr, uint64_t pos)
{
    GWBUF* result;
    /* Allocate a GWBUF for the event */
    if ((result = gwbuf_alloc(hdr->event_size - BINLOG_EVENT_HDR_LEN + 1)))
    {
        uint8_t *data = GWBUF_DATA(result);
        int n = pread(router->binlog_fd, data, hdr->event_size - BINLOG_EVENT_HDR_LEN,
                      pos + BINLOG_EVENT_HDR_LEN);
        /** NULL-terminate for QUERY_EVENT processing */
        data[hdr->event_size - BINLOG_EVENT_HDR_LEN] = '\0';

        if (n != hdr->event_size - BINLOG_EVENT_HDR_LEN)
        {
            if (n == -1)
            {
                char err_msg[STRERROR_BUFLEN];
                MXS_ERROR("Error reading the event at %lu in %s. "
                          "%s, expected %d bytes.",
                          pos, router->binlog_name,
                          strerror_r(errno, err_msg, sizeof(err_msg)),
                          hdr->event_size - BINLOG_EVENT_HDR_LEN);
            }
            else
            {
                MXS_ERROR("Short read when reading the event at %lu in %s. "
                          "Expected %d bytes got %d bytes.",
                          pos, router->binlog_name,
                          hdr->event_size - BINLOG_EVENT_HDR_LEN, n);
            }
            gwbuf_free(result);
            result = NULL;
        }
    }
    else
    {
        MXS_ERROR("Failed to allocate memory for binlog entry, "
                  "size %d at %lu.",
                  hdr->event_size, pos);
    }
    return result;
}

void notify_all_clients(AVRO_INSTANCE *router)
{
    AVRO_CLIENT *client = router->clients;
    int notified = 0;

    while (client)
    {
        spinlock_acquire(&client->catch_lock);
        if (client->cstate & AVRO_WAIT_DATA)
        {
            notified++;
            avro_notify_client(client);
        }
        spinlock_release(&client->catch_lock);

        client = client->next;
    }

    if (notified > 0)
    {
        MXS_INFO("Notified %d clients about new data.", notified);
    }
}

/**
 * @brief Read all replication events from a binlog file.
 *
 * Routine detects errors and pending transactions
 *
 * @param router        The router instance
 * @param fix           Whether to fix or not errors
 * @param debug         Whether to enable or not the debug for events
 * @return              How the binlog was closed
 * @see enum avro_binlog_end
 */
avro_binlog_end_t avro_read_all_events(AVRO_INSTANCE *router)
{
    uint8_t hdbuf[BINLOG_EVENT_HDR_LEN];
    unsigned long long pos = router->current_pos;
    unsigned long long last_known_commit = 4;
    char next_binlog[BINLOG_FNAMELEN + 1];
    REP_HEADER hdr;
    int pending_transaction = 0;
    uint8_t *ptr;
    bool found_chksum = false;

    /** For statistics */
    unsigned long events = 0;
    unsigned long event_bytes = 0;
    unsigned long max_bytes = 0;

    uint64_t total_commits = 0, total_rows = 0;

    bool rotate_seen = false;
    bool stop_seen = false;

    if (router->binlog_fd == -1)
    {
        MXS_ERROR("Current binlog file %s is not open", router->binlog_name);
        return AVRO_BINLOG_ERROR;
    }

    while (1)
    {
        int n;
        /* Read the header information from the file */
        if ((n = pread(router->binlog_fd, hdbuf, BINLOG_EVENT_HDR_LEN, pos)) != BINLOG_EVENT_HDR_LEN)
        {
            switch (n)
            {
                case 0:
                    break;
                case -1:
                {
                    char err_msg[BLRM_STRERROR_R_MSG_SIZE + 1] = "";
                    strerror_r(errno, err_msg, BLRM_STRERROR_R_MSG_SIZE);
                    MXS_ERROR("Failed to read binlog file %s at position %llu"
                              " (%s).", router->binlog_name,
                              pos, err_msg);

                    if (errno == EBADF)
                        MXS_ERROR("Bad file descriptor in read binlog for file %s"
                                  ", descriptor %d.",
                                  router->binlog_name, router->binlog_fd);
                    break;
                }
                default:
                    MXS_ERROR("Short read when reading the header. "
                              "Expected 19 bytes but got %d bytes. "
                              "Binlog file is %s, position %llu",
                              n, router->binlog_name, pos);
                    break;
            }

            router->current_pos = pos;

            if (pending_transaction > 0)
            {
                MXS_ERROR("Binlog '%s' ends at position %lu and has an incomplete transaction at %lu. "
                          "Stopping file conversion.", router->binlog_name,
                          router->current_pos, router->binlog_position);
                return AVRO_OPEN_TRANSACTION;
            }
            else
            {
                /* any error */
                if (n != 0)
                {
                    return AVRO_BINLOG_ERROR;
                }
                else
                {
                    MXS_INFO("Processed %lu transactions and %lu row events.",
                             total_commits, total_rows);
                    if (rotate_seen)
                    {
                        rotate_to_file(router, pos, next_binlog);
                        return AVRO_OK;
                    }
                    else
                    {
                        return rotate_to_next_file_if_exists(router, pos, stop_seen);
                    }
                }
            }
        }

        /* fill replication header struct */
        hdr.timestamp = EXTRACT32(hdbuf);
        hdr.event_type = hdbuf[4];
        hdr.serverid = EXTRACT32(&hdbuf[5]);
        hdr.event_size = extract_field(&hdbuf[9], 32);
        hdr.next_pos = EXTRACT32(&hdbuf[13]);
        hdr.flags = EXTRACT16(&hdbuf[17]);

        /* Check event type against MAX_EVENT_TYPE */

        if (hdr.event_type > MAX_EVENT_TYPE_MARIADB10)
        {
            MXS_ERROR("Invalid MariaDB 10 event type 0x%x. "
                      "Binlog file is %s, position %llu",
                      hdr.event_type, router->binlog_name, pos);
            router->binlog_position = last_known_commit;
            router->current_pos = pos;
            return AVRO_BINLOG_ERROR;
        }

        if (hdr.event_size <= 0)
        {
            MXS_ERROR("Event size error: "
                      "size %d at %llu.",
                      hdr.event_size, pos);

            router->binlog_position = last_known_commit;
            router->current_pos = pos;
            return AVRO_BINLOG_ERROR;
        }

        GWBUF *result = read_event_data(router, &hdr, pos);

        if (result == NULL)
        {
            router->binlog_position = last_known_commit;
            router->current_pos = pos;
            MXS_WARNING("an error has been found. "
                        "Setting safe pos to %lu, current pos %lu",
                        router->binlog_position, router->current_pos);
            return AVRO_BINLOG_ERROR;
        }

        /* check for pending transaction */
        if (pending_transaction == 0)
        {
            last_known_commit = pos;
        }

        /* get event content */
        ptr = GWBUF_DATA(result);

        MXS_DEBUG("%s(%x) - %llu", binlog_event_name(hdr.event_type), hdr.event_type, pos);

        /* check for FORMAT DESCRIPTION EVENT */
        if (hdr.event_type == FORMAT_DESCRIPTION_EVENT)
        {
            int event_header_length;
            int event_header_ntypes;
            int n_events;

            /** Extract the event header lengths */
            event_header_length = ptr[2 + 50 + 4];
            event_header_ntypes = hdr.event_size - event_header_length - (2 + 50 + 4 + 1);
            memcpy(router->event_type_hdr_lens, ptr + 2 + 50 + 5, event_header_ntypes);
            router->event_types = event_header_ntypes;

            switch (event_header_ntypes)
            {
                case 168: /* mariadb 10 LOG_EVENT_TYPES*/
                    event_header_ntypes -= 163;
                    break;

                case 165: /* mariadb 5 LOG_EVENT_TYPES*/
                    event_header_ntypes -= 160;
                    break;

                default: /* mysql 5.6 LOG_EVENT_TYPES = 35 */
                    event_header_ntypes -= 35;
                    break;
            }

            n_events = hdr.event_size - event_header_length - (2 + 50 + 4 + 1);

            if (event_header_ntypes < n_events)
            {
                uint8_t *checksum = ptr + hdr.event_size - event_header_length - event_header_ntypes;
                if (checksum[0] == 1)
                {
                    found_chksum = true;
                }
            }
        }
        /* Decode CLOSE/STOP Event */
        else if (hdr.event_type == STOP_EVENT)
        {
            char next_file[BLRM_BINLOG_NAME_STR_LEN + 1];
            stop_seen = true;
            snprintf(next_file, sizeof(next_file), BINLOG_NAMEFMT, router->fileroot,
                     blr_file_get_next_binlogname(router->binlog_name));
        }
        else if (hdr.event_type == TABLE_MAP_EVENT)
        {
            handle_table_map_event(router, &hdr, ptr);
        }
        else if ((hdr.event_type >= WRITE_ROWS_EVENTv0 && hdr.event_type <= DELETE_ROWS_EVENTv1) ||
                 (hdr.event_type >= WRITE_ROWS_EVENTv2 && hdr.event_type <= DELETE_ROWS_EVENTv2))
        {
            router->row_count++;
            handle_row_event(router, &hdr, ptr);
        }
        /* Decode ROTATE EVENT */
        else if (hdr.event_type == ROTATE_EVENT)
        {
            int len = hdr.event_size - BINLOG_EVENT_HDR_LEN - 8;

            if (found_chksum)
            {
                len -= 4;
            }

            if (len > BINLOG_FNAMELEN)
            {
                MXS_WARNING("Truncated binlog name from %d to %d characters.",
                            len, BINLOG_FNAMELEN);
                len = BINLOG_FNAMELEN;
            }

            memcpy(next_binlog, ptr + 8, len);
            next_binlog[len] = 0;
            rotate_seen = true;

        }
        else if (hdr.event_type == MARIADB10_GTID_EVENT)
        {
            uint64_t n_sequence; /* 8 bytes */
            uint32_t domainid; /* 4 bytes */
            unsigned int flags; /* 1 byte */
            n_sequence = extract_field(ptr, 64);
            domainid = extract_field(ptr + 8, 32);
            flags = *(ptr + 8 + 4);
            router->gtid.domain = domainid;
            router->gtid.server_id = hdr.serverid;
            router->gtid.seq = n_sequence;
            router->gtid.event_num = 1;

            if (flags == 0)
            {
                pending_transaction = 1;
            }
        }
        /**
         * Check QUERY_EVENT
         *
         * Check for BEGIN ( ONLY for mysql 5.6, mariadb 5.5 )
         * Check for COMMIT (not transactional engines)
         */
        else if (hdr.event_type == QUERY_EVENT)
        {
            int trx_before = pending_transaction;
            handle_query_event(router, &hdr, &pending_transaction, ptr);

            if (trx_before != pending_transaction)
            {
                /** A non-transactional engine finished a transaction */
                router->trx_count++;
            }
        }
        else if (hdr.event_type == XID_EVENT)
        {
            router->trx_count++;
            pending_transaction = 0;

            if (router->row_count >= router->row_target ||
                router->trx_count >= router->trx_target)
            {
                notify_all_clients(router);
                avro_flush_all_tables(router);
                avro_save_conversion_state(router);
                total_rows += router->row_count;
                total_commits += router->trx_count;
                router->row_count = router->trx_count = 0;
            }
        }

        gwbuf_free(result);

        /* pos and next_pos sanity checks */
        if (hdr.next_pos > 0 && hdr.next_pos < pos)
        {
            MXS_INFO("Binlog %s: next pos %u < pos %llu, truncating to %llu",
                     router->binlog_name,
                     hdr.next_pos,
                     pos,
                     pos);

            break;
        }

        if (hdr.next_pos > 0 && hdr.next_pos != (pos + hdr.event_size))
        {
            MXS_INFO("Binlog %s: next pos %u != (pos %llu + event_size %u), truncating to %llu",
                     router->binlog_name,
                     hdr.next_pos,
                     pos,
                     hdr.event_size,
                     pos);

            break;
        }

        /* set pos to new value */
        if (hdr.next_pos > 0)
        {

            if (pending_transaction)
            {
                event_bytes += hdr.event_size;

                if (event_bytes > max_bytes)
                {
                    max_bytes = event_bytes;
                }
            }

            pos = hdr.next_pos;
            router->current_pos = pos;
        }
        else
        {

            MXS_ERROR("Current event type %d @ %llu has nex pos = %u : exiting",
                      hdr.event_type, pos, hdr.next_pos);
            break;
        }

        events++;
    }

    return AVRO_BINLOG_ERROR;
}

/**
 *
 * @param router
 * @return
 */
void avro_load_metadata_from_schemas(AVRO_INSTANCE *router)
{
    // TODO: implement this
}

/**
 * @brief Load stored CREATE TABLE statements from file
 * @param router Avro router instance
 * @return True on success
 */
bool avro_load_created_tables(AVRO_INSTANCE *router)
{
    bool rval = false;
    char createlist[PATH_MAX + 1];
    snprintf(createlist, sizeof(createlist), "%s/%s", router->avrodir, ddl_list_name);
    struct stat st;

    if (stat(createlist, &st) == 0)
    {
        size_t len = st.st_size;
        char* buffer = malloc(len + 1);
        FILE *file = fopen(createlist, "rb");

        if (file)
        {
            if (buffer && fread(buffer, 1, len, file) == len)
            {
                buffer[len] = '\0';
                char *saveptr;
                char *tok = strtok_r(buffer, "\n", &saveptr);
                rval = true;

                while (tok)
                {
                    if (is_create_table_statement(router, tok, strlen(tok)))
                    {
                        TABLE_CREATE *created = table_create_alloc(tok, "");

                        if (created)
                        {
                            char table_ident[MYSQL_TABLE_MAXLEN + MYSQL_DATABASE_MAXLEN + 2];
                            snprintf(table_ident, sizeof(table_ident), "%s.%s", created->database, created->table);

                            if (hashtable_fetch(router->created_tables, table_ident))
                            {
                                hashtable_delete(router->created_tables, table_ident);
                            }
                            hashtable_add(router->created_tables, table_ident, created);
                        }
                        else
                        {
                            rval = false;
                            break;
                        }
                    }
                    tok = strtok_r(NULL, "\n", &saveptr);
                }
                free(buffer);
            }

            fclose(file);
        }
    }
    return rval;
}

/**
 * @brief Flush all Avro records to disk
 * @param router Avro router instance
 */
void avro_flush_all_tables(AVRO_INSTANCE *router)
{
    HASHITERATOR *iter = hashtable_iterator(router->open_tables);

    if (iter)
    {
        char *key;
        while ((key = (char*)hashtable_next(iter)))
        {
            AVRO_TABLE *table = hashtable_fetch(router->open_tables, key);

            if (table)
            {
                avro_file_writer_flush(table->avro_file);
            }
        }
        hashtable_iterator_free(iter);
    }
}

/**
 * @brief Detection of table creation statements
 * @param router Avro router instance
 * @param ptr Pointer to statement
 * @param len Statement length
 * @return True if the statement creates a new table
 */
bool is_create_table_statement(AVRO_INSTANCE *router, char* ptr, size_t len)
{
    int rc = 0;
    pcre2_match_data *mdata = pcre2_match_data_create_from_pattern(router->create_table_re, NULL);

    if (mdata)
    {
        rc = pcre2_match(router->create_table_re, (PCRE2_SPTR) ptr, len, 0, 0, mdata, NULL);
        pcre2_match_data_free(mdata);
    }

    return rc > 0;
}


/**
 * @brief Detection of table alteration statements
 * @param router Avro router instance
 * @param ptr Pointer to statement
 * @param len Statement length
 * @return True if the statement alters a table
 */
bool is_alter_table_statement(AVRO_INSTANCE *router, char* ptr, size_t len)
{
    int rc = 0;
    pcre2_match_data *mdata = pcre2_match_data_create_from_pattern(router->alter_table_re, NULL);

    if (mdata)
    {
        rc = pcre2_match(router->alter_table_re, (PCRE2_SPTR) ptr, len, 0, 0, mdata, NULL);
        pcre2_match_data_free(mdata);
    }

    return rc > 0;
}

/** Database name offset */
#define DBNM_OFF 8

/** Varblock offset */
#define VBLK_OFF 4 + 4 + 1 + 2

/** Post-header offset */
#define PHDR_OFF 4 + 4 + 1 + 2 + 2

/**
 * Save the CREATE TABLE statement to disk and replace older versions of the table
 * in the router's hashtable.
 * @param router Avro router instance
 * @param created Created table
 * @return False if an error occurred and true if successful
 */
bool save_and_replace_table_create(AVRO_INSTANCE *router, TABLE_CREATE *created)
{
    char createlist[PATH_MAX + 1];
    snprintf(createlist, sizeof(createlist), "%s/%s", router->avrodir, ddl_list_name);

    if (!table_create_save(created, createlist))
    {
        return false;
    }

    char table_ident[MYSQL_TABLE_MAXLEN + MYSQL_DATABASE_MAXLEN + 2];
    snprintf(table_ident, sizeof(table_ident), "%s.%s", created->database, created->table);

    spinlock_acquire(&router->lock); // Is this necessary?
    TABLE_CREATE *old = hashtable_fetch(router->created_tables, table_ident);

    if (old)
    {
        HASHITERATOR *iter = hashtable_iterator(router->table_maps);

        char *key;
        while ((key = hashtable_next(iter)))
        {
            if (strcmp(key, table_ident) == 0)
            {
                hashtable_delete(router->table_maps, key);
            }
        }

        hashtable_iterator_free(iter);

        hashtable_delete(router->created_tables, table_ident);
    }

    hashtable_add(router->created_tables, table_ident, created);
    ss_dassert(created->columns > 0);
    spinlock_release(&router->lock);
    return true;
}

void unify_whitespace(char *sql, int len)
{
    for (int i = 0; i < len; i++)
    {
        if (isspace(sql[i]) && sql[i] != ' ')
        {
            sql[i] = ' ';
        }
    }
}

/**
 * @brief Simple detection of CREATE TABLE statements
 *
 * @param router Avro router instance
 * @param hdr Replication header
 * @param pending_transaction Pointer where status of pending transaction is stored
 * @param ptr Pointer to the start of the event payload
 */
void handle_query_event(AVRO_INSTANCE *router, REP_HEADER *hdr, int *pending_transaction, uint8_t *ptr)
{
    int dblen = ptr[DBNM_OFF];
    int vblklen = ptr[VBLK_OFF];
    int len = hdr->event_size - BINLOG_EVENT_HDR_LEN - (PHDR_OFF + vblklen + 1 + dblen) + 1;
    char *sql = (char *) ptr + PHDR_OFF + vblklen + 1 + dblen;
    char db[dblen + 1];
    strncpy(db, (char*) ptr + PHDR_OFF + vblklen, sizeof(db));

    unify_whitespace(sql, len);
    size_t sqlsz = len, tmpsz = len;
    char *tmp = malloc(len);
    remove_mysql_comments((const char**)&sql, &sqlsz, &tmp, &tmpsz);
    sql = tmp;
    len = tmpsz;

    if (is_create_table_statement(router, sql, len))
    {
        TABLE_CREATE *created = table_create_alloc(sql, db);

        if (created && !save_and_replace_table_create(router, created))
        {
            MXS_ERROR("Failed to save statement to disk: %.*s", len, sql);
        }
    }
    else if (is_alter_table_statement(router, sql, len))
    {
        char ident[MYSQL_TABLE_MAXLEN + MYSQL_DATABASE_MAXLEN + 2];
        char full_ident[MYSQL_TABLE_MAXLEN + MYSQL_DATABASE_MAXLEN + 2];
        read_alter_identifier(sql, sql + len, ident, sizeof(ident));

        if (strnlen(db, 1) && strchr(ident, '.') == NULL)
        {
            snprintf(full_ident, sizeof(full_ident), "%s.%s", db, ident);
        }
        else
        {
            strncpy(full_ident, ident, sizeof(full_ident));
        }

        TABLE_CREATE *created = hashtable_fetch(router->created_tables, full_ident);
        ss_dassert(created);

        if (created)
        {
            table_create_alter(created, sql, sql + len);
        }
        else
        {
            MXS_ERROR("Alter statement to a table with no create statement.");
        }
    }
    /* A transaction starts with this event */
    else if (strncmp(sql, "BEGIN", 5) == 0)
    {
        *pending_transaction = 1;
    }
    /* Commit received for non transactional tables, i.e. MyISAM */
    else if (strncmp(sql, "COMMIT", 6) == 0)
    {
        // TODO: Handle COMMIT
        *pending_transaction = 0;
    }

    free(tmp);
}
