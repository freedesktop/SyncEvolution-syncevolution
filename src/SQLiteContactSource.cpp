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
#include "EvolutionSyncSource.h"

#ifdef ENABLE_SQLITE

#include "SQLiteContactSource.h"

#include <common/base/Log.h>
#include "vocl/VConverter.h"

#include <algorithm>
#include <cctype>
#include <sstream>

using namespace vocl;

#include <boost/algorithm/string/case_conv.hpp>

enum {
    PERSON_LAST,
    PERSON_MIDDLE,
    PERSON_FIRST,
    PERSON_PREFIX,
    PERSON_SUFFIX,
    PERSON_LASTSORT,
    PERSON_FIRSTSORT,
    PERSON_ORGANIZATION,
    PERSON_DEPARTMENT,
    PERSON_UNIT,
    PERSON_NOTE,
    PERSON_BIRTHDAY,
    PERSON_JOBTITLE,
    PERSON_TITLE,
    PERSON_NICKNAME,
    PERSON_FULLNAME,
    PERSON_CATEGORIES,

    PERSON_AIM,
    PERSON_GROUPWISE,
    PERSON_ICQ,
    PERSON_YAHOO,

    PERSON_FILEAS,
    PERSON_ANNIVERSARY,

    PERSON_ASSISTANT,
    PERSON_MANAGER,
    PERSON_SPOUSE,

    PERSON_URL,
    PERSON_BLOG_URL,
    PERSON_VIDEO_URL,

    LAST_COL
};

void SQLiteContactSource::open()
{
    static const SQLiteUtil::Mapping mapping[LAST_COL + 1] = {
        { "Last", "ABPerson" },
        { "Middle", "ABPerson" },
        { "First", "ABPerson" },
        { "Prefix", "ABPerson" },
        { "Suffix", "ABPerson" },
        { "FirstSort", "ABPerson" },
        { "LastSort", "ABPerson" },
        { "Organization", "ABPerson" },
        { "Department", "ABPerson" },
        { "Unit", "ABPerson" },
        { "Note", "ABPerson", "NOTE" },
        { "Birthday", "ABPerson", "BDAY" },
        { "JobTitle", "ABPerson", "ROLE" },
        { "Title", "ABPerson", "TITLE" },
        { "Nickname", "ABPerson", "NICKNAME" },
        { "CompositeNameFallback", "ABPerson", "FN" },
        { "Categories", "ABPerson", "CATEGORIES" },

        { "AIM", "ABPerson", "X-AIM" },
        { "Groupwise", "ABPerson", "X-GROUPWISE" },
        { "ICQ", "ABPerson", "X-ICQ" },
        { "Yahoo", "ABPerson", "X-YAHOO" },

        { "FileAs", "ABPerson", "X-EVOLUTION-FILE-AS" },
        { "Anniversary", "ABPerson", "X-EVOLUTION-ANNIVERSARY" },

        { "Assistant", "ABPerson", "X-EVOLUTION-ASSISTANT" },
        { "Manager", "ABPerson", "X-EVOLUTION-MANAGER" },
        { "Spouse", "ABPerson", "X-EVOLUTION-SPOUSE" },

    
        { "URL", "ABPerson", "URL" },
        { "BlogURL", "ABPerson", "X-EVOLUTION-BLOG-URL" },
        { "VideoURL", "ABPerson", "X-EVOLUTION-VIDEO-URL" },

        { NULL }
    };
    static const char *schema = 
        "BEGIN TRANSACTION;"
        "CREATE TABLE ABPerson (ROWID INTEGER PRIMARY KEY AUTOINCREMENT, "
        "First TEXT, "
        "Last TEXT, "
        "Middle TEXT, "
        "FirstPhonetic TEXT, "
        "MiddlePhonetic TEXT, "
        "LastPhonetic TEXT, "
        "Organization TEXT, "
        "Department TEXT, "
        "Unit TEXT, "
        "Note TEXT, "
        "Kind INTEGER, "
        "Birthday TEXT, "
        "JobTitle TEXT, "
        "Title TEXT, "
        "Nickname TEXT, "
        "Prefix TEXT, "
        "Suffix TEXT, "
        "FirstSort TEXT, "
        "LastSort TEXT, "
        "CreationDate INTEGER, "
        "ModificationDate INTEGER, "
        "CompositeNameFallback TEXT, "
        "Categories TEXT, "
        "AIM TEXT, "
        "Groupwise TEXT, "
        "ICQ Text, "
        "Yahoo TEXT, "
        "Anniversary TEXT, "
        "Assistant TEXT, "
        "Manager TEXT, "
        "Spouse TEXT, "
        "URL TEXT, "
        "BlogURL TEXT, "
        "VideoURL TEXT, "
        "FileAs TEXT);"
        "COMMIT;";

    string id = getDatabaseID();
    m_sqlite.open(getName(),
                  id.c_str(),
                  mapping,
                  schema);
}

