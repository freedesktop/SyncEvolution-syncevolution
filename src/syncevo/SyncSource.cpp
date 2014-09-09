/*
 * Copyright (C) 2005-2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#ifndef _GNU_SOURCE
# define _GNU_SOURCE 1
#endif
#include <dlfcn.h>

#include <syncevo/SyncSource.h>
#include <syncevo/SyncContext.h>
#include <syncevo/util.h>
#include <syncevo/SuspendFlags.h>

#include <syncevo/SynthesisEngine.h>
#include <synthesis/SDK_util.h>
#include <synthesis/sync_dbapidef.h>

#include <boost/bind.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/lambda/lambda.hpp>

#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#include <fstream>
#include <iostream>

#ifdef ENABLE_UNIT_TESTS
#include "test.h"
#endif

#include <syncevo/declarations.h>
SE_BEGIN_CXX

void SyncSourceBase::throwError(const SourceLocation &where, const string &action, int error)
{
    std::string what = action + ": " + strerror(error);
    // be as specific if we can be: relevant for the file backend,
    // which is expected to return STATUS_NOT_FOUND == 404 for "file
    // not found"
    if (error == ENOENT) {
        throwError(where, STATUS_NOT_FOUND, what);
    } else {
        throwError(where, what);
    }
}

void SyncSourceBase::throwError(const SourceLocation &where, const string &failure)
{
    Exception::throwError(where, string(getDisplayName()) + ": " + failure);
}

void SyncSourceBase::throwError(const SourceLocation &where, SyncMLStatus status, const string &failure)
{
    Exception::throwError(where, status, getDisplayName() + ": " + failure);
}

SyncMLStatus SyncSourceBase::handleException(HandleExceptionFlags flags)
{
    SyncMLStatus res = Exception::handle(getDisplayName(), flags);
    return res == STATUS_FATAL ?
        STATUS_DATASTORE_FAILURE :
        res;
}

void SyncSourceBase::getDatastoreXML(string &xml, XMLConfigFragments &fragments)
{
    stringstream xmlstream;
    SynthesisInfo info;

    getSynthesisInfo(info, fragments);

    xmlstream <<
        "      <plugin_module>SyncEvolution</plugin_module>\n";
    if (info.m_earlyStartDataRead) {
        xmlstream <<
            "      <plugin_earlystartdataread>yes</plugin_earlystartdataread>\n";
    }
    if (info.m_readOnly) {
         xmlstream <<
             "      <!-- if this is set to 'yes', SyncML clients can only read\n"
             "           from the database, but make no modifications -->\n"
             "      <readonly>yes</readonly>\n";
    }
    xmlstream <<
        "      <plugin_datastoreadmin>" <<
        (serverModeEnabled() ? "yes" : "no") <<
        "</plugin_datastoreadmin>\n"
        "      <fromremoteonlysupport> yes </fromremoteonlysupport>\n"
        "      <canrestart>yes</canrestart>\n";
    if (info.m_globalIDs) {
        xmlstream << 
            "      <syncmode>1122583000</syncmode>";
    }
    xmlstream <<
        "\n"
        "      <!-- conflict strategy: Newer item wins\n"
        "           You can set 'server-wins' or 'client-wins' as well\n"
        "           if you want to give one side precedence\n"
        "      -->\n"
        "      <conflictstrategy>newer-wins</conflictstrategy>\n"
        "\n"
        "      <!-- on slowsync: do not duplicate items even if not fully equal\n"
        "           You can set this to 'duplicate' to avoid possible data loss\n"
        "           resulting from merging\n"
        "      -->\n"
        "      <slowsyncstrategy>newer-wins</slowsyncstrategy>\n"
        "\n"
        "      <!-- text db plugin is designed for UTF-8, make sure data is passed as UTF-8 (and not the ISO-8859-1 default) -->\n"
        "      <datacharset>UTF-8</datacharset>\n"
        "      <!-- use C-language (unix style) linefeeds (\n, 0x0A) -->\n"
        "      <datalineends>unix</datalineends>\n"
        "\n"
        "      <!-- set this to 'UTC' if time values should be stored in UTC into the database\n"
        "           rather than local time. 'SYSTEM' denotes local server time zone. -->\n"
        "      <datatimezone>SYSTEM</datatimezone>\n"
        "\n"
        "      <!-- plugin DB may have its own identifiers to determine the point in time of changes, so\n"
        "           we must make sure this identifier is stored (and not only the sync time) -->\n"
        "      <storesyncidentifiers>yes</storesyncidentifiers>\n"
        "\n";
    
    xmlstream <<
        "      <!-- Mapping of the fields to the fieldlist -->\n"
        "      <fieldmap fieldlist='" << info.m_fieldlist << "'>\n";
    if (!info.m_profile.empty()) {
        xmlstream <<
            "        <initscript><![CDATA[\n"
            "           string itemdata;\n"
            "        ]]></initscript>\n"
            "        <beforewritescript><![CDATA[\n";
        if(!info.m_beforeWriteScript.empty()) {
            xmlstream << 
                "           " << info.m_beforeWriteScript << "\n";
        }
        xmlstream <<
            "           itemdata = MAKETEXTWITHPROFILE(" << info.m_profile << ", \"" << info.m_backendRule << "\");\n"
            "        ]]></beforewritescript>\n"
            "        <afterreadscript><![CDATA[\n"
            "           PARSETEXTWITHPROFILE(itemdata, " << info.m_profile << ", \"" << info.m_backendRule << "\");\n";
        if(!info.m_afterReadScript.empty()) {
            xmlstream << 
                "           " << info.m_afterReadScript<< "\n";
        }
        xmlstream <<
            "        ]]></afterreadscript>\n"
            "        <map name='data' references='itemdata' type='string'/>\n";
    }
    xmlstream << 
        "        <automap/>\n"
        "      </fieldmap>\n"
        "\n";

    xmlstream <<
        "      <!-- datatypes supported by this datastore -->\n"
        "      <typesupport>\n" <<
        info.m_datatypes <<
        "      </typesupport>\n";

    // arbitrary configuration options, can override the ones above
    xmlstream << info.m_datastoreOptions;

    xml = xmlstream.str();
}

string SyncSourceBase::getNativeDatatypeName()
{
    SynthesisInfo info;
    XMLConfigFragments fragments;
    getSynthesisInfo(info, fragments);
    return info.m_native;
}

SyncSourceBase::Operations::Operations(SyncSourceName &source) :
    m_startDataRead(source),
    m_endDataRead(source),
    m_startDataWrite(source),
    m_finalizeLocalID(source),
    m_endDataWrite(source),
    m_readNextItem(source),
    m_readItemAsKey(source),
    m_insertItemAsKey(source),
    m_updateItemAsKey(source),
    m_deleteItem(source),
    m_loadAdminData(source),
    m_saveAdminData(source),
    m_insertMapItem(source),
    m_updateMapItem(source),
    m_deleteMapItem(source),
    m_deleteBlob(source)
{
}

static SyncMLStatus BumpCounter(int32_t &counter)
{
    counter++;
    return STATUS_OK;
}

SyncSource::SyncSource(const SyncSourceParams &params) :
    SyncSourceConfig(params.m_name, params.m_nodes),
    m_operations(*this),
    m_numDeleted(0),
    m_added(0),
    m_updated(0),
    m_deleted(0),
    m_forceSlowSync(false),
    m_database("", ""),
    m_name(params.getDisplayName()),
    m_needChanges(true)
{
    m_operations.m_insertItemAsKey.getPreSignal().connect(boost::bind(BumpCounter, boost::ref(m_added)));
    m_operations.m_updateItemAsKey.getPreSignal().connect(boost::bind(BumpCounter, boost::ref(m_updated)));
    m_operations.m_deleteItem.getPreSignal().connect(boost::bind(BumpCounter, boost::ref(m_deleted)));
}

SDKInterface *SyncSource::getSynthesisAPI() const
{
    return m_synthesisAPI.empty() ?
        NULL :
        static_cast<SDKInterface *>(m_synthesisAPI[m_synthesisAPI.size() - 1]);
}

void SyncSource::pushSynthesisAPI(sysync::SDK_InterfaceType *synthesisAPI)
{
    m_synthesisAPI.push_back(synthesisAPI);
}

void SyncSource::popSynthesisAPI() {
    m_synthesisAPI.pop_back();
}

bool SyncSource::isUsable()
{
    if (getOperations().m_isEmpty) {
        try {
            SE_LOG_INFO(getDisplayName(), "checking usability...");
            getOperations().m_isEmpty();
            return true;
        } catch (...) {
            std::string explanation;
            Exception::handle(explanation, HANDLE_EXCEPTION_NO_ERROR);
            SE_LOG_INFO(getDisplayName(), "%s", explanation.c_str());
            return false;
        }
    } else {
        // Cannot check, assume it is usable.
        return true;
    }
}


SourceRegistry &SyncSource::getSourceRegistry()
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
    SourceRegistry &registry(SyncSource::getSourceRegistry());

    // insert sorted by description to have deterministic ordering
    for(SourceRegistry::iterator it = registry.begin();
        it != registry.end();
        ++it) {
        if ((*it)->m_shortDescr > shortDescr) {
            registry.insert(it, this);
            return;
        }
    }
    registry.push_back(this);
}

class InactiveSyncSource : public DummySyncSource
{
public:
    InactiveSyncSource(const SyncSourceParams &params) : DummySyncSource(params) {}

    virtual bool isInactive() const { return true; }
    virtual Databases getDatabases() { throwError(SE_HERE, "inactive"); return Databases(); }
    virtual void open() { throwError(SE_HERE, "inactive"); }
};

SyncSource *RegisterSyncSource::InactiveSource(const SyncSourceParams &params)
{
    return new InactiveSyncSource(params);
}

TestRegistry &SyncSource::getTestRegistry()
{
    static TestRegistry testRegistry;
    return testRegistry;
}

RegisterSyncSourceTest::RegisterSyncSourceTest(const string &configName, const string &testCaseName) :
    m_configName(configName),
    m_testCaseName(testCaseName)
{
    SyncSource::getTestRegistry().push_back(this);
}

static class ScannedModules {
public:
    ScannedModules() {
#ifdef ENABLE_MODULES
        list<pair <string, boost::shared_ptr<ReadDir> > > dirs;
        /* If enviroment variable SYNCEVOLUTION_BACKEND_DIR is set, will search
        backends from this path instead. */
        string backend_dir (SYNCEVO_BACKEND);
        char *backend_env = getenv("SYNCEVOLUTION_BACKEND_DIR");
        if (backend_env && strlen(backend_env)){
            backend_dir = backend_env;
        }
        boost::shared_ptr<ReadDir> dir (new ReadDir (backend_dir, false));
        string dirpath (backend_dir);
        // Base name (= no dir, no .so suffix) mapping to full file
        // name (including .so).
        std::map<std::string, std::string> candidates;
        // scan directories for matching module names
        do {
            debug<<"Scanning backend libraries in " <<dirpath <<endl;
            BOOST_FOREACH (const string &entry, *dir) {
                if (isDir (dirpath + '/' + entry)) {
                    /* This is a 2-level dir, this corresponds to loading
                     * backends from current building directory. The library
                     * should reside in .libs sub directory.*/
                    string path = dirpath + '/' + entry +"/.libs";
                    if (isDir (path)) {
                        boost::shared_ptr<ReadDir> subdir (new ReadDir (path, false));
                        dirs.push_back (make_pair(path, subdir));
                    }
                    continue;
                }
                if (boost::ends_with(entry, ".so")) {
                    string fullpath = dirpath + '/' + entry;
                    fullpath = normalizePath(fullpath);
                    candidates[entry.substr(0, entry.size() - 3)] = fullpath;
                }
            }
            if (!dirs.empty()){
                dirpath = dirs.front().first;
                dir = dirs.front().second;
                dirs.pop_front();
            } else {
                break;
            }
        } while (true);

        // Look at foo-<version> before foo. If there is more than
        // one version and the version sorts lexically, the "highest"
        // one will be checked first, too.
        //
        // The intention is to try loading syncebook-2 (with explicit
        // library dependencies) first, then skip syncebook if loading
        // of syncebook-2 succeeded. If loading of syncebook-2 fails
        // due to missing libraries, we proceed to use syncebook.
        BOOST_REVERSE_FOREACH (const StringPair &entry, candidates) {
            const std::string &basename = entry.first;
            const std::string &fullpath = entry.second;
            std::string replacement;
            std::string modname;
            size_t offset = basename.rfind('-');
            if (offset != basename.npos) {
                modname = basename.substr(0, offset);
            } else {
                modname = basename;
            }
            BOOST_FOREACH (const std::string &l, m_available) {
                if (boost::starts_with(l, modname)) {
                    replacement = l;
                    break;
                }
            }
            if (!replacement.empty()) {
                debug << "Skipping " << basename << " = " << fullpath << " because a more recent version of it was already loaded: " << replacement;
                continue;
            }

            // Open the shared object so that backend can register
            // itself. We keep that pointer, so never close the
            // module!
            // RTLD_LAZY is needed for the WebDAV backend, which
            // needs to do an explicit dlopen() of libneon in compatibility
            // mode before any of the neon functions can be resolved.
            void *dlhandle = dlopen(fullpath.c_str(), RTLD_LAZY|RTLD_GLOBAL);
            // remember which modules were found and which were not
            if (dlhandle) {
                debug<<"Loading backend library "<<basename<<endl;
                info<<"Loading backend library "<<fullpath<<endl;
                m_available.push_back(basename);
            } else {
                debug<<"Loading backend library "<<basename<<"failed "<< dlerror()<<endl;
            }
        }
