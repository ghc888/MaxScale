/*
 * This file is distributed as part of MaxScale by MariaDB Corporation.  It is free
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
 * Copyright MariaDB Corporation Ab 2014
 */

#include <filter.h>
#include <modinfo.h>
#include <modutil.h>
#include <buffer.h>
#include <log_manager.h>
#include <strings.h>
#include <mysql_client_server_protocol.h>
#include <maxscale/poll.h>
#include <modutil.h>

/**
 * @file datastream.c - Streaming of bulk inserts
 */

MODULE_INFO info =
{
    MODULE_API_FILTER,
    MODULE_ALPHA_RELEASE,
    FILTER_VERSION,
    "Data streaming filter"
};

static char *version_str = "1.0.0";

static FILTER *createInstance(char **options, FILTER_PARAMETER **params);
static void *newSession(FILTER *instance, SESSION *session);
static void closeSession(FILTER *instance, void *session);
static void freeSession(FILTER *instance, void *session);
static void setDownstream(FILTER *instance, void *fsession, DOWNSTREAM *downstream);
static void setUpstream(FILTER *instance, void *session, UPSTREAM *upstream);
static int routeQuery(FILTER *instance, void *fsession, GWBUF *queue);
static void diagnostic(FILTER *instance, void *fsession, DCB *dcb);
static int clientReply(FILTER* instance, void *session, GWBUF *reply);
static bool extract_insert_target(GWBUF *buffer, char* target, int len);
static GWBUF* create_load_data_command(const char *target);
static GWBUF* convert_to_stream(GWBUF* buffer, uint8_t packet_num);

static FILTER_OBJECT MyObject =
{
    createInstance,
    newSession,
    closeSession,
    freeSession,
    setDownstream,
    setUpstream,
    routeQuery,
    clientReply,
    diagnostic,
};

/**
 * Instance structure
 */
typedef struct
{
    char *source; /*< Source address to restrict matches */
    char *user; /*< User name to restrict matches */
} DS_INSTANCE;

enum ds_state
{
    DS_STREAM_CLOSED,
    DS_REQUEST_SENT,
    DS_REQUEST_ACCEPTED,
    DS_STREAM_OPEN,
    DS_CLOSING_STREAM
};

/**
 * The session structure for this regex filter
 */
typedef struct
{
    DOWNSTREAM down; /* The downstream filter */
    UPSTREAM up;
    SPINLOCK lock;
    GWBUF *queue;
    GWBUF *writebuf;
    bool active;
    bool in_trx; /*< Whether BEGIN has been seen */
    uint8_t packet_num;
    DCB* client_dcb;
    enum ds_state state; /*< Whether a LOAD DATA LOCAL INFILE was sent or not */
} DS_SESSION;

/**
 * Implementation of the mandatory version entry point
 *
 * @return version string of the module
 */
char *
version()
{
    return version_str;
}

/**
 * The module initialisation routine, called when the module
 * is first loaded.
 */
void
ModuleInit()
{
}

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
FILTER_OBJECT *
GetModuleObject()
{
    return &MyObject;
}

/**
 * Free a insertstream instance.
 * @param instance instance to free
 */
void free_instance(DS_INSTANCE *instance)
{
    if (instance)
    {
        free(instance->source);
        free(instance->user);
        free(instance);
    }
}

static const char load_data_template[] = "LOAD DATA LOCAL INFILE 'maxscale.data' INTO TABLE %s FIELDS TERMINATED BY ',' LINES TERMINATED BY '\n'";

/**
 * Create an instance of the filter for a particular service
 * within MaxScale.
 *
 * @param options   The options for this filter
 * @param params    The array of name/value pair parameters for the filter
 *
 * @return The instance data for this new instance
 */
static FILTER *
createInstance(char **options, FILTER_PARAMETER **params)
{
    DS_INSTANCE *my_instance;

    if ((my_instance = calloc(1, sizeof(DS_INSTANCE))) != NULL)
    {
        for (int i = 0; params && params[i]; i++)
        {
            if (!strcmp(params[i]->name, "source"))
            {
                my_instance->source = strdup(params[i]->value);
            }
            else if (!strcmp(params[i]->name, "user"))
            {
                my_instance->user = strdup(params[i]->value);
            }
            else if (!filter_standard_parameter(params[i]->name))
            {
                MXS_ERROR("insertstream: Unexpected parameter '%s'.",
                          params[i]->name);
            }
        }
    }
    return (FILTER *) my_instance;
}

/**
 * Associate a new session with this instance of the filter.
 *
 * @param instance  The filter instance data
 * @param session   The session itself
 * @return Session specific data for this session
 */
static void *
newSession(FILTER *instance, SESSION *session)
{
    DS_INSTANCE *my_instance = (DS_INSTANCE *) instance;
    DS_SESSION *my_session;
    char *remote, *user;

    if ((my_session = calloc(1, sizeof(DS_SESSION))) != NULL)
    {
        my_session->state = DS_STREAM_CLOSED;
        my_session->active = true;
        my_session->in_trx = false;
        my_session->client_dcb = session->client_dcb;

        if (my_instance->source
            && (remote = session_get_remote(session)) != NULL)
        {
            if (strcmp(remote, my_instance->source))
            {
                my_session->active = false;
            }
        }

        if (my_instance->user && (user = session_getUser(session))
            && strcmp(user, my_instance->user))
        {
            my_session->active = false;
        }
    }

    return my_session;
}

