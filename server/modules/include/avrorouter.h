/**
 * MaxScale AVRO router
 *
 */
#ifndef _MXS_AVRO_H
#define _MXS_AVRO_H
#include <stdbool.h>
#include <stdint.h>
#include <blr_constants.h>
#include <dcb.h>
#include <service.h>
#include <spinlock.h>
#include <mysql_binlog.h>
#include <dbusers.h>
#include <avro.h>
#include <cdc.h>
#include <maxscale_pcre2.h>
#include <maxavro.h>
#include <binlog_common.h>

/**
 * How often to call the router status function (seconds)
 */
#define AVRO_STATS_FREQ          60
#define AVRO_NSTATS_MINUTES      30

/**
 * Avro block grouping defaults
 */
#define AVRO_DEFAULT_BLOCK_TRX_COUNT 50
#define AVRO_DEFAULT_BLOCK_ROW_COUNT 1000

#define MAX_MAPPED_TABLES 1024

/** Avro filename maxlen */
#ifdef NAME_MAX
#define AVRO_MAX_FILENAME_LEN NAME_MAX
#else
#define AVRO_MAX_FILENAME_LEN 255
#endif

static char *avro_client_states[] = { "Unregistered", "Registered", "Processing", "Errored" };
static char *avro_client_client_mode[] = { "Catch-up", "Busy", "Wait_for_data"};

/** How a binlog file is closed */
typedef enum avro_binlog_end
{
    AVRO_OK = 0, /*< A newer binlog file exists with a rotate event to that file */
    AVRO_LAST_FILE, /* Last binlog which is closed */
    AVRO_OPEN_TRANSACTION, /*< The binlog ends with an open transaction */
    AVRO_BINLOG_ERROR /*< An error occurred while processing the binlog file */
} avro_binlog_end_t;

/** How many numbers each table version has (db.table.000001.avro) */
#define TABLE_MAP_VERSION_DIGITS 6

/** Maximum version number*/
#define TABLE_MAP_VERSION_MAX 999999

/** Maximum column name length */
#define TABLE_MAP_MAX_NAME_LEN 64

/** A CREATE TABLE abstraction */
typedef struct table_create
{
    uint64_t columns;
    char **column_names;
    char *table;
    char *database;
    char *table_definition;
    char gtid[GTID_MAX_LEN]; /*< the current GTID event or NULL if GTID is not enabled */
    int version; /*< How many versions of this table have been used */
    bool was_used; /*< Has this schema been persisted to disk */
} TABLE_CREATE;

/** A representation of a table map event read from a binary log. A table map
 * maps a table to a unique ID which can be used to match row events to table map
 * events. The table map event tells us how the table is laid out and gives us
 * some meta information on the columns. */
typedef struct table_map
{
    uint64_t id;
    uint64_t columns;
    uint16_t flags;
    uint8_t *column_types;
    uint8_t *null_bitmap;
    uint8_t *column_metadata;
    size_t column_metadata_size;
    TABLE_CREATE *table_create; /*< The definition of the table */
    int version;
    char version_string[TABLE_MAP_VERSION_DIGITS + 1];
    char *table;
    char *database;
    char gtid[GTID_MAX_LEN + 1]; /*< the current GTID event or NULL if GTID is not enabled */
} TABLE_MAP;

/**
 * The statistics for this AVRO router instance
 */
typedef struct
{
    int             n_clients;      /*< Number slave sessions created     */
    int             n_reads;        /*< Number of record reads */
    uint64_t        n_binlogs;      /*< Number of binlog records from master */
    uint64_t        n_rotates;      /*< Number of binlog rotate events */
    int             n_masterstarts; /*< Number of times connection restarted */
    time_t          lastReply;
    uint64_t        events[MAX_EVENT_TYPE_END + 1]; /*< Per event counters */
    uint64_t        lastsample;
    int             minno;
    int             minavgs[AVRO_NSTATS_MINUTES];
} AVRO_ROUTER_STATS;

/**
 * Client statistics
 */
typedef struct
{
    int             n_events;       /*< Number of events sent */
    unsigned long   n_bytes;        /*< Number of bytes sent */
    int             n_requests;     /*< Number of requests received */
    int             n_queries;      /*< Number of queries */
    int             n_failed_read;
    uint64_t        lastsample;
    int             minno;
    int             minavgs[AVRO_NSTATS_MINUTES];
} AVRO_CLIENT_STATS;

typedef struct avro_table_t
{
    char* filename; /*< Absolute filename */
    char* json_schema; /*< JSON representation of the schema */
    avro_file_writer_t avro_file; /*< Current Avro data file */
    avro_value_iface_t *avro_writer_iface; /*< Avro C API writer interface */
    avro_schema_t avro_schema; /*< Native Avro schema of the table */
} AVRO_TABLE;

/**
 * The client structure used within this router.
 * This represents the clients that are requesting AVRO files from MaxScale.
 */