#endif
    }
    list<string> m_available;
    std::ostringstream debug, info;
} scannedModules;

string SyncSource::backendsInfo() {
    return scannedModules.info.str();
}
string SyncSource::backendsDebug() {
    return scannedModules.debug.str();
}

void SyncSource::requestAnotherSync()
{
    // At the moment the per-source request to restart cannot be
    // stored; instead only a per-session request is set. That's okay
    // for now because restarting is limited to sessions with only
    // one source active (intentional simplification).
    SE_LOG_DEBUG(getDisplayName(), "requesting another sync");
    SyncContext::requestAnotherSync();
}


SyncSource *SyncSource::createSource(const SyncSourceParams &params, bool error, SyncConfig *config)
{
    SourceType sourceType = getSourceType(params.m_nodes);

    if (sourceType.m_backend == "virtual") {
        SyncSource *source = NULL;
        source = new VirtualSyncSource(params, config);
        if (error && !source) {
            Exception::throwError(SE_HERE, params.getDisplayName() + ": virtual datastore cannot be instantiated");
        }
        return source;
    }

    const SourceRegistry &registry(getSourceRegistry());
    auto_ptr<SyncSource> source;
    BOOST_FOREACH(const RegisterSyncSource *sourceInfos, registry) {
        auto_ptr<SyncSource> nextSource(sourceInfos->m_create(params));
        if (nextSource.get()) {
            if (source.get()) {
                Exception::throwError(SE_HERE, params.getDisplayName() + ": backend " + sourceType.m_backend +
                                        " is ambiguous, avoid the alias and pick a specific backend instead directly");
            }
            source = nextSource;
        }
    }
    if (source.get()) {
        return source.release();
    }

    if (error) {
        string backends;
        if (!scannedModules.m_available.empty()) {
            backends += "by any of the backend modules (";
            backends += boost::join(scannedModules.m_available, ", ");
            backends += ") ";
        }
        string problem =
            StringPrintf("%s%sbackend not supported %sor not correctly configured (backend=%s databaseFormat=%s syncFormat=%s)",
                         params.m_name.c_str(),
                         params.m_name.empty() ? "" : ": ",
                         backends.c_str(),
                         sourceType.m_backend.c_str(),
                         sourceType.m_localFormat.c_str(),
                         sourceType.m_format.c_str());
        Exception::throwError(SE_HERE, SyncMLStatus(sysync::LOCERR_CFGPARSE), problem);
    }

    return NULL;
}

