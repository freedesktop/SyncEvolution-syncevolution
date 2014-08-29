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

#include <syncevo/TrackingSyncSource.h>
#include <syncevo/SafeConfigNode.h>
#include <syncevo/PrefixConfigNode.h>

#include <boost/bind.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

TrackingSyncSource::TrackingSyncSource(const SyncSourceParams &params,
                                       int granularitySeconds) :
    TestingSyncSource(params)
{
    boost::shared_ptr<ConfigNode> safeNode(new SafeConfigNode(params.m_nodes.getTrackingNode()));
    m_trackingNode.reset(new PrefixConfigNode("item-", safeNode));
    m_metaNode = safeNode;
    m_operations.m_checkStatus = boost::bind(&TrackingSyncSource::checkStatus, this, _1);
    m_operations.m_isEmpty = boost::bind(&TrackingSyncSource::isEmpty, this);
    SyncSourceRevisions::init(this, this, granularitySeconds, m_operations);
}

void TrackingSyncSource::checkStatus(SyncSourceReport &changes)
{
    // use the most reliable (and most expensive) method by default
    ChangeMode mode = CHANGES_FULL;

    // assume that we do a regular sync, with reusing stored information
    // if possible
    string oldRevision = m_metaNode->readProperty("databaseRevision");
    if (!oldRevision.empty()) {
        string newRevision = databaseRevision();
        SE_LOG_DEBUG(getDisplayName(), "old database revision '%s', new revision '%s'",
                     oldRevision.c_str(),
                     newRevision.c_str());
        if (newRevision == oldRevision) {
            SE_LOG_DEBUG(getDisplayName(), "revisions match, no item changes");
            mode = CHANGES_NONE;
        }
    }
    if (mode == CHANGES_FULL) {
        SE_LOG_DEBUG(getDisplayName(), "using full item scan to detect changes");
    }

    detectChanges(*m_trackingNode, mode);

    // copy our item counts into the report
    changes.setItemStat(ITEM_LOCAL, ITEM_ADDED, ITEM_TOTAL, getNewItems().size());
    changes.setItemStat(ITEM_LOCAL, ITEM_UPDATED, ITEM_TOTAL, getUpdatedItems().size());
    changes.setItemStat(ITEM_LOCAL, ITEM_REMOVED, ITEM_TOTAL, getDeletedItems().size());
    changes.setItemStat(ITEM_LOCAL, ITEM_ANY, ITEM_TOTAL, getAllItems().size());
}

void TrackingSyncSource::beginSync(const std::string &lastToken, const std::string &resumeToken)
{
    // use the most reliable (and most expensive) method by default
    ChangeMode mode = CHANGES_FULL;

    // resume token overrides the normal token; safe to ignore in most
    // cases and this detectChanges() is done independently of the
    // token, but let's do it right here anyway
    string token;
    if (!resumeToken.empty()) {
        token = resumeToken;
    } else {
        token = lastToken;
    }
    // slow sync if token is empty
    if (token.empty()) {
        SE_LOG_DEBUG(getDisplayName(), "slow sync or testing, do full item scan to detect changes");
        mode = CHANGES_SLOW;
    } else {
        string oldRevision = m_metaNode->readProperty("databaseRevision");
        if (!oldRevision.empty()) {
            string newRevision = databaseRevision();
            SE_LOG_DEBUG(getDisplayName(), "old database revision '%s', new revision '%s'",
                         oldRevision.c_str(),
                         newRevision.c_str());
            if (newRevision == oldRevision) {
                SE_LOG_DEBUG(getDisplayName(), "revisions match, no item changes");
                mode = CHANGES_NONE;
            }
        }
    }
    if (mode == CHANGES_FULL) {
        SE_LOG_DEBUG(getDisplayName(), "using full item scan to detect changes");
    }

    bool forceSlowSync = detectChanges(*m_trackingNode, mode);
    if (forceSlowSync) {
        // tell engine that we need a slow sync
        SE_THROW_EXCEPTION_STATUS(StatusException,
                                  "change detection incomplete, must do slow sync",
                                  STATUS_SLOW_SYNC_508);
    }
}

std::string TrackingSyncSource::endSync(bool success)
{
    // store changes persistently
    flush();

    if (success) {
        string updatedRevision = databaseRevision();
        m_metaNode->setProperty("databaseRevision", updatedRevision);
        // flush both nodes, just in case; in practice, the properties
        // end up in the same file and only get flushed once
        m_trackingNode->flush();
        m_metaNode->flush();
    } else {
        // The Synthesis docs say that we should rollback in case of
        // failure. Cannot do that for data, so lets at least keep
        // the revision map unchanged.
    }

    // no token handling at the moment (not needed for clients):
    // return a non-empty token to distinguish an incremental
    // sync from a slow sync in beginSync()
    return "1";
}

TrackingSyncSource::InsertItemResult TrackingSyncSource::continueInsertItem(const boost::function<InsertItemResult ()> &check, const std::string &luid)
{
    resetDatabaseRevision();
    InsertItemResult res = check();
    if (res.m_state == ITEM_AGAIN) {
        // Delay updating the revision.
        res.m_continue = InsertItemResult::Continue_t(boost::bind(&TrackingSyncSource::continueInsertItem, this,
                                                                  res.m_continue, luid));
    } else if (res.m_state != ITEM_NEEDS_MERGE) {
        updateRevision(*m_trackingNode, luid, res.m_luid, res.m_revision);
    }
    return res;
}

TrackingSyncSource::InsertItemResult TrackingSyncSource::doInsertItem(const std::string &luid, const std::string &item, bool raw)
{
    // insertItem() is overloaded, need to disambiguate here.
    return continueInsertItem(boost::bind(static_cast<InsertItemResult (TrackingSyncSource::*)(const std::string &luid, const std::string &item, bool raw)>(&TrackingSyncSource::insertItem),
                                          this, luid, item, raw),
                              luid);
}

TrackingSyncSource::InsertItemResult TrackingSyncSource::insertItem(const std::string &luid, const std::string &item)
{
    return doInsertItem(luid, item, false);
}

TrackingSyncSource::InsertItemResult TrackingSyncSource::insertItemRaw(const std::string &luid, const std::string &item)
{
    InsertItemResult res = doInsertItem(luid, item, true);
    while (res.m_state == ITEM_AGAIN) {
        // Flush and wait, because caller (command line, restore) is
        // not prepared to deal with asynchronous execution.
        flushItemChanges();
        finishItemChanges();
        res = res.m_continue();
    }
    return res;
}

void TrackingSyncSource::readItem(const std::string &luid, std::string &item)
{
    readItem(luid, item, false);
}

void TrackingSyncSource::readItemRaw(const std::string &luid, std::string &item)
{
    readItem(luid, item, true);
}

void TrackingSyncSource::resetDatabaseRevision()
{
    if (m_metaNode) {
        // Reset old revision. If anything goes wrong, then we
        // don't want to rely on a possibly incorrect optimization.
        m_metaNode->setProperty("databaseRevision", "");
        m_metaNode->flush();
    }
}

void TrackingSyncSource::deleteItem(const std::string &luid)
{
    resetDatabaseRevision();
    removeItem(luid);
    deleteRevision(*m_trackingNode, luid);
}

void TrackingSyncSource::enableServerMode()
{
    SyncSourceAdmin::init(m_operations, this);
    SyncSourceBlob::init(m_operations, getCacheDir());
}

bool TrackingSyncSource::serverModeEnabled() const
{
    return m_operations.m_loadAdminData;
}

std::string TrackingSyncSource::getPeerMimeType() const
{
    return getMimeType();
}

SE_END_CXX
