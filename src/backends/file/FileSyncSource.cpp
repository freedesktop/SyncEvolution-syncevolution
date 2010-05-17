/*
 * Copyright (C) 2007-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include "config.h"

#ifdef ENABLE_FILE

#include "FileSyncSource.h"

// SyncEvolution includes a copy of Boost header files.
// They are safe to use without creating additional
// build dependencies. boost::filesystem requires a
// library and therefore is avoided here. Some
// utility functions from SyncEvolution are used
// instead, plus standard C/Posix functions.
#include <boost/algorithm/string/case_conv.hpp>

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include <syncevo/util.h>

#include <sstream>
#include <fstream>

#include <boost/bind.hpp>

#include <syncevo/SyncContext.h>
#include <syncevo/declarations.h>
SE_BEGIN_CXX

FileSyncSource::FileSyncSource(const SyncSourceParams &params,
                               const string &dataformat) :
    TrackingSyncSource(params),
    m_raw(true),
    m_entryCounter(0)
{
    if (m_raw) {
        // replace default operations from TrackingSyncSource/SyncSourceSerialize
        m_operations.m_readItemAsKey = 0;
        m_operations.m_insertItemAsKey = 0;
        m_operations.m_updateItemAsKey = 0;
        m_operations.m_readItem = boost::bind(&FileSyncSource::readItemDirect, this, _1, _2);
        m_operations.m_insertItem = boost::bind(&FileSyncSource::insertItemDirect, this, _1, _2);
        m_operations.m_updateItem = boost::bind(&FileSyncSource::updateItemDirect, this, _1, _2, _3);
    }

    if (dataformat.empty()) {
        throwError("a data format must be specified");
    }
    size_t sep = dataformat.find(':');
    if (sep == dataformat.npos) {
        throwError(string("data format not specified as <mime type>:<mime version>: " + dataformat));
    }
    m_mimeType.assign(dataformat, 0, sep);
    m_mimeVersion = dataformat.substr(sep + 1);
    m_supportedTypes = dataformat;
}

const char *FileSyncSource::getMimeType() const
{
    return m_mimeType.c_str();
}

const char *FileSyncSource::getMimeVersion() const
{
    return m_mimeVersion.c_str();
}

void FileSyncSource::getSynthesisInfo(SynthesisInfo &info,
                                      XMLConfigFragments &fragments)
{
    if (m_raw) {
        // use a new datatype whose name is composed from mime type and version
        info.m_native = string("file:") + info.m_native + m_mimeType + ":" + m_mimeVersion;
        info.m_fieldlist = "";
        info.m_profile = "";
        info.m_datatypes =
            string("        <use datatype='") + info.m_native + "' mode='rw' preferred='yes'/>\n";
        fragments.m_datatypes[info.m_native] =
            StringPrintf("<datatype name='%s' basetype=\"simple\">\n"
                         "    <typestring>%s</typestring>\n"
                         "    <versionstring>%s</versionstring>\n"
                         "</datatype>\n",
                         info.m_native.c_str(),
                         m_mimeType.c_str(),
                         m_mimeVersion.c_str());
    } else {
        TrackingSyncSource::getSynthesisInfo(info, fragments);
    }
}

void FileSyncSource::open()
{
    const string &database = getDatabaseID();
    const string prefix("file://");
    string basedir;
    bool createDir = false;

    // file:// is optional. It indicates that the
    // directory is to be created.
    if (boost::starts_with(database, prefix)) {
        basedir = database.substr(prefix.size());
        createDir = true;
    } else {
        basedir = database;
    }

    // check and, if allowed and necessary, create it
    if (!isDir(basedir)) {
        if (errno == ENOENT && createDir) {
            mkdir_p(basedir.c_str());
        } else {
            throwError(basedir, errno);
        }
    }

    // success!
    m_basedir = basedir;
}

bool FileSyncSource::isEmpty()
{
    DIR *dir = NULL;
    bool empty = true;

    try {
        dir = opendir(m_basedir.c_str());
        if (!dir) {
            SyncContext::throwError(m_basedir, errno);
        }
        errno = 0;
        struct dirent *entry = readdir(dir);
        while (entry) {
            if (strcmp(entry->d_name, ".") &&
                strcmp(entry->d_name, "..")) {
                empty = false;
                break;
            }
            entry = readdir(dir);
        }
        if (errno) {
            SyncContext::throwError(m_basedir, errno);
        }
    } catch(...) {
        if (dir) {
            closedir(dir);
        }
        throw;
    }

    closedir(dir);
    return empty;
}

void FileSyncSource::close()
{
    m_basedir.clear();
}

FileSyncSource::Databases FileSyncSource::getDatabases()
{
    Databases result;

    result.push_back(Database("select database via directory path",
                              "[file://]<path>"));
    return result;
}

void FileSyncSource::listAllItems(RevisionMap_t &revisions)
{
    ReadDir dirContent(m_basedir);

    BOOST_FOREACH(const string &entry, dirContent) {
        string filename = createFilename(entry);
        string revision = getATimeString(filename);
        long entrynum = atoll(entry.c_str());
        if (entrynum >= m_entryCounter) {
            m_entryCounter = entrynum + 1;
        }
        revisions[entry] = revision;
    }
}

void FileSyncSource::readItem(const string &uid, std::string &item, bool raw)
{
    string filename = createFilename(uid);

    if (!ReadFile(filename, item)) {
        throwError(filename + ": reading failed", errno);
    }
}

TrackingSyncSource::InsertItemResult FileSyncSource::insertItem(const string &uid, const std::string &item, bool raw)
{
    string newuid = uid;
    string creationTime;
    string filename;

    // Inserting a new and updating an existing item often uses
    // very similar code. In this case only the code for determining
    // the filename differs.
    //
    // In other sync sources the database might also have limitations
    // for the content of different items, for example, only one
    // VCALENDAR:EVENT with a certain UID. If the server does not
    // recognize that and sends a new item which collides with an
    // existing one, then the existing one should be updated.

    if (uid.size()) {
        // valid local ID: update that file
        filename = createFilename(uid);
    } else {
        // no local ID: create new file
        while (true) {
            ostringstream buff;
            buff << m_entryCounter;
            filename = createFilename(buff.str());

            // only create and truncate if file does not
            // exist yet, otherwise retry with next counter
            struct stat dummy;
            if (stat(filename.c_str(), &dummy)) {
                if (errno == ENOENT) {
                    newuid = buff.str();
                    break;
                } else {
                    throwError(filename, errno);
                }
            }

            m_entryCounter++;
        }
    }

    ofstream out;
    out.open(filename.c_str());
    out.write(item.c_str(), item.size());
    out.close();
    if (!out.good()) {
        throwError(filename + ": writing failed", errno);
    }

    return InsertItemResult(newuid,
                            getATimeString(filename),
                            false /* true if adding item was turned into update */);
}