SyncSource *SyncSource::createTestingSource(const string &name, const string &type, bool error,
                                            const char *prefix)
{
    std::string config = "target-config@client-test";
    const char *server = getenv("CLIENT_TEST_SERVER");
    if (server) {
        config += "-";
        config += server;
    }
    boost::shared_ptr<SyncConfig> context(new SyncConfig(config));
    SyncSourceNodes nodes = context->getSyncSourceNodes(name);
    SyncSourceParams params(name, nodes, context);
    PersistentSyncSourceConfig sourceconfig(name, nodes);
    sourceconfig.setSourceType(type);
    if (prefix) {
        sourceconfig.setDatabaseID(string(prefix) + name + "_1");
    }
    return createSource(params, error);
}

VirtualSyncSource::VirtualSyncSource(const SyncSourceParams &params, SyncConfig *config) :
    DummySyncSource(params)
{
    if (config) {
        std::string evoSyncSource = getDatabaseID();
        BOOST_FOREACH(std::string name, getMappedSources()) {
            if (name.empty()) {
                throwError(SE_HERE, StringPrintf("configuration of underlying datastores contains empty datastore name: database = '%s'",
                                                 evoSyncSource.c_str()));
            }
            SyncSourceNodes source = config->getSyncSourceNodes(name);
            SyncSourceParams params(name, source, boost::shared_ptr<SyncConfig>(config, SyncConfigNOP()));
            boost::shared_ptr<SyncSource> syncSource(createSource(params, true, config));
            m_sources.push_back(syncSource);
        }
        if (m_sources.size() != 2) {
            throwError(SE_HERE, StringPrintf("configuration of underlying datastores must contain exactly one calendar and one todo datastore (like calendar+todo): database = '%s'",
                                             evoSyncSource.c_str()));
        }
    }

    m_operations.m_isEmpty = boost::bind(&VirtualSyncSource::isEmpty, this);
}

void VirtualSyncSource::open()
{
    getDataTypeSupport();
    BOOST_FOREACH(boost::shared_ptr<SyncSource> &source, m_sources) {
        source->open();
    }
}

bool VirtualSyncSource::isEmpty()
{
    bool empty = true;
    SuspendFlags &s = SuspendFlags::getSuspendFlags();

    BOOST_FOREACH(boost::shared_ptr<SyncSource> &source, m_sources) {
        // Operation might not be implemented, in which case we have to
        // assume "not empty".
        if (!source->getOperations().m_isEmpty ||
            !source->getOperations().m_isEmpty()) {
            empty = false;
            // Keep checking, because isEmpty() is also used for
            // isUsable() and we have to touch all sources for that.
        }
        // Operation might have been aborted by user, need to check.
        s.checkForNormal();
    }
    return empty;
}


void VirtualSyncSource::close()
{
    BOOST_FOREACH(boost::shared_ptr<SyncSource> &source, m_sources) {
        source->close();
    }
}

std::vector<std::string> VirtualSyncSource::getMappedSources()
{
    std::string evoSyncSource = getDatabaseID();
    std::vector<std::string> mappedSources = unescapeJoinedString (evoSyncSource, ',');
    return mappedSources;
}

std::string VirtualSyncSource::getDataTypeSupport()
{
    string datatypes;
    SourceType sourceType = getSourceType();
    string type = sourceType.m_format;

    datatypes = getDataTypeSupport(type, sourceType.m_forceFormat);
    return datatypes;
}

SyncSource::Databases VirtualSyncSource::getDatabases()
{
    SyncSource::Databases dbs;
    BOOST_FOREACH (boost::shared_ptr<SyncSource> &source, m_sources) {
        SyncSource::Databases sub = source->getDatabases();
        if (sub.empty()) {
            return dbs;
        }
    }
    Database db ("calendar+todo", "");
    dbs.push_back (db);
    return dbs;
}

void SyncSourceSession::init(SyncSource::Operations &ops)
{
    ops.m_startDataRead = boost::bind(&SyncSourceSession::startDataRead, this, _1, _2);
    ops.m_endDataRead = boost::lambda::constant(sysync::LOCERR_OK);
    ops.m_startDataWrite = boost::lambda::constant(sysync::LOCERR_OK);
    ops.m_endDataWrite = boost::bind(&SyncSourceSession::endDataWrite, this, _1, _2);
}

sysync::TSyError SyncSourceSession::startDataRead(const char *lastToken, const char *resumeToken)
{
    try {
        beginSync(lastToken ? lastToken : "",
                  resumeToken ? resumeToken : "");
    } catch (const StatusException &ex) {
        SyncMLStatus status = ex.syncMLStatus();
        if (status == STATUS_SLOW_SYNC_508) {
            // Not an error. Return it normally, without ERROR logging
            // in our caller.
            return status;
        } else {
            throw;
        }
    }
    return sysync::LOCERR_OK;
}

sysync::TSyError SyncSourceSession::endDataWrite(bool success, char **newToken)
{
    std::string token = endSync(success);
    *newToken = StrAlloc(token.c_str());
    return sysync::LOCERR_OK;
}

void SyncSourceChanges::init(SyncSource::Operations &ops)
{
    ops.m_readNextItem = boost::bind(&SyncSourceChanges::iterate, this, _1, _2, _3);
}

SyncSourceChanges::SyncSourceChanges() :
    m_first(true)
{
}

bool SyncSourceChanges::addItem(const string &luid, State state)
{
    pair<Items_t::iterator, bool> res = m_items[state].insert(luid);
    return res.second;
}

bool SyncSourceChanges::reset()
{
    bool removed = false;
    for (int i = 0; i < MAX; i++) {
        if (!m_items[i].empty()) {
            m_items[i].clear();
            removed = true;
        }
    }
    m_first = true;
    return removed;
}

