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

#include "config.h"

#ifdef ENABLE_SQLITE

#include "SQLiteUtil.h"
#include "base/util/StringBuffer.h"
#include "vocl/VConverter.h"

#include <stdarg.h>
#include <sstream>

void SQLiteUtil::throwError(const string &operation)
{
    string descr = m_name + ": '" + m_fileid + "': " + operation + " failed";

    if (m_db) {
        const char *error = sqlite3_errmsg(m_db);
        descr += ": ";
        descr += error ? error : "unspecified error";
    }

    throw runtime_error(descr);
}

sqlite3_stmt *SQLiteUtil::prepareSQLWrapper(const char *sql, const char **nextsql)
{
    sqlite3_stmt *stmt = NULL;

    checkSQL(sqlite3_prepare(m_db, sql, -1, &stmt, nextsql), sql);
    return stmt;
}

sqlite3_stmt *SQLiteUtil::prepareSQL(const char *sqlfmt, ...)
{
    StringBuffer sql;
    va_list ap;
   
    va_start(ap, sqlfmt);
    sql.vsprintf(sqlfmt, ap);
    va_end(ap);

    return prepareSQLWrapper(sql.c_str());
}


SQLiteUtil::key_t SQLiteUtil::findKey(const char *database, const char *keyname, const char *key)
{
    sqliteptr query(prepareSQL("SELECT ROWID FROM %s WHERE %s = '%s';", database, keyname, key));

    int res = checkSQL(sqlite3_step(query), "getting key");
    if (res == SQLITE_ROW) {
        return sqlite3_column_int64(query, 0);
    } else {
        return -1;
    }
}

string SQLiteUtil::findColumn(const char *database, const char *keyname, const char *key, const char *column, const char *def)
{
    sqliteptr query(prepareSQL("SELECT %s FROM %s WHERE %s = '%s';", column, database, keyname, key));

    int res = checkSQL(sqlite3_step(query), "getting key");
    if (res == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(query, 0);

        return text ? (const char *)text : def;
    } else {
        return def;
    }
}

string SQLiteUtil::getTextColumn(sqlite3_stmt *stmt, int col, const char *def)
{
    const unsigned char *text = sqlite3_column_text(stmt, col);
    return text ? (const char *)text : def;
}

SQLiteUtil::syncml_time_t SQLiteUtil::getTimeColumn(sqlite3_stmt *stmt, int col)
{
    // assumes that the database stores the result of time() directly
    return sqlite3_column_int64(stmt, col);
}

string SQLiteUtil::time2str(SQLiteUtil::syncml_time_t t)
{
    char buffer[128];
    sprintf(buffer, "%lu", t);
    return buffer;
}

void SQLiteUtil::rowToVObject(sqlite3_stmt *stmt, vocl::VObject &vobj)
{
    const unsigned char *text;
    const char *tablename;
    int i;

    for (i = 0; m_mapping[i].colname; i++) {
        if (m_mapping[i].colindex < 0 ||
            !m_mapping[i].propname) {
            continue;
        }
        tablename = sqlite3_column_table_name(stmt, m_mapping[i].colindex);
        if (!tablename || strcasecmp(tablename, m_mapping[i].tablename)) {
            continue;
        }
        text = sqlite3_column_text(stmt, m_mapping[i].colindex);
        if (text) {
            vobj.addProperty(m_mapping[i].propname, (const char *)text);
        }
    }
}

sqlite3_stmt *SQLiteUtil::vObjectToRow(vocl::VObject &vobj,
                                       const string &tablename,
                                       int numparams,
                                       const string &cols,
                                       const string &values)
{
    stringstream cols_stream;
    stringstream values_stream;
    int i;

    cols_stream << cols;
    values_stream << values;

    // figure out which columns we will fill
    for (i = 0; m_mapping[i].colname; i++) {
        if (m_mapping[i].colindex < 0 ||
            !m_mapping[i].propname ||
            tablename != m_mapping[i].tablename) {
            continue;
        }
        
        vocl::VProperty *vprop = vobj.getProperty(m_mapping[i].propname);
        if (vprop) {
            if (cols.size()) {
                cols_stream << ", ";
                values_stream << ", ";
            }
            cols_stream << m_mapping[i].colname;
            values_stream << "?";
        }
    }

    // create statement
    sqliteptr insert(prepareSQL("INSERT INTO ABPerson( %s ) "
                                         "VALUES( %s );",
                                         cols_stream.str().c_str(),
                                         values_stream.str().c_str()));
    // now bind our parameters
    int param = numparams + 1;
    for (i = 0; m_mapping[i].colname; i++) {
        if (m_mapping[i].colindex < 0 ||
            !m_mapping[i].propname ||
            tablename != m_mapping[i].tablename) {
            continue;
        }
        
        vocl::VProperty *vprop = vobj.getProperty(m_mapping[i].propname);
        if (vprop) {
            const char *text = vprop->getValue();
            checkSQL(sqlite3_bind_text(insert, param++, text ? text : "", -1, SQLITE_TRANSIENT));
        }
    }

    return insert.release();
}

void SQLiteUtil::open(const string &name,
                      const string &fileid,
                      const SQLiteUtil::Mapping *mapping,
                      const char *schema)
{
    close();
    m_name = name;
    m_fileid = fileid;

    const string prefix("file://");
    bool create = fileid.substr(0, prefix.size()) == prefix;
    string filename = create ? fileid.substr(prefix.size()) : fileid;

    if (!create && access(filename.c_str(), F_OK)) {
        throw runtime_error(m_name + ": no such database: '" + filename + "'");
    }

    sqlite3 *db;
    int res = sqlite3_open(filename.c_str(), &db);
    m_db = db;
    checkSQL(res, "opening");

    // check whether file is empty = newly created, define schema if that's the case
    sqliteptr check(prepareSQLWrapper("SELECT * FROM sqlite_master;"));
    switch (sqlite3_step(check)) {
    case SQLITE_ROW:
        // okay
        break;
    case SQLITE_DONE: {
        // empty
        const char *nextsql = schema;
        while (nextsql && *nextsql) {
            const char *sql = nextsql;
            sqliteptr create(prepareSQLWrapper(sql, &nextsql));
            while (true) {
                int res = sqlite3_step(create);
                if (res == SQLITE_DONE) {
                    break;
                } else if (res == SQLITE_ROW) {
                    // continue
                } else {
                    throwError("creating database");\
                }
            }
        }
        break;
    }
    default:
        throwError("checking content");
        break;
    }

    // query database schema to find columns we need
    int i;
    for (i = 0; mapping[i].colname; i++) ;
    m_mapping.set(new Mapping[i + 1]);
    sqliteptr query;
    string tablename;
    for (i = 0; mapping[i].colname; i++) {
        m_mapping[i] = mapping[i];

        // switching to a different table?
        if (tablename != m_mapping[i].tablename) {
            tablename = m_mapping[i].tablename;
            query.set(prepareSQL("SELECT * FROM %s;", tablename.c_str()));
        }

        // search for this column name
        for (m_mapping[i].colindex = sqlite3_column_count(query) - 1;
             m_mapping[i].colindex >= 0;
             m_mapping[i].colindex--) {
            const char *name = sqlite3_column_name(query, m_mapping[i].colindex);
            if (name && !strcasecmp(m_mapping[i].colname, name)) {
                break;
            }
        }
    }
    memset(&m_mapping[i], 0, sizeof(m_mapping[i]));
}

void SQLiteUtil::close()
{
    m_db = NULL;
}


#endif /* ENABLE_SQLITE */
