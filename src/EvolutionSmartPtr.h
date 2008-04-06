/*
 * Copyright (C) 2005-2006 Patrick Ohly
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

#ifndef INCL_EVOLUTION_SMART_POINTER
# define INCL_EVOLUTION_SMART_POINTER

#include <stdlib.h>
#ifdef HAVE_EDS
#include <glib-object.h>
#ifdef ENABLE_EBOOK
#include <libebook/e-book.h>
#endif
#ifdef ENABLE_ECAL
#include <libecal/e-cal.h>
#endif
#endif

#include <stdexcept>
#include <string>
#include <memory>
using namespace std;

#include <boost/scoped_array.hpp>
#include <boost/shared_ptr.hpp>

template<class T> class EvolutionUnrefFree {
 public:
    static void unref(T *pointer) { free(pointer); }
};

class EvolutionUnref {
 public:
    /**
     * C character string - beware, Funambol C++ client library strings must
     * be returned with delete [], use boost::scoped_array
     */
    static void unref(char *pointer) { free(pointer); }

#ifdef HAVE_EDS
    static void unref(GObject *pointer) { g_object_unref(pointer); }
    /** free a list of GObject and the objects */
    static void unref(GList *pointer) {
        if (pointer) {
            GList *next = pointer;
            do {
                g_object_unref(G_OBJECT(next->data));
                next = next->next;
            } while (next);
            g_list_free(pointer);
        }
    }
#ifdef ENABLE_EBOOK
    static void unref(EBookQuery *pointer) { e_book_query_unref(pointer); }
#endif
#ifdef ENABLE_ECAL
    static void unref(icalcomponent *pointer) { icalcomponent_free(pointer); }
    static void unref(icaltimezone *pointer) { icaltimezone_free(pointer, 1); }
#endif
#endif
};

/**
 * a smart pointer implementation for objects for which
 * a unref() function exists;
 * trying to store a NULL pointer raises an exception,
 * unreferencing valid objects is done automatically
 */
template<class T, class base = T, class R = EvolutionUnref > class eptr {
 protected:
    T *m_pointer;
    
  public:
    /**
     * create a smart pointer that owns the given object;
     * passing a NULL pointer and a name for the object raises an error
     */
    eptr(T *pointer = NULL, const char *objectName = NULL) :
        m_pointer( pointer )
    {
        if (!pointer && objectName ) {
            throw std::runtime_error(std::string("Error allocating ") + objectName);
        }
    };
    ~eptr()
    {
        set( NULL );
    }

    /** assignment and copy construction transfer ownership to the copy */
    eptr(eptr &other) {
        m_pointer = other.m_pointer;
        other.m_pointer = NULL;
    }
    eptr & operator = (eptr &other) {
        m_pointer = other.m_pointer;
        other.m_pointer = NULL;
        return *this;
    }

    /**
     * store another object in this pointer, replacing any which was
     * referenced there before;
     * passing a NULL pointer and a name for the object raises an error
     */
    void set( T *pointer, const char *objectName = NULL )
    {
        if (m_pointer) {
            R::unref((base *)m_pointer);
        }
        if (!pointer && objectName) {
            throw std::runtime_error(std::string("Error allocating ") + objectName);
        }
        m_pointer = pointer;
    }

    /**
     * transfer ownership over the pointer to caller and stop tracking it:
     * pointer tracked by smart pointer is set to NULL and the original
     * pointer is returned
     */
    T *release() { T *res = m_pointer; m_pointer = NULL; return res; }

    eptr<T, base, R> &operator = ( T *pointer ) { set( pointer ); return *this; }
    T *get() { return m_pointer; }
    T *operator-> () { return m_pointer; }
    T &operator* ()  { return *m_pointer; }
    operator T * () { return m_pointer; }
    operator void * () { return (void *)m_pointer; }
    operator bool () { return m_pointer != NULL; }
};

template <class T> class CxxUnref {
 public:
    static void unref(T *pointer) { delete pointer; }
};

/** eptr for normal C++ objects */
template <class T> class cxxptr : public eptr<T, T, CxxUnref<T> > {
 public:
    cxxptr(T *pointer = NULL, const char *objectName = NULL) :
        eptr<T, T, CxxUnref<T> > (pointer, objectName)
    {
    };
};

template <class T> class ArrayUnref {
 public:
    static void unref(T *pointer) { delete [] pointer; }
};
    

/** eptr for array of objects or types */
template <class T> class arrayptr : public eptr<T, T, ArrayUnref<T> > {
 public:
    arrayptr(T *pointer = NULL, const char *objectName = NULL) :
        eptr<T, T, ArrayUnref<T> > (pointer, objectName)
    {
    };
};

#endif