/**
 * Close a session with the filter, this is the mechanism
 * by which a filter may cleanup data structure etc.
 *
 * @param instance  The filter instance data
 * @param session   The session being closed
 */
static void
closeSession(FILTER *instance, void *session)
{
}

/**
 * Free the memory associated with this filter session.
 *
 * @param instance  The filter instance data
 * @param session   The session being closed
 */
static void
freeSession(FILTER *instance, void *session)
{
    free(session);
    return;
}

/**
 * Set the downstream component for this filter.
 *
 * @param instance  The filter instance data
 * @param session   The session being closed
 * @param downstream    The downstream filter or router
 */
static void
setDownstream(FILTER *instance, void *session, DOWNSTREAM *downstream)
{
    DS_SESSION *my_session = (DS_SESSION*) session;
    my_session->down = *downstream;
}

/**
 * Set the filter upstream
 * @param instance Filter instance
 * @param session Filter session
 * @param upstream Upstream filter
 */
static void setUpstream(FILTER *instance, void *session, UPSTREAM *upstream)
{
    DS_SESSION *my_session = (DS_SESSION*) session;
    my_session->up = *upstream;
}

/**
 * The routeQuery entry point. This is passed the query buffer
 * to which the filter should be applied. Once applied the
 * query should normally be passed to the downstream component
 * (filter or router) in the filter chain.
 *
 * @param instance  The filter instance data
 * @param session   The filter session
 * @param queue     The query data
 */
static int
routeQuery(FILTER *instance, void *session, GWBUF *queue)
{
    DS_SESSION *my_session = (DS_SESSION *) session;
    char target[MYSQL_TABLE_MAXLEN + MYSQL_DATABASE_MAXLEN + 1];
    bool send_ok = false;

    my_session->writebuf = gwbuf_append(my_session->writebuf, queue);

    if ((queue = modutil_get_complete_packets(&my_session->writebuf)) == NULL)
    {
        return 1;
    }

    if (my_session->active && modutil_is_SQL(queue) && my_session->in_trx &&
        extract_insert_target(queue, target, sizeof(target)))
    {
        if (queue->next != NULL)
        {
            queue = gwbuf_make_contiguous(queue);
        }

        spinlock_acquire(&my_session->lock);

        if (my_session->state == DS_STREAM_CLOSED)
        {
            my_session->queue = queue;
            my_session->state = DS_REQUEST_SENT;
            my_session->packet_num = 0;
            spinlock_release(&my_session->lock);
            queue = create_load_data_command(target);
        }
        else if (my_session->state == DS_STREAM_OPEN)
        {
            uint8_t packet_num = ++my_session->packet_num;
            spinlock_release(&my_session->lock);
            send_ok = true;
            queue = convert_to_stream(queue, packet_num);
        }
        else
        {
            if (my_session->state == DS_REQUEST_ACCEPTED)
            {
                my_session->state = DS_STREAM_OPEN;
                send_ok = true;
            }
            spinlock_release(&my_session->lock);
        }
    }
    else
    {
        bool send_empty = false;
        uint8_t packet_num;
        char* sql;
        int size;

        spinlock_acquire(&my_session->lock);

        if (my_session->state == DS_STREAM_OPEN)
        {
            my_session->state = DS_CLOSING_STREAM;
            send_empty = true;
            packet_num = ++my_session->packet_num;
            my_session->queue = queue;
            my_session->in_trx = false;
        }
        else if (my_session->state == DS_REQUEST_ACCEPTED)
        {
            my_session->state = DS_STREAM_OPEN;
            send_ok = true;
        }
        else if (modutil_extract_SQL(queue, &sql, &size))
        {
            if (strncasecmp(sql, "begin", MIN(5, size)) ||
                strncasecmp(sql, "start transaction", MIN(17, size)))
            {
                my_session->in_trx = true;
            }
        }

        spinlock_release(&my_session->lock);

        if (send_empty)
        {
            char empty_packet[] = {0, 0, 0, packet_num};
            queue = gwbuf_alloc_and_load(sizeof(empty_packet), &empty_packet[0]);
        }
    }

    if (send_ok)
    {
        modutil_send_ok_packet(my_session->client_dcb, 1);
    }

    return my_session->down.routeQuery(my_session->down.instance,
                                       my_session->down.session, queue);
}

static char* get_value(char* data, uint32_t datalen, char** dest, uint32_t* destlen)
{
    char *value_start = strnchr_esc_mysql(data, '(', datalen);

    if (value_start)
    {
        value_start++;
        char *value_end = strnchr_esc_mysql(value_start, ')', datalen - (value_start - data));

        if (value_end)
        {
            *destlen = value_end - value_start;
            *dest = value_start;
            return value_end;
        }
    }

    return NULL;
}

