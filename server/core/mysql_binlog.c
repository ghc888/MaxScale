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
 * @file mysql_binlog.c - Extracting information from binary logs
 */

#include <mysql_binlog.h>
#include <mysql_utils.h>
#include <stdlib.h>
#include <log_manager.h>
#include <string.h>
#include <skygw_debug.h>
#include <dbusers.h>
#include <strings.h>

/**
 * @brief Convert a table column type to a string
 *
 * @param type The table column type
 * @return The type of the column in human readable format
 * @see lestr_consume
 */
const char* column_type_to_string(uint8_t type)
{
    switch (type)
    {
        case TABLE_COL_TYPE_DECIMAL:
            return "DECIMAL";
        case TABLE_COL_TYPE_TINY:
            return "TINY";
        case TABLE_COL_TYPE_SHORT:
            return "SHORT";
        case TABLE_COL_TYPE_LONG:
            return "LONG";
        case TABLE_COL_TYPE_FLOAT:
            return "FLOAT";
        case TABLE_COL_TYPE_DOUBLE:
            return "DOUBLE";
        case TABLE_COL_TYPE_NULL:
            return "NULL";
        case TABLE_COL_TYPE_TIMESTAMP:
            return "TIMESTAMP";
        case TABLE_COL_TYPE_LONGLONG:
            return "LONGLONG";
        case TABLE_COL_TYPE_INT24:
            return "INT24";
        case TABLE_COL_TYPE_DATE:
            return "DATE";
        case TABLE_COL_TYPE_TIME:
            return "TIME";
        case TABLE_COL_TYPE_DATETIME:
            return "DATETIME";
        case TABLE_COL_TYPE_YEAR:
            return "YEAR";
        case TABLE_COL_TYPE_NEWDATE:
            return "NEWDATE";
        case TABLE_COL_TYPE_VARCHAR:
            return "VARCHAR";
        case TABLE_COL_TYPE_BIT:
            return "BIT";
        case TABLE_COL_TYPE_TIMESTAMP2:
            return "TIMESTAMP2";
        case TABLE_COL_TYPE_DATETIME2:
            return "DATETIME2";
        case TABLE_COL_TYPE_TIME2:
            return "TIME2";
        case TABLE_COL_TYPE_NEWDECIMAL:
            return "NEWDECIMAL";
        case TABLE_COL_TYPE_ENUM:
            return "ENUM";
        case TABLE_COL_TYPE_SET:
            return "SET";
        case TABLE_COL_TYPE_TINY_BLOB:
            return "TINY_BLOB";
        case TABLE_COL_TYPE_MEDIUM_BLOB:
            return "MEDIUM_BLOB";
        case TABLE_COL_TYPE_LONG_BLOB:
            return "LONG_BLOB";
        case TABLE_COL_TYPE_BLOB:
            return "BLOB";
        case TABLE_COL_TYPE_VAR_STRING:
            return "VAR_STRING";
        case TABLE_COL_TYPE_STRING:
            return "STRING";
        case TABLE_COL_TYPE_GEOMETRY:
            return "GEOMETRY";
        default:
            break;
    }
    return "UNKNOWN";
}

bool column_is_blob(uint8_t type)
{
    switch (type)
    {
        case TABLE_COL_TYPE_TINY_BLOB:
        case TABLE_COL_TYPE_MEDIUM_BLOB:
        case TABLE_COL_TYPE_LONG_BLOB:
        case TABLE_COL_TYPE_BLOB:
            return true;
    }
    return false;
}

/**
 * @brief Check if the column is a string type column
 *
 * @param type Type of the column
 * @return True if the column is a string type column
 * @see lestr_consume
 */
