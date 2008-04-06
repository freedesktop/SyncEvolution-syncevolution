/*
 * Copyright (C) 2008 Patrick Ohly
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY, TITLE, NONINFRINGEMENT or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307  USA
 */

#include <config.h>
#include "SyncEvolutionUtil.h"
#include "EvolutionSyncClient.h"
#include <base/test.h>

#include <boost/scoped_array.hpp>

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#ifdef ENABLE_UNIT_TESTS
CPPUNIT_REGISTRY_ADD_TO_DEFAULT("SyncEvolution");
#endif

string normalizePath(const string &path)
{
    string res;

    res.reserve(path.size());
    size_t index = 0;
    while (index < path.size()) {
        char curr = path[index];
        res += curr;
        index++;
        if (curr == '/') {
            while (index < path.size() &&
                   (path[index] == '/' ||
                    (path[index] == '.' &&
                     index + 1 < path.size() &&
                     (path[index + 1] == '.' ||
                      path[index + 1] == '/')))) {
                index++;
            }
        }
    }
    if (!res.empty() && res[res.size() - 1] == '/') {
        res.resize(res.size() - 1);
    }
    return res;
}

void mkdir_p(const string &path)
{
    boost::scoped_array<char> dirs(new char[path.size() + 1]);
    char *curr = dirs.get();
    strcpy(curr, path.c_str());
    do {
        char *nextdir = strchr(curr, '/');
        if (nextdir) {
            *nextdir = 0;
            nextdir++;
        }
        if (*curr) {
            if (access(dirs.get(),
                       nextdir ? (R_OK|X_OK) : (R_OK|X_OK|W_OK)) &&
                (errno != ENOENT ||
                 mkdir(dirs.get(), 0777))) {
                EvolutionSyncClient::throwError(string(dirs.get()) + ": " + strerror(errno));
            }
        }
        if (nextdir) {
            nextdir[-1] = '/';
        }
        curr = nextdir;
    } while (curr);
}

void rm_r(const string &path)
{
    if (!unlink(path.c_str()) ||
        errno == ENOENT) {
        return;
    }

    if (errno != EISDIR) {
        EvolutionSyncClient::throwError(path + ": " + strerror(errno));
    }

    ReadDir dir(path);
    for (ReadDir::const_iterator it = dir.begin();
         it != dir.end();
         ++it) {
        rm_r(path + "/" + *it);
    }
    if (rmdir(path.c_str())) {
        EvolutionSyncClient::throwError(path + ": " + strerror(errno));
    }
}

bool isDir(const string &path)
{
    DIR *dir = opendir(path.c_str());
    if (dir) {
        closedir(dir);
        return true;
    } else if (errno != ENOTDIR && errno != ENOENT) {
        EvolutionSyncClient::throwError(path + ": " + strerror(errno));
    }

    return false;
}

UUID::UUID()
{
    static class InitSRand {
    public:
        InitSRand() {
            srand(time(NULL));
        }
    } initSRand;

    char buffer[16 * 4 + 5];
    sprintf(buffer, "%08x-%04x-%04x-%02x%02x-%08x%04x",
            rand() & 0xFFFFFFFF,
            rand() & 0xFFFF,
            rand() & 0x0FFF | 0x4000 /* RFC 4122 time_hi_and_version */,
            rand() & 0xBF | 0x80 /* clock_seq_hi_and_reserved */,
            rand() & 0xFF,
            rand() & 0xFFFFFFFF,
            rand() & 0xFFFF
            );
    this->assign(buffer);
}


ReadDir::ReadDir(const string &path) : m_path(path)
{
    DIR *dir = NULL;

    try {
        dir = opendir(path.c_str());
        if (!dir) {
            EvolutionSyncClient::throwError(path + ": " + strerror(errno));
        }
        errno = 0;
        struct dirent *entry = readdir(dir);
        while (entry) {
            if (strcmp(entry->d_name, ".") &&
                strcmp(entry->d_name, "..")) {
                m_entries.push_back(entry->d_name);
            }
            entry = readdir(dir);
        }
        if (errno) {
            EvolutionSyncClient::throwError(path + ": " + strerror(errno));
        }
    } catch(...) {
        if (dir) {
            closedir(dir);
        }
        throw;
    }

    closedir(dir);
}
