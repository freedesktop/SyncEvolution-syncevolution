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

#include "TrackingSyncSource.h"
#include "SafeConfigNode.h"

#include <ctype.h>

TrackingSyncSource::TrackingSyncSource(const EvolutionSyncSourceParams &params) :
    EvolutionSyncSource(params),
    m_trackingNode(new SafeConfigNode(params.m_nodes.m_trackingNode))
{
}

void TrackingSyncSource::beginSyncThrow(bool needAll,
                                       bool needPartial,
                                       bool deleteLocal)
{
    RevisionMap_t revisions;
    listAllItems(revisions);

    // slow sync or refresh-from-server/client: clear tracking node and
    // recreate it based on current content of database
    if (!needPartial) {
        map<string, string> props = m_trackingNode->readProperties();

        for (map<string, string>::iterator it = props.begin();
             it != props.end();
             it++) {
            const string &uid(it->first);
            m_deletedItems.addItem(uid.c_str());
            m_trackingNode->removeProperty(uid);
        }
    }

    for (RevisionMap_t::const_iterator it = revisions.begin();
         it != revisions.end();
         it++) {
        const string &uid = it->first;
        const string &revision = it->second;

        if (deleteLocal) {
            deleteItem(uid);
        } else {
            // always remember the item, need full list to find deleted items
            m_allItems.addItem(uid);

            if (needPartial) {
                string serverRevision(m_trackingNode->readProperty(uid));

                if (!serverRevision.size()) {
                    m_newItems.addItem(uid);
                    m_trackingNode->setProperty(uid, revision);
                } else {
                    if (revision != serverRevision) {
                        m_updatedItems.addItem(uid);
                        m_trackingNode->setProperty(uid, revision);
                    }
                }
            } else {
                // refresh-from-client: make sure that all items we are about
                // to send to server are also in our tracking node (otherwise
                // the next incremental sync will go wrong)
                m_trackingNode->setProperty(uid, revision);
            }
        }
    }

    // clear information about all items that we recognized as deleted
    if (needPartial) {
        map<string, string> props = m_trackingNode->readProperties();

        for (map<string, string>::iterator it = props.begin();
             it != props.end();
             it++) {
            const string &uid(it->first);
            if (m_allItems.find(uid) == m_allItems.end()) {
                m_deletedItems.addItem(uid.c_str());
                m_trackingNode->removeProperty(uid);
            }
        }
    }
            
    if (!needAll) {
        // did not need full list after all...
        m_allItems.clear();
    }
}

void TrackingSyncSource::endSyncThrow()
{
    // store changes persistently
    flush();

    if (!hasFailed()) {
        m_trackingNode->flush();
    } else {
        // SyncEvolution's error handling for failed sources
        // forces a slow sync the next time. Therefore the
        // content of the tracking node is irrelevant in
        // case of a failure.
    }
}

void TrackingSyncSource::exportData(ostream &out)
{
    RevisionMap_t revisions;
    listAllItems(revisions);

    for (RevisionMap_t::const_iterator it = revisions.begin();
         it != revisions.end();
         it++) {
        const string &uid = it->first;
        cxxptr<SyncItem> item(createItem(uid), "sync item");

        out << (char *)item->getData() << "\n";
    }
}

int TrackingSyncSource::addItemThrow(SyncItem& item)
{
    string uid;
    bool merged = false;
    string revision = insertItem(uid, item, merged);
    item.setKey(uid.c_str());
    m_trackingNode->setProperty(uid, revision);
    return merged ? STC_CONFLICT_RESOLVED_WITH_MERGE : STC_OK;
}

int TrackingSyncSource::updateItemThrow(SyncItem& item)
{
    const string olduid = item.getKey();
    string newuid = olduid;
    bool merged = false;
    string revision = insertItem(newuid, item, merged);
    if (olduid != newuid) {
        m_trackingNode->removeProperty(olduid);
    }
    item.setKey(newuid.c_str());
    m_trackingNode->setProperty(newuid, revision);
    return merged ? STC_CONFLICT_RESOLVED_WITH_MERGE : STC_OK;
}

int TrackingSyncSource::deleteItemThrow(SyncItem& item)
{
    const string uid = item.getKey();
    deleteItem(uid);
    m_trackingNode->removeProperty(uid);
    return STC_OK;
}

void TrackingSyncSource::setItemStatusThrow(const char *uid, int status)
{
}