bool column_is_variable_string(uint8_t type)
{
    switch (type)
    {
        case TABLE_COL_TYPE_DECIMAL:
        case TABLE_COL_TYPE_VARCHAR:
        case TABLE_COL_TYPE_BIT:
        case TABLE_COL_TYPE_NEWDECIMAL:
        case TABLE_COL_TYPE_VAR_STRING:
        case TABLE_COL_TYPE_GEOMETRY:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Detect BIT type columns
 * @param type Type of the column
 * @return  True if the column is a BIT
 */
bool column_is_bit(uint8_t type)
{
    return type == TABLE_COL_TYPE_BIT;
}

/**
 * Check if a column is of a temporal type
 * @param type Column type
 * @return True if the type is temporal
 */
bool column_is_temporal(uint8_t type)
{
    switch (type)
    {
        case TABLE_COL_TYPE_YEAR:
        case TABLE_COL_TYPE_DATE:
        case TABLE_COL_TYPE_TIME:
        case TABLE_COL_TYPE_DATETIME:
        case TABLE_COL_TYPE_DATETIME2:
        case TABLE_COL_TYPE_TIMESTAMP:
        case TABLE_COL_TYPE_TIMESTAMP2:
            return true;
    }
    return false;
}

/**
 * @brief Check if the column is a string type column
 *
 * @param type Type of the column
 * @return True if the column is a string type column
 * @see lestr_consume
 */
bool column_is_fixed_string(uint8_t type)
{
    return type == TABLE_COL_TYPE_STRING;
}

/**
 * Check if a column is an ENUM or SET
 * @param type Column type
 * @return True if column is either ENUM or SET
 */
bool fixed_string_is_enum(uint8_t type)
{
    return type == TABLE_COL_TYPE_ENUM || type == TABLE_COL_TYPE_SET;
}

/**
 * @brief Unpack a YEAR type
 *
 * The value seems to be stored as an offset from the year 1900
 * @param val Stored value
 * @param dest Destination where unpacked value is stored
 */
static void unpack_year(uint8_t *ptr, struct tm *dest)
{
    memset(dest, 0, sizeof(*dest));
    dest->tm_year = *ptr;
}

#ifdef USE_OLD_DATETIME
/**
 * @brief Unpack a DATETIME
 *
 * The DATETIME is stored as a 8 byte value with the values stored as multiples
 * of 100. This means that the stored value is in the format YYYYMMDDHHMMSS.
 * @param val Value read from the binary log
 * @param dest Pointer where the unpacked value is stored
 */
static void unpack_datetime(uint8_t *ptr, uint8_t decimals, struct tm *dest)
{
    uint32_t second = val - ((val / 100) * 100);
    val /= 100;
    uint32_t minute = val - ((val / 100) * 100);
    val /= 100;
    uint32_t hour = val - ((val / 100) * 100);
    val /= 100;
    uint32_t day = val - ((val / 100) * 100);
    val /= 100;
    uint32_t month = val - ((val / 100) * 100);
    val /= 100;
    uint32_t year = val;

    memset(dest, 0, sizeof(struct tm));
    dest->tm_year = year - 1900;
    dest->tm_mon = month;
    dest->tm_mday = day;
    dest->tm_hour = hour;
    dest->tm_min = minute;
    dest->tm_sec = second;
}
#endif

/**
 * Unpack a 5 byte reverse byte order value
 * @param data pointer to data
 * @return Unpacked value
 */
static inline uint64_t unpack5(uint8_t* data)
{
    uint64_t rval = data[4];
    rval += ((uint64_t)data[3]) << 8;
    rval += ((uint64_t)data[2]) << 16;
    rval += ((uint64_t)data[1]) << 24;
    rval += ((uint64_t)data[0]) << 32;
    return rval;
}

/** The DATETIME values are stored in the binary logs with an offset */
#define DATETIME2_OFFSET 0x8000000000LL

/**
 * @brief Unpack a DATETIME2
 *
 * The DATETIME2 is only used by row based replication in newer MariaDB servers.
 * @param val Value read from the binary log
 * @param dest Pointer where the unpacked value is stored
 */
static void unpack_datetime2(uint8_t *ptr, uint8_t decimals, struct tm *dest)
{
    int64_t unpacked = unpack5(ptr) - DATETIME2_OFFSET;
    if (unpacked < 0)
    {
        unpacked = -unpacked;
    }

    uint64_t date = unpacked >> 17;
    uint64_t yearmonth = date >> 5;
    uint64_t time = unpacked % (1 << 17);

    memset(dest, 0, sizeof(*dest));
    dest->tm_sec = time % (1 << 6);
    dest->tm_min = (time >> 6) % (1 << 6);
    dest->tm_hour = time >> 12;
    dest->tm_mday = date % (1 << 5);
    dest->tm_mon = yearmonth % 13;
    dest->tm_year = yearmonth / 13;
}

/** Unpack a "reverse" byte order value */
#define unpack4(data) (data[3] + (data[2] << 8) + (data[1] << 16) + (data[0] << 24))

/**
 * @brief Unpack a TIMESTAMP
 *
 * The timestamps are stored with the high bytes first
 * @param val The stored value
 * @param dest Destination where the result is stored
 */
static void unpack_timestamp(uint8_t *ptr, uint8_t decimals, struct tm *dest)
{
    time_t t = unpack4(ptr);
    localtime_r(&t, dest);
}

#define unpack3(data) (data[2] + (data[1] << 8) + (data[0] << 16))

/**
 * @brief Unpack a TIME
 *
 * The TIME is stored as a 3 byte value with the values stored as multiples
 * of 100. This means that the stored value is in the format HHMMSS.
 * @param val Value read from the binary log
 * @param dest Pointer where the unpacked value is stored
 */
static void unpack_time(uint8_t *ptr, struct tm *dest)
{
    uint64_t val = unpack3(ptr);
    uint32_t second = val - ((val / 100) * 100);
    val /= 100;
    uint32_t minute = val - ((val / 100) * 100);
    val /= 100;
    uint32_t hour = val;

    memset(dest, 0, sizeof(struct tm));
    dest->tm_hour = hour;
    dest->tm_min = minute;
    dest->tm_sec = second;
}

/**
 * @brief Unpack a DATE value
 * @param ptr Pointer to packed value
 * @param dest Pointer where the unpacked value is stored
 */
static void unpack_date(uint8_t *ptr, struct tm *dest)
{
    uint64_t val = ptr[0] + (ptr[1] << 8) + (ptr[2] << 16);
    memset(dest, 0, sizeof(struct tm));
    dest->tm_mday = val & 31;
    dest->tm_mon = (val >> 5) & 15;
    dest->tm_year = (val >> 9) - 1900;
}

/**
 * @brief Unpack an ENUM or SET field
 * @param ptr Pointer to packed value
 * @param metadata Pointer to field metadata
 * @return Length of the processed field in bytes
 */
uint64_t unpack_enum(uint8_t *ptr, uint8_t *metadata, uint8_t *dest)
{
    memcpy(dest, ptr, metadata[1]);
    return metadata[1];
}

/**
 * @brief Unpack a BIT
 *
 * A part of the BIT values are stored in the NULL value bitmask of the row event.
 * This makes extracting them a bit more complicated since the other fields
 * in the table could have an effect on the location of the stored values.
 *
 * It is possible that the BIT value is fully stored in the NULL value bitmask
 * which means that the actual row data is zero bytes for this field.
 * @param ptr Pointer to packed value
 * @param null_mask NULL field mask
 * @param col_count Number of columns in the row event
 * @param curr_col_index Current position of the field in the row event (zero indexed)
 * @param metadata Field metadata
 * @param dest Destination where the value is stored
 * @return Length of the processed field in bytes
 */
uint64_t unpack_bit(uint8_t *ptr, uint8_t *null_mask, uint32_t col_count,
                    uint32_t curr_col_index, uint8_t *metadata, uint64_t *dest)
{
    if (metadata[1])
    {
        memcpy(ptr, dest, metadata[1]);
    }
    return metadata[1];
}


/**
 * @brief Get the length of a temporal field
 * @param type Field type
 * @param decimals How many decimals the field has
 * @return Number of bytes the temporal value takes
 */
static size_t temporal_field_size(uint8_t type, uint8_t decimals)
{
    switch (type)
    {
        case TABLE_COL_TYPE_YEAR:
            return 1;

        case TABLE_COL_TYPE_TIME:
        case TABLE_COL_TYPE_DATE:
            return 3;

        case TABLE_COL_TYPE_DATETIME:
        case TABLE_COL_TYPE_TIMESTAMP:
            return 4;

        case TABLE_COL_TYPE_TIMESTAMP2:
            return 4 + ((decimals + 1) / 2);

        case TABLE_COL_TYPE_DATETIME2:
            return 5 + ((decimals + 1) / 2);

        default:
            MXS_ERROR("Unknown field type: %x %s", type, column_type_to_string(type));
            break;
    }

    return 0;
}

/**
 * @brief Unpack a temporal value
 *
 * MariaDB and MySQL both store temporal values in a special format. This function
 * unpacks them from the storage format and into a common, usable format.
 * @param type Column type
 * @param val Extracted packed value
 * @param tm Pointer where the unpacked temporal value is stored
 */
uint64_t unpack_temporal_value(uint8_t type, uint8_t *ptr, uint8_t *metadata, struct tm *tm)
{
    switch (type)
    {
        case TABLE_COL_TYPE_YEAR:
            unpack_year(ptr, tm);
            break;

        case TABLE_COL_TYPE_DATETIME:
            // This is not used with MariaDB RBR
            //unpack_datetime(ptr, *metadata, tm);
            break;

        case TABLE_COL_TYPE_DATETIME2:
            unpack_datetime2(ptr, *metadata, tm);
            break;

        case TABLE_COL_TYPE_TIME:
            unpack_time(ptr, tm);
            break;

        case TABLE_COL_TYPE_DATE:
            unpack_date(ptr, tm);
            break;

        case TABLE_COL_TYPE_TIMESTAMP:
        case TABLE_COL_TYPE_TIMESTAMP2:
            unpack_timestamp(ptr, *metadata, tm);
            break;
    }
    return temporal_field_size(type, *metadata);
}

void format_temporal_value(char *str, size_t size, uint8_t type, struct tm *tm)
{
    const char *format = "";

    switch (type)
    {
        case TABLE_COL_TYPE_DATETIME:
        case TABLE_COL_TYPE_DATETIME2:
        case TABLE_COL_TYPE_TIMESTAMP:
        case TABLE_COL_TYPE_TIMESTAMP2:
            format = "%Y-%m-%d %H:%M:%S";
            break;

        case TABLE_COL_TYPE_TIME:
            format = "%H:%M:%S";
            break;

        case TABLE_COL_TYPE_DATE:
            format = "%Y-%m-%d";
            break;

        case TABLE_COL_TYPE_YEAR:
            format = "%Y";
            break;

        default:
            MXS_ERROR("Unexpected temporal type: %x %s", type, column_type_to_string(type));
            ss_dassert(false);
            break;
    }
    strftime(str, size, format, tm);
}

/**
 * @brief Extract a value from a row event
 *
 * This function extracts a single value from a row event and stores it for
 * further processing. Integer values are usable immediately but temporal
 * values need to be unpacked from the compact format they are stored in.
 * @param ptr Pointer to the start of the field value
 * @param type Column type of the field
 * @param metadata Pointer to the field metadata
 * @param val Destination where the extracted value is stored
 * @return Number of bytes copied
 * @see extract_temporal_value
 */
size_t unpack_numeric_field(uint8_t *src, uint8_t type, uint8_t *metadata, uint8_t *dest)
{
    size_t size = 0;
    switch (type)
    {
        case TABLE_COL_TYPE_LONG:
        case TABLE_COL_TYPE_FLOAT:
            size = 4;
            break;

        case TABLE_COL_TYPE_INT24:
            size = 3;
            break;

        case TABLE_COL_TYPE_LONGLONG:
        case TABLE_COL_TYPE_DOUBLE:
            size = 8;
            break;

        case TABLE_COL_TYPE_SHORT:
            size = 2;
            break;

        case TABLE_COL_TYPE_TINY:
            size = 1;
            break;

        default:
            MXS_ERROR("Bad column type: %x %s", type, column_type_to_string(type));
            break;
    }

    memcpy(dest, src, size);
    return size;
}
