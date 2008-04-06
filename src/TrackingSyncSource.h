/*
 * Copyright (C) 2008 Patrick Ohly
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

#ifndef INCL_TRACKINGSYNCSOURCE
#define INCL_TRACKINGSYNCSOURCE

#include "EvolutionSyncSource.h"
#include "ConfigNode.h"

#include <boost/shared_ptr.hpp>
#include <string>
#include <map>
using namespace std;

/**
 * This class implements change tracking. Data sources which want to use
 * this functionality have to provide the following functionality
 * by implementing the pure virtual functions below:
 * - open() the data
 * - enumerate all existing items
 * - provide UID and "revision string": 
 *   The UID must remain *constant* when the user edits an item (it
 *   may change when SyncEvolution changes an item), whereas the
 *   revision string must *change* each time the item is changed
 *   by anyone.
 *   Both can be arbitrary strings, but keeping them simple (printable
 *   ASCII, no white spaces, no equal sign) makes debugging simpler
 *   because they can be stored as they are as key/value pairs in the
 *   sync source's change tracking config node (the .other.ini files when
 *   using file-based configuration). More complex strings use escape
 *   sequences introduced with an exclamation mark for unsafe characters.
 * - import/export/update single items
 * - persistently store all changes in flush()
 * - clean up in close()
 *
 * A derived class may (but doesn't have to) override additional
 * functions to modify or replace the default implementations, e.g.:
 * - dumping the complete database (export())
 *
 * Potential implementations of the revision string are:
 * - a modification time stamp
 * - a hash value of a textual representation of the item
 *   (beware, such a hash might change as the textual representation
 *    changes even though the item is unchanged)
 */
class TrackingSyncSource : public EvolutionSyncSource
{
  public:
    /**
     * Creates a new tracking sync source.
     */
    TrackingSyncSource(const EvolutionSyncSourceParams &params);

    /**
     * returns a list of all know sources for the kind of items
     * supported by this sync source
     */
    virtual sources getSyncBackends() = 0;

    /**
     * Actually opens the data source specified in the constructor,
     * will throw the normal exceptions if that fails. Should
     * not modify the state of the sync source: that can be deferred
     * until the server is also ready and beginSync() is called.
     */
    virtual void open() = 0;

    /**
     * exports all items one after the other, separated by blank line;
     * if that format is not suitable, then the derived class must
     * override this call
     */
    virtual void exportData(ostream &out);

    typedef map<string, string> RevisionMap_t;

    /**
     * fills the complete mapping from UID to revision string of all
     * currently existing items
     */
    virtual void listAllItems(RevisionMap_t &revisions) = 0;

    /**
     * Create or modify an item.
     *
     * The sync source should be flexible: if the UID is non-empty, it
     * shall modify the item referenced by the UID. If the UID is
     * empty, but the item already exists (e.g., a calendar event
     * which was imported by the user manually), then the existing
     * item should also be updated.
     *
     * Passing a UID of an item which does not exist is an error.
     * This error should be reported instead of covering it up by
     * (re)creating the item.
     *
     * Errors are signalled by throwing an exception. 
     *
     * @param uid      in: identifies the item to be modified, empty for creating;
     *                 out: UID after the operation
     * @param item     contains the new content of the item and its MIME type
     * @retval merged  set this to true if an existing item was updated instead of adding it;
     *                 guaranteed to be initialized to false before the call
     * @return the new revision string
     */
    virtual string insertItem(string &uid, const SyncItem &item, bool &merged) = 0;

    /**
     * Extract information for the item identified by UID
     * and store it in a new SyncItem. The caller must
     * free that item. May throw exceptions.
     *
     * @param uid      identifies the item
     */
    virtual SyncItem *createItem(const string &uid) = 0;

    /**
     * removes and item
     */
    virtual void deleteItem(const string &uid) = 0;

    /**
     * write all changes, throw error if that fails
     */
    virtual void flush() = 0;
    
    /**
     * closes the data source so that it can be reopened
     *
     * Just as open() it should not affect the state of
     * the database unless some previous action requires
     * it.
     */
    virtual void close() = 0;

    /**
     * file suffix for database files
     */
    virtual string fileSuffix() = 0;

    /**
     * Returns the preferred mime type of the items handled by the sync source.
     * Example: "text/x-vcard"
     */
    virtual const char *getMimeType() const = 0;

    /**
     * Returns the version of the mime type used by client.
     * Example: "2.1"
     */
    virtual const char *getMimeVersion() const = 0;

    /**
     * A string representing the source types (with versions) supported by the SyncSource.
     * The string must be formatted as a sequence of "type:version" separated by commas ','.
     * For example: "text/x-vcard:2.1,text/vcard:3.0".
     * The version can be left empty, for example: "text/x-s4j-sifc:".
     * Supported types will be sent as part of the DevInf.
     */
    virtual const char* getSupportedTypes() const = 0;

 protected:
    /** log a one-line info about an item */
    virtual void logItem(const string &uid, const string &info, bool debug = false) = 0;
    virtual void logItem(const SyncItem &item, const string &info, bool debug = false) = 0;

    virtual void setItemStatusThrow(const char *key, int status);

  private:
    /* implementations of EvolutionSyncSource callbacks */
    virtual void beginSyncThrow(bool needAll,
                                bool needPartial,
                                bool deleteLocal);
    virtual void endSyncThrow();
    virtual int addItemThrow(SyncItem& item);
    virtual int updateItemThrow(SyncItem& item);
    virtual int deleteItemThrow(SyncItem& item);

    /** cannot be cloned because clones would have to coordinate access to change tracking */
    ArrayElement *clone() { return NULL; }

    boost::shared_ptr<ConfigNode> m_trackingNode;
};

#endif // INCL_TRACKINGSYNCSOURCE
