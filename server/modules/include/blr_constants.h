#ifndef _BLR_DEFINES_H
#define _BLR_DEFINES_H

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
 * Copyright MariaDB Corporation Ab 2014-2015
 */

/**
 * @file blr_defines.h - Various definitions for binlogrouter
 */

#define BINLOG_FNAMELEN		255
#define BLR_PROTOCOL		"MySQLBackend"
#define BINLOG_MAGIC		{ 0xfe, 0x62, 0x69, 0x6e }
#define BINLOG_MAGIC_SIZE	4
#define BINLOG_NAMEFMT		"%s.%06d"
#define BINLOG_NAME_ROOT	"mysql-bin"

#define BINLOG_EVENT_HDR_LEN	19

/**
 * Binlog event types
 */
#define START_EVENT_V3				0x01
#define QUERY_EVENT				0x02
#define STOP_EVENT				0x03
#define ROTATE_EVENT				0x04
#define INTVAR_EVENT				0x05
#define LOAD_EVENT				0x06
#define SLAVE_EVENT				0x07
#define CREATE_FILE_EVENT			0x08
#define APPEND_BLOCK_EVENT			0x09
#define EXEC_LOAD_EVENT				0x0A
#define DELETE_FILE_EVENT			0x0B
#define NEW_LOAD_EVENT				0x0C
#define RAND_EVENT				0x0D
#define USER_VAR_EVENT				0x0E
#define FORMAT_DESCRIPTION_EVENT		0x0F
#define XID_EVENT				0x10
#define BEGIN_LOAD_QUERY_EVENT			0x11
#define EXECUTE_LOAD_QUERY_EVENT		0x12
#define TABLE_MAP_EVENT				0x13
#define WRITE_ROWS_EVENTv0			0x14
#define UPDATE_ROWS_EVENTv0			0x15
#define DELETE_ROWS_EVENTv0			0x16
#define WRITE_ROWS_EVENTv1			0x17
#define UPDATE_ROWS_EVENTv1			0x18
#define DELETE_ROWS_EVENTv1			0x19
#define INCIDENT_EVENT				0x1A
#define HEARTBEAT_EVENT				0x1B
#define IGNORABLE_EVENT				0x1C
#define ROWS_QUERY_EVENT			0x1D
#define WRITE_ROWS_EVENTv2			0x1E
#define UPDATE_ROWS_EVENTv2			0x1F
#define DELETE_ROWS_EVENTv2			0x20
#define GTID_EVENT				0x21
#define ANONYMOUS_GTID_EVENT			0x22
#define PREVIOUS_GTIDS_EVENT			0x23

#define MAX_EVENT_TYPE				0x23

/* New MariaDB event numbers start from 0xa0 */
#define MARIADB_NEW_EVENTS_BEGIN		0xa0
#define MARIADB_ANNOTATE_ROWS_EVENT		0xa0
/* New MariaDB 10 event numbers start from here */
#define MARIADB10_BINLOG_CHECKPOINT_EVENT	0xa1
#define MARIADB10_GTID_EVENT			0xa2
#define MARIADB10_GTID_GTID_LIST_EVENT		0xa3

#define MAX_EVENT_TYPE_MARIADB10		0xa3

/* Maximum event type so far */
#define MAX_EVENT_TYPE_END			MAX_EVENT_TYPE_MARIADB10

/**
 * Binlog event flags
 */
#define LOG_EVENT_BINLOG_IN_USE_F		0x0001
#define LOG_EVENT_FORCED_ROTATE_F		0x0002
#define LOG_EVENT_THREAD_SPECIFIC_F		0x0004
#define LOG_EVENT_SUPPRESS_USE_F		0x0008
#define LOG_EVENT_UPDATE_TABLE_MAP_VERSION_F	0x0010
#define LOG_EVENT_ARTIFICIAL_F			0x0020
#define LOG_EVENT_RELAY_LOG_F			0x0040
#define LOG_EVENT_IGNORABLE_F			0x0080
#define LOG_EVENT_NO_FILTER_F			0x0100
#define LOG_EVENT_MTS_ISOLATE_F			0x0200