sysync::TSyError SyncSourceChanges::iterate(sysync::ItemID aID,
                                            sysync::sInt32 *aStatus,
                                            bool aFirst)
{
    aID->item = NULL;
    aID->parent = NULL;

    if (m_first || aFirst) {
        m_it = m_items[ANY].begin();
        m_first = false;
    }

    if (m_it == m_items[ANY].end()) {
        *aStatus = sysync::ReadNextItem_EOF;
    } else {
        const string &luid = *m_it;

        if (m_items[NEW].find(luid) != m_items[NEW].end() ||
            m_items[UPDATED].find(luid) != m_items[UPDATED].end()) {
            *aStatus = sysync::ReadNextItem_Changed;
        } else {
            *aStatus = sysync::ReadNextItem_Unchanged;
        }
        aID->item = StrAlloc(luid.c_str());
        ++m_it;
    }

    return sysync::LOCERR_OK;
}

void SyncSourceDelete::init(SyncSource::Operations &ops)
{
    ops.m_deleteItem = boost::bind(&SyncSourceDelete::deleteItemSynthesis, this, _1);
}

sysync::TSyError SyncSourceDelete::deleteItemSynthesis(sysync::cItemID aID)
{
    deleteItem(aID->item);
    incrementNumDeleted();
    return sysync::LOCERR_OK;
}


void SyncSourceSerialize::getSynthesisInfo(SynthesisInfo &info,
                                           XMLConfigFragments &fragments)
{
    string type = getMimeType();

    // default remote rule (local-storage.xml): suppresses empty properties
    info.m_backendRule = "LOCALSTORAGE";

    // We store entire items locally and thus have to make sure that
    // they are complete by having the engine merge incoming and local
    // data.
    info.m_datastoreOptions += "      <updateallfields>true</updateallfields>\n";

    if (type == "text/x-vcard") {
        info.m_native = "vCard21";
        info.m_fieldlist = "contacts";
        info.m_profile = "\"vCard\", 1";
        info.m_datatypes =
            "        <use datatype='vCard21' mode='rw' preferred='yes'/>\n"
            "        <use datatype='vCard30' mode='rw'/>\n";
    } else if (type == "text/vcard") {
        info.m_native = "vCard30";
        info.m_fieldlist = "contacts";
        info.m_profile = "\"vCard\", 2";
        info.m_datatypes =
            "        <use datatype='vCard21' mode='rw'/>\n"
            "        <use datatype='vCard30' mode='rw' preferred='yes'/>\n";
        // If a backend overwrites the m_beforeWriteScript, then it must
        // include $VCARD_OUTGOING_PHOTO_VALUE_SCRIPT in its own script,
        // otherwise it will be sent invalid, empty PHOTO;TYPE=unknown;VALUE=binary:
        // properties.
        info.m_beforeWriteScript = "$VCARD_BEFOREWRITE_SCRIPT;\n";
        // Likewise for reading. This is needed to ensure proper merging
        // of contact data.
        info.m_afterReadScript = "$VCARD_AFTERREAD_SCRIPT;\n";
    } else if (type == "text/x-calendar" || type == "text/x-vcalendar") {
        info.m_native = "vCalendar10";
        info.m_fieldlist = "calendar";
        info.m_profile = "\"vCalendar\", 1";
        info.m_datatypes =
            "        <use datatype='vCalendar10' mode='rw' preferred='yes'/>\n"
            "        <use datatype='iCalendar20' mode='rw'/>\n";
        /**
         * here are two default implementations. If user wants to reset it,
         * just implement its own getSynthesisInfo. If user wants to use this default
         * implementations and its new scripts, it is possible to append its implementations
         * to info.m_afterReadScript and info.m_beforeWriteScript.
         */
        info.m_afterReadScript = "$VCALENDAR10_AFTERREAD_SCRIPT;\n";
        info.m_beforeWriteScript = "$VCALENDAR10_BEFOREWRITE_SCRIPT;\n"
            "$CALENDAR_BEFOREWRITE_SCRIPT;\n";
    } else if (type == "text/calendar" ||
               boost::starts_with(type, "text/calendar+")) {
        info.m_native = "iCalendar20";
        info.m_fieldlist = "calendar";
        info.m_profile = "\"vCalendar\", 2";
        info.m_datatypes =
            "        <use datatype='vCalendar10' mode='rw'/>\n"
            "        <use datatype='iCalendar20' mode='rw' preferred='yes'/>\n";
        info.m_beforeWriteScript = "$CALENDAR_BEFOREWRITE_SCRIPT;\n";
    } else if (type == "text/plain") {
        info.m_fieldlist = "Note";
        info.m_profile = "\"Note\", 2";
    } else {
        throwError(SE_HERE, string("default MIME type not supported: ") + type);
    }

    SourceType sourceType = getSourceType();
    if (!sourceType.m_format.empty()) {
        type = sourceType.m_format;
    }
    info.m_datatypes = getDataTypeSupport(type, sourceType.m_forceFormat);
}

std::string SyncSourceBase::getDataTypeSupport(const std::string &type,
                                               bool forceFormat)
{
    std::string datatypes;

    if (type == "text/x-vcard:2.1" || type == "text/x-vcard") {
        datatypes =
            "        <use datatype='vCard21' mode='rw' preferred='yes'/>\n";
        if (!forceFormat) {
            datatypes +=
                "        <use datatype='vCard30' mode='rw'/>\n";
        }
    } else if (type == "text/vcard:3.0" || type == "text/vcard") {
        datatypes =
            "        <use datatype='vCard30' mode='rw' preferred='yes'/>\n";
        if (!forceFormat) {
            datatypes +=
                "        <use datatype='vCard21' mode='rw'/>\n";
        }
    } else if (type == "text/x-vcalendar:1.0" || type == "text/x-vcalendar"
             || type == "text/x-calendar:1.0" || type == "text/x-calendar") {
        datatypes =
            "        <use datatype='vcalendar10' mode='rw' preferred='yes'/>\n";
        if (!forceFormat) {
            datatypes +=
                "        <use datatype='icalendar20' mode='rw'/>\n";
        }
    } else if (type == "text/calendar:2.0" || type == "text/calendar") {
        datatypes =
            "        <use datatype='icalendar20' mode='rw' preferred='yes'/>\n";
        if (!forceFormat) {
            datatypes +=
                "        <use datatype='vcalendar10' mode='rw'/>\n";
        }
    } else if (type == "text/calendar+plain") {
        datatypes =
            "        <use datatype='icalendar20' mode='rw' preferred='yes'/>\n"
            "        <use datatype='journaltext10' mode='rw'/>\n"
            "        <use datatype='journaltext11' mode='rw'/>\n";
    } else if (type == "text/plain:1.0" || type == "text/plain") {
        // note10 are the same as note11, so ignore force format
        datatypes =
            "        <use datatype='note10' mode='rw' preferred='yes'/>\n"
            "        <use datatype='note11' mode='rw'/>\n";
    } else if (type.empty()) {
        throwError(SE_HERE, "no MIME type configured");
    } else {
        throwError(SE_HERE, string("configured MIME type not supported: ") + type);
    }

    return datatypes;
}

sysync::TSyError SyncSourceSerialize::readItemAsKey(sysync::cItemID aID, sysync::KeyH aItemKey)
{
    std::string item;

    readItem(aID->item, item);
    TSyError res = getSynthesisAPI()->setValue(aItemKey, "data", item.c_str(), item.size());
    return res;
}

