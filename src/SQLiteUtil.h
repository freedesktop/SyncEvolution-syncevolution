/*
 * Copyright (C) 2007 Patrick Ohly
 * Copyright (C) 2007 Funambol
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef INCL_SQLITESYNCSOURCE
#define INCL_SQLITESYNCSOURCE

#ifdef ENABLE_SQLITE

#include <sqlite3.h>
#include "EvolutionSmartPtr.h"

#include <string>
using namespace std;

namespace vocl {
    class VObject;
}

class SQLiteUnref {
 public:
    static void unref(sqlite3 *db) { sqlite3_close(db); }
    static void unref(sqlite3_stmt *stmt) { sqlite3_finalize(stmt); }
};

typedef eptr<sqlite3_stmt, sqlite3_stmt, SQLiteUnref> sqliteptr;

/**
 * This class implements access to SQLite database files:
 * - opening the database file
 * - error reporting
 * - creating a database file
 * - converting to and from a VObject via a simple property<->column name mapping
 */
class SQLiteUtil
{
  public:
    /** information about the database mapping */
    struct Mapping {
        const char *colname;        /**< column name in SQL table */
        const char *tablename;      /**< name of the SQL table which has this column */
        const char *propname;       /**< optional: vcard/vcalendar property which corresponds to this */
        int colindex;               /**< determined dynamically in open(): index of the column, -1 if not present */
    };

    const Mapping &getMapping(int i) { return m_mapping[i]; }

    /**
     * @param name        a name for the data source, used for error messages
     * @param fileid      a descriptor which identifies the file to be opened:
     *                    currently valid syntax is file:// followed by path
     * @param mapping     array with database mapping, terminated by NULL colname
     * @param schema      database schema to use when creating new databases, may be NULL
     */
    void open(const string &name,
              const string &fileid,
              const Mapping *mapping,
              const char *schema);

    void close();

    /**
     * throw error for a specific sqlite3 operation on m_db
     * @param operation   a description of the operation which failed
     */
    void throwError(const string &operation);

    /**
     * wrapper around sqlite3_prepare() which operates on the current
     * database and throws an error if the call fails
     *
     * @param sqlfmt         printf-style format string for query, followed by parameters for sprintf
     */
    sqlite3_stmt *prepareSQL(const char *sqlfmt, ...);

    /**
     * wrapper around sqlite3_prepare() which operates on the current
     * database and throws an error if the call fails
     *
     * @param sql       preformatted SQL statement(s)
     * @param nextsql   pointer to next statement in sql
     */
    sqlite3_stmt *prepareSQLWrapper(const char *sql, const char **nextsql = NULL);


    /** checks the result of an sqlite3 call, throws an error if faulty, otherwise returns the result */
    int checkSQL(int res, const char *operation = "SQLite call") {
        if (res != SQLITE_OK && res != SQLITE_ROW && res != SQLITE_DONE) {
            throwError(operation);
        }
        return res;
    }

    /** type used for row keys */
    typedef long long key_t;
    string toString(key_t key) { char buffer[32]; sprintf(buffer, "%lld", key); return buffer; }
#define SQLITE3_COLUMN_KEY sqlite3_column_int64

    /** return row ID for a certain row */
    key_t findKey(const char *database, const char *keyname, const char *key);

    /** return a specific column for a row identified by a certain key column as text, returns default text if not found */
    string findColumn(const char *database, const char *keyname, const char *key, const char *column, const char *def);

    /** a wrapper for sqlite3_column_test() which will check for NULL and returns default text instead */
    string getTextColumn(sqlite3_stmt *stmt, int col, const char *def = "");

    typedef unsigned long syncml_time_t;
    /** transform column to same time base as used by SyncML libary (typically time()) */
    syncml_time_t getTimeColumn(sqlite3_stmt *stmt, int col);

    /** convert time to string */
    static string time2str(syncml_time_t t);

    /** copies all columns which directly map to a property into the vobj */
    void rowToVObject(sqlite3_stmt *stmt, vocl::VObject &vobj);

    /**
     * Creates a SQL INSERT INTO <tablename> ( <cols> ) VALUES ( <values> )
     * statement and binds all rows/values that map directly from the vobj.
     *
     * @param numparams      number of ? placeholders in values; the caller has
     *                       to bind those before executing the statement
     */
    sqlite3_stmt *vObjectToRow(vocl::VObject &vobj,
                               const string &tablename,
                               int numparams,
                               const string &cols,
                               const string &values);

 private:
    /* copy of open() parameters */
    arrayptr<Mapping> m_mapping;
    string m_name;
    string m_fileid;

    /** current database */
    eptr<sqlite3, sqlite3, SQLiteUnref> m_db;
};

#endif // ENABLE_SQLITE
#endif // INCL_SQLITESYNCSOURCE