/**
 * How often to call the binlog status function (seconds)
 */
#define	BLR_STATS_FREQ		60
#define BLR_NSTATS_MINUTES	30

/**
 * High and Low water marks for the slave dcb. These values can be overriden
 * by the router options highwater and lowwater.
 */
#define DEF_LOW_WATER		1000
#define	DEF_HIGH_WATER		10000

/**
 * Default burst sizes for slave catchup
 */
#define DEF_SHORT_BURST		15
#define DEF_LONG_BURST		500
#define DEF_BURST_SIZE		1024000	/* 1 Mb */

/**
 * master reconnect backoff constants
 * BLR_MASTER_BACKOFF_TIME	The increments of the back off time (seconds)
 * BLR_MAX_BACKOFF		Maximum number of increments to backoff to
 */
#define	BLR_MASTER_BACKOFF_TIME	10
#define BLR_MAX_BACKOFF		60

/* max size for error message returned to client */
#define BINLOG_ERROR_MSG_LEN	385

/* network latency extra wait tme for heartbeat check */
#define BLR_NET_LATENCY_WAIT_TIME	1

/* default heartbeat interval in seconds */
#define BLR_HEARTBEAT_DEFAULT_INTERVAL	300

/* strings and numbers in SQL replies */
#define BLR_TYPE_STRING			0xf
#define BLR_TYPE_INT			0x03

/* string len for COM_STATISTICS output */
#define BLRM_COM_STATISTICS_SIZE	1000

/* string len for strerror_r message */
#define BLRM_STRERROR_R_MSG_SIZE	128

/* string len for task message name */
#define BLRM_TASK_NAME_LEN		80

/* string len for temp binlog filename  */
#define BLRM_BINLOG_NAME_STR_LEN	80

/* string len for temp binlog filename  */
#define BLRM_SET_HEARTBEAT_QUERY_LEN	80

/* string len for master registration query  */
#define BLRM_MASTER_REGITRATION_QUERY_LEN	255

/* Read Binlog position states */
#define SLAVE_POS_READ_OK			0x00
#define SLAVE_POS_READ_ERR			0xff
#define SLAVE_POS_READ_UNSAFE			0xfe
#define SLAVE_POS_BAD_FD			0xfd
#define SLAVE_POS_BEYOND_EOF			0xfc

/**
 * Some useful macros for examining the MySQL Response packets
 */
#define MYSQL_RESPONSE_OK(buf)	(*((uint8_t *)GWBUF_DATA(buf) + 4) == 0x00)
#define MYSQL_RESPONSE_EOF(buf)	(*((uint8_t *)GWBUF_DATA(buf) + 4) == 0xfe)
#define MYSQL_RESPONSE_ERR(buf)	(*((uint8_t *)GWBUF_DATA(buf) + 4) == 0xff)
#define MYSQL_ERROR_CODE(buf)	((uint8_t *)GWBUF_DATA(buf) + 5)
#define MYSQL_ERROR_MSG(buf)	((uint8_t *)GWBUF_DATA(buf) + 7)
#define MYSQL_COMMAND(buf)	(*((uint8_t *)GWBUF_DATA(buf) + 4))

/**
 * Macros to extract common fields
 */
#define INLINE_EXTRACT		1	/* Set to 0 for debug purposes */

#if INLINE_EXTRACT
#define	EXTRACT16(x)		(*(uint8_t *)(x) | (*((uint8_t *)(x) + 1) << 8))
#define	EXTRACT24(x)		(*(uint8_t *)(x) | \
					(*((uint8_t *)(x) + 1) << 8) | \
					(*((uint8_t *)(x) + 2) << 16))
#define	EXTRACT32(x)		(*(uint8_t *)(x) | \
					(*((uint8_t *)(x) + 1) << 8) | \
					(*((uint8_t *)(x) + 2) << 16) | \
					(*((uint8_t *)(x) + 3) << 24))
#else
#define	EXTRACT16(x)		extract_field((x), 16)
#define	EXTRACT24(x)		extract_field((x), 24)
#define	EXTRACT32(x)		extract_field((x), 32)
#endif

#endif
