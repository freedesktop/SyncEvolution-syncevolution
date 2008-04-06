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

#include "EvolutionSyncSource.h"
#include "EvolutionSyncClient.h"
#include "SyncEvolutionUtil.h"
#include <common/base/Log.h>

#include <list>
using namespace std;

#include <dlfcn.h>

#ifdef HAVE_EDS
ESource *EvolutionSyncSource::findSource( ESourceList *list, const string &id )
{
    for (GSList *g = e_source_list_peek_groups (list); g; g = g->next) {
        ESourceGroup *group = E_SOURCE_GROUP (g->data);
        GSList *s;
        for (s = e_source_group_peek_sources (group); s; s = s->next) {
            ESource *source = E_SOURCE (s->data);
            char *uri = e_source_get_uri(source);
            bool found = id.empty() ||
                !id.compare(e_source_peek_name(source)) ||
                (uri && !id.compare(uri));
            g_free(uri);
            if (found) {
                return source;
            }
        }
    }
    return NULL;
}
#endif // HAVE_EDS

void EvolutionSyncSource::throwError( const string &action
#ifdef HAVE_EDS
, GError *gerror
#endif
                                      )
{
    string gerrorstr;
#ifdef HAVE_EDS
    if (gerror) {
        gerrorstr += ": ";
        gerrorstr += gerror->message;
        g_clear_error(&gerror);
    } else
#endif
        gerrorstr = ": failure";

    EvolutionSyncClient::throwError(string(getName()) + ": " + action + gerrorstr);
}


void EvolutionSyncSource::resetItems()
{
    m_allItems.clear();
    m_newItems.clear();
    m_updatedItems.clear();
    m_deletedItems.clear();
}

string EvolutionSyncSource::getPropertyValue(ManagementNode &node, const string &property)
{
    char *value = node.readPropertyValue(property.c_str());
    string res;

    if (value) {
        res = value;
        delete [] value;
    }

    return res;
}

void EvolutionSyncSource::handleException()
{
    try {
        throw;
    } catch (std::exception &ex) {
        setErrorF(getLastErrorCode() == ERR_NONE ? ERR_UNSPECIFIED : getLastErrorCode(),
                  "%s", ex.what());
        LOG.error("%s", getLastErrorMsg());
    }
}

SourceRegistry &EvolutionSyncSource::getSourceRegistry()
{
    static SourceRegistry sourceRegistry;
    return sourceRegistry;
}

RegisterSyncSource::RegisterSyncSource(const string &shortDescr,
                                       bool enabled,
                                       Create_t create,
                                       const string &typeDescr,
                                       const Values &typeValues) :
    m_shortDescr(shortDescr),
    m_enabled(enabled),
    m_create(create),
    m_typeDescr(typeDescr),
    m_typeValues(typeValues)
{
    SourceRegistry &registry(EvolutionSyncSource::getSourceRegistry());

    // insert sorted by description to have deterministic ordering
    for (SourceRegistry::iterator it = registry.begin();
         it != registry.end();
         ++it) {
        if ((*it)->m_shortDescr > shortDescr) {
            registry.insert(it, this);
            return;
        }
    }
    registry.push_back(this);
}

#if 0
static ostream & operator << (ostream &out, const RegisterSyncSource &rhs)
{
    out << rhs.m_shortDescr << (rhs.m_enabled ? " (enabled)" : " (disabled)");
}
#endif

EvolutionSyncSource *const RegisterSyncSource::InactiveSource = (EvolutionSyncSource *)1;

static class ScannedModules {
public:
    ScannedModules() {
#ifdef ENABLE_MODULES
        list<string> *state;

        // possible extension: scan directories for matching module names instead of hard-coding known names
        const char *modules[] = {
            "syncebook.so.0",
            "syncecal.so.0",
            "syncsqlite.so.0",
            "addressbook.so.0",
            NULL
        };

        for (int i = 0; modules[i]; i++) {
            void *dlhandle;

            // Open the shared object so that backend can register
            // itself. We keep that pointer, so never close the
            // module!
            dlhandle = dlopen(modules[i], RTLD_NOW|RTLD_GLOBAL);
            // remember which modules were found and which were not
            state = dlhandle ? &m_available : &m_missing;
            state->push_back(modules[i]);
        }
#endif
    }
    list<string> m_available;
    list<string> m_missing;
} scannedModules;


EvolutionSyncSource *EvolutionSyncSource::createSource(const EvolutionSyncSourceParams &params, bool error)
{
    string sourceTypeString = getSourceTypeString(params.m_nodes);
    pair<string, string> sourceType = getSourceType(params.m_nodes);

    const SourceRegistry &registry(getSourceRegistry());
    for (SourceRegistry::const_iterator it = registry.begin();
         it != registry.end();
         ++it) {
        EvolutionSyncSource *source = (*it)->m_create(params);
        if (source) {
            if (source == RegisterSyncSource::InactiveSource) {
                EvolutionSyncClient::throwError(params.m_name + ": access to " + (*it)->m_shortDescr +
                                                " not enabled, therefore type = " + sourceTypeString + " not supported");
            }
            return source;
        }
    }

    if (error) {
        string problem = params.m_name + ": type '" + sourceTypeString + "' not supported";
        if (scannedModules.m_available.size()) {
            problem += " by any of the backends (";
            problem += join(string(", "), scannedModules.m_available.begin(), scannedModules.m_available.end());
            problem += ")";
        }
        if (scannedModules.m_missing.size()) {
            problem += ". The following backend(s) were not found: ";
            problem += join(string(", "), scannedModules.m_missing.begin(), scannedModules.m_missing.end());
        }
        EvolutionSyncClient::throwError(problem);
    }

    return NULL;
}

