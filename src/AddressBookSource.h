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

#ifndef INCL_ADDRESSBOOKSOURCE
#define INCL_ADDRESSBOOKSOURCE

#include <config.h>
#include "EvolutionSyncSource.h"
#include "EvolutionSmartPtr.h"

#ifdef ENABLE_ADDRESSBOOK

#include <AddressBook/ABAddressBookC.h>
#include "DeviceManagementNode.h"

/**
 * a smart pointer for CoreFoundation object references
 *
 * trying to store a NULL pointer raises an exception,
 * unreferencing valid objects is done automatically
 *
 * @param T         the pointer type
 * @param release   CFRelease() is only called when passing true
 */
template<class T, bool doRelease = 
#ifdef IPHONE
    // by default do not release anything because that has led
    // to crashes: this is the safe default in case of doubt
    false
#else
    true
#endif
    > class ref {
    /** do not allow copy construction */
    ref( const ref &other) {};

    /** do not allow copying */
    void operator = ( const ref &other ) {}

 protected:
    T m_pointer;
    
  public:
    /**
     * create a smart pointer that owns the given object;
     * passing a NULL pointer and a name for the object raises an error
     */
    ref(T pointer = NULL, const char *objectName = NULL) :
        m_pointer( pointer )
    {
        if (!pointer && objectName ) {
            throw std::runtime_error(std::string("Error allocating ") + objectName);
        }
    };
    ~ref()
    {
        set( NULL );
    }

    /**
     * store another object in this pointer, replacing any which was
     * referenced there before;
     * passing a NULL pointer and a name for the object raises an error
     */
    void set( T pointer, const char *objectName = NULL )
    {
        if (m_pointer && doRelease) {
            CFRelease(m_pointer);
        }
        if (!pointer && objectName) {
            throw std::runtime_error(std::string("Error allocating ") + objectName);
        }
        m_pointer = pointer;
    }

    ref<T> &operator = ( T pointer ) { set( pointer ); return *this; }
    T operator-> () { return m_pointer; }
    T operator * () { return m_pointer; }
    operator T () { return m_pointer; }
    operator bool () { return m_pointer != NULL; }

    T release() {
        T res = m_pointer;
        m_pointer = NULL;
        return res;
    }
};

#if 0
/* template typedefs would have been handy here, but are not specified in C++ (yet) */
#ifdef IPHONE
/** do not free some particular objects on the iPhone because that crashes */
template<class T> typedef ref<T, false> iphoneref;
#else
template<class T> typedef ref<T, true> iphoneref;
#endif

#else

#ifdef IPHONE
# define IPHONE_RELEASE false
#else
# define IPHONE_RELEASE true
#endif

#endif


/**
 * The AddressBookSource synchronizes the Mac OS X and iPhone system
 * address book using the "AddressBook" framework. Changes are tracked
 * by comparing the current time stamp of a contact against its time
 * stamp from the previous sync, stored in a separate key/value
 * database. Contacts are converted to/from vCard 2.1 using custom
 * code because a) the mapping can be chosen so that typical SyncML
 * servers understand it and b) the iPhone's AddressBook does not have
 * vcard import/export functions.
 *
 * On the iPhone the interface is similar, but not the same. These
 * differences are hidden behind "ifdef IPHONE" which depends (for
 * simplicity reasons) on the __arm__ define.
 *
 * Some of the differences and how they are handled are listed here.
 * - ABC instead of AB prefix, other renames: map Mac OS X name to iPhone
 *   name before including AddressBook.h, then use Mac OS X names
 * - CFRelease() and CFCopyDescription on ABMultiValueRef crash (bugs?!):
 *   use ref<T, IPHONE_RELEASE> for those instead the normal ref smart pointer,
 *   avoid CFCopyDescription()
 * - UID is integer, not CFStringRef: added wrapper function
 * - the address of kABC*Property identifies properties, not the CFStringRef
 *   at that address, caused toolchain problems when initializing data
 *   with these addresses: added one additional address indirection
 * - UIDs are assigned to added contacts only during saving, but are needed
 *   earlier: save after adding each contact (affects performance and aborted
 *   sync changes address book - perhaps better guess UID?)
 * - Mac OS X 10.4 still uses the kABHomePageProperty (a single string),
 *   the iPhone switched to the more recent kABCURLProperty/kABURLsProperty:
 *   conversion code is slightly different
 * - iPhone does not have a title (e.g. "sir") property, only the job title
 * - label constants are not part of the framework:
 *   defined in AddressSourceConstants
 */
class AddressBookSource : public EvolutionSyncSource
{
    AddressBookSource(const EvolutionSyncSourceParams &params, bool asVCard30);
    AddressBookSource(const AddressBookSource &other);
    virtual ~AddressBookSource() { close(); }

    void setVCard30(bool asVCard30) { m_asVCard30 = asVCard30; }
    bool getVCard30() { return m_asVCard30; }


    //
    // implementation of EvolutionSyncSource
    //
    virtual sources getSyncBackends();
    virtual void open();
    virtual void close(); 
    virtual void exportData(ostream &out);
    virtual string fileSuffix() { return "vcf"; }
    virtual const char *getMimeType() { return m_asVCard30 ? "text/vcard" : "text/x-vcard"; }
    virtual const char *getMimeVersion() { return m_asVCard30 ? "3.0" : "2.1"; }
    virtual const char *getSupportedTypes() { return m_asVCard30 ? "text/vcard:3.0" : "text/x-vcard:2.1"; }
   
    virtual SyncItem *createItem(const string &uid) { return createItem(uid, m_asVCard30); }
    virtual SyncItem *createItem(const string &uid, bool asVCard30);
    
    //
    // implementation of SyncSource
    //
    virtual ArrayElement *clone() { return new AddressBookSource(*this); }
    
  protected:
    //
    // implementation of EvolutionSyncSource callbacks
    //
    virtual void beginSyncThrow(bool needAll,
                                bool needPartial,
                                bool deleteLocal);
    virtual void endSyncThrow();
    virtual int addItemThrow(SyncItem& item);
    virtual int updateItemThrow(SyncItem& item);
    virtual int deleteItemThrow(SyncItem& item);
    virtual void logItem(const string &uid, const string &info, bool debug = false);
    virtual void logItem(const SyncItem &item, const string &info, bool debug = false);

    /** insert item, optionally replacing the one with the specified uid */
    virtual int insertItem(SyncItem &item, const char *uid);

  private:
    /** valid after open(): the address book that this source references */
    ABAddressBookRef m_addressbook;

    /**
     * Stores the modification time of all items sent to or received from the server.
     * Items listed here and not in the current address book have been deleted.
     * More recent items were modified, new items are not listed here.
     *
     * The DeviceManagementNode must have the readProperties() and removeProperty()
     * functions. Currently that's only the case for the DeviceManagementNode included
     * with SyncEvolution, but not with the ones included in the C++ client library.
     */
    eptr<spdm::DeviceManagementNode> m_modTimes;

    /** the config path for the modification time node */
    string m_modNodeName;

    /** returns absolute modification time or (if that doesn't exist) the creation time */
    double getModTime(ABRecordRef record);

    /** unless selected otherwise send items as vCard 2.1 */
    bool m_asVCard30;
};

#endif // ENABLE_EBOOK

#endif // INCL_ADDRESSBOOKSOURCE
