/*
 * Copyright (C) 2011 Intel Corporation
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

#ifndef INCL_GLIB_SUPPORT
# define INCL_GLIB_SUPPORT

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <syncevo/util.h>

#ifdef HAVE_GLIB
# include <glib-object.h>
# include <gio/gio.h>
#else
typedef void *GMainLoop;
#endif

#include <boost/shared_ptr.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/utility.hpp>
#include <boost/foreach.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/type_traits/remove_pointer.hpp>
#include <boost/type_traits/function_traits.hpp>
#include <boost/utility/value_init.hpp>
#include <boost/lambda/lambda.hpp>

#include <iterator>
#include <memory>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

enum {
    GLIB_SELECT_NONE = 0,
    GLIB_SELECT_READ = 1,
    GLIB_SELECT_WRITE = 2
};

enum GLibSelectResult {
    GLIB_SELECT_TIMEOUT,      /**< returned because not ready after given amount of time */
    GLIB_SELECT_READY,        /**< fd is ready */
    GLIB_SELECT_QUIT          /**< something else caused the loop to quit, return to caller immediately */
};

/**
 * Waits for one particular file descriptor to become ready for reading
 * and/or writing. Keeps the given loop running while waiting.
 *
 * @param  loop       loop to keep running; must not be NULL
 * @param  fd         file descriptor to watch, -1 for none
 * @param  direction  read, write, both, or none (then fd is ignored)
 * @param  timeout    timeout in seconds + nanoseconds from now, NULL for no timeout, empty value for immediate return
 * @return see GLibSelectResult
 */
GLibSelectResult GLibSelect(GMainLoop *loop, int fd, int direction, Timespec *timeout);

#ifdef HAVE_GLIB

// Signal callback. Specializations will handle varying number of parameters.
template<class S> struct GObjectSignalHandler {
    // static void handler();
    // No specialization defined for the requested function prototype.
};