EvolutionSyncSource *EvolutionSyncSource::createTestingSource(const string &name, const string &type, bool error)
{
    EvolutionSyncConfig config("testing");
    SyncSourceNodes nodes = config.getSyncSourceNodes(name);
    EvolutionSyncSourceParams params(name, nodes, "");
    PersistentEvolutionSyncSourceConfig sourceconfig(name, nodes);
    sourceconfig.setSourceType(type);
    return createSource(params, error);
}

int EvolutionSyncSource::beginSync()
{
    string buffer;
    buffer += getName();
    buffer += ": sync mode is ";
    SyncMode mode = getSyncMode();
    buffer += mode == SYNC_SLOW ? "'slow'" :
        mode == SYNC_TWO_WAY ? "'two-way'" :
        mode == SYNC_REFRESH_FROM_SERVER ? "'refresh from server'" :
        mode == SYNC_REFRESH_FROM_CLIENT ? "'refresh from client'" :
        mode == SYNC_ONE_WAY_FROM_SERVER ? "'one-way from server'" :
        mode == SYNC_ONE_WAY_FROM_CLIENT ? "'one-way from client'" :
        mode == SYNC_NONE ? "'none' (for debugging)" :
        "???";
    LOG.info( buffer.c_str() );

    // start background thread if not running yet:
    // necessary to catch problems with Evolution backend
    EvolutionSyncClient::startLoopThread();

    try {
        // reset anchors now, once we proceed there is no going back
        // (because the change marker is about to be moved)
        // and the sync must either complete or result in a slow sync
        // the next time
        getConfig().setLast(0);
        
        const char *error = getenv("SYNCEVOLUTION_BEGIN_SYNC_ERROR");
        if (error && strstr(error, getName())) {
            EvolutionSyncClient::throwError("artificial error in beginSync()");
        }

        // reset state
        m_isModified = false;
        m_allItems.clear();
        m_newItems.clear();
        m_updatedItems.clear();
        m_deletedItems.clear();

        // determine what to do
        bool needAll = false;
        bool needPartial = false;
        bool deleteLocal = false;
        switch (mode) {
         case SYNC_SLOW:
            needAll = true;
            m_isModified = true;
            break;
         case SYNC_ONE_WAY_FROM_CLIENT:
         case SYNC_TWO_WAY:
            needPartial = true;
            break;
         case SYNC_REFRESH_FROM_SERVER:
            deleteLocal = true;
            m_isModified = true;
            break;
         case SYNC_REFRESH_FROM_CLIENT:
            needAll = true;
            m_isModified = true;
            break;
         case SYNC_NONE:
            // special mode for testing: prepare both all and partial lists
            needAll = needPartial = true;
            break;
         case SYNC_ONE_WAY_FROM_SERVER:
            // nothing to do, just wait for server's changes
            break;
         default:
            EvolutionSyncClient::throwError("unsupported sync mode, valid are only: slow, two-way, refresh");
            break;
        }

        beginSyncThrow(needAll, needPartial, deleteLocal);
    } catch( ... ) {
        handleException();
        setFailed(true);
        return 1;
    }
    return 0;
}

int EvolutionSyncSource::endSync()
{
    try {
        endSyncThrow();
    } catch ( ... ) {
        handleException();
        setFailed(true);
    }

    // Do _not_ tell the caller (SyncManager) if an error occurred
    // because that causes Sync4jClient to abort processing for all
    // sync sources. Instead deal with failed sync sources in
    // EvolutionSyncClient::sync().
    return 0;
}

void EvolutionSyncSource::setItemStatus(const char *key, int status)
{
    try {
        // TODO: logging
        setItemStatusThrow(key, status);
    } catch (...) {
        handleException();
        setFailed(true);
    }
}

int EvolutionSyncSource::addItem(SyncItem& item)
{
    return processItem("add", &EvolutionSyncSource::addItemThrow, item, true);
}

int EvolutionSyncSource::updateItem(SyncItem& item)
{
    return processItem("update", &EvolutionSyncSource::updateItemThrow, item, true);
}

int EvolutionSyncSource::deleteItem(SyncItem& item)
{
    return processItem("delete", &EvolutionSyncSource::deleteItemThrow, item, false);
}

int EvolutionSyncSource::processItem(const char *action,
                                     int (EvolutionSyncSource::*func)(SyncItem& item),
                                     SyncItem& item,
                                     bool needData)
{
    int status = STC_COMMAND_FAILED;
    
    try {
        logItem(item, action);
        if (needData && (item.getDataSize() < 0 || !item.getData())) {
            // Something went wrong in the server: update or add without data.
            // Shouldn't happen, but it did with one server and thus this
            // security check was added to prevent segfaults.
            logItem(item, "ignored due to missing data");
            status = STC_OK;
        } else {
            status = (this->*func)(item);
        }
        m_isModified = true;
    } catch (...) {
        handleException();
        setFailed(true);
    }
    return status;
}

void EvolutionSyncSource::setItemStatusThrow(const char *key, int status)
{
    switch (status) {
     case STC_ALREADY_EXISTS:
        // found pair during slow sync, that's okay
        break;
     default:
        if (status < 200 || status > 300) {
            LOG.error("%s: unexpected SyncML status response %d for item %.80s\n",
                      getName(), status, key);
            setFailed(true);
        }
    }
}
