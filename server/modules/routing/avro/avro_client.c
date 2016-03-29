/*
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
 * Copyright MariaDB Corporation Ab 2015-2016
 */

/**
 * @file avro_client.c - contains code for the AVRO router to client communication
 *
 * @verbatim
 * Revision History
 *
 * Date     Who         Description
 * 10/03/2016   Massimiliano Pinto   Initial implementation
 * 11/03/2016   Massimiliano Pinto   Addition of JSON output
 *
 * @endverbatim
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <service.h>
#include <server.h>
#include <router.h>
#include <atomic.h>
#include <spinlock.h>
#include <blr.h>
#include <dcb.h>
#include <spinlock.h>
#include <housekeeper.h>
#include <sys/stat.h>
#include <skygw_types.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <version.h>
#include <zlib.h>

/* AVRO */
#include <avrorouter.h>
#include <maxavro.h>

extern int load_mysql_users(SERVICE *service);
extern int blr_save_dbusers(ROUTER_INSTANCE *router);
extern void blr_master_close(ROUTER_INSTANCE* router);
extern void blr_file_use_binlog(ROUTER_INSTANCE *router, char *file);
extern int blr_file_new_binlog(ROUTER_INSTANCE *router, char *file);
extern int blr_file_write_master_config(ROUTER_INSTANCE *router, char *error);
extern char *blr_extract_column(GWBUF *buf, int col);
extern uint32_t extract_field(uint8_t *src, int bits);
extern int MaxScaleUptime();

/* AVRO */
static int avro_client_do_registration(AVRO_INSTANCE *, AVRO_CLIENT *, GWBUF *);
static int avro_client_binlog_dump(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, GWBUF *queue);
int avro_client_callback(DCB *dcb, DCB_REASON reason, void *data);
static void avro_client_process_command(AVRO_INSTANCE *router, AVRO_CLIENT *client, GWBUF *queue);
static bool avro_client_stream_data(AVRO_CLIENT *client);
void avro_notify_client(AVRO_CLIENT *client);
void poll_fake_write_event(DCB *dcb);
GWBUF* read_avro_json_schema(const char *avrofile, const char* dir);
GWBUF* read_avro_binary_schema(const char *avrofile, const char* dir);
const char* get_avrofile_name(const char *file_ptr, int data_len, char *dest);
bool file_in_dir(const char *dir, const char *file);

/**
 * Process a request packet from the slave server.
 *
 * @param router    The router instance this defines the master for this replication chain
 * @param client    The client specific data
 * @param queue     The incoming request packet
 */
int
avro_client_handle_request(AVRO_INSTANCE *router, AVRO_CLIENT *client, GWBUF *queue)
{
    int reg_ret;

    switch (client->state)
    {
        case AVRO_CLIENT_ERRORED:
            /* force disconnection */
            return 1;
            break;
        case AVRO_CLIENT_UNREGISTERED:
            /* Cal registration routine */
            reg_ret = avro_client_do_registration(router, client, queue);

            /* discard data in incoming buffer */
            gwbuf_free(queue);

            if (reg_ret == 0)
            {
                client->state = AVRO_CLIENT_ERRORED;
                dcb_printf(client->dcb, "ERR, code 12, msg: Registration failed");
                /* force disconnection */
                dcb_close(client->dcb);
                return 0;
            }
            else
            {
                /* Send OK ack to client */
                dcb_printf(client->dcb, "OK");

                client->state = AVRO_CLIENT_REGISTERED;
                MXS_INFO("%s: Client [%s] has completed REGISTRATION action",
                         client->dcb->service->name,
                         client->dcb->remote != NULL ? client->dcb->remote : "");

                break;
            }
        case AVRO_CLIENT_REGISTERED:
        case AVRO_CLIENT_REQUEST_DATA:
            if (client->state == AVRO_CLIENT_REGISTERED)
            {
                client->state = AVRO_CLIENT_REQUEST_DATA;
            }

            /* Process command from client */
            avro_client_process_command(router, client, queue);

            break;
        default:
            client->state = AVRO_CLIENT_ERRORED;
            return 1;
            break;
    }

    return 0;
}

/**
 * Handle the REGISTRATION command
 *
 * @param dcb    DCB with allocateid protocol
 * @param data   GWBUF with registration message
 * @return       1 for successful registration 0 otherwise
 *
 */