typedef struct avro_client
{
#if defined(SS_DEBUG)
    skygw_chk_t     rses_chk_top;
#endif
    DCB             *dcb;           /*< The client DCB */
    int             state;          /*< The state of this client */
    char            *gtid;          /*< GTID the client requests */
    char            *schemaid;      /*< SchemaID the client requests */
    char            *uuid;          /*< Client UUID */
    char            *user;          /*< Username if given */
    char            *passwd;        /*< Password if given */
    SPINLOCK        catch_lock;     /*< Event catchup lock */
    SPINLOCK        rses_lock;      /*< Protects rses_deleted */
    struct avro_instance *router;   /*< Pointer to the owning router */
    struct avro_client *next;
    MAXAVRO_FILE  *read_handle;   /*< Current open file handle */
    uint64_t         requested_pos; /*< The last record we sent */
    uint64_t         last_sent_pos; /*< The last record we sent */
    AVRO_CLIENT_STATS  stats;       /*< Slave statistics */
    time_t          connect_time;   /*< Connect time of slave */
    MAXAVRO_FILE    avro_file;     /*< Avro file struct */
    char avro_binfile[AVRO_MAX_FILENAME_LEN + 1];
    unsigned int    cstate;         /*< Catch up state */
#if defined(SS_DEBUG)
    skygw_chk_t     rses_chk_tail;
#endif
} AVRO_CLIENT;

/**
 *  * The per instance data for the AVRO router.
 *   */
typedef struct avro_instance
{
    SERVICE                 *service;       /*< Pointer to the service using this router */
    AVRO_CLIENT             *clients;       /*< Link list of all the CDC client connections  */
    SPINLOCK                lock;           /*< Spinlock for the instance data */
    int                     initbinlog;     /*< Initial binlog file number */
    char                    *fileroot;      /*< Root of binlog filename */
    unsigned int            state;          /*< State of the AVRO router */
    uint8_t                 lastEventReceived; /*< Last even received */
    uint32_t                lastEventTimestamp; /*< Timestamp from last event */
    char                    *binlogdir;     /*< The directory where the binlog files are stored */
    char                    *avrodir;       /*< The directory with the AVRO files */
    char                    binlog_name[BINLOG_FNAMELEN + 1];
    /*< Name of the current binlog file */
    uint64_t                binlog_position;
    /*< last committed transaction position */
    uint64_t                current_pos;
    /*< Current binlog position */
    int                     binlog_fd;      /*< File descriptor of the binlog file being read */
    pcre2_code              *create_table_re;
    pcre2_code              *alter_table_re;
    uint8_t event_types;
    uint8_t event_type_hdr_lens[MAX_EVENT_TYPE_END];
    char    current_gtid[GTID_MAX_LEN + 1];
    TABLE_MAP     *active_maps[MAX_MAPPED_TABLES];
    HASHTABLE     *table_maps;
    HASHTABLE     *open_tables;
    HASHTABLE     *created_tables;
    char              prevbinlog[BINLOG_FNAMELEN + 1];
    int               rotating;     /*< Rotation in progress flag */
    SPINLOCK          fileslock;    /*< Lock for the files queue above */
    AVRO_ROUTER_STATS      stats;        /*< Statistics for this router */
    int task_delay; /*< Delay in seconds until the next conversion takes place */
    uint64_t        trx_count; /*< Transactions processed */
    uint64_t        trx_target; /*< Minimum about of transactions that will trigger
                                 * a flush of all tables */
    uint64_t        row_count; /*< Row events processed */
    uint64_t        row_target; /*< Minimum about of row events that will trigger
                                 * a flush of all tables */
    struct avro_instance  *next;
} AVRO_INSTANCE;

extern void read_table_info(uint8_t *ptr, uint8_t post_header_len, uint64_t *table_id,
                     char* dest, size_t len);
extern TABLE_MAP *table_map_alloc(uint8_t *ptr, uint8_t hdr_len, TABLE_CREATE* create,
                           const char* gtid);
extern void* table_map_free(TABLE_MAP *map);
extern TABLE_CREATE* table_create_alloc(const char* sql, const char* db, const char* gtid);
extern void* table_create_free(TABLE_CREATE* value);
extern bool table_create_save(TABLE_CREATE *create, const char *filename);
extern bool table_create_alter(TABLE_CREATE *create, const char *sql, const char *end);
extern void read_alter_identifier(const char *sql, const char *end, char *dest, int size);
extern int avro_client_handle_request(AVRO_INSTANCE *, AVRO_CLIENT *, GWBUF *);
extern void avro_client_rotate(AVRO_INSTANCE *router, AVRO_CLIENT *client, uint8_t *ptr);
extern bool avro_open_binlog(const char *binlogdir, const char *file, int *fd);
extern void avro_close_binlog(int fd);
extern avro_binlog_end_t avro_read_all_events(AVRO_INSTANCE *router);
extern AVRO_TABLE* avro_table_alloc(const char* filepath, const char* json_schema);
extern void* avro_table_free(AVRO_TABLE *table);
extern void avro_flush_all_tables(AVRO_INSTANCE *router);
extern char* json_new_schema_from_table(TABLE_MAP *map);
extern void save_avro_schema(const char *path, const char* schema, TABLE_MAP *map);
extern bool handle_table_map_event(AVRO_INSTANCE *router, REP_HEADER *hdr, uint8_t *ptr);
extern bool handle_row_event(AVRO_INSTANCE *router, REP_HEADER *hdr, uint8_t *ptr);

#define AVRO_CLIENT_UNREGISTERED 0x0000
#define AVRO_CLIENT_REGISTERED   0x0001
#define AVRO_CLIENT_REQUEST_DATA 0x0002
#define AVRO_CLIENT_ERRORED      0x0003
#define AVRO_CLIENT_MAXSTATE     0x0003

/**
 * Client catch-up status
 */
#define AVRO_CS_BUSY             0x0001
#define AVRO_WAIT_DATA           0x0002

#endif