void SQLiteContactSource::close()
{
    m_sqlite.close();
}

SQLiteContactSource::sources SQLiteContactSource::getSyncBackends()
{
    sources res;

    res.push_back(EvolutionSyncSource::source("select database via file path",
                                              "file:///<absolute path>"));
    return res;
}

void SQLiteContactSource::listAllItems(RevisionMap_t &revisions)
{
    sqliteptr all(m_sqlite.prepareSQL("SELECT ROWID, CreationDate, ModificationDate FROM ABPerson;"));
    while (m_sqlite.checkSQL(sqlite3_step(all)) == SQLITE_ROW) {
        string uid = m_sqlite.toString(SQLITE3_COLUMN_KEY(all, 0));
        string modTime = m_sqlite.time2str(m_sqlite.getTimeColumn(all, 2));
        revisions.insert(RevisionMap_t::value_type(uid, modTime));
    }
}

SyncItem *SQLiteContactSource::createItem(const string &uid)
{
    logItem(uid, "extracting from database", true);

    sqliteptr contact(m_sqlite.prepareSQL("SELECT * FROM ABPerson WHERE ROWID = '%s';", uid.c_str()));
    if (m_sqlite.checkSQL(sqlite3_step(contact)) != SQLITE_ROW) {
        throw runtime_error(string(getName()) + ": contact not found: " + uid);
    }

    VObject vobj;
    string tmp;

    vobj.addProperty("BEGIN", "VCARD");
    vobj.addProperty("VERSION", "2.1");
    vobj.setVersion("2.1");

    tmp = m_sqlite.getTextColumn(contact, m_sqlite.getMapping(PERSON_LAST).colindex);
    tmp += VObject::SEMICOLON_REPLACEMENT;
    tmp += m_sqlite.getTextColumn(contact, m_sqlite.getMapping(PERSON_MIDDLE).colindex);
    tmp += VObject::SEMICOLON_REPLACEMENT;
    tmp += m_sqlite.getTextColumn(contact, m_sqlite.getMapping(PERSON_FIRST).colindex);
    tmp += VObject::SEMICOLON_REPLACEMENT;
    tmp += m_sqlite.getTextColumn(contact, m_sqlite.getMapping(PERSON_PREFIX).colindex);
    tmp += VObject::SEMICOLON_REPLACEMENT;
    tmp += m_sqlite.getTextColumn(contact, m_sqlite.getMapping(PERSON_SUFFIX).colindex);
    if (tmp.size() > 4) {
        vobj.addProperty("N", tmp.c_str());
    }

    tmp = m_sqlite.getTextColumn(contact, m_sqlite.getMapping(PERSON_ORGANIZATION).colindex);
    tmp += VObject::SEMICOLON_REPLACEMENT;
    tmp += m_sqlite.getTextColumn(contact, m_sqlite.getMapping(PERSON_DEPARTMENT).colindex);
    tmp += VObject::SEMICOLON_REPLACEMENT;
    tmp += m_sqlite.getTextColumn(contact, m_sqlite.getMapping(PERSON_UNIT).colindex);
    if (tmp.size() > 2) {
        vobj.addProperty("ORG", tmp.c_str());
    }
 
    m_sqlite.rowToVObject(contact, vobj);
    vobj.addProperty("END", "VCARD");
    vobj.fromNativeEncoding();

    arrayptr<char> finalstr(vobj.toString(), "VOCL string");
    LOG.debug("%s", (char *)finalstr);

    cxxptr<SyncItem> item( new SyncItem( uid.c_str() ) );
    item->setData( (char *)finalstr, strlen(finalstr) );
    item->setDataType( getMimeType() );
    item->setModificationTime( 0 );

    return item.release();
}