static int
avro_client_do_registration(AVRO_INSTANCE *router, AVRO_CLIENT *client, GWBUF *data)
{
    const char reg_uuid[] = "REGISTER UUID=";
    const int reg_uuid_len = strlen(reg_uuid);
    int data_len = GWBUF_LENGTH(data) - reg_uuid_len;
    char *request = GWBUF_DATA(data);
    int ret = 0;

    if (strstr(request, reg_uuid) != NULL)
    {
        char *sep_ptr;
        int uuid_len = (data_len > CDC_UUID_LEN) ? CDC_UUID_LEN : data_len;
        /* 36 +1 */
        char uuid[CDC_UUID_LEN + 1];
        strncpy(uuid, request + reg_uuid_len, uuid_len);
        uuid[uuid_len] = '\0';

        if ((sep_ptr = strchr(uuid, ',')) != NULL)
        {
            *sep_ptr = '\0';
        }
        if ((sep_ptr = strchr(uuid + strlen(uuid), ' ')) != NULL)
        {
            *sep_ptr = '\0';
        }
        if ((sep_ptr = strchr(uuid, ' ')) != NULL)
        {
            *sep_ptr = '\0';
        }

        if (strlen(uuid) < uuid_len)
        {
            data_len -= (uuid_len - strlen(uuid));
        }

        uuid_len = strlen(uuid);

        client->uuid = strdup(uuid);

        if (data_len > 0)
        {
            /* Check for CDC request type */
            char *tmp_ptr = strstr(request + sizeof(reg_uuid) + uuid_len, "TYPE=");
            if (tmp_ptr)
            {
                if (memcmp(tmp_ptr + 5, "AVRO", 4) == 0)
                {
                    ret = 1;
                    client->state = AVRO_CLIENT_REGISTERED;
                    client->format = AVRO_FORMAT_AVRO;
                }
                else if (memcmp(tmp_ptr + 5, "JSON", 4) == 0)
                {
                    ret = 1;
                    client->state = AVRO_CLIENT_REGISTERED;
                    client->format = AVRO_FORMAT_JSON;
                }
                else
                {
                    fprintf(stderr, "Registration TYPE not supported, only AVRO\n");
                }
            }
            else
            {
                fprintf(stderr, "TYPE not found in Registration\n");
            }
        }
        else
        {
            fprintf(stderr, "Registration data_len = 0\n");
        }
    }
    return ret;
}

/**
 * Extract the GTID the client requested
 * @param gtid
 * @param start
 * @param end
 */
void extract_gtid_request(gtid_pos_t *gtid, const char *start, int len)
{
    const char *ptr = start;
    int read = 0;

    while (ptr < start + len)
    {
        if (!isdigit(*ptr))
        {
            ptr++;
        }
        else
        {
            char *end;
            switch (read)
            {
                case 0:
                    gtid->domain = strtol(ptr, &end, 10);
                    break;
                case 1:
                    gtid->server_id = strtol(ptr, &end, 10);
                    break;
                case 2:
                    gtid->seq = strtol(ptr, &end, 10);
                    break;
            }
            read++;
            ptr = end;
        }
    }
}

/**
 * Process command from client
 *
 * @param router     The router instance
 * @param client     The specific client data
 * @param data       GWBUF with command
 *
 */
static void
avro_client_process_command(AVRO_INSTANCE *router, AVRO_CLIENT *client, GWBUF *queue)
{
    const char req_data[] = "REQUEST-DATA";
    const size_t req_data_len = sizeof(req_data) - 1;
    uint8_t *data = GWBUF_DATA(queue);
    char *command_ptr = strstr((char *)data, req_data);

    if (command_ptr != NULL)
    {
        char *file_ptr = command_ptr + req_data_len;
        int data_len = GWBUF_LENGTH(queue) - req_data_len;

        if (data_len > 1)
        {
            const char *gtid_ptr = get_avrofile_name(file_ptr, data_len, client->avro_binfile);

            if (gtid_ptr)
            {
                client->requested_gtid = true;
                extract_gtid_request(&client->gtid, gtid_ptr, data_len - (gtid_ptr - file_ptr));
                memcpy(&client->gtid_start, &client->gtid, sizeof(client->gtid_start));
            }

            if (file_in_dir(router->avrodir, client->avro_binfile))
            {
                /** Send the first schema */
                GWBUF *schema = NULL;

                switch (client->format)
                {
                    case AVRO_FORMAT_JSON:
                        schema = read_avro_json_schema(client->avro_binfile, router->avrodir);
                        break;

                    case AVRO_FORMAT_AVRO:
                        schema = read_avro_binary_schema(client->avro_binfile, router->avrodir);
                        break;

                    default:
                        MXS_ERROR("Unknown client format: %d", client->format);
                }

                if (schema)
                {
                    client->dcb->func.write(client->dcb, schema);
                }

                /* set callback routine for data sending */
                dcb_add_callback(client->dcb, DCB_REASON_DRAINED, avro_client_callback, client);

                /* Add fake event that will call the avro_client_callback() routine */
                poll_fake_write_event(client->dcb);
            }
            else
            {
                dcb_printf(client->dcb, "ERR NO-FILE File '%s' not found.", client->avro_binfile);
            }
        }
        else
        {
            dcb_printf(client->dcb, "ERR REQUEST-DATA with no data");
        }
    }
    else
    {
        GWBUF *reply = gwbuf_alloc(5);
        memcpy(GWBUF_DATA(reply), "ECHO:", 5);
        reply = gwbuf_append(reply, queue);
        client->dcb->func.write(client->dcb, reply);
    }
}

