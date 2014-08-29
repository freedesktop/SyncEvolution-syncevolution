/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#ifndef INCL_TRACKINGSYNCSOURCE
#define INCL_TRACKINGSYNCSOURCE

#include <syncevo/SyncSource.h>
#include <syncevo/ConfigNode.h>

#include <boost/shared_ptr.hpp>
#include <string>
#include <map>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * This class implements change tracking. Data sources which want to use
 * this functionality have to provide the following functionality
 * by implementing the pure virtual functions below:
 * - open() the data
 * - enumerate all existing items
 * - provide LUID and "revision string": 
 *   The LUID must remain *constant* when the user edits an item (it
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
class TrackingSyncSource : public TestingSyncSource,
    virtual public SyncSourceRevisions,
    virtual public SyncSourceBlob,
    virtual public SyncSourceAdmin
{
  public:
    /**
     * Creates a new tracking sync source.
     *
     * @param granularity    sync sources whose revision string
     *                       is based on time should specify the number
     *                       of seconds which has to pass before changes
     *                       are detected reliably (see SyncSourceRevisions
     *                       for details), otherwise pass 0
     */
    TrackingSyncSource(const SyncSourceParams &params,
                       int granularitySeconds = 1);
    ~TrackingSyncSource() {}

    /**
     * ConfigNode used for change tracking in SyncSourceRevisions.
     * Derived classes might need that when implementing operations
     * which have side effects on other items (for example,
     * EvolutionCalendarSource::removeItem()).
     */
    ConfigNode &getTrackingNode() { return *m_trackingNode; }

    /**
     * returns a list of all know sources for the kind of items
     * supported by this sync source
     */
    virtual Databases getDatabases() = 0;

    /**
     * Actually opens the data source specified in the constructor,
     * will throw the normal exceptions if that fails. Should
     * not modify the state of the sync source.
     *
     * The expectation is that this call is fairly light-weight, but
     * does enough checking to determine whether the source is
     * usable. More expensive operations (like determining changes)
     * should be done in the beginSync() callback.
     *
     * In clients, it will be called for all sources before
     * the sync starts. In servers, it is called for each source once
     * the client asks for it, but not sooner.
     */
    virtual void open() = 0;

    /**
     * A quick check whether the source currently has data. Currently
     * used as part of the "allow slow sync" checking after open() and
     * before beginSync(). Returning false is acceptable when it is
     * uncertain and too expensive to check.
     */
    virtual bool isEmpty() = 0;

    /**
     * A unique identifier for the current state of the complete database.
     * The semantic is the following:
     * - empty string implies "state unknown" or "identifier not supported" (the default implementation)
     * - id not empty and id_1 == id_2 implies "nothing has changed";
     *   the inverse is not true (ids may be different although nothing has changed)
     */
    virtual std::string databaseRevision() { return ""; }

    /**
     * fills the complete mapping from LUID to revision string of all
     * currently existing items
     *
     * Usually both LUID and revision string must be non-empty. The
     * only exception is a refresh-from-client: in that case the
     * revision string may be empty. The implementor of this call
     * cannot know whether empty strings are allowed, therefore it
     * should not throw errors when it cannot create a non-empty
     * string. The caller of this method will detect situations where
     * a non-empty string is necessary and none was provided.
     */
    virtual void listAllItems(SyncSourceRevisions::RevisionMap_t &revisions) = 0;

    /**
     * Called at the start of the sync session to tell
     * the derived class about the cached information if (and only
     * if) listAllItems() and updateAllItems() were not called. The derived class
     * might not need this information, so the default implementation
     * simply ignores.
     *
     * A more complex API could have been defined to only prepare the
     * information when needed, but that seemed unnecessarily complex.
     */
    virtual void setAllItems(const RevisionMap_t &revisions) {}

    /**
     * updates the revision map to reflect the current state
     *
     * May be called instead of listAllItems() if the caller has
     * a valid list to start from. If the implementor
     * cannot update the list, it must start from scratch by
     * reseting the list and calling listAllItems(). The default
     * implementation of this method does that.
     */
    virtual void updateAllItems(SyncSourceRevisions::RevisionMap_t &revisions) {
        revisions.clear();
        listAllItems(revisions);
    }

    /**
     * Create or modify an item.
     *
     * The sync source should be flexible: if the LUID is non-empty, it
     * shall modify the item referenced by the LUID. If the LUID is
     * empty, the normal operation is to add it. But if the item
     * already exists (e.g., a calendar event which was imported
     * by the user manually), then the existing item should be
     * updated also in the second case.
     *
     * Passing a LUID of an item which does not exist is an error.
     * This error should be reported instead of covering it up by
     * (re)creating the item.
     *
     * Errors are signaled by throwing an exception. Returning empty
     * strings in the result is an error which triggers an "item could
     * not be stored" error.
     *
     * @param luid     identifies the item to be modified, empty for creating
     * @param item     contains the new content of the item
     * @param raw      item has internal format instead of engine format;
     *                 testing and backup/restore might use such an internal format
     *                 which may be different (more complete!) than the
     *                 format when talking to the sync engine
     * @return the result of inserting the item
     */
    virtual InsertItemResult insertItem(const std::string &luid, const std::string &item, bool raw) = 0;

    /**
     * Return item data in engine format.
     *
     * Must throw a STATUS_NOT_FOUND (= 404) StatusException when the
     * item does not exist.
     *
     * @param luid     identifies the item
     * @param raw      return item in internal format instead of engine format
     * @retval item    item data
     */
    virtual void readItem(const std::string &luid, std::string &item, bool raw) = 0;

    /**
     * delete the item (renamed so that it can be wrapped by deleteItem())
     *
     * Must throw a STATUS_NOT_FOUND (= 404) StatusException when the
     * item does not exist.
     */
    virtual void removeItem(const string &luid) = 0;

    /**
     * optional: write all changes, throw error if that fails
     *
     * This is called while the sync is still active whereas
     * close() is called afterwards. Reporting problems
     * as early as possible may be useful at some point,
     * but currently doesn't make a relevant difference.
     */
    virtual void flush() {}
    
    /**
     * closes the data source so that it can be reopened
     *
     * Just as open() it should not affect the state of
     * the database unless some previous action requires
     * it.
     */
    virtual void close() = 0;

    /**
     * Returns the preferred mime type of the items handled by the sync source.
     * Example: "text/x-vcard"
     */
    virtual std::string getMimeType() const = 0;

    /**
     * Returns the version of the mime type used by client.
     * Example: "2.1"
     */
    virtual std::string getMimeVersion() const = 0;

    using SyncSource::getName;

  private:
    void checkStatus(SyncSourceReport &changes);
    boost::shared_ptr<ConfigNode> m_trackingNode;

    /**
     * Stores meta information besides the item list:
     * - "databaseRevision" = result of databaseRevision() at end of last sync
     *
     * Shares the same key/value store as m_trackingNode,
     * which uses the "item-" prefix in its keys to
     * avoid name clashes.
     */
    boost::shared_ptr<ConfigNode> m_metaNode;

 protected:
    /* implementations of SyncSource callbacks */
    virtual void beginSync(const std::string &lastToken, const std::string &resumeToken);
    virtual std::string endSync(bool success);
    virtual void deleteItem(const string &luid);
    virtual InsertItemResult insertItem(const std::string &luid, const std::string &item);
    virtual void readItem(const std::string &luid, std::string &item);
    virtual InsertItemResult insertItemRaw(const std::string &luid, const std::string &item);
    virtual void readItemRaw(const std::string &luid, std::string &item);
    virtual void enableServerMode();
    virtual bool serverModeEnabled() const;
    virtual std::string getPeerMimeType() const;

 private:
    InsertItemResult doInsertItem(const std::string &luid, const std::string &item, bool raw);
    InsertItemResult continueInsertItem(const boost::function<InsertItemResult ()> &check, const std::string &luid);
    void resetDatabaseRevision();
};


SE_END_CXX
#endif // INCL_TRACKINGSYNCSOURCE