string SQLiteContactSource::insertItem(string &uid, const SyncItem &item, bool &merged)
{
    string creationTime;
    std::auto_ptr<VObject> vobj(VConverter::parse((char *)((SyncItem &)item).getData()));
    if (vobj.get() == 0) {
        throwError(string("parsing contact ") + ((SyncItem &)item).getKey());
    }
    vobj->toNativeEncoding();

    int numparams = 0;
    stringstream cols;
    stringstream values;
    VProperty *prop;

    // parse up to three fields of ORG
    prop = vobj->getProperty("ORG");
    string organization, department, unit;
    if (prop && prop->getValue()) {
        string fn = prop->getValue();
        size_t sep1 = fn.find(VObject::SEMICOLON_REPLACEMENT);
        size_t sep2 = sep1 == fn.npos ? fn.npos : fn.find(VObject::SEMICOLON_REPLACEMENT, sep1 + 1);

        organization = fn.substr(0, sep1);
        if (sep1 != fn.npos) {
            department = fn.substr(sep1 + 1, (sep2 == fn.npos) ? fn.npos : sep2 - sep1 - 1);
        }
        if (sep2 != fn.npos) {
            unit = fn.substr(sep2 + 1);
        }
    }
    cols << m_sqlite.getMapping(PERSON_ORGANIZATION).colname << ", " <<
        m_sqlite.getMapping(PERSON_DEPARTMENT).colname << ", " <<
        m_sqlite.getMapping(PERSON_UNIT).colname << ", ";
    values << "?, ?, ?, ";
    numparams += 3;

    // parse the name, insert empty fields if not found
    prop = vobj->getProperty("N");
    string first, middle, last, prefix, suffix, firstsort, lastsort;
    if (prop && prop->getValue()) {
        string fn = prop->getValue();
        size_t sep1 = fn.find(VObject::SEMICOLON_REPLACEMENT);
        size_t sep2 = sep1 == fn.npos ? fn.npos : fn.find(VObject::SEMICOLON_REPLACEMENT, sep1 + 1);
        size_t sep3 = sep2 == fn.npos ? fn.npos : fn.find(VObject::SEMICOLON_REPLACEMENT, sep2 + 1);
        size_t sep4 = sep3 == fn.npos ? fn.npos : fn.find(VObject::SEMICOLON_REPLACEMENT, sep3 + 1);

        last = fn.substr(0, sep1);
        if (sep1 != fn.npos) {
            middle = fn.substr(sep1 + 1, (sep2 == fn.npos) ? fn.npos : sep2 - sep1 - 1);
        }
        if (sep2 != fn.npos) {
            first = fn.substr(sep2 + 1, (sep3 == fn.npos) ? fn.npos : sep3 - sep2 - 1);
        }
        if (sep3 != fn.npos) {
            prefix = fn.substr(sep3 + 1, (sep4 == fn.npos) ? fn.npos : sep4 - sep3 - 1);
        }
        if (sep4 != fn.npos) {
            suffix = fn.substr(sep4 + 1);
        }
    }
    cols << m_sqlite.getMapping(PERSON_FIRST).colname << ", " <<
        m_sqlite.getMapping(PERSON_MIDDLE).colname << ", " <<
        m_sqlite.getMapping(PERSON_LAST).colname << ", " <<
        m_sqlite.getMapping(PERSON_PREFIX).colname << ", " <<
        m_sqlite.getMapping(PERSON_SUFFIX).colname << ", " <<
        m_sqlite.getMapping(PERSON_LASTSORT).colname << ", " <<
        m_sqlite.getMapping(PERSON_FIRSTSORT).colname;
    values << "?, ?, ?, ?, ?, ?, ?";
    numparams += 7;


    // synthesize sort keys: upper case with specific order of first/last name
    firstsort = first + " " + last;
    boost::to_upper(firstsort);
    lastsort = last + " " + first;
    boost::to_upper(lastsort);

    // optional fixed UID, potentially fixed creation time
    if (uid.size()) {
        creationTime = m_sqlite.findColumn("ABPerson", "ROWID", uid.c_str(), "CreationDate", "");
        cols << ", ROWID";
        values << ", ?";
        numparams++;
    }
    cols << ", CreationDate, ModificationDate";
    values << ", ?, ?";
    numparams += 2;

    // delete complete row so that we can recreate it
    if (uid.size()) {
        sqliteptr remove(m_sqlite.prepareSQL("DELETE FROM ABPerson WHERE ROWID == ?;"));
        m_sqlite.checkSQL(sqlite3_bind_text(remove, 1, uid.c_str(), -1, SQLITE_TRANSIENT));
        m_sqlite.checkSQL(sqlite3_step(remove));
    }

    string cols_str = cols.str();
    string values_str = values.str();
    sqliteptr insert(m_sqlite.vObjectToRow(*vobj,
                                                    "ABPerson",
                                                    numparams,
                                                    cols.str(),
                                                    values.str()));

    // now bind parameter values in the same order as the columns specification above
    int param = 1;
    m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, organization.c_str(), -1, SQLITE_TRANSIENT));
    m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, department.c_str(), -1, SQLITE_TRANSIENT));
    m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, unit.c_str(), -1, SQLITE_TRANSIENT));
    m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, first.c_str(), -1, SQLITE_TRANSIENT));
    m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, middle.c_str(), -1, SQLITE_TRANSIENT));
    m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, last.c_str(), -1, SQLITE_TRANSIENT));
    m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, prefix.c_str(), -1, SQLITE_TRANSIENT));
    m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, suffix.c_str(), -1, SQLITE_TRANSIENT));
    m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, lastsort.c_str(), -1, SQLITE_TRANSIENT));
    m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, firstsort.c_str(), -1, SQLITE_TRANSIENT));
    if (uid.size()) {
        m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, uid.c_str(), -1, SQLITE_TRANSIENT));
        m_sqlite.checkSQL(sqlite3_bind_text(insert, param++, creationTime.c_str(), -1, SQLITE_TRANSIENT));
    } else {
        m_sqlite.checkSQL(sqlite3_bind_int64(insert, param++, (long long)time(NULL)));
    }
    SQLiteUtil::syncml_time_t modificationTime = time(NULL);
    m_sqlite.checkSQL(sqlite3_bind_int64(insert, param++, modificationTime));
                      
    m_sqlite.checkSQL(sqlite3_step(insert));
                      
    if (!uid.size()) {
        // figure out which UID was assigned to the new contact
        uid = m_sqlite.findColumn("SQLITE_SEQUENCE", "NAME", "ABPerson", "SEQ", "");
    }
    return m_sqlite.time2str(modificationTime);
}