/**
 * @brief Check if a file exists in a directory
 *
 * @param dir Directory where the file is searched
 * @param file File to search
 * @return True if file exists
 */
bool file_in_dir(const char *dir, const char *file)
{
    char path[PATH_MAX + 1];

    snprintf(path, sizeof(path), "%s/%s", dir, file);

    return access(path, F_OK) == 0;
}

/**
 * @brief Form the full Avro file name
 *
 * @param file_ptr Requested file
 * @param data_len Length of string pointed by @p file_ptr
 * @param dest Destination where the file name is stored. Must be at least
 * @p data_len + 1 bytes.
 */
const char* get_avrofile_name(const char *file_ptr, int data_len, char *dest)
{
    while (isspace(*file_ptr))
    {
        file_ptr++;
        data_len--;
    }

    char avro_file[data_len + 1];
    memcpy(avro_file, file_ptr, data_len);
    avro_file[data_len] = '\0';

    char *cmd_sep = strchr(avro_file, ' ');
    const char *rval = NULL;

    if (cmd_sep)
    {
        *cmd_sep++ = '\0';
        rval = file_ptr + (cmd_sep - avro_file);
        ss_dassert(rval < file_ptr + data_len);
    }

    /** Exact file version specified */
    if ((cmd_sep = strchr(avro_file, '.')) && (cmd_sep = strchr(cmd_sep + 1, '.')) &&
        strlen(cmd_sep + 1) > 0)
    {
        snprintf(dest, AVRO_MAX_FILENAME_LEN, "%s.avro", avro_file);
    }
    /** No version specified, send all files */
    else
    {
        snprintf(dest, AVRO_MAX_FILENAME_LEN, "%s.000001.avro", avro_file);
    }

    return rval;
}

static int send_row(DCB *dcb, json_t* row)
{
    char *json = json_dumps(row, JSON_PRESERVE_ORDER);
    GWBUF *buf;
    int rc = 0;

    if (json && (buf = gwbuf_alloc_and_load(strlen(json), (void*)json)))
    {
        rc = dcb->func.write(dcb, buf);
    }
    else
    {
        MXS_ERROR("Failed to dump JSON value.");
        rc = 0;
    }
    free(json);
    return rc;
}

static void set_current_gtid(AVRO_CLIENT *client, json_t *row)
{
    json_t *obj = json_object_get(row, avro_sequence);
    ss_dassert(json_is_integer(obj));
    client->gtid.seq = json_integer_value(obj);

    obj = json_object_get(row, avro_server_id);
    ss_dassert(json_is_integer(obj));
    client->gtid.server_id = json_integer_value(obj);

    obj = json_object_get(row, avro_domain);
    ss_dassert(json_is_integer(obj));
    client->gtid.domain = json_integer_value(obj);
}

/**
 * @brief Stream Avro data in JSON format
 *
 * @param file File to stream from
 * @param dcb DCB to stream to
 * @return True if more data is readable, false if all data was sent
 */
static bool stream_json(AVRO_CLIENT *client)
{
    int bytes = 0;
    MAXAVRO_FILE *file = client->file_handle;
    DCB *dcb = client->dcb;

    do
    {
        json_t *row;
        int rc = 1;
        while (rc > 0 && (row = maxavro_record_read_json(file)))
        {
            rc = send_row(dcb, row);
            set_current_gtid(client, row);
            json_decref(row);
        }
        bytes += file->block_size;
    }
    while (maxavro_next_block(file) && bytes < AVRO_DATA_BURST_SIZE);

    return bytes >= AVRO_DATA_BURST_SIZE;
}

/**
 * @brief Stream Avro data in native Avro format
 *
 * @param file File to stream from
 * @param dcb DCB to stream to
 * @return True if streaming was successful, false if an error occurred
 */