template<> struct GObjectSignalHandler<void ()> {
    static void handler(gpointer data) throw () {
        try {
            (*reinterpret_cast< boost::function<void ()> *>(data))();
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};
template<class A1> struct GObjectSignalHandler<void (A1)> {
    static void handler(A1 a1, gpointer data) throw () {
        try {
            (*reinterpret_cast< boost::function<void (A1)> *>(data))(a1);
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};
template<class A1, class A2> struct GObjectSignalHandler<void (A1, A2)> {
    static void handler(A1 a1, A2 a2, gpointer data) throw () {
        try {
            (*reinterpret_cast< boost::function<void (A1, A2)> *>(data))(a1, a2);
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};
template<class A1, class A2, class A3> struct GObjectSignalHandler<void (A1, A2, A3)> {
    static void handler(A1 a1, A2 a2, A3 a3, gpointer data) throw () {
        try {
            (*reinterpret_cast< boost::function<void (A1, A2, A3)> *>(data))(a1, a2, a3);
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};
template<class A1, class A2, class A3, class A4> struct GObjectSignalHandler<void (A1, A2, A3, A4)> {
    static void handler(A1 a1, A2 a2, A3 a3, A4 a4, gpointer data) throw () {
        try {
            (*reinterpret_cast< boost::function<void (A1, A2, A3, A4)> *>(data))(a1, a2, a3, a4);
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};
template<class A1, class A2, class A3, class A4, class A5> struct GObjectSignalHandler<void (A1, A2, A3, A4, A5)> {
    static void handler(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, gpointer data) throw () {
        try {
            (*reinterpret_cast< boost::function<void (A1, A2, A3, A4, A5)> *>(data))(a1, a2, a3, a4, a5);
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};
template<class A1, class A2, class A3, class A4, class A5, class A6> struct GObjectSignalHandler<void (A1, A2, A3, A4, A5, A6)> {
    static void handler(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, gpointer data) throw () {
        try {
            (*reinterpret_cast< boost::function<void (A1, A2, A3, A4, A5, A6)> *>(data))(a1, a2, a3, a4, a5, a6);
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};
template<class A1, class A2, class A3, class A4, class A5, class A6, class A7> struct GObjectSignalHandler<void (A1, A2, A3, A4, A5, A6, A7)> {
    static void handler(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, gpointer data) throw () {
        try {
            (*reinterpret_cast< boost::function<void (A1, A2, A3, A4, A5, A6, A7)> *>(data))(a1, a2, a3, a4, a5, a6, a7);
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};
template<class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8> struct GObjectSignalHandler<void (A1, A2, A3, A4, A5, A6, A7, A8)> {
    static void handler(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, gpointer data) throw () {
        try {
            (*reinterpret_cast< boost::function<void (A1, A2, A3, A4, A5, A6, A7, A8)> *>(data))(a1, a2, a3, a4, a5, a6, a7, a8);
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};
template<class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9> struct GObjectSignalHandler<void (A1, A2, A3, A4, A5, A6, A7, A8, A9)> {
    static void handler(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, gpointer data) throw () {
        try {
            (*reinterpret_cast< boost::function<void (A1, A2, A3, A4, A5, A6, A7, A8, A9)> *>(data))(a1, a2, a3, a4, a5, a6, a7, a8, a9);
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

enum RefOwnership
{
    TRANSFER_REF = false, /**<
                           * Create new smart pointer which steals an existing reference without
                           * increasing the reference count of the object.
                           */
    ADD_REF = true        /**<
                           * Create new smart pointer which increases the reference count when
                           * storing the pointer to the object.
                           */
};


template<class C> class TrackGObject : public boost::intrusive_ptr<C> {
    typedef boost::intrusive_ptr<C> Base_t;

    // Frees the instance of boost::function which was allocated
    // by connectSignal.
    template<class S> static void signalDestroy(gpointer data, GClosure *closure) throw () {
        try {
            delete reinterpret_cast< boost::function<void ()> *>(data);
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }

 public:
    TrackGObject(C *ptr, RefOwnership ownership) : Base_t(ptr, (bool)ownership) {}
    TrackGObject() {}
    TrackGObject(const TrackGObject &other) : Base_t(other) {}
    operator C * () const { return Base_t::get(); }
    operator bool () const { return Base_t::get() != NULL; }
    C * ref() const { return static_cast<C *>(g_object_ref(Base_t::get())); }

    static  TrackGObject steal(C *ptr) { return TrackGObject(ptr, TRANSFER_REF); }

    template<class S> guint connectSignal(const char *signal,
                                          const boost::function<S> &callback) {
        return g_signal_connect_data(Base_t::get(), signal,
                                     G_CALLBACK(&GObjectSignalHandler<S>::handler),
                                     new boost::function<S>(callback),
                                     &signalDestroy<S>,
                                     GConnectFlags(0));
    }
    void disconnectSignal(guint handlerID) {
        g_signal_handler_disconnect(static_cast<gpointer>(Base_t::get()),
                                    handlerID);
    }
};

template<class C> class StealGObject : public TrackGObject<C> {
 public:
    StealGObject(C *ptr) : TrackGObject<C>(ptr, TRANSFER_REF) {}
    StealGObject() {}
    StealGObject(const StealGObject &other) : TrackGObject<C>(other) {}
};

template<class C> class TrackGLib : public boost::intrusive_ptr<C> {
    typedef boost::intrusive_ptr<C> Base_t;

 public:
 TrackGLib(C *ptr, RefOwnership ownership) : Base_t(ptr, (bool)ownership) {}
    TrackGLib() {}
    TrackGLib(const TrackGLib &other) : Base_t(other) {}
    operator C * () const { return Base_t::get(); }
    operator bool () const { return Base_t::get() != NULL; }
    C * ref() const { return static_cast<C *>(g_object_ref(Base_t::get())); }

    static  TrackGLib steal(C *ptr) { return TrackGLib(ptr, TRANSFER_REF); }
};

template<class C> class StealGLib : public TrackGLib<C> {
 public:
    StealGLib(C *ptr) : TrackGLib<C>(ptr, TRANSFER_REF) {}
    StealGLib() {}
    StealGLib(const StealGLib &other) : TrackGLib<C>(other) {}
};

/**
 * Defines a shared pointer for a GObject-based type, with intrusive
 * reference counting. Use *outside* of SyncEvolution namespace
 * (i.e. outside of SE_BEGIN/END_CXX. This is necessary because some
 * functions must be put into the boost namespace. The type itself is
 * *inside* the SyncEvolution namespace.
 *
 * connectSignal() connects a GObject signal to a boost::function with
 * the function signature S. Returns the handler ID, which can be
 * passed to g_signal_handler_disconnect() to remove the connection.
 *
 * Example:
 * SE_GOBJECT_TYPE(GFile)
 * SE_GOBJECT_TYPE(GObject)
 * SE_BEGIN_CXX
 * {
 *   // reference normally increased during construction,
 *   // steal() avoids that
 *   GFileCXX filecxx = GFileCXX::steal(g_file_new_for_path("foo"));
 *   GFile *filec = filecxx.get(); // does not increase reference count
 *   // file freed here as filecxx gets destroyed
 * }
 *
 * GObjectCXX object(...);
 * // Define signature explicitly because it cannot be guessed from
 * // boost::bind() result.
 * object.connectSignal<void (GObject *gobject, GParamSpec *pspec)>("notify",
 *                                                                  boost::bind(...));
 * // Signature is taken from boost::function parameter.
 * guint handlerID =
 *     object.connectSignal("notify",
 *                          boost::function<void (GObject *, GParamSpec *)>(boost::bind(...)));
 * object.disconnectSignal(handlerID);
 * SE_END_CXX
 */
#define SE_GOBJECT_TYPE(_x) \
    void inline intrusive_ptr_add_ref(_x *ptr) { g_object_ref(ptr); } \
    void inline intrusive_ptr_release(_x *ptr) { g_object_unref(ptr); } \
    SE_BEGIN_CXX \
    typedef TrackGObject<_x> _x ## CXX; \
    typedef StealGObject<_x> _x ## StealCXX; \
    SE_END_CXX \

/**
 * Defines a CXX smart pointer similar to SE_GOBJECT_TYPE,
 * but for types which have their own _ref and _unref
 * calls.
 *
 * Example:
 * SE_GLIB_TYPE(GMainLoop, g_main_loop)
 */
#define SE_GLIB_TYPE(_x, _func_prefix) \
    void inline intrusive_ptr_add_ref(_x *ptr) { _func_prefix ## _ref(ptr); } \
    void inline intrusive_ptr_release(_x *ptr) { _func_prefix ## _unref(ptr); } \
    SE_BEGIN_CXX \
    typedef TrackGLib<_x> _x ## CXX; \
    typedef StealGLib<_x> _x ## StealCXX; \
    SE_END_CXX

SE_END_CXX

SE_GOBJECT_TYPE(GFile)
SE_GOBJECT_TYPE(GFileMonitor)
SE_GLIB_TYPE(GMainLoop, g_main_loop)
SE_GLIB_TYPE(GAsyncQueue, g_async_queue)
SE_GLIB_TYPE(GHashTable, g_hash_table)

SE_BEGIN_CXX

/**
 * Wrapper around g_file_monitor_file().
 * Not copyable because monitor is tied to specific callback
 * via memory address.
 */
class GLibNotify : public boost::noncopyable
{
 public:
    typedef boost::function<void (GFile *, GFile *, GFileMonitorEvent)> callback_t;

    GLibNotify(const char *file, 
               const callback_t &callback);
 private:
    GFileMonitorCXX m_monitor;
    callback_t m_callback;
};

class SourceLocation; // Exception.h

/**
 * Wraps GError. Where a GError** is expected, simply pass
 * a GErrorCXX instance.
 */
struct GErrorCXX {
    GError *m_gerror;

    /** empty error, NULL pointer */
    GErrorCXX() : m_gerror(NULL) {}

    /** copies error content */
    GErrorCXX(const GErrorCXX &other) : m_gerror(g_error_copy(other.m_gerror)) {}
    GErrorCXX &operator =(const GErrorCXX &other) {
        if (m_gerror != other.m_gerror) {
            if (m_gerror) {
                g_clear_error(&m_gerror);
            }
            if (other.m_gerror) {
                m_gerror = g_error_copy(other.m_gerror);
            }
        }
        return *this;
    }
    GErrorCXX &operator =(const GError* err) {
        if (err != m_gerror) {
            if (m_gerror) {
                g_clear_error(&m_gerror);
            }
            if (err) {
                m_gerror = g_error_copy(err);
            }
        }
        return *this;
    }

    /** takes over ownership */
    void take (GError *err) {
        if (err != m_gerror) {
            if (m_gerror) {
                g_clear_error(&m_gerror);
            }
            m_gerror = err;
        }
    }

    /** For convenient access to GError members (message, domain, ...) */
    const GError * operator-> () const { return m_gerror; }

    /**
     * For passing to C functions. They must not free the GError,
     * because GErrorCXX retains ownership.
     */
    operator const GError * () const { return m_gerror; }

    /** error description, with fallback if not set (not expected, so not localized) */
    operator const char * () { return m_gerror ? m_gerror->message : "<<no error>>"; }

    /** clear error */
    ~GErrorCXX() { g_clear_error(&m_gerror); }

    /** clear error if any is set */
    void clear() { g_clear_error(&m_gerror); }

    /** transfer ownership of error back to caller */
    GError *release() { GError *gerror = m_gerror; m_gerror = NULL; return gerror; }

    /** checks whether the current error is the one passed as parameters */
    bool matches(GQuark domain, gint code) { return g_error_matches(m_gerror, domain, code); }

    /**
     * Use this when passing GErrorCXX instance to C functions which need to set it.
     * Make sure the pointer isn't set yet (new GErrorCXX instance, reset if
     * an error was encountered before) or the GNOME functions will complain
     * when overwriting the existing error.
     */
    operator GError ** () { return &m_gerror; }

    /** true if error set */
    operator bool () { return m_gerror != NULL; }

    /**
     * always throws an exception, including information from GError if available:
     * <action>: <error message>|failure
     */
    void throwError(const SourceLocation &where, const std::string &action);
    static void throwError(const SourceLocation &where, const std::string &action, const GError *err);
};

template<class T> void NoopDestructor(T *) {}
template<class T> void GObjectDestructor(T *ptr) { g_object_unref(ptr); }
template<class T> void GFreeDestructor(T *ptr) { g_free(static_cast<void *>(ptr)); }

/**
 * Copies strings from a collection into a newly allocated, NULL
 * terminated array. Copying the strings is optional. Suggested
 * usage is:
 *
 * C collection;
 * collection.push_back(...);
 * boost::scoped_array<char *> array(AllocStringArray(collection));
 *
 */
template<typename T> char **AllocStringArray(const T &strings,
                                             const char **(*allocArray)(size_t) = NULL,
                                             void (*freeArray)(const char **) = NULL,
                                             const char *(*copyString)(const char *) = NULL,
                                             const void (*freeString)(char *) = NULL)
{
    size_t arraySize = strings.size() + 1;
    const char **array = NULL;
    array = allocArray ? allocArray(arraySize) : new const char *[arraySize];
    if (!array) {
        throw std::bad_alloc();
    }
    try {
        memset(array, 0, sizeof(*array) * arraySize);
        size_t i = 0;
        BOOST_FOREACH(const std::string &str, strings) {
            array[i] = copyString ? copyString(str.c_str()) : str.c_str();
            if (!array[i]) {
                throw std::bad_alloc();
            }
            i++;
        }
    } catch (...) {
        if (freeString) {
            for (const char **ptr = array;
                 *ptr;
                 ptr++) {
                freeString(const_cast<char *>(*ptr));
            }
        }
        if (freeArray) {
            freeArray(array);
        }
        throw;
    }
    return const_cast<char **>(array);
}


/**
 * Wraps a G[S]List of pointers to a specific type.
 * Can be used with boost::FOREACH and provides forward iterators
 * (two-way iterators and reverse iterators also possible, but not implemented).
 * Frees the list and optionally (not turned on by default) also frees
 * the data contained in it, using the provided destructor class.
 * Use GObjectDestructor for GObject instances.
 *
 * @param T    the type of the instances pointed to inside the list
 * @param L    GList or GSList
 * @param D    destructor function freeing a T instance
 */
template< class T, class L, void (*D)(T*) = NoopDestructor<T> > struct GListCXX : boost::noncopyable {
    L *m_list;

    static void listFree(GSList *l) { g_slist_free(l); }
    static void listFree(GList *l) { g_list_free(l); }

    static GSList *listPrepend(GSList *list, T *entry) { return g_slist_prepend(list, (gpointer)entry); }
    static GList *listPrepend(GList *list, T *entry) { return g_list_prepend(list, (gpointer)entry); }

    static GSList *listAppend(GSList *list, T *entry) { return g_slist_append(list, (gpointer)entry); }
    static GList *listAppend(GList *list, T *entry) { return g_list_append(list, (gpointer)entry); }

 public:
    typedef T * value_type;

    /** by default initialize an empty list; if parameter is not NULL,
        owership is transferred to the new instance of GListCXX */
    GListCXX(L *list = NULL) : m_list(list) {}

    /** free list */
    ~GListCXX() { clear(); }

    /** free old content, take owership of new one */
    void reset(L *list = NULL) {
        clear();
        m_list = list;
    }

    bool empty() { return m_list == NULL; }

    /** clear error if any is set */
    void clear() {
#if 1
        BOOST_FOREACH(T *entry, *this) {
            D(entry);
        }
#else
        for (iterator it = begin();
             it != end();
             ++it) {
            D(*it);
        }
#endif
        listFree(m_list);
        m_list = NULL;
    }

    /**
     * Use this when passing GListCXX instance to C functions which need to set it.
     * Make sure the pointer isn't set yet (new GListCXX instance or cleared).
     */
    operator L ** () { return &m_list; }

    /**
     * Cast to plain G[S]List, for use in functions which do not modify the list.
     */
    operator L * () { return m_list; }

    class iterator : public std::iterator<std::forward_iterator_tag, T *> {
        L *m_entry;
    public:
        iterator(L *list) : m_entry(list) {}
        iterator(const iterator &other) : m_entry(other.m_entry) {}
        /**
         * boost::foreach needs a reference as return code here,
         * which forces us to do type casting on the address of the void * pointer,
         * then dereference the pointer. The reason is that typecasting the
         * pointer value directly yields an rvalue, which can't be used to initialize
         * the reference return value.
         */
        T * &operator -> () const { return *getEntryPtr(); }
        T * &operator * () const { return *getEntryPtr(); }
        iterator & operator ++ () { m_entry = m_entry->next; return *this; }
        iterator operator ++ (int) { return iterator(m_entry->next); }
        bool operator == (const iterator &other) { return m_entry == other.m_entry; }
        bool operator != (const iterator &other) { return m_entry != other.m_entry; }

    private:
        /**
         * Used above, necessary to hide the fact that we do type
         * casting tricks. Otherwise the compiler will complain about
         * *(T **)&m_entry->data with "dereferencing type-punned
         * pointer will break strict-aliasing rules".
         *
         * That warning is about breaking assumptions that the compiler
         * uses for optimizations. The hope is that those optimzations
         * aren't done here, and/or are disabled by using a function.
         */
        T** getEntryPtr() const { return (T **)&m_entry->data; }
    };
    iterator begin() { return iterator(m_list); }
    iterator end() { return iterator(NULL); }

    class const_iterator : public std::iterator<std::forward_iterator_tag, T *> {
        L *m_entry;
        T *m_value;

    public:
        const_iterator(L *list) : m_entry(list) {}
        const_iterator(const const_iterator &other) : m_entry(other.m_entry) {}
        T * &operator -> () const { return *getEntryPtr(); }
        T * &operator * () const { return *getEntryPtr(); }
        const_iterator & operator ++ () { m_entry = m_entry->next; return *this; }
        const_iterator operator ++ (int) { return iterator(m_entry->next); }
        bool operator == (const const_iterator &other) { return m_entry == other.m_entry; }
        bool operator != (const const_iterator &other) { return m_entry != other.m_entry; }

    private:
        T** getEntryPtr() const { return (T **)&m_entry->data; }
    };

    const_iterator begin() const { return const_iterator(m_list); }
    const_iterator end() const { return const_iterator(NULL); }

    void push_back(T *entry) { m_list = listAppend(m_list, entry); }
    void push_front(T *entry) { m_list = listPrepend(m_list, entry); }
};

/** use this for a list which owns the strings it points to */
typedef GListCXX<char, GList, GFreeDestructor<char> > GStringListFreeCXX;
/** use this for a list which does not own the strings it points to */
typedef GListCXX<char, GList> GStringListNoFreeCXX;

/**
 * Wraps a C gchar array and takes care of freeing the memory.
 */
class PlainGStr : public boost::shared_ptr<gchar>
{
    public:
        PlainGStr() {}
        PlainGStr(gchar *str) : boost::shared_ptr<char>(str, g_free) {}
        PlainGStr(const PlainGStr &other) : boost::shared_ptr<gchar>(other) {}    
        operator const gchar *() const { return &**this; }
        const gchar *c_str() const { return &**this; }
};

/**
 * Wraps a glib string array, frees with g_strfreev().
 */
class PlainGStrArray : public boost::shared_ptr<gchar *>
{
    public:
        PlainGStrArray() {}
        PlainGStrArray(gchar **array) : boost::shared_ptr<char *>(array, g_strfreev) {}
        PlainGStrArray(const PlainGStrArray &other) : boost::shared_ptr<char *>(other) {}
        operator gchar * const *() const { return &**this; }
        gchar * &at(size_t index) { return get()[index]; }
 private:
        // Hide this operator because boost::shared_ptr has problems with it,
        // probably because of missing traits for a pointer type. Instead use
        // at().
        gchar * operator[] (size_t index);
};

// empty template, need specialization based on parameter and return types
template <class T, class F, F *finish, class A1, class A2, class A3, class A4, class A5> struct GAsyncReady5 {};
template <class T, class F, F *finish, class A1, class A2, class A3, class A4> struct GAsyncReady4 {};
template <class T, class F, F *finish, class A1, class A2, class A3> struct GAsyncReady3 {};
template <class T, class F, F *finish, class A1, class A2> struct GAsyncReady2 {};
template <class T, class F, F *finish, class A1> struct GAsyncReady1 {};

// empty template, need specializations based on arity
template<class F, F *finish, int arity> struct GAsyncReadyCXX {};

// five parameters of finish function
template<class F, F *finish> struct GAsyncReadyCXX<F, finish, 5> :
    public GAsyncReady5<typename boost::function_traits<F>::result_type,
                        F, finish,
                        typename boost::function_traits<F>::arg1_type,
                        typename boost::function_traits<F>::arg2_type,
                        typename boost::function_traits<F>::arg3_type,
                        typename boost::function_traits<F>::arg4_type,
                        typename boost::function_traits<F>::arg5_type>
{
};

// four parameters
template<class F, F *finish> struct GAsyncReadyCXX<F, finish, 4> :
    public GAsyncReady4<typename boost::function_traits<F>::result_type,
                        F, finish,
                        typename boost::function_traits<F>::arg1_type,
                        typename boost::function_traits<F>::arg2_type,
                        typename boost::function_traits<F>::arg3_type,
                        typename boost::function_traits<F>::arg4_type>
{
};

// three parameters
template<class F, F *finish> struct GAsyncReadyCXX<F, finish, 3> :
    public GAsyncReady3<typename boost::function_traits<F>::result_type,
                        F, finish,
                        typename boost::function_traits<F>::arg1_type,
                        typename boost::function_traits<F>::arg2_type,
                        typename boost::function_traits<F>::arg3_type>
{
};

// two parameters
template<class F, F *finish> struct GAsyncReadyCXX<F, finish, 2> :
    public GAsyncReady2<typename boost::function_traits<F>::result_type,
                        F, finish,
                        typename boost::function_traits<F>::arg1_type,
                        typename boost::function_traits<F>::arg2_type>
{
};

// one parameter
template<class F, F *finish> struct GAsyncReadyCXX<F, finish, 1> :
    public GAsyncReady1<typename boost::function_traits<F>::result_type,
                        F, finish,
                        typename boost::function_traits<F>::arg1_type>
{
};


// For finish functions with return parameters the assumption is that
// they have a non-void return value. Otherwise there would be no need
// for the return parameters.
//
// result = GObject, GAsyncResult, A3, A4, A5
template<class T, class F, F *finish, class A1, class A3, class A4, class A5> struct GAsyncReady5<T, F, finish, A1, GAsyncResult *, A3, A4, A5>
{
    typedef typename boost::remove_pointer<A3>::type A3_t;
    typedef typename boost::remove_pointer<A4>::type A4_t;
    typedef typename boost::remove_pointer<A5>::type A5_t;
    typedef boost::function<void (T, A3_t, A4_t, A5_t)> CXXFunctionCB_t;

    static void handleGLibResult(GObject *sourceObject,
                                 GAsyncResult *result,
                                 gpointer userData) throw () {
        try {
            A3_t retval1 = boost::value_initialized<A3_t>();
            A4_t retval2 = boost::value_initialized<A4_t>();
            A5_t retval3 = boost::value_initialized<A5_t>();
            T t = finish(reinterpret_cast<A1>(sourceObject),
                         result, &retval1, &retval2, &retval3);
            std::auto_ptr<CXXFunctionCB_t> cb(static_cast<CXXFunctionCB_t *>(userData));
            (*cb)(t, retval1, retval2, retval3);
        } catch (...) {
            // called from C, must not let exception escape
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

// result = GObject, GAsyncResult, A3, A4, GError
template<class T, class F, F *finish, class A1, class A3, class A4> struct GAsyncReady5<T, F, finish, A1, GAsyncResult *, A3, A4, GError **>
{
    typedef typename boost::remove_pointer<A3>::type A3_t;
    typedef typename boost::remove_pointer<A4>::type A4_t;
    typedef boost::function<void (T, A3_t, A4_t, const GError *)> CXXFunctionCB_t;

    static void handleGLibResult(GObject *sourceObject,
                                 GAsyncResult *result,
                                 gpointer userData) throw () {
        try {
            GErrorCXX gerror;
            A3_t retval1 = boost::value_initialized<A3_t>();
            A4_t retval2 = boost::value_initialized<A4_t>();
            T t = finish(reinterpret_cast<A1>(sourceObject),
                         result, &retval1, &retval2, gerror);
            std::auto_ptr<CXXFunctionCB_t> cb(static_cast<CXXFunctionCB_t *>(userData));
            (*cb)(t, retval1, retval2, gerror);
        } catch (...) {
            // called from C, must not let exception escape
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

// result = GObject, GAsyncResult, A3, A4
template<class T, class F, F *finish, class A1, class A3, class A4> struct GAsyncReady4<T, F, finish, A1, GAsyncResult *, A3, A4>
{
    typedef typename boost::remove_pointer<A3>::type A3_t;
    typedef typename boost::remove_pointer<A4>::type A4_t;
    typedef boost::function<void (T, A3_t, A4_t)> CXXFunctionCB_t;

    static void handleGLibResult(GObject *sourceObject,
                                 GAsyncResult *result,
                                 gpointer userData) throw () {
        try {
            A3_t retval1 = boost::value_initialized<A3_t>();
            A4_t retval2 = boost::value_initialized<A4_t>();
            T t = finish(reinterpret_cast<A1>(sourceObject),
                         result, &retval1, &retval2);
            std::auto_ptr<CXXFunctionCB_t> cb(static_cast<CXXFunctionCB_t *>(userData));
            (*cb)(t, retval1, retval2);
        } catch (...) {
            // called from C, must not let exception escape
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

// result = GObject, GAsyncResult, A3, GError
template<class T, class F, F *finish, class A1, class A3> struct GAsyncReady4<T, F, finish, A1, GAsyncResult *, A3, GError **>
{
    typedef typename boost::remove_pointer<A3>::type A3_t;
    typedef boost::function<void (T, A3_t, const GError *)> CXXFunctionCB_t;

    static void handleGLibResult(GObject *sourceObject,
                                 GAsyncResult *result,
                                 gpointer userData) throw () {
        try {
            GErrorCXX gerror;
            A3_t retval = boost::value_initialized<A3_t>();
            T t = finish(reinterpret_cast<A1>(sourceObject),
                         result, &retval, gerror);
            std::auto_ptr<CXXFunctionCB_t> cb(static_cast<CXXFunctionCB_t *>(userData));
            (*cb)(t, retval, gerror);
        } catch (...) {
            // called from C, must not let exception escape
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

// res = GObject, GAsyncResult, GError
template <class T, class F, F *finish, class A1> struct GAsyncReady3<T, F, finish, A1, GAsyncResult *, GError **>{
    typedef boost::function<void (T, const GError *)> CXXFunctionCB_t;

    static void handleGLibResult(GObject *sourceObject,
                                 GAsyncResult *result,
                                 gpointer userData) throw () {
        try {
            GErrorCXX gerror;
            T t = finish(reinterpret_cast<A1>(sourceObject),
                         result, gerror);
            std::auto_ptr<CXXFunctionCB_t> cb(static_cast<CXXFunctionCB_t *>(userData));
            (*cb)(t, gerror);
        } catch (...) {
            // called from C, must not let exception escape
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

// void = GObject, GAsyncResult, GError
template<class F, F *finish, class A1> struct GAsyncReady3<void, F, finish, A1, GAsyncResult *, GError **>
{
    typedef boost::function<void (const GError *)> CXXFunctionCB_t;

    static void handleGLibResult(GObject *sourceObject,
                                 GAsyncResult *result,
                                 gpointer userData) throw () {
        try {
            GErrorCXX gerror;
            finish(reinterpret_cast<A1>(sourceObject),
                   result, gerror);
            std::auto_ptr<CXXFunctionCB_t> cb(static_cast<CXXFunctionCB_t *>(userData));
            (*cb)(gerror);
        } catch (...) {
            // called from C, must not let exception escape
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

// result = GObject, GAsyncResult
template<class T, class F, F *finish, class A1> struct GAsyncReady2<T, F, finish, A1, GAsyncResult *>
{
    typedef boost::function<void (T)> CXXFunctionCB_t;

    static void handleGLibResult(GObject *sourceObject,
                                 GAsyncResult *result,
                                 gpointer userData) throw () {
        try {
            T t = finish(reinterpret_cast<A1>(sourceObject),
                         result);
            std::auto_ptr<CXXFunctionCB_t> cb(static_cast<CXXFunctionCB_t *>(userData));
            (*cb)(t);
        } catch (...) {
            // called from C, must not let exception escape
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

// result = GAsyncResult, GError
template<class T, class F, F *finish> struct GAsyncReady2<T, F, finish, GAsyncResult *, GError **> {
 public:
    typedef boost::function<void (T, const GError *)> CXXFunctionCB_t;

    static void handleGLibResult(GObject *sourceObject,
                                 GAsyncResult *result,
                                 gpointer userData) throw () {
        try {
            GErrorCXX gerror;
            T t = finish(result, gerror);
            std::auto_ptr<CXXFunctionCB_t> cb(static_cast<CXXFunctionCB_t *>(userData));
            (*cb)(t, gerror);
        } catch (...) {
            // called from C, must not let exception escape
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

// void  = GObject, GAsyncResult
template<class F, F *finish, class A1> struct GAsyncReady2<void, F, finish, A1, GAsyncResult *>
{
    typedef boost::function<void ()> CXXFunctionCB_t;

    static void handleGLibResult(GObject *sourceObject,
                                 GAsyncResult *result,
                                 gpointer userData) throw () {
        try {
            finish(reinterpret_cast<A1>(sourceObject),
                   result);
            std::auto_ptr<CXXFunctionCB_t> cb(static_cast<CXXFunctionCB_t *>(userData));
            (*cb)();
        } catch (...) {
            // called from C, must not let exception escape
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

// void = GAsyncResult, GError
template<class F, F *finish> struct GAsyncReady2<void, F, finish, GAsyncResult *, GError **>
{
    typedef boost::function<void (const GError *)> CXXFunctionCB_t;

    static void handleGLibResult(GObject *sourceObject,
                                 GAsyncResult *result,
                                 gpointer userData) throw () {
        try {
            GErrorCXX gerror;
            finish(result, gerror);
            std::auto_ptr<CXXFunctionCB_t> cb(static_cast<CXXFunctionCB_t *>(userData));
            (*cb)(gerror);
        } catch (...) {
            // called from C, must not let exception escape
            Exception::handle(HANDLE_EXCEPTION_FATAL);
        }
    }
};

/**
 * convenience macro for picking the GAsyncReadyCXX that matches the _prepare call:
 * first switch based on arity of the finish function, then on its type
 */
#define SYNCEVO_GLIB_CALL_ASYNC_CXX(_prepare) \
    GAsyncReadyCXX< boost::remove_pointer<typeof(_prepare ## _finish)>::type, \
                    & _prepare ## _finish, \
                    boost::function_traits<boost::remove_pointer<typeof(_prepare ## _finish)>::type>::arity >

/**
 * Macro for asynchronous methods which use a GAsyncReadyCallback to
 * indicate completion. The assumption is that there is a matching
 * _finish function for the function which starts the operation.
 *
 * The boost::function callback will be called exactly once, with the
 * following parameters:
 * - return value of the _finish call, if non-void
 * - all return parameters of the _finish call, in the order
 *   in which they appear there
 * - a GError is passed as "const GError *", with NULL if the
 *   _finish function did not set an error; it does not have to
 *   be freed.
 *
 * Other parameters must be freed if required by the _finish semantic.
 *
 * Use boost::bind() with a boost::weak_ptr as second
 * parameter when the callback belongs to an instance which is
 * not guaranteed to be around when the operation completes.
 *
 * Example:
 *
 *  static void asyncCB(const GError *gerror, const char *func, bool &failed, bool &done) {
 *      done = true;
 *      if (gerror) {
 *          failed = true;
 *          // log gerror->message or store in GErrorCXX
 *      }
 *  }
 *
 *  bool done = false, failed = false;
 *  SYNCEVO_GLIB_CALL_ASYNC(folks_individual_aggregator_prepare,
 *                          boost::bind(asyncCB, _1,
 *                                      "folks_individual_aggregator_prepare",
 *                                      boost::ref(failed), boost::ref(done)),
 *                          aggregator);
 *
 *  // Don't continue unless finished, because the callback will write
 *  // into "done" and possibly "failed".
 *  while (!done) {
 *      g_main_context_iteration(NULL, true);
 *  }
 *
 * @param _prepare     name of the function which starts the operation
 * @param _cb          boost::function with GError pointer and optional result value;
 *                     exceptions are considered fatal
 * @param _args        parameters of _prepare, without the final GAsyncReadyCallback + user_data pair;
 *                     usually at least a GCancellable pointer is part of the arguments
 */
#define SYNCEVO_GLIB_CALL_ASYNC(_prepare, _cb, _args...) \
    _prepare(_args, \
             SYNCEVO_GLIB_CALL_ASYNC_CXX(_prepare)::handleGLibResult, \
             new SYNCEVO_GLIB_CALL_ASYNC_CXX(_prepare)::CXXFunctionCB_t(_cb))

// helper class for finish method with some kind of result other than void
template<class T> class GAsyncReadyDoneCXX
{
 public:
    template<class R> static void storeResult(GErrorCXX &gerrorStorage,
                                              R &resultStorage,
                                              bool &done,
                                              T result,
                                              const GError *gerror) {
        done = true;
        gerrorStorage = gerror;
        resultStorage = result;
    }

    template<class R> static boost::function<void (T, const GError *)> createCB(R &result, GErrorCXX &gerror, bool &done) {
        return boost::bind(storeResult<R>, boost::ref(gerror), boost::ref(result), boost::ref(done), _1, _2);
    }
};

// helper class for finish method with void result
template<> class GAsyncReadyDoneCXX<void>
{
 public:
    static void storeResult(GErrorCXX &gerrorStorage,
                            bool &done,
                            const GError *gerror) {
        done = true;
        gerrorStorage = gerror;
    }

    static boost::function<void (const GError *)> createCB(const int *dummy, GErrorCXX &gerror, bool &done) {
        return boost::bind(storeResult, boost::ref(gerror), boost::ref(done), _1);
    }
};

/**
 * Like SYNCEVO_GLIB_CALL_ASYNC, but blocks until the operation
 * has finished.
 *
 * @param _res         an instance which will hold the result when done, NULL when result is void
 * @param _gerror      a GErrorCXX instance which will hold an error
 *                     pointer afterwards in case of a failure
 */
#define SYNCEVO_GLIB_CALL_SYNC(_res, _gerror, _prepare, _args...) \
    do { \
        bool done = false; \
        SYNCEVO_GLIB_CALL_ASYNC(_prepare, \
                                GAsyncReadyDoneCXX<boost::function<typeof(_prepare ## _finish)>::result_type>::createCB(_res, _gerror, done), \
                                _args); \
        GRunWhile(! boost::lambda::var(done)); \
    } while (false); \

#endif // HAVE_GLIB

SE_END_CXX

#endif // INCL_GLIB_SUPPORT

