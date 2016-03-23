#ifndef MYSQL_BINLOG_H
#define MYSQL_BINLOG_H

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

/**
 * @file mysql_binlog.h - Extracting information from binary logs
 */

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/** Maximum GTID string length */
#define GTID_MAX_LEN 64

/** Table map column types */
#define TABLE_COL_TYPE_DECIMAL 0x00
#define TABLE_COL_TYPE_TINY 0x01
#define TABLE_COL_TYPE_SHORT 0x02
#define TABLE_COL_TYPE_LONG 0x03
#define TABLE_COL_TYPE_FLOAT 0x04
#define TABLE_COL_TYPE_DOUBLE 0x05
#define TABLE_COL_TYPE_NULL 0x06
#define TABLE_COL_TYPE_TIMESTAMP 0x07
#define TABLE_COL_TYPE_LONGLONG 0x08
#define TABLE_COL_TYPE_INT24 0x09
#define TABLE_COL_TYPE_DATE 0x0a
#define TABLE_COL_TYPE_TIME 0x0b
#define TABLE_COL_TYPE_DATETIME 0x0c
#define TABLE_COL_TYPE_YEAR 0x0d
#define TABLE_COL_TYPE_NEWDATE 0x0e
#define TABLE_COL_TYPE_VARCHAR 0x0f
#define TABLE_COL_TYPE_BIT 0x10
#define TABLE_COL_TYPE_TIMESTAMP2 0x11
#define TABLE_COL_TYPE_DATETIME2 0x12
#define TABLE_COL_TYPE_TIME2 0x13
#define TABLE_COL_TYPE_NEWDECIMAL 0xf6
#define TABLE_COL_TYPE_ENUM 0xf7
#define TABLE_COL_TYPE_SET 0xf8
#define TABLE_COL_TYPE_TINY_BLOB 0xf9
#define TABLE_COL_TYPE_MEDIUM_BLOB 0xfa
#define TABLE_COL_TYPE_LONG_BLOB 0xfb
#define TABLE_COL_TYPE_BLOB 0xfc
#define TABLE_COL_TYPE_VAR_STRING 0xfd
#define TABLE_COL_TYPE_STRING 0xfe
#define TABLE_COL_TYPE_GEOMETRY 0xff

/**
 * RBR row event flags
 */
#define ROW_EVENT_END_STATEMENT 0x0001
#define ROW_EVENT_NO_FKCHECK 0x0002
#define ROW_EVENT_NO_UKCHECK 0x0004
#define ROW_EVENT_HAS_COLUMNS 0x0008

/** The table ID used for end of statement row events */
#define TABLE_DUMMY_ID 0x00ffffff


const char* column_type_to_string(uint8_t type);

/** Column type checking functions */
bool column_is_variable_string(uint8_t type);
bool column_is_fixed_string(uint8_t type);
bool column_is_blob(uint8_t type);
bool column_is_temporal(uint8_t type);
bool column_is_bit(uint8_t type);

/** Various types are stored as fixed string types and the real type is stored
 * in the table metadata */
bool fixed_string_is_enum(uint8_t type);

/** Value unpacking */
uint64_t unpack_temporal_value(uint8_t type, uint8_t *ptr, uint8_t* metadata, struct tm *tm);
uint64_t unpack_enum(uint8_t *ptr, uint8_t *metadata, uint8_t *dest);
uint64_t unpack_numeric_field(uint8_t *ptr, uint8_t type, uint8_t* metadata, uint8_t* val);
uint64_t unpack_bit(uint8_t *ptr, uint8_t *null_mask, uint32_t col_count,
                    uint32_t curr_col_index, uint8_t *metadata, uint64_t *dest);

void format_temporal_value(char *str, size_t size, uint8_t type, struct tm *tm);

#endif /* MYSQL_BINLOG_H */