static bool stream_binary(AVRO_CLIENT *client)
{
    GWBUF *buffer;
    uint64_t bytes = 0;
    int rc = 1;
    MAXAVRO_FILE *file = client->file_handle;
    DCB *dcb = client->dcb;

    while (rc > 0 && bytes < AVRO_DATA_BURST_SIZE)
    {
        bytes += file->block_size;
        if ((buffer = maxavro_record_read_binary(file)))
        {
            rc = dcb->func.write(dcb, buffer);
        }
        else
        {
            rc = 0;
        }
    }

    return bytes >= AVRO_DATA_BURST_SIZE;
}

/**
 *
 * @param client
 * @param file
 */
static bool seek_to_gtid(AVRO_CLIENT *client, MAXAVRO_FILE* file)
{
    bool seeking = true;

    do
    {
        json_t *row;
        while ((row = maxavro_record_read_json(file)))
        {
            json_t *obj = json_object_get(row, avro_sequence);
            ss_dassert(json_is_integer(obj));
            uint64_t value = json_integer_value(obj);

            /** If a larger GTID is found, use that */
            if (value >= client->gtid.seq)
            {
                obj = json_object_get(row, avro_server_id);
                ss_dassert(json_is_integer(obj));
                value = json_integer_value(obj);

                if (value == client->gtid.server_id)
                {
                    obj = json_object_get(row, avro_domain);
                    ss_dassert(json_is_integer(obj));
                    value = json_integer_value(obj);

                    if (value == client->gtid.domain)
                    {
                        MXS_INFO("Found GTID %lu-%lu-%lu for %s@%s",
                                 client->gtid.domain, client->gtid.server_id,
                                 client->gtid.seq, client->dcb->user, client->dcb->remote);
                        seeking = false;
                    }
                }
            }

            /** We'll send the first found row immediately since we have already
             * read the row into memory */
            if (!seeking)
            {
                send_row(client->dcb, row);
            }

            json_decref(row);
        }
    }
    while (seeking && maxavro_next_block(file));

    return !seeking;
}

/**
 * Print JSON output from selected AVRO file
 *
 * @param router     The router instance
 * @param client     The specific client data
 * @param avro_file  The requested AVRO file
 * @return True if more data needs to be read
 */
static bool avro_client_stream_data(AVRO_CLIENT *client)
{
    bool read_more = false;
    AVRO_INSTANCE *router = client->router;

    if (strnlen(client->avro_binfile, 1))
    {
        char filename[PATH_MAX + 1];
        snprintf(filename, PATH_MAX, "%s/%s", router->avrodir, client->avro_binfile);

        spinlock_acquire(&client->file_lock);
        if (client->file_handle == NULL)
        {
            client->file_handle = maxavro_file_open(filename);
        }
        spinlock_release(&client->file_lock);

        switch (client->format)
        {
            case AVRO_FORMAT_JSON:
                /** Currently only JSON format supports seeking to a GTID */
                if (client->requested_gtid && seek_to_gtid(client, client->file_handle))
                {
                    client->requested_gtid = false;
                }

                read_more = stream_json(client);
                break;

            case AVRO_FORMAT_AVRO:
                read_more = stream_binary(client);
                break;

            default:
                MXS_ERROR("Unexpected format: %d", client->format);
                break;
        }


        if (maxavro_get_error(client->file_handle) != MAXAVRO_ERR_NONE)
        {
            MXS_ERROR("Reading Avro file failed with error '%s'.",
                      maxavro_get_error_string(client->file_handle));
        }

        /* update client struct */
        memcpy(&client->avro_file, client->file_handle, sizeof(client->avro_file));

        /* may be just use client->avro_file->records_read and remove this var */
        client->last_sent_pos = client->avro_file.records_read;
    }
    else
    {
        fprintf(stderr, "No file specified\n");
        dcb_printf(client->dcb, "ERR avro file not specified");
    }

    return read_more;
}

GWBUF* read_avro_json_schema(const char *avrofile, const char* dir)
{
    GWBUF* rval = NULL;
    const char *suffix = strrchr(avrofile, '.');

    if (suffix)
    {
        char buffer[PATH_MAX + 1];
        snprintf(buffer, sizeof(buffer), "%s/%.*s.avsc", dir, (int)(suffix - avrofile),
                 avrofile);
        FILE *file = fopen(buffer, "rb");

        if (file)
        {
            int nread;
            while ((nread = fread(buffer, 1, sizeof(buffer), file)) > 0)
            {
                while (isspace(buffer[nread - 1]))
                {
                    nread--;
                }

                GWBUF * newbuf = gwbuf_alloc_and_load(nread, buffer);

                if (newbuf)
                {
                    rval = gwbuf_append(rval, newbuf);
                }
            }

            fclose(file);
        }
        else
        {
            char err[STRERROR_BUFLEN];
            MXS_ERROR("Failed to open file '%s': %d, %s", buffer, errno,
                      strerror_r(errno, err, sizeof(err)));
        }
    }
    return rval;
}