SyncSource::Operations::InsertItemAsKeyResult_t SyncSourceSerialize::insertItemAsKey(sysync::KeyH aItemKey, sysync::ItemID newID)
{
    SharedBuffer data;
    TSyError res = getSynthesisAPI()->getValue(aItemKey, "data", data);

    if (!res) {
        InsertItemResult inserted = insertItem("", data.get());
        switch (inserted.m_state) {
        case ITEM_OKAY:
            break;
        case ITEM_AGAIN:
            // Skip setting the newID.
            return Operations::InsertItemAsKeyContinue_t(boost::bind(&SyncSourceSerialize::insertContinue, this, _2, inserted.m_continue));
            break;
        case ITEM_REPLACED:
            res = sysync::DB_DataReplaced;
            break;
        case ITEM_MERGED:
            res = sysync::DB_DataMerged;
            break;
        case ITEM_NEEDS_MERGE:
            res = sysync::DB_Conflict;
            break;
        }
        newID->item = StrAlloc(inserted.m_luid.c_str());
    }

    return res;
}

SyncSource::Operations::UpdateItemAsKeyResult_t SyncSourceSerialize::updateItemAsKey(sysync::KeyH aItemKey, sysync::cItemID aID, sysync::ItemID newID)
{
    SharedBuffer data;
    TSyError res = getSynthesisAPI()->getValue(aItemKey, "data", data);

    if (!res) {
        InsertItemResult inserted = insertItem(aID->item, data.get());
        switch (inserted.m_state) {
        case ITEM_OKAY:
            break;
        case ITEM_AGAIN:
            // Skip setting the newID.
            return Operations::UpdateItemAsKeyContinue_t(boost::bind(&SyncSourceSerialize::insertContinue, this, _3, inserted.m_continue));
            break;
        case ITEM_REPLACED:
            res = sysync::DB_DataReplaced;
            break;
        case ITEM_MERGED:
            res = sysync::DB_DataMerged;
            break;
        case ITEM_NEEDS_MERGE:
            res = sysync::DB_Conflict;
            break;
        }
        newID->item = StrAlloc(inserted.m_luid.c_str());
    }

    return res;
}

sysync::TSyError SyncSourceSerialize::insertContinue(sysync::ItemID newID, const InsertItemResult::Continue_t &cont)
{
    // The engine cannot tell us when it needs results (for example,
    // in the "final message received from peer" case in
    // TSyncSession::EndMessage(), so assume that it does whenever it
    // calls us again => flush and wait.
    flushItemChanges();
    finishItemChanges();

    InsertItemResult inserted = cont();
    TSyError res = sysync::LOCERR_OK;
    switch (inserted.m_state) {
    case ITEM_OKAY:
        break;
    case ITEM_AGAIN:
        // Skip setting the newID.
        return sysync::LOCERR_AGAIN;
        break;
    case ITEM_REPLACED:
        res = sysync::DB_DataReplaced;
        break;
    case ITEM_MERGED:
        res = sysync::DB_DataMerged;
        break;
    case ITEM_NEEDS_MERGE:
        res = sysync::DB_Conflict;
        break;
    }
    newID->item = StrAlloc(inserted.m_luid.c_str());
    return res;
}

SyncSourceSerialize::InsertItemResult SyncSourceSerialize::insertItemRaw(const std::string &luid, const std::string &item)
{
    InsertItemResult result = insertItem(luid, item);

    while (result.m_state == ITEM_AGAIN) {
        // Flush and wait, because caller (command line, restore) is
        // not prepared to deal with asynchronous execution.
        flushItemChanges();
        finishItemChanges();
        result = result.m_continue();
    }

    return result;
}

void SyncSourceSerialize::readItemRaw(const std::string &luid, std::string &item)
{
    return readItem(luid, item);
}



void SyncSourceSerialize::init(SyncSource::Operations &ops)
{
    ops.m_readItemAsKey = boost::bind(&SyncSourceSerialize::readItemAsKey,
                                      this, _1, _2);
    ops.m_insertItemAsKey = boost::bind(&SyncSourceSerialize::insertItemAsKey,
                                        this, _1, _2);
    ops.m_updateItemAsKey = boost::bind(&SyncSourceSerialize::updateItemAsKey,
                                        this, _1, _2, _3);
}


void ItemCache::init(const SyncSource::Operations::ConstBackupInfo &oldBackup,
                     const SyncSource::Operations::BackupInfo &newBackup,
                     bool legacy)
{
    m_counter = 1;
    m_legacy = legacy;
    m_backup = newBackup;
    m_hash2counter.clear();
    m_dirname = oldBackup.m_dirname;
    if (m_dirname.empty() || !oldBackup.m_node) {
        return;
    }

    long numitems;
    if (!oldBackup.m_node->getProperty("numitems", numitems)) {
        return;
    }
    for (long counter = 1; counter <= numitems; counter++) {
        stringstream key;
        key << counter << m_hashSuffix;
        Hash_t hash;
        if (oldBackup.m_node->getProperty(key.str(), hash)) {
            m_hash2counter[hash] = counter;
        }
    }
}

void ItemCache::reset()
{
    // clean directory and start counting at 1 again
    m_counter = 1;
    rm_r(m_backup.m_dirname);
    mkdir_p(m_backup.m_dirname);
    m_backup.m_node->clear();
}

string ItemCache::getFilename(Hash_t hash)
{
    Map_t::const_iterator it = m_hash2counter.find(hash);
    if (it != m_hash2counter.end()) {
        stringstream dirname;
        dirname << m_dirname << "/" << it->second;
        return dirname.str();
    } else {
        return "";
    }
}

const char *ItemCache::m_hashSuffix =
#ifdef USE_SHA256
    "-sha256"
#else
    "-hash"
#endif
;

void ItemCache::backupItem(const std::string &item,
                           const std::string &uid,
                           const std::string &rev)
{
    stringstream filename;
    filename << m_backup.m_dirname << "/" << m_counter;

    ItemCache::Hash_t hash = hashFunc(item);
    string oldfilename = getFilename(hash);
    if (!oldfilename.empty()) {
        // found old file with same content, reuse it via hardlink
        if (link(oldfilename.c_str(), filename.str().c_str())) {
            // Hard linking failed. Record this, then continue
            // by ignoring the old file.
            SE_LOG_DEBUG(NULL, "hard linking old %s new %s: %s",
                         oldfilename.c_str(),
                         filename.str().c_str(),
                         strerror(errno));
            oldfilename.clear();
        }
    }

    if (oldfilename.empty()) {
        // write new file instead of reusing old one
        ofstream out(filename.str().c_str());
        out.write(item.c_str(), item.size());
        out.close();
        if (out.fail()) {
            SE_THROW(string("error writing ") + filename.str() + ": " + strerror(errno));
        }
    }

    stringstream key;
    key << m_counter << "-uid";
    m_backup.m_node->setProperty(key.str(), uid);
    if (m_legacy) {
        // clear() does not remove the existing content, which was
        // intended here. This should have been key.str(""). As a
        // result, keys for -rev are longer than intended because they
        // start with the -uid part. We cannot change it now, because
        // that would break compatibility with nodes that use the
        // older, longer keys for -rev.
        // key.clear();
    } else {
        key.str("");
    }
    key << m_counter << "-rev";
    m_backup.m_node->setProperty(key.str(), rev);
    key.str("");
    key << m_counter << ItemCache::m_hashSuffix;
    m_backup.m_node->setProperty(key.str(), hash);

    m_counter++;
}