static GWBUF* convert_to_stream(GWBUF* buffer, uint8_t packet_num)
{
    /** Remove the INSERT INTO ... from the buffer */
    char *dataptr = (char*) GWBUF_DATA(buffer);
    char *modptr = strnchr_esc_mysql(dataptr + 5, '(', GWBUF_LENGTH(buffer));

    /** Leave some space for the header so we don't have to allocate a new one */
    buffer = gwbuf_consume(buffer, (modptr - dataptr) - 4);
    uint32_t len_before = gwbuf_length(buffer) - 4;
    char* header_start = (char*)GWBUF_DATA(buffer);
    char* store_end = dataptr = header_start + 4;
    char* end = buffer->end;
    char* value;
    uint32_t valuesize;

    /** Remove the parentheses from the insert and recalculate the packet length */
    while ((dataptr = get_value(dataptr, end - dataptr, &value, &valuesize)))
    {
        memmove(store_end, value, valuesize);
        store_end += valuesize;
        *store_end++ = '\n';
    }

    gwbuf_rtrim(buffer, (char*)buffer->end - store_end);
    uint32_t len = gwbuf_length(buffer) - 4;

    *header_start++ = len;
    *header_start++ = len >> 8;
    *header_start++ = len >> 16;
    *header_start = packet_num;

    return buffer;
}

static int clientReply(FILTER* instance, void *session, GWBUF *reply)
{
    DS_SESSION *my_session = (DS_SESSION*) session;

    spinlock_acquire(&my_session->lock);

    if (my_session->state == DS_REQUEST_SENT && !MYSQL_IS_ERROR_PACKET(GWBUF_DATA(reply)))
    {
        gwbuf_free(reply);
        ss_dassert(my_session->queue);
        
        my_session->state = DS_REQUEST_ACCEPTED;
        GWBUF* queue = my_session->queue;
        my_session->queue = NULL;
        
        /** The request is packet 0 and the response is packet 1 so we'll
         * have to send the data in packet number 2 */
        my_session->packet_num += 2;
        uint8_t packet_num = my_session->packet_num;
        spinlock_release(&my_session->lock);
        
        GWBUF* databuf = convert_to_stream(queue, packet_num);
        poll_add_epollin_event_to_dcb(my_session->client_dcb, databuf);
    }
    else if (my_session->state == DS_CLOSING_STREAM)
    {
        gwbuf_free(reply);
        poll_add_epollin_event_to_dcb(my_session->client_dcb, my_session->queue);
        my_session->state = DS_STREAM_CLOSED;
        spinlock_release(&my_session->lock);
    }
    else
    {
        spinlock_release(&my_session->lock);
        return my_session->up.clientReply(my_session->up.instance,
                                          my_session->up.session,
                                          reply);
    }

    return 0;
}

/**
 * Diagnostics routine
 *
 * If fsession is NULL then print diagnostics on the filter
 * instance as a whole, otherwise print diagnostics for the
 * particular session.
 *
 * @param   instance    The filter instance
 * @param   fsession    Filter session, may be NULL
 * @param   dcb     The DCB for diagnostic output
 */
static void
diagnostic(FILTER *instance, void *fsession, DCB *dcb)
{
    DS_INSTANCE *my_instance = (DS_INSTANCE *) instance;

    if (my_instance->source)
    {
        dcb_printf(dcb, "\t\tReplacement limited to connections from     %s\n", my_instance->source);
    }
    if (my_instance->user)
    {
        dcb_printf(dcb, "\t\tReplacement limit to user           %s\n", my_instance->user);
    }
}

/**
 * Check if a buffer contains an insert statement
 *
 * @param   buffer Buffer to analyze
 * @return  True if the buffer contains an insert statement
 */
static bool extract_insert_target(GWBUF *buffer, char* target, int len)
{
    char *ptr = (char*) GWBUF_DATA(buffer) + MYSQL_HEADER_LEN + 1;

    while (isspace(*ptr))
    {
        ptr++;
    }

    if (strncasecmp(ptr, "insert", 6) == 0)
    {
        ptr += 6;

        while (isspace(*ptr))
        {
            ptr++;
        }

        if (strncasecmp(ptr, "into", 4) == 0)
        {
            ptr += 4;

            while (isspace(*ptr))
            {
                ptr++;
            }

            char *start = ptr;
            while (!isspace(*ptr) && ptr < (char*) buffer->end)
            {
                ptr++;
            }

            snprintf(target, len, "%.*s", (int) (ptr - start), start);
            return true;
        }
    }

    return false;
}

static GWBUF* create_load_data_command(const char *target)
{
    char str[sizeof(load_data_template) + strlen(target) + 1];
    snprintf(str, sizeof(str), load_data_template, target);
    uint32_t payload = strlen(str) + 1;

    GWBUF *rval = gwbuf_alloc(payload + 4);
    if (rval)
    {
        uint8_t *ptr = GWBUF_DATA(rval);
        *ptr++ = payload;
        *ptr++ = payload >> 8;
        *ptr++ = payload >> 16;
        *ptr++ = 0;
        *ptr++ = 0x03;
        memcpy(ptr, str, payload - 1);
    }

    return rval;
}