void FileSyncSource::removeItem(const string &uid)
{
    string filename = createFilename(uid);

    if (unlink(filename.c_str()) &&
        errno != ENOENT) {
        throwError(filename, errno);
    }
}

string FileSyncSource::getATimeString(const string &filename)
{
    struct stat buf;
    if (stat(filename.c_str(), &buf)) {
        throwError(filename, errno);
    }
    time_t mtime = buf.st_mtime;

    ostringstream revision;
    revision << mtime;

    return revision.str();
}

string FileSyncSource::createFilename(const string &entry)
{
    string filename = m_basedir + "/" + entry;
    return filename;
}

sysync::TSyError FileSyncSource::readItemDirect(sysync::cItemID aID, char *&data) throw()
{
    sysync::TSyError res = sysync::LOCERR_OK;
    try {
        string buffer;
        res = readItem(aID->item, buffer, true);
        data = strdup(buffer.c_str());
    } catch (...) {
        res = handleException();
    }
    return res;
}

sysync::TSyError FileSyncSource::insertItemDirect(const char *data, sysync::ItemID newID) throw()
{
    sysync::TSyError res = sysync::LOCERR_OK;
    try {
        res = insertItem("", data, true); 
    } catch (...) {
        res = handleException();
    }
    return res;
}

sysync::TSyError FileSyncSource::updateItemDirect(const char *data, sysync::cItemID aID, sysync::ItemID updID) throw()
{
    sysync::TSyError res = sysync::LOCERR_OK;
    try {
        res = insertItem(aID->item, data, true);
    } catch (...) {
        res = handleException();
    }
    return res;
}

SE_END_CXX

#endif /* ENABLE_FILE */

#ifdef ENABLE_MODULES
# include "FileSyncSourceRegister.cpp"
#endif