void ItemCache::finalize(BackupReport &report)
{
    stringstream value;
    value << m_counter - 1;
    m_backup.m_node->setProperty("numitems", value.str());
    m_backup.m_node->flush();

    report.setNumItems(m_counter - 1);
}

void SyncSourceRevisions::initRevisions()
{
    if (!m_revisionsSet) {
        // might still be filled with garbage from previous run
        m_revisions.clear();
        listAllItems(m_revisions);
        m_revisionsSet = true;
    }
}


void SyncSourceRevisions::backupData(const SyncSource::Operations::ConstBackupInfo &oldBackup,
                                     const SyncSource::Operations::BackupInfo &newBackup,
                                     BackupReport &report)
{
    ItemCache cache;
    cache.init(oldBackup, newBackup, true);

    bool startOfSync = newBackup.m_mode == SyncSource::Operations::BackupInfo::BACKUP_BEFORE;
    RevisionMap_t buffer;
    RevisionMap_t *revisions;
    if (startOfSync) {
        initRevisions();
        revisions = &m_revisions;
    } else {
        listAllItems(buffer);
        revisions = &buffer;
    }

    // Ensure that source knows what we are going to read.
    std::vector<std::string> uids;
    uids.reserve(revisions->size());
    BOOST_FOREACH(const StringPair &mapping, *revisions) {
        uids.push_back(mapping.first);
    }

    // We may dump after a hint was already set when starting the
    // sync. Remember that and restore it when done. If we fail, we
    // don't need to restore, because then syncing will abort or skip
    // the source.
    ReadAheadOrder oldOrder;
    ReadAheadItems oldLUIDs;
    getReadAheadOrder(oldOrder, oldLUIDs);

    setReadAheadOrder(READ_SELECTED_ITEMS, uids);

    string item;
    errno = 0;
    BOOST_FOREACH(const StringPair &mapping, *revisions) {
        const string &uid = mapping.first;
        const string &rev = mapping.second;
        m_raw->readItemRaw(uid, item);
        cache.backupItem(item, uid, rev);
    }

    setReadAheadOrder(oldOrder, oldLUIDs);
    cache.finalize(report);
}

void SyncSourceRevisions::restoreData(const SyncSource::Operations::ConstBackupInfo &oldBackup,
                                      bool dryrun,
                                      SyncSourceReport &report)
{
    RevisionMap_t revisions;
    listAllItems(revisions);

    long numitems;
    string strval;
    strval = oldBackup.m_node->readProperty("numitems");
    stringstream stream(strval);
    stream >> numitems;

    for (long counter = 1; counter <= numitems; counter++) {
        stringstream key;
        key << counter << "-uid";
        string uid = oldBackup.m_node->readProperty(key.str());
        key.clear();
        key << counter << "-rev";
        string rev = oldBackup.m_node->readProperty(key.str());
        RevisionMap_t::iterator it = revisions.find(uid);
        report.incrementItemStat(report.ITEM_LOCAL,
                                 report.ITEM_ANY,
                                 report.ITEM_TOTAL);
        if (it != revisions.end() &&
            it->second == rev) {
            // item exists in backup and database with same revision:
            // nothing to do
        } else {
            // add or update, so need item
            stringstream filename;
            filename << oldBackup.m_dirname << "/" << counter;
            string data;
            if (!ReadFile(filename.str(), data)) {
                throwError(SE_HERE, StringPrintf("restoring %s from %s failed: could not read file",
                                        uid.c_str(),
                                        filename.str().c_str()));
            }
            // TODO: it would be nicer to recreate the item
            // with the original revision. If multiple peers
            // synchronize against us, then some of them
            // might still be in sync with that revision. By
            // updating the revision here we force them to
            // needlessly receive an update.
            //
            // For the current peer for which we restore this is
            // avoided by the revision check above: unchanged
            // items aren't touched.
            SyncSourceReport::ItemState state =
                it == revisions.end() ?
                SyncSourceReport::ITEM_ADDED :   // not found in database, create anew
                SyncSourceReport::ITEM_UPDATED;  // found, update existing item
            try {
                report.incrementItemStat(report.ITEM_LOCAL,
                                         state,
                                         report.ITEM_TOTAL);
                if (!dryrun) {
                    m_raw->insertItemRaw(it == revisions.end() ? "" : uid,
                                         data);
                }
            } catch (...) {
                report.incrementItemStat(report.ITEM_LOCAL,
                                         state,
                                         report.ITEM_REJECT);
                throw;
            }
        }

        // remove handled item from revision list so
        // that when we are done, the only remaining
        // items listed there are the ones which did
        // no exist in the backup
        if (it != revisions.end()) {
            revisions.erase(it);
        }
    }

    // now remove items that were not in the backup
    BOOST_FOREACH(const StringPair &mapping, revisions) {
        try {
            report.incrementItemStat(report.ITEM_LOCAL,
                                     report.ITEM_REMOVED,
                                     report.ITEM_TOTAL);
            if (!dryrun) {
                m_del->deleteItem(mapping.first);
            }
        } catch(...) {
            report.incrementItemStat(report.ITEM_LOCAL,
                                     report.ITEM_REMOVED,
                                     report.ITEM_REJECT);
            throw;
        }
    }
}

bool SyncSourceRevisions::detectChanges(ConfigNode &trackingNode, ChangeMode mode)
{
    bool forceSlowSync = false;

    // erase content which might have been set in a previous call
    reset();
    if (!m_firstCycle) {
        // detectChanges() must have been called before;
        // don't trust our cached revisions in that case (not updated during sync!)
        // TODO: keep the revision map up-to-date as part of a sync and reuse it
        m_revisionsSet = false;
    } else {
        m_firstCycle = false;
    }

    if (mode == CHANGES_NONE) {
        // shortcut because nothing changed: just copy our known item list
        ConfigProps props;
        trackingNode.readProperties(props);

        RevisionMap_t revisions;
        BOOST_FOREACH(const StringPair &mapping, props) {
            const string &uid = mapping.first;
            const string &revision = mapping.second;
            addItem(uid);
            revisions[uid] = revision;
        }
        setAllItems(revisions);
        return false;
    }

    if (!m_revisionsSet &&
        mode == CHANGES_FULL) {
        ConfigProps props;
        trackingNode.readProperties(props);
        if (!props.empty()) {
            // We were not asked to throw away all old information and
            // there is some that may be worth salvaging, so let's give
            // our derived class a chance to update it instead of having
            // to reread everything.
            //
            // The exact number of items at which the update method is
            // more efficient depends on the derived class; here we assume
            // that even a single item makes it worthwhile. The derived
            // class can always ignore the information if it has different
            // tradeoffs.
            //
            // TODO (?): an API which only provides the information
            // on demand...
            m_revisions.clear();
            m_revisions.insert(props.begin(), props.end());
            updateAllItems(m_revisions);
            // continue with m_revisions initialized below
            m_revisionsSet = true;
        }
    }

    // traditional, slow fallback follows...
    initRevisions();

    // Check whether we have valid revision information.  If not, then
    // we need to do a slow sync. The assumption here is that an empty
    // revision string marks missing information. When we don't need
    // change information, not having a revision string is okay.
    if (needChanges() &&
        !m_revisions.empty() &&
        m_revisions.begin()->second.empty()) {
        forceSlowSync = true;
        mode = CHANGES_SLOW;
    }

    // If we don't need changes, then override the mode so that
    // we don't compute them below.
    if (!needChanges()) {
        mode = CHANGES_SLOW;
    }

    // Delay setProperty calls until after checking all uids.
    // Necessary for MapSyncSource, which shares the revision among
    // several uids. Another advantage is that we can do the "find
    // deleted items" check with less entries (new items not added
    // yet).
    StringMap revUpdates;

    if (mode == CHANGES_SLOW) {
        // Make tracking node identical to current set of items
        // by re-adding them below.
        trackingNode.clear();
    }

    BOOST_FOREACH(const StringPair &mapping, m_revisions) {
        const string &uid = mapping.first;
        const string &revision = mapping.second;

        // always remember the item, need full list
        addItem(uid);

        // avoid unnecessary work in CHANGES_SLOW mode
        if (mode == CHANGES_SLOW) {
            trackingNode.setProperty(uid, revision);
        } else {
            // detect changes
            string serverRevision(trackingNode.readProperty(uid));
            if (!serverRevision.size()) {
                addItem(uid, NEW);
                revUpdates[uid] = revision;
            } else {
                if (revision != serverRevision) {
                    addItem(uid, UPDATED);
                    revUpdates[uid] = revision;
                }
            }
        }
    }

    if (mode != CHANGES_SLOW) {
        // clear information about all items that we recognized as deleted
        ConfigProps props;
        trackingNode.readProperties(props);

        BOOST_FOREACH(const StringPair &mapping, props) {
            const string &uid(mapping.first);
            if (getAllItems().find(uid) == getAllItems().end()) {
                addItem(uid, DELETED);
                trackingNode.removeProperty(uid);
            }
        }

        // now update tracking node
        BOOST_FOREACH(const StringPair &update, revUpdates) {
            trackingNode.setProperty(update.first, update.second);
        }
    }

    return forceSlowSync;
}