void SQLiteContactSource::deleteItem(const string &uid)
{
    sqliteptr del;

    del.set(m_sqlite.prepareSQL("DELETE FROM ABPerson WHERE "
                                "ABPerson.ROWID = ?;"));
    m_sqlite.checkSQL(sqlite3_bind_text(del, 1, uid.c_str(), -1, SQLITE_TRANSIENT));
    m_sqlite.checkSQL(sqlite3_step(del));
}

void SQLiteContactSource::flush()
{
    // Our change tracking is time based.
    // Don't let caller proceed without waiting for
    // one second to prevent being called again before
    // the modification time stamp is larger than it
    // is now.
    sleep(1);
}

void SQLiteContactSource::logItem(const string &uid, const string &info, bool debug)
{
    if (LOG.getLevel() >= (debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO)) {
        (LOG.*(debug ? &Log::debug : &Log::info))("%s: %s %s",
                                                  getName(),
                                                  m_sqlite.findColumn("ABPerson",
                                                                      "ROWID",
                                                                      uid.c_str(),
                                                                      "FirstSort",
                                                                      uid.c_str()).c_str(),
                                                  info.c_str());
    }
}

void SQLiteContactSource::logItem(const SyncItem &item, const string &info, bool debug)
{
    if (LOG.getLevel() >= (debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO)) {
        string data = (const char *)item.getData();

        // avoid pulling in a full vcard parser by just searching for a specific property,
        // FN in this case
        string name = "???";
        string prop = "\nFN:";
        size_t start = data.find(prop);
        if (start != data.npos) {
            start += prop.size();
            size_t end = data.find("\n", start);
            if (end != data.npos) {
                name = data.substr(start, end - start);
            }
        }

        (LOG.*(debug ? &Log::debug : &Log::info))("%s: %s %s",
                                                  getName(),
                                                  name.c_str(),
                                                  info.c_str());
    }
}

#endif /* ENABLE_SQLITE */

#ifdef ENABLE_MODULES
# include "SQLiteContactSourceRegister.cpp"
#endif
