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

#ifndef INCL_SYNCEVOLUTION_TMPFILE
#define INCL_SYNCEVOLUTION_TMPFILE

#include <stdexcept>
#include <string>

#include <pcrecpp.h>


/**
 * Exception class for TmpFile.
 */
class TmpFileException : public std::runtime_error
{
    public:
        TmpFileException(const std::string &what)
            : std::runtime_error(what)
            { }
};


/**
 * Class for handling temporary files, either read/write access
 * or memory mapped. Optionally creates a pipe instead of a plain file.
 *
 * Reading is done mapping the plain file into memory (file) or simply
 * reading from the file descriptor (file or pipe).
 *
 * Closing and removing a mapped file is supported by calling close()
 * after map().
 */
class TmpFile
{
    public:
        enum Type {
            FILE,
            PIPE
        };

        TmpFile();
        virtual ~TmpFile();

        /**
         * Create a temporary file or pipe.
         */
        void create(Type type = FILE);

        /**
         * Create a temporary file with an already existing
         * file descriptor. New instance owns the FD.
         */
        void create(int fd);

        /**
         * Map a view of file and optionally return pointer and/or size.
         *
         * File should already have a correct size.
         *
         * @param mapptr Pointer to variable for mapped pointer. (can be NULL)
         * @param mapsize Pointer to variable for mapped size. (can be NULL)
         */
        void map(void **mapptr = 0, size_t *mapsize = 0);

        /**
         * Unmap a view of file.
         */
        void unmap();

        /**
         * File descriptor, ready for reading from start of file or pipe
         * after create().
         */
        int getFD() const { return m_fd; }

        /**
         * FILE by default, otherwise the value given to create().
         */
        Type getType() const { return m_type; }

        /**
         * Returns amount of bytes not mapped into memory yet, zero if none.
         */
        size_t moreData() const;

        /**
         * Remove the file. If the process crashes, the file will be removed,
         * but the process itself can still map and use the file content.
         */
        void remove();
        /**
         * Remove and close the file.
         *
         * Calling this after map() will make the file disappear from
         * filesystem but the mapping will be valid until unmapped or
         * instance of this class is destroyed.
         */
        void close();

        /**
         * Retrieve file name of the file.
         *
         * @return file name
         */
        const std::string & filename() const
            { return m_filename; }
        /**
         * Retrieve descriptor of the file.
         *
         * @return descriptor
         */
        int fd()
            { return m_fd; }
        /**
         * Size of the mapping.
         *
         * @return mapped size
         */
        size_t size() const
            { return m_mapsize; }
        /**
         * Pointer to the mapping.
         *
         * @return pointer to the mapping
         */
        operator void *()
            { return m_mapptr; }
        /**
         * @overload
         */
        operator const void *() const
            { return m_mapptr; }

        /**
         * Retrieve pcrecpp::StringPiece object for the mapped view.
         *
         * @return pcrecpp::StringPiece of the mapped view
         */
        pcrecpp::StringPiece stringPiece();

    protected:
        Type m_type;
        int m_fd;
        void *m_mapptr;
        size_t m_mapsize;
        std::string m_filename;

};

#endif  // INCL_SYNCEVOLUTION_TMPFILE

