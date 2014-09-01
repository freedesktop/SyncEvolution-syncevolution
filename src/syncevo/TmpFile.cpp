/*
 * Copyright (C) 2012 Intel Corporation
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


#include <cstdio>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "TmpFile.h"
#include "util.h"


TmpFile::TmpFile() :
    m_type(FILE),
    m_fd(-1),
    m_mapptr(0),
    m_mapsize(0)
{
}


TmpFile::~TmpFile()
{
    try {
        unmap();
        close();
        if (m_type == PIPE &&
            !m_filename.empty()) {
            unlink(m_filename.c_str());
        }
    } catch (std::exception &x) {
        fprintf(stderr, "TmpFile::~TmpFile(): %s\n", x.what());
    } catch (...) {
        fputs("TmpFile::~TmpFile(): unknown exception\n", stderr);
    }
}


void TmpFile::create(Type type)
{
    gchar *filename = NULL;
    GError *error = NULL;

    if (m_fd >= 0 || m_mapptr || m_mapsize) {
        throw TmpFileException("TmpFile::create(): busy");
    }
    m_fd = g_file_open_tmp(NULL, &filename, &error);
    if (error != NULL) {
        throw TmpFileException(
            std::string("TmpFile::create(): g_file_open_tmp(): ") +
            std::string(error->message));
    }
    m_filename = filename;
    g_free(filename);
    m_type = type;
    if (type == PIPE) {
        // We merely use the normal file to get a temporary file name which
        // is guaranteed to be unique. There's a slight chance for a denial-of-service
        // attack when someone creates a link or normal file directly after we remove
        // the file, but because mknod neither overwrites an existing entry nor follows
        // symlinks, the effect is smaller compared to opening a file.
        unlink(m_filename.c_str());
        if (mknod(m_filename.c_str(), S_IFIFO|S_IRWXU, 0)) {
            m_filename = "";
            throw TmpFileException(SyncEvo::StringPrintf("mknod(%s): %s",
                                                         m_filename.c_str(),
                                                         strerror(errno)));
        }
        // Open without blocking. Necessary because otherwise we end up
        // waiting here. Opening later also does not work, because then
        // obexd gets stuck in its open() call while we wait for it to
        // acknowledge the start of the transfer.
        m_fd = open(m_filename.c_str(), O_RDONLY|O_NONBLOCK, 0);
        if (m_fd < 0) {
            throw TmpFileException(SyncEvo::StringPrintf("open(%s): %s",
                                                         m_filename.c_str(),
                                                         strerror(errno)));
        }
        // From now on, block on the pipe.
        fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFL) & ~O_NONBLOCK);
    }
}

void TmpFile::create(int fd)
{
    if (m_fd >= 0 || m_mapptr || m_mapsize) {
        throw TmpFileException("TmpFile::create(): busy");
    }
    m_fd = fd;
    m_filename.clear();
    m_type = FILE;
}

void TmpFile::map(void **mapptr, size_t *mapsize)
{
    struct stat sb;

    if (m_mapptr || m_mapsize) {
        throw TmpFileException("TmpFile::map(): busy");
    }
    if (m_fd < 0) {
        throw TmpFileException("TmpFile::map(): m_fd < 0");
    }
    if (fstat(m_fd, &sb) != 0) {
        throw TmpFileException("TmpFile::map(): fstat()");
    }
    // TODO (?): make this configurable.
    //
    // At the moment, SyncEvolution either only reads from a file
    // (and thus MAP_SHARED vs. MAP_PRIVATE doesn't matter, and
    // PROT_WRITE doesn't hurt), or writes for some other process
    // to read the data (hence needing MAP_SHARED).
    m_mapptr = mmap(NULL, sb.st_size, PROT_READ|PROT_WRITE, MAP_SHARED,
                    m_fd, 0);
    if (m_mapptr == MAP_FAILED) {
        m_mapptr = 0;
        throw TmpFileException("TmpFile::map(): mmap()");
    }
    m_mapsize = sb.st_size;

    if (mapptr != NULL) {
        *mapptr = m_mapptr;
    }
    if (mapsize != NULL) {
        *mapsize = m_mapsize;
    }
}


void TmpFile::unmap()
{
    if (m_mapptr && m_mapsize) {
        munmap(m_mapptr, m_mapsize);
    }
    m_mapsize = 0;
    m_mapptr = 0;
}

size_t TmpFile::moreData() const
{
    if (m_fd >= 0) {
        struct stat sb;
        if (fstat(m_fd, &sb) != 0) {
            throw TmpFileException("TmpFile::map(): fstat()");
        }
        if ((!m_mapptr && sb.st_size) ||
            (sb.st_size > 0 && m_mapsize < (size_t)sb.st_size)) {
            return sb.st_size - m_mapsize;
        }
    }

    return 0;
}


void TmpFile::remove()
{
    if (!m_filename.empty()) {
        unlink(m_filename.c_str());
        m_filename.clear();
    }
}

void TmpFile::close()
{
    remove();
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}


pcrecpp::StringPiece TmpFile::stringPiece()
{
    pcrecpp::StringPiece sp;

    if (!(m_mapptr && m_mapsize)) {
        map();
    }
    sp.set(m_mapptr, static_cast<int> (m_mapsize));
    return sp;
}