GWBUF* read_avro_binary_schema(const char *avrofile, const char* dir)
{
    GWBUF* rval = NULL;
    char buffer[PATH_MAX + 1];
    snprintf(buffer, sizeof(buffer), "%s/%s", dir, avrofile);
    MAXAVRO_FILE *file = maxavro_file_open(buffer);

    if (file)
    {
        rval = maxavro_file_binary_header(file);
        maxavro_file_close(file);
    }
    else
    {
        MXS_ERROR("Failed to open file '%s'.", buffer);
    }

    return rval;
}

/**
 * Rotate to a new Avro file
 * @param client Avro client session
 * @param fullname Absolute path to the file to rotate to
 */
static void rotate_avro_file(AVRO_CLIENT *client, char *fullname)
{
    char *filename = strrchr(fullname, '/') + 1;
    strncpy(client->avro_binfile, filename, sizeof(client->avro_binfile));
    client->last_sent_pos = 0;

    GWBUF *schema = read_avro_json_schema(client->avro_binfile, client->router->avrodir);

    if (schema)
    {
        client->dcb->func.write(client->dcb, schema);
    }

    spinlock_acquire(&client->file_lock);
    maxavro_file_close(client->file_handle);
    client->file_handle = maxavro_file_open(fullname);

    if (client->file_handle == NULL)
    {
        MXS_ERROR("Failed to open file: %s", filename);
    }

    spinlock_release(&client->file_lock);
}

/**
 * Print the name of the next Avro file
 * @param file Current filename
 * @param dir Directory where the files exist
 * @param dest Destination where the full path to the file is printed
 * @param len Size of @p dest
 */
static void print_next_filename(const char *file, const char *dir, char *dest, size_t len)
{
    char buffer[strlen(file) + 1];
    strncpy(buffer, file, sizeof(buffer));
    char *ptr = strrchr(buffer, '.');

    if (ptr)
    {
        ptr--;
        while (ptr > buffer && *(ptr) != '.')
        {
            ptr--;
        }

        int filenum = strtol(ptr + 1, NULL, 10);
        *ptr = '\0';
        snprintf(dest, len, "%s/%s.%06d.avro",
                 dir, buffer, filenum + 1);
    }
}

/**
 * @brief The client callback for sending data
 *
 * @param dcb Client DCB
 * @param reason Why the callback was called
 * @param userdata Data provided when the callback was added
 * @return Always 0
 */
int avro_client_callback(DCB *dcb, DCB_REASON reason, void *userdata)
{
    if (reason == DCB_REASON_DRAINED)
    {
        AVRO_CLIENT *client = (AVRO_CLIENT*)userdata;

        spinlock_acquire(&client->catch_lock);
        if (client->cstate & AVRO_CS_BUSY)
        {
            spinlock_release(&client->catch_lock);
            return 0;
        }

        client->cstate |= AVRO_CS_BUSY;
        spinlock_release(&client->catch_lock);

        /** Stream the data to the client */
        bool read_more = avro_client_stream_data(client);

        char filename[PATH_MAX + 1];
        print_next_filename(client->avro_binfile, client->router->avrodir,
                            filename, sizeof(filename));

        bool next_file;
        /** If the next file is available, send it to the client */
        if ((next_file = (access(filename, R_OK) == 0)))
        {
            rotate_avro_file(client, filename);
        }

        spinlock_acquire(&client->catch_lock);
        client->cstate &= ~AVRO_CS_BUSY;
        client->cstate |= AVRO_WAIT_DATA;

        if (next_file || read_more)
        {
#ifdef SS_DEBUG
            if (read_more)
            {
                MXS_DEBUG("Burst limit hit, need to read more data.");
            }
#endif
            avro_notify_client(client);
        }
        spinlock_release(&client->catch_lock);
    }

    return 0;
}

/**
 * @brief Notify a client that new data is available
 *
 * The client catch_lock must be held when calling this function.
 *
 * @param client Client to notify
 */
void avro_notify_client(AVRO_CLIENT *client)
{
    /* Add fake event that will call the avro_client_callback() routine */
    poll_fake_write_event(client->dcb);
    client->cstate &= ~AVRO_WAIT_DATA;
}