void SyncSourceRevisions::updateRevision(ConfigNode &trackingNode,
                                         const std::string &old_luid,
                                         const std::string &new_luid,
                                         const std::string &revision)
{
    if (!needChanges()) {
        return;
    }

    databaseModified();
    if (old_luid != new_luid) {
        trackingNode.removeProperty(old_luid);
    }
    if (new_luid.empty() || revision.empty()) {
        throwError(SE_HERE, "need non-empty LUID and revision string");
    }
    trackingNode.setProperty(new_luid, revision);
}

void SyncSourceRevisions::deleteRevision(ConfigNode &trackingNode,
                                         const std::string &luid)
{
    if (!needChanges()) {
        return;
    }
    databaseModified();
    trackingNode.removeProperty(luid);
}

SyncMLStatus SyncSourceRevisions::sleepSinceModification()
{
    Timespec current = Timespec::monotonic();
    // Don't let this get interrupted by user abort.
    // It is needed for correct change tracking.
    while ((current - m_modTimeStamp).duration() < m_revisionAccuracySeconds) {
        Sleep(m_revisionAccuracySeconds - (current - m_modTimeStamp).duration());
        current = Timespec::monotonic();
    }
    return STATUS_OK;
}

void SyncSourceRevisions::databaseModified()
{
    m_modTimeStamp = Timespec::monotonic();
}

void SyncSourceRevisions::init(SyncSourceRaw *raw,
                               SyncSourceDelete *del,
                               int granularity,
                               SyncSource::Operations &ops)
{
    m_raw = raw;
    m_del = del;
    m_revisionAccuracySeconds = granularity;
    m_revisionsSet = false;
    m_firstCycle = false;
    if (raw) {
        ops.m_backupData = boost::bind(&SyncSourceRevisions::backupData,
                                       this, _1, _2, _3);
    }
    if (raw && del) {
        ops.m_restoreData = boost::bind(&SyncSourceRevisions::restoreData,
                                        this, _1, _2, _3);
    }
    ops.m_endDataWrite.getPostSignal().connect(boost::bind(&SyncSourceRevisions::sleepSinceModification,
                                                           this));
}

std::string SyncSourceLogging::getDescription(sysync::KeyH aItemKey)
{
    try {
        std::list<std::string> values;

        BOOST_FOREACH(const std::string &field, m_fields) {
            SharedBuffer value;
            if (!getSynthesisAPI()->getValue(aItemKey, field, value) &&
                value.size()) {
                values.push_back(std::string(value.get()));
            }
        }

        std::string description = boost::join(values, m_sep);
        return description;
    } catch (...) {
        // Instead of failing we log the error and ask
        // the caller to log the UID. That way transient
        // errors or errors in the logging code don't
        // prevent syncs.
        handleException();
        return "";
    }
}

std::string SyncSourceLogging::getDescription(const string &luid)
{
    return "";
}

SyncMLStatus SyncSourceLogging::insertItemAsKey(sysync::KeyH aItemKey, sysync::ItemID newID)
{
    std::string description = getDescription(aItemKey);
    SE_LOG_INFO(getDisplayName(),
                description.empty() ? "%s <%s>" : "%s \"%s\"",
                "adding",
                !description.empty() ? description.c_str() : "???");
    return STATUS_OK;
}

SyncMLStatus SyncSourceLogging::updateItemAsKey(sysync::KeyH aItemKey, sysync::cItemID aID, sysync::ItemID newID)
{
    std::string description = getDescription(aItemKey);
    SE_LOG_INFO(getDisplayName(),
                description.empty() ? "%s <%s>" : "%s \"%s\"",
                "updating",
                !description.empty() ? description.c_str() : aID ? aID->item : "???");
    return STATUS_OK;
}

SyncMLStatus SyncSourceLogging::deleteItem(sysync::cItemID aID)
{
    std::string description = getDescription(aID->item);
    SE_LOG_INFO(getDisplayName(),
                description.empty() ? "%s <%s>" : "%s \"%s\"",
                "deleting",
                !description.empty() ? description.c_str() : aID->item);
    return STATUS_OK;
}

void SyncSourceLogging::init(const std::list<std::string> &fields,
                             const std::string &sep,
                             SyncSource::Operations &ops)
{
    m_fields = fields;
    m_sep = sep;

    ops.m_insertItemAsKey.getPreSignal().connect(boost::bind(&SyncSourceLogging::insertItemAsKey,
                                                             this, _2, _3));
    ops.m_updateItemAsKey.getPreSignal().connect(boost::bind(&SyncSourceLogging::updateItemAsKey,
                                                             this, _2, _3, _4));
    ops.m_deleteItem.getPreSignal().connect(boost::bind(&SyncSourceLogging::deleteItem,
                                                        this, _2));
}

sysync::TSyError SyncSourceAdmin::loadAdminData(const char *aLocDB,
                                                const char *aRemDB,
                                                char **adminData)
{
    std::string data = m_configNode->readProperty(m_adminPropertyName);
    *adminData = StrAlloc(StringEscape::unescape(data, '!').c_str());
    resetMap();
    return sysync::LOCERR_OK;
}

sysync::TSyError SyncSourceAdmin::saveAdminData(const char *adminData)
{
    m_configNode->setProperty(m_adminPropertyName,
                              StringEscape::escape(adminData, '!', StringEscape::INI_VALUE));

    // The exact ordering of API calls is not defined. In practice, EndDataWrite is
    // followed by Insert/Update/DeleteMapItem, finished by saveAdminData. Therefore
    // we flush here.
    flush();

    return sysync::LOCERR_OK;
}

bool SyncSourceAdmin::readNextMapItem(sysync::MapID mID, bool aFirst)
{
    if (aFirst) {
        resetMap();
    }
    if (m_mappingIterator != m_mapping.end()) {
        entry2mapid(m_mappingIterator->first, m_mappingIterator->second, mID);
        ++m_mappingIterator;
        return true;
    } else {
        return false;
    }
}

sysync::TSyError SyncSourceAdmin::insertMapItem(sysync::cMapID mID)
{
    string key, value;
    mapid2entry(mID, key, value);

#if 0
    StringMap::iterator it = m_mapping.find(key);
    if (it != m_mapping.end()) {
        // error, exists already
        return sysync::DB_Forbidden;
    } else {
        m_mapping[key] = value;
        return sysync::LOCERR_OK;
    }
#else
    m_mapping[key] = value;
    return sysync::LOCERR_OK;
#endif
}

sysync::TSyError SyncSourceAdmin::updateMapItem(sysync::cMapID mID)
{
    string key, value;
    mapid2entry(mID, key, value);

    ConfigProps::iterator it = m_mapping.find(key);
    if (it == m_mapping.end()) {
        // error, does not exist
        return sysync::DB_Forbidden;
    } else {
        m_mapping[key] = value;
        return sysync::LOCERR_OK;
    }
}

sysync::TSyError SyncSourceAdmin::deleteMapItem(sysync::cMapID mID)
{
    string key, value;
    mapid2entry(mID, key, value);

    ConfigProps::iterator it = m_mapping.find(key);
    if (it == m_mapping.end()) {
        // error, does not exist
        return sysync::DB_Forbidden;
    } else {
        m_mapping.erase(it);
        return sysync::LOCERR_OK;
    }
}

SyncMLStatus SyncSourceAdmin::flush()
{
    m_configNode->flush();
    if (m_mappingLoaded) {
        m_mappingNode->clear();
        m_mappingNode->writeProperties(m_mapping);
        m_mappingNode->flush();
    }
    return STATUS_OK;
}

void SyncSourceAdmin::resetMap()
{
    m_mapping.clear();
    m_mappingNode->readProperties(m_mapping);
    m_mappingIterator = m_mapping.begin();
    m_mappingLoaded = true;
}


void SyncSourceAdmin::mapid2entry(sysync::cMapID mID, string &key, string &value)
{
    key = StringPrintf("%s-%x",
                       StringEscape::escape(mID->localID ? mID->localID : "", '!', StringEscape::INI_WORD).c_str(),
                       mID->ident);
    if (mID->remoteID && mID->remoteID[0]) {
        value = StringPrintf("%s %x",
                             StringEscape::escape(mID->remoteID ? mID->remoteID : "", '!', StringEscape::INI_WORD).c_str(),
                             mID->flags);
    } else {
        value = StringPrintf("%x", mID->flags);
    }
}

void SyncSourceAdmin::entry2mapid(const string &key, const string &value, sysync::MapID mID)
{
    size_t found = key.rfind('-');
    mID->localID = StrAlloc(StringEscape::unescape(key.substr(0,found), '!').c_str());
    if (found != key.npos) {
        mID->ident =  strtol(key.substr(found+1).c_str(), NULL, 16);
    } else {
        mID->ident = 0;
    }
    std::vector< std::string > tokens;
    boost::split(tokens, value, boost::is_from_range(' ', ' '));
    if (tokens.size() >= 2) {
        // if branch from mapid2entry above
        mID->remoteID = StrAlloc(StringEscape::unescape(tokens[0], '!').c_str());
        mID->flags = strtol(tokens[1].c_str(), NULL, 16);
    } else {
        // else branch from above
        mID->remoteID = NULL;
        mID->flags = strtol(tokens[0].c_str(), NULL, 16);
    }
}

void SyncSourceAdmin::init(SyncSource::Operations &ops,
                           const boost::shared_ptr<ConfigNode> &config,
                           const std::string adminPropertyName,
                           const boost::shared_ptr<ConfigNode> &mapping)
{
    m_configNode = config;
    m_adminPropertyName = adminPropertyName;
    m_mappingNode = mapping;
    m_mappingLoaded = false;

    ops.m_loadAdminData = boost::bind(&SyncSourceAdmin::loadAdminData,
                                      this, _1, _2, _3);
    ops.m_saveAdminData = boost::bind(&SyncSourceAdmin::saveAdminData,
                                      this, _1);
    if (mapping->isVolatile()) {
        // Don't provide map item operations. SynthesisDBPlugin will
        // tell the Synthesis engine not to call these (normally needed
        // for suspend/resume, which we don't support in volatile mode
        // because we don't store any meta data persistently).
        //
        // ops.m_readNextMapItem = boost::lambda::constant(false);
        // ops.m_insertMapItem = boost::lambda::constant(sysync::LOCERR_OK);
        // ops.m_updateMapItem = boost::lambda::constant(sysync::LOCERR_OK);
        // ops.m_deleteMapItem = boost::lambda::constant(sysync::LOCERR_OK);
    } else {
        ops.m_readNextMapItem = boost::bind(&SyncSourceAdmin::readNextMapItem,
                                            this, _1, _2);
        ops.m_insertMapItem = boost::bind(&SyncSourceAdmin::insertMapItem,
                                          this, _1);
        ops.m_updateMapItem = boost::bind(&SyncSourceAdmin::updateMapItem,
                                          this, _1);
        ops.m_deleteMapItem = boost::bind(&SyncSourceAdmin::deleteMapItem,
                                          this, _1);
    }
}

void SyncSourceAdmin::init(SyncSource::Operations &ops,
                           SyncSource *source)
{
    init(ops,
         source->getProperties(true),
         SourceAdminDataName,
         source->getServerNode());
}

void SyncSourceBlob::init(SyncSource::Operations &ops,
                          const std::string &dir)
{
    m_blob.Init(getSynthesisAPI(),
                getName().c_str(),
                dir, "", "", "");
    ops.m_readBlob = boost::bind(&SyncSourceBlob::readBlob, this,
                                 _1, _2, _3, _4, _5, _6, _7);
    ops.m_writeBlob = boost::bind(&SyncSourceBlob::writeBlob, this,
                                  _1, _2, _3, _4, _5, _6, _7);
    ops.m_deleteBlob = boost::bind(&SyncSourceBlob::deleteBlob, this,
                                   _1, _2);
}

void TestingSyncSource::removeAllItems()
{
    // remove longest luids first:
    // for luid=UID[+RECURRENCE-ID] that will
    // remove children from a merged event first,
    // which is better supported by certain servers
    Items_t items = getAllItems();
    for (Items_t::reverse_iterator it = items.rbegin();
         it != items.rend();
         ++it) {
        deleteItem(*it);
    }
}



#ifdef ENABLE_UNIT_TESTS

class SyncSourceTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(SyncSourceTest);
    CPPUNIT_TEST(backendsAvailable);
    CPPUNIT_TEST_SUITE_END();

    void backendsAvailable()
    {
        //We expect backendsInfo() to be empty if !ENABLE_MODULES
        //Otherwise, there should be at least some backends.
#ifdef ENABLE_MODULES
        CPPUNIT_ASSERT( !SyncSource::backendsInfo().empty() );
#endif
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(SyncSourceTest);

#endif // ENABLE_UNIT_TESTS


SE_END_CXX

