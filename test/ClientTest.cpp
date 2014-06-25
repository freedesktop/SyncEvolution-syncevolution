/*
 * Copyright (C) 2008 Funambol, Inc.
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

/** @cond API */
/** @addtogroup ClientTest */
/** @{ */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef ENABLE_INTEGRATION_TESTS

#include "ClientTest.h"
#include "test.h"
#include "ClientTestAssert.h"
#include <SyncSource.h>
#include <TransportAgent.h>
#include <Logging.h>
#include <syncevo/util.h>
#include <syncevo/SyncContext.h>
#include <VolatileConfigNode.h>
#include <syncevo/Cmdline.h>

#include <synthesis/dataconversion.h>

#include <memory>
#include <vector>
#include <set>
#include <utility>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <stdarg.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <boost/bind.hpp>
#include <boost/tokenizer.hpp>
#include <boost/assign.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/lambda/if.hpp>
#include <boost/lambda/casts.hpp>
#include <boost/lambda/switch.hpp>

#include <pcrecpp.h>

#include <syncevo/declarations.h>

namespace CppUnit {

/**
 * behaves like an int and can be compared against one in ASSERT_EQUAL,
 * but includes the item list when being printed
 */
struct ItemCount
{
    SyncEvo::SyncSourceChanges::Items_t m_items;

    ItemCount() {}
    ItemCount(const SyncEvo::SyncSourceChanges::Items_t &items) : m_items(items) {}
    int size() const { return m_items.size(); }
    operator int () const { return size(); }
};

static std::ostream &operator << (ostream &out, const ItemCount &count)
{
    out << count.size() << " ( ";
    BOOST_FOREACH(const std::string &id, count.m_items) {
        out << id << " ";
    }
    out << ")";
    return out;
}

template<> struct assertion_traits<ItemCount>
{
    template <class E> static bool equal(const E &expected, const ItemCount &count) { return expected == count; }
    static std::string toString(const ItemCount &count)
    {
        std::ostringstream out;
        out << count;
        return out.str();
    }
};

/** comparison between arbitrary type A and B */
template <class A, class B>
void assertEquals(const A& expected,
                  const B& actual,
                  SourceLine sourceLine,
                  const std::string &message)
{
    if (!assertion_traits<B>::equal(expected,actual)) {
        Asserter::failNotEqual(assertion_traits<A>::toString(expected),
                               assertion_traits<B>::toString(actual),
                               sourceLine,
                               message);
    }
}

}

SE_BEGIN_CXX

/**
 * A command line using keyring as configured, but no interactive
 * password lookup. The base Cmdline class uses a generic SyncContext
 * which uses a SimpleUserInterface without keyring support.
 */
class TestCmdline : public Cmdline
{
public:
    // If we could inherit the Cmdline constructor, life would be a lot
    // easier... because we can't, we have to copy-and-paste the code
    // and rely on protected inheritance of Cmdline members.
    TestCmdline(const char *arg, ...) :
        Cmdline(std::vector<std::string>())
    {
        va_list argList;
        va_start(argList, arg);
        for (const char *curr = arg;
             curr;
             curr = va_arg(argList, const char *)) {
            m_args.push_back(curr);
        }
        va_end(argList);
        m_argc = m_args.size();
        m_argvArray.reset(new const char *[m_args.size()]);
        for (int i = 0; i < m_argc; i++) {
            m_argvArray[i] = m_args[i].c_str();
        }
        m_argv = m_argvArray.get();
    }

    virtual SyncContext *createSyncClient() {
        std::auto_ptr<SyncContext> context(new SyncContext(m_server, true));
        boost::shared_ptr<SimpleUserInterface> ui(new SimpleUserInterface(context->getKeyring()));
        context->setUserInterface(ui);
        return context.release();
    }
};

static set<ClientTest::Cleanup_t> cleanupSet;

/**
 * true when running as server,
 * relevant for sources instantiated by us
 * and testConversion, which does not work in
 * server mode (Synthesis engine not in the right
 * state when we try to run the test)
 */
static bool isServerMode()
{
    const char *serverMode = getenv("CLIENT_TEST_MODE");
    return serverMode && !strcmp(serverMode, "server");
}

/**
 * CLIENT_TEST_SERVER env variable or "" if unset
 */
std::string currentServer()
{
    const char *tmp = getenv("CLIENT_TEST_SERVER");
    return tmp ? tmp : "";
}

/**
 * This function checks whether the data on the server is accessible
 * directly. This is the case for tests where the server side is
 * syncevo-http-server or the sync is local. In both cases, this
 * method checks the config of <server>_1 (the first client's sync config)
 * to find the peer and the uri of the given local source. The peer
 * is expected to a source under that name (no alias!).
 *
 * CLIENT_TEST_SERVER must be set. <server>_1 must exist and (if
 * syncing via HTTP) have a deviceId that matches a remoteDeviceId in
 * the config used by syncevo-http-server.
 *
 * @return pair of <peer sync config> + <peer source name>; sync config name empty if not found
 */
std::pair<std::string, std::string> getPeerConfig(const std::string &source)
{
    static const char LOCAL_SYNC[] = "local://";
    SyncConfig local(currentServer() + "_1");
    std::vector<std::string> syncURLs = local.getSyncURL();
    boost::shared_ptr<PersistentSyncSourceConfig> sourceConfig(local.getSyncSourceConfig(source));
    std::string uri = sourceConfig->getURI();
    if (uri.empty()) {
        uri = source;
    }
    std::string peerConfig;
    if (syncURLs.size() == 1) {
        const std::string &syncURL = syncURLs.front();
        if (boost::starts_with(syncURL, LOCAL_SYNC)) {
            // Local sync. "target-config" is implied and may be relevant
            // later when using the peer source.
            peerConfig = syncURL.substr(strlen(LOCAL_SYNC));
            if (boost::starts_with(peerConfig, "@")) {
                peerConfig = "target-config" + peerConfig;
            }
        }
    }

    if (peerConfig.empty()) {
        // Check for local HTTP server.
        std::string deviceId = local.getDevID();
        BOOST_FOREACH (const StringPair &peer, SyncConfig::getConfigs()) {
            SyncConfig remote(peer.first);
            if (remote.getRemoteDevID() == deviceId) {
                peerConfig = peer.first;
                break;
            }
        }
    }

    return std::make_pair(peerConfig, uri);
}

/**
 * Tests involving a specific peer use testcases/synctests/<server
 * name>/<source name>/<test name>/<aspect>, where <aspect> is test
 * specific. The resulting string typically references a directory
 * with individual items. <aspect> can be empty.
 */
std::string getPeerTestdata(const std::string &source, const std::string &test, const std::string &aspect)
{
    std::string path = StringPrintf("testcases/synctests/%s/%s/%s/%s",
                                    currentServer().c_str(),
                                    source.c_str(),
                                    test.c_str(),
                                    aspect.c_str());
    return path;
}

/**
 * CLIENT_TEST_NUM_ITEMS env variable or 100
 */
int defNumItems()
{
    char *numitems = getenv("CLIENT_TEST_NUM_ITEMS");
    return numitems ? atoi(numitems) : 100;
}

static SyncMode RefreshFromPeerMode()
{
    return isServerMode() ? SYNC_REFRESH_FROM_CLIENT : SYNC_REFRESH_FROM_SERVER;
}

static SyncMode RefreshFromLocalMode()
{
    return isServerMode() ? SYNC_REFRESH_FROM_SERVER : SYNC_REFRESH_FROM_CLIENT;
}

static SyncMode OneWayFromPeerMode()
{
    return isServerMode() ? SYNC_ONE_WAY_FROM_CLIENT : SYNC_ONE_WAY_FROM_SERVER;
}

static SyncMode OneWayFromLocalMode()
{
    return isServerMode() ? SYNC_ONE_WAY_FROM_SERVER : SYNC_ONE_WAY_FROM_CLIENT;
}

/**
 * remove a certain property from buffer, return removed line
 */
static string stripProperty(std::string &data, const std::string &prop)
{
    std::string res;

    size_t start = data.find(prop);
    if (start != data.npos) {
        size_t end = data.find('\n', start);
        if (end != data.npos) {
            size_t len = end + 1 - start;
            res = data.substr(start, len);
            data.erase(start, len);
        }
    }

    return res;
}

/**
 * insert a property (must include line end) before the end of an item
 */
static void insertProperty(std::string &data,
                           const std::string &prop,
                           const std::string &endProp = "END:VEVENT")
{
    size_t pos = data.find(endProp);
    data.insert(pos, prop);
}

/**
 * remove parameter in all properties
 */
static void stripParameters(std::string &data,
                            const std::string &param)
{
    while (true) {
        size_t start = data.find(";" + param + "=");
        if (start == data.npos) {
            break;
        }
        size_t end = data.find_first_of(";:", start + 1);
        if (end == data.npos) {
            break;
        }
        data.erase(start, end - start);
    }
}

static void stripComponent(std::string &data,
                           const std::string &comp)
{
    size_t start = data.find("BEGIN:" + comp);
    if (start != data.npos) {
        size_t end = data.find("END:" + comp);
        if (end != data.npos) {
            end = data.find('\n', end);
            if (end != data.npos) {
                data.erase(start, end + 1 - start);
            }
        }
    }
}

/**
 * Using this pointer automates the open()/beginSync()/endSync()/close()
 * life cycle: it automatically calls these functions when a new
 * pointer is assigned or deleted.
 *
 * Anchors are stored globally in a hash which uses the tracking node
 * name as key. This name happens to be the unique file path that
 * is created for each source (see TestEvolution::createSource() and
 * SyncConfig::getSyncSourceNodes()).
 */
class TestingSyncSourcePtr : public std::auto_ptr<TestingSyncSource>
{
    typedef std::auto_ptr<TestingSyncSource> base_t;
    bool m_active;

    static StringMap m_anchors;
    static std::string m_testName;

public:
    TestingSyncSourcePtr() : m_active(false) {}
    TestingSyncSourcePtr(const TestingSyncSourcePtr &other) : m_active(false) {
        CPPUNIT_ASSERT(!other.get());
    }
    ~TestingSyncSourcePtr()
    {
        // We can skip the full cleanup if the test has already failed.
        // Also avoids letting an exception escape from the
        // destructor during exception handling (= program aborted!)
        // when the endSync() call invoked by reset() needs to
        // report a proble. CT_ASSERT_NO_THROW() itself catches that
        // exception, but then forwards it, and thus does not
        // prevent the exception from escaping.
        if (!std::uncaught_exception()) {
            CT_ASSERT_NO_THROW(reset(NULL));
        }
    }

    enum Flags {
        SLOW,           /**< erase anchor, start accessing database from scratch */
        INCREMENTAL     /**< allow source to do incremental data read */
    };

    void reset(TestingSyncSource *source = NULL, Flags flags = INCREMENTAL)
    {
        if (get() && m_active) {
            stopAccess();
        }
        // avoid deleting the instance that we are setting
        // (shouldn't happen)
        if (get() != source) {
            base_t::reset(source);
        }
        if (source) {
            startAccess(flags);
        }
    }

    /**
     * done automatically as part of reset(), only to be called
     * after an explicit stopAccess()
     */
    void startAccess(Flags flags = INCREMENTAL)
    {
        CT_ASSERT(get());
        CT_ASSERT(!m_active);
        int delay = atoi(getEnv("CLIENT_TEST_SOURCE_DELAY", "0"));
        if (delay) {
            CLIENT_TEST_LOG("CLIENT_TEST_SOURCE_DELAY: sleep for %d seconds", delay);
            sleep(delay);
        }
        CT_ASSERT_NO_THROW(get()->open());
        string node = get()->getTrackingNode()->getName();
        string anchor = m_anchors[node];
        if (flags == SLOW) {
            anchor = "";
        }
        get()->beginSync(anchor, "");
        if (isServerMode()) {
            CT_ASSERT_NO_THROW(get()->enableServerMode());
        }
        // the replaced m_endSession callback was invoked here,
        // which shouldn't have been necessary - not calling
        // m_endDataWrite post-signal at the moment
        // CT_ASSERT_NO_THROW(get()->getOperations().m_endDataWrite.getPostSignal()());
        m_active = true;
    }

    /**
     * finish change tracking, source must be activated again
     * with startAccess()
     */
    void stopAccess()
    {
        CT_ASSERT(get());
        CT_ASSERT(m_active);
        m_active = false;
        char *dummy = const_cast<char *>("testing-source");
        CT_ASSERT_NO_THROW(get()->getOperations().m_endDataWrite.getPostSignal()(*get(), OPERATION_FINISHED, sysync::LOCERR_OK, true, &dummy));
        string node = get()->getTrackingNode()->getName();
        string anchor;
        CT_ASSERT_NO_THROW(anchor = get()->endSync(true));
        m_anchors[node] = anchor;
        CT_ASSERT_NO_THROW(get()->close());
    }
};

StringMap TestingSyncSourcePtr::m_anchors;
std::string TestingSyncSourcePtr::m_testName;

bool SyncOptions::defaultWBXML()
{
    const char *t = getenv("CLIENT_TEST_XML");
    if (t && (!strcmp(t, "1") || !strcasecmp(t, "t"))) {
        // use XML
        return false;
    } else {
        return true;
    }
}

std::list<std::string> listItemsOfType(TestingSyncSource *source, int state)
{
    std::list<std::string> res;

    BOOST_FOREACH(const string &luid, source->getItems(SyncSourceChanges::State(state))) {
        res.push_back(luid);
    }
    return res;
}
static std::list<std::string> listNewItems(TestingSyncSource *source) { return listItemsOfType(source, SyncSourceChanges::NEW); }
static std::list<std::string> listUpdatedItems(TestingSyncSource *source) { return listItemsOfType(source, SyncSourceChanges::UPDATED); }
static std::list<std::string> listDeletedItems(TestingSyncSource *source) { return listItemsOfType(source, SyncSourceChanges::DELETED); }
static std::list<std::string> listItems(TestingSyncSource *source) { return listItemsOfType(source, SyncSourceChanges::ANY); }

static CppUnit::ItemCount countItemsOfType(TestingSyncSource *source, int type) { return source->getItems(SyncSourceChanges::State(type)); }
static CppUnit::ItemCount countNewItems(TestingSyncSource *source) { return countItemsOfType(source, SyncSourceChanges::NEW); }
static CppUnit::ItemCount countUpdatedItems(TestingSyncSource *source) { return countItemsOfType(source, SyncSourceChanges::UPDATED); }
static CppUnit::ItemCount countDeletedItems(TestingSyncSource *source) { return countItemsOfType(source, SyncSourceChanges::DELETED); }
static CppUnit::ItemCount countItems(TestingSyncSource *source) { return countItemsOfType(source, SyncSourceChanges::ANY); }

/** insert new item, return LUID */
static std::string importItem(TestingSyncSource *source, const ClientTestConfig &config, std::string &data)
{
    CT_ASSERT(source);
    if (data.size()) {
        SyncSourceRaw::InsertItemResult res;
        SOURCE_ASSERT_NO_FAILURE(source, res = source->insertItemRaw("", config.m_mangleItem(data, false, "")));
        CT_ASSERT(!res.m_luid.empty());
        return res.m_luid;
    } else {
        return "";
    }
}

/** overwrite existing item */
static void updateItem(TestingSyncSource *source, std::string &data, const std::string &luid)
{
    CT_ASSERT(source);
    CT_ASSERT(!data.empty());
    CT_ASSERT(!luid.empty());

    SyncSourceRaw::InsertItemResult res;
    SOURCE_ASSERT_NO_FAILURE(source, res = source->insertItemRaw(luid, data));
    CT_ASSERT_EQUAL(luid, res.m_luid);
}

/** remove existing item */
static void removeItem(TestingSyncSource *source, const std::string &luid)
{
    CT_ASSERT(source);
    CT_ASSERT(!luid.empty());

    SOURCE_ASSERT_NO_FAILURE(source, source->deleteItem(luid));
}

static void restoreStorage(const ClientTest::Config &config, ClientTest &client)
{
}

static void backupStorage(const ClientTest::Config &config, ClientTest &client)
{
}

/** adds the supported tests to the instance itself */
void LocalTests::addTests() {
    if (config.m_createSourceA) {
        ADD_TEST(LocalTests, testOpen);
        ADD_TEST(LocalTests, testIterateTwice);
        ADD_TEST(LocalTests, testDelete404);
        ADD_TEST(LocalTests, testReadItem404);
        if (!config.m_insertItem.empty()) {
            ADD_TEST(LocalTests, testSimpleInsert);
            ADD_TEST(LocalTests, testLocalDeleteAll);
            ADD_TEST(LocalTests, testComplexInsert);
            if (config.m_uniqueID) {
                ADD_TEST(LocalTests, testInsertTwice);
            }

            if (!config.m_updateItem.empty()) {
                ADD_TEST(LocalTests, testLocalUpdate);

                if (config.m_createSourceB) {
                    ADD_TEST(LocalTests, testChanges);
                    ADD_TEST(LocalTests, testChangesMultiCycles);

                    if (!config.m_linkedSources.empty()) {
                        ADD_TEST(LocalTests, testLinkedSources);
                    }
                }
            }

            if (config.m_import &&
                config.m_dump &&
                config.m_compare &&
                !config.m_testcases.empty()) {
                ADD_TEST(LocalTests, testImport);
                ADD_TEST(LocalTests, testImportDelete);
                if (!config.m_essentialProperties.empty()) {
                    ADD_TEST(LocalTests, testRemoveProperties);
                }
            }

            if (!config.m_templateItem.empty()) {
                ADD_TEST(LocalTests, testManyChanges);
            }

            // create a sub-suite for each set of linked items
            for (int i = 0; i < (int)config.m_linkedItems.size(); i++) {
                const ClientTestConfig::LinkedItems_t &items = config.m_linkedItems[i];
                CppUnit::TestSuite *linked = new CppUnit::TestSuite(getName() + "::LinkedItems" + items.m_name);
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsParent);
                if (config.m_linkedItemsRelaxedSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsChild);
                }
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsParentChild);
                if (items[1].find("RECURRENCE-ID") != items[1].npos &&
                    config.m_sourceKnowsItemSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsInsertBothUpdateChildNoIDs);
                }
                if (config.m_linkedItemsRelaxedSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsChildParent);
                }
                if (config.m_linkedItemsRelaxedSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsChildChangesParent);
                }
                if (config.m_linkedItemsRelaxedSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsRemoveParentFirst);
                }
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsRemoveNormal);
                if (config.m_sourceKnowsItemSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsInsertParentTwice);
                    if (config.m_linkedItemsRelaxedSemantic) {
                        ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsInsertChildTwice);
                    }
                }
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsParentUpdate);
                if (config.m_linkedItemsRelaxedSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsUpdateChild);
                    if (items[1].find("RECURRENCE-ID") != items[1].npos &&
                        config.m_sourceKnowsItemSemantic) {
                        ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsUpdateChildNoIDs);
                    }
                }
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsInsertBothUpdateChild);
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsInsertBothUpdateParent);

                // tests independent of data, only add to default item set
                if (i == 0) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsSingle404);
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsMany404);
                }
                addTest(linked);
            }

            // Create a sub-suite for each set of linked items.
            // items.size() can be fairly large for these tests,
            // so avoid testing all possible combinations.
            BOOST_FOREACH(const ClientTestConfig::LinkedItems_t &items,
                          config.m_linkedItemsSubset) {
                CppUnit::TestSuite *linked = new CppUnit::TestSuite(getName() + "::LinkedItems" + items.m_name);
                int stride = (items.size() + 4) / 5;
                for (int start = 0;
                     (size_t)start < items.size();
                     start += stride ) {
                    for (int skip = 0;
                         !skip || (size_t)(start + skip + 1) < items.size();
                         skip++) {
                        ADD_TEST_TO_SUITE_SUFFIX(linked, LocalTests, testSubset,
                                                 StringPrintf("Start%dSkip%d", start, skip));
                    }
                    // add a test which uses start, start + 1 and last item
                    // if that leads to a gap (EXDATE)
                    if (start > 0 && items.size() - start > 3) {
                        ADD_TEST_TO_SUITE_SUFFIX(linked, LocalTests, testSubset,
                                                 StringPrintf("Start%dExdate", start));
                    }
                }
                addTest(linked);
            }
        }
    }
}

std::string LocalTests::insert(CreateSource createSource, const std::string &data, bool relaxed, std::string *inserted, const std::string &uniqueUIDSuffix) {
    restoreStorage(config, client);

    // create source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));

    // count number of already existing items
    int numItems = 0;
    CT_ASSERT_NO_THROW(numItems = countItems(source.get()));
    SyncSourceRaw::InsertItemResult res;
    std::string mangled = config.m_mangleItem(data, false, uniqueUIDSuffix);
    if (inserted) {
        *inserted = mangled;
    }
    SOURCE_ASSERT_NO_FAILURE(source.get(), res = source->insertItemRaw("", mangled));
    CT_ASSERT(!res.m_luid.empty());

    bool updated = false;
    if (res.m_state == ITEM_NEEDS_MERGE) {
        // conflict detected, overwrite existing item as done in the past
        std::string luid = res.m_luid;
        SOURCE_ASSERT_NO_FAILURE(source.get(), res = source->insertItemRaw(luid, mangled));
        CT_ASSERT_EQUAL(luid, res.m_luid);
        CT_ASSERT(res.m_state == ITEM_OKAY);
        updated = true;
    }

    // delete source again
    CT_ASSERT_NO_THROW(source.reset());

    if (!relaxed) {
        // two possible results:
        // - a new item was added
        // - the item was matched against an existing one
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));
        CT_ASSERT_EQUAL(numItems + ((res.m_state == ITEM_REPLACED || res.m_state == ITEM_MERGED || updated) ? 0 : 1),
                        countItems(source.get()));
        CT_ASSERT_EQUAL(0, countNewItems(source.get()));
        CT_ASSERT_EQUAL(0, countUpdatedItems(source.get()));
        CT_ASSERT_EQUAL(0, countDeletedItems(source.get()));
    }
    backupStorage(config, client);

    return res.m_luid;
}

/** deletes specific item locally via sync source */
static std::string updateItem(CreateSource createSource, const ClientTestConfig &config, const std::string &uid, const std::string &data, std::string *updated = NULL) {
    std::string newuid;

    CT_ASSERT(createSource.createSource);

    // create source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));

    // insert item
    SyncSourceRaw::InsertItemResult res;
    std::string mangled;
    CT_ASSERT_NO_THROW(mangled = config.m_mangleItem(data, true, ""));
    if (updated) {
        *updated = mangled;
    }
    SOURCE_ASSERT_NO_FAILURE(source.get(), res = source->insertItemRaw(uid, mangled.c_str()));
    SOURCE_ASSERT(source.get(), !res.m_luid.empty());

    return res.m_luid;
}

/** updates specific item locally via sync source */
static void removeItem(CreateSource createSource, const std::string &luid)
{
    CT_ASSERT(createSource.createSource);

    // create source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));

    // remove item
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->deleteItem(luid));
}

void LocalTests::update(CreateSource createSource, const std::string &data, bool check, const std::string &uniqueUIDSuffix) {
    CT_ASSERT(createSource.createSource);

    restoreStorage(config, client);

    // create source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));

    // get existing item, then update it
    SyncSourceChanges::Items_t::const_iterator it;
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getAllItems().begin());
    CT_ASSERT(it != source->getAllItems().end());
    string luid = *it;
    SyncSourceRaw::InsertItemResult res;
    std::string mangled = config.m_mangleItem(data, true, uniqueUIDSuffix);
    SOURCE_ASSERT_NO_FAILURE(source.get(), res = source->insertItemRaw(luid, mangled));
    CT_ASSERT_NO_THROW(source.reset());
    CT_ASSERT_EQUAL(luid, res.m_luid);
    CT_ASSERT_EQUAL(ITEM_OKAY, res.m_state);

    if (!check) {
        return;
    }

    // check that the right changes are reported when reopening the source
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));
    CT_ASSERT_EQUAL(1, countItems(source.get()));
    CT_ASSERT_EQUAL(0, countNewItems(source.get()));
    CT_ASSERT_EQUAL(0, countUpdatedItems(source.get()));
    CT_ASSERT_EQUAL(0, countDeletedItems(source.get()));
    
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getAllItems().begin());
    CT_ASSERT(it != source->getAllItems().end());
    CT_ASSERT_EQUAL(luid, *it);

    backupStorage(config, client);
}

void LocalTests::update(CreateSource createSource, const std::string &data, const std::string &luid) {
    CT_ASSERT(createSource.createSource);

    restoreStorage(config, client);
    // create source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));

    // update it
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->insertItemRaw(luid, config.m_mangleItem(data, true, "")));

    backupStorage(config, client);
}

/** deletes all items locally via sync source */
void LocalTests::deleteAll(CreateSource createSource) {
    CT_ASSERT(createSource.createSource);

    restoreStorage(config, client);
    // create source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));

    // delete all items
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->removeAllItems());
    CT_ASSERT_NO_THROW(source.reset());

    // check that all items are gone
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));
    SOURCE_ASSERT_MESSAGE(
        "should be empty now",
        source.get(),
        countItems(source.get()) == 0);
    CT_ASSERT_EQUAL( 0, countNewItems(source.get()) );
    CT_ASSERT_EQUAL( 0, countUpdatedItems(source.get()) );
    CT_ASSERT_EQUAL( 0, countDeletedItems(source.get()) );
    backupStorage(config, client);
}

/** deletes specific item locally via sync source */
static void deleteItem(CreateSource createSource, const std::string &uid) {
    CT_ASSERT(createSource.createSource);

    // create source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));

    // delete item
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->deleteItem(uid));
}

/**
 * takes two databases, exports them,
 * then compares them using synccompare
 *
 * @param refFile      existing file with source reference items, NULL uses a dump of sync source A instead
 * @param copy         a sync source which contains the copied items, begin/endSync will be called
 * @param raiseAssert  raise assertion if comparison yields differences (defaults to true)
 */
bool LocalTests::compareDatabases(const char *refFile, TestingSyncSource &copy, bool raiseAssert) {
    CT_ASSERT(config.m_dump);

    std::string sourceFile, copyFile;

    if (refFile) {
        sourceFile = refFile;
    } else {
        sourceFile = getCurrentTest() + ".A.test.dat";
        simplifyFilename(sourceFile);
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, config.m_dump(client, *source.get(), sourceFile));
        CT_ASSERT_NO_THROW(source.reset());
    }

    copyFile = getCurrentTest() + ".B.test.dat";
    simplifyFilename(copyFile);
    SOURCE_ASSERT_EQUAL(&copy, 0, config.m_dump(client, copy, copyFile));

    return compareDatabases(sourceFile, copyFile);
}

bool LocalTests::compareDatabases(const std::string &refFile, const std::string &actualFile, bool raiseAssert)
{
    bool equal = false;
    CT_ASSERT_NO_THROW(equal = config.m_compare(client, refFile.c_str(), actualFile.c_str()));
    CT_ASSERT(!raiseAssert || equal);

    return equal;
}

/**
 * compare data in source with vararg list of std::string pointers, NULL terminated
 */
void LocalTests::compareDatabases(TestingSyncSource &copy,
                                  ...)
{
    std::string sourceFile = getCurrentTest() + ".ref.test.dat";
    simplifyFilename(sourceFile);
    ofstream out(sourceFile.c_str());
    va_list ap;
    va_start(ap, copy);
    std::string *item;
    while ((item = va_arg(ap, std::string *)) != NULL) {
        out << *item;
    }
    va_end(ap);
    out.close();
    compareDatabases(sourceFile.c_str(), copy);
}

void LocalTests::compareDatabasesRef(TestingSyncSource &copy,
                                     const std::list<std::string> &items)
{
    std::string sourceFile = getCurrentTest() + ".ref.test.dat";
    simplifyFilename(sourceFile);
    ofstream out(sourceFile.c_str());
    BOOST_FOREACH(const std::string &item, items) {
        out << item;
    }
    out.close();
    compareDatabases(sourceFile.c_str(), copy);
}


std::string LocalTests::createItem(int item, const std::string &revision, int size)
{
    std::string data = config.m_mangleItem(config.m_templateItem, false, "");
    std::stringstream prefix;

    // string to be inserted at start of unique properties;
    // avoid adding white space (not sure whether it is valid for UID)
    prefix << std::setfill('0') << std::setw(3) << item << "-";

    BOOST_FOREACH (std::string curProp,
                   boost::tokenizer< boost::char_separator<char> >(config.m_uniqueProperties,
                                                                   boost::char_separator<char>(":"))) {
        std::string property;
        // property is expected to not start directly at the
        // beginning
        property = "\n";
        property += curProp;
        property += ":";
        size_t off = data.find(property);
        if (off != data.npos) {
            data.insert(off + property.size(), prefix.str());
        }
    }
    boost::replace_all(data, "<<UNIQUE>>", prefix.str());
    boost::replace_all(data, "<<REVISION>>", revision);
    if (size > 0 && (int)data.size() < size) {
        int additionalBytes = size - (int)data.size();
        int added = 0;
        /* vCard 2.1 and vCal 1.0 need quoted-printable line breaks */
        bool quoted = data.find("VERSION:1.0") != data.npos ||
            data.find("VERSION:2.1") != data.npos;
        size_t toreplace = 1;

        CT_ASSERT(!config.m_sizeProperty.empty());

        /* stuff the item so that it reaches at least that size */
        size_t off = data.find(config.m_sizeProperty);
        CT_ASSERT(off != data.npos);
        std::stringstream stuffing;
        if (quoted) {
            stuffing << ";ENCODING=QUOTED-PRINTABLE:";
        } else {
            stuffing << ":";
        }

        // insert after the first line, it often acts as the summary
        if (data.find("BEGIN:VJOURNAL") != data.npos) {
            size_t start = data.find(":", off);
            CT_ASSERT( start != data.npos );
            size_t eol = data.find("\\n", off);
            CT_ASSERT( eol != data.npos );
            stuffing << data.substr(start + 1, eol - start + 1);
            toreplace += eol - start + 1;
        }

        while(added < additionalBytes) {
            int linelen = 0;

            while(added + 4 < additionalBytes &&
                  linelen < 60) {
                stuffing << 'x';
                added++;
                linelen++;
            }
            // insert line breaks to allow folding
            if (quoted) {
                stuffing << "x=0D=0Ax";
                added += 8;
            } else {
                stuffing << "x\\nx";
                added += 4;
            }
        }
        off = data.find(":", off);
        data.replace(off, toreplace, stuffing.str());
    }

    return data;
}


/**
 * insert artificial items, number of them 100
 * unless passed explicitly
 *
 * @param createSource    a factory for the sync source that is to be used
 * @param startIndex      IDs are generated starting with this value
 * @param numItems        number of items to be inserted if non-null, otherwise config.numItems is used
 * @param size            minimum size for new items
 * @return LUIDs of all inserted items
 */
std::list<std::string> LocalTests::insertManyItems(CreateSource createSource, int startIndex, int numItems, int size) {
    std::list<std::string> luids;

    CT_ASSERT(!config.m_templateItem.empty());

    restoreStorage(config, client);
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    CT_ASSERT(startIndex > 1 || !countItems(source.get()));

    int firstIndex = startIndex;
    if (firstIndex < 0) {
        firstIndex = 1;
    }
    int lastIndex = firstIndex + (numItems >= 1 ? numItems : defNumItems()) - 1;
    for (int item = firstIndex; item <= lastIndex; item++) {
        std::string data = createItem(item, "", size);
        luids.push_back(importItem(source.get(), config, data));
    }
    backupStorage(config, client);

    return luids;
}

std::list<std::string> LocalTests::insertManyItems(TestingSyncSource *source, int startIndex, int numItems, int size) {
    std::list<std::string> luids;

    CT_ASSERT(!config.m_templateItem.empty());

    CT_ASSERT(startIndex > 1 || !countItems(source));
    int firstIndex = startIndex;
    if (firstIndex < 0) {
        firstIndex = 1;
    }
    int lastIndex = firstIndex + (numItems >= 1 ? numItems : defNumItems()) - 1;
    for (int item = firstIndex; item <= lastIndex; item++) {
        std::string data = createItem(item, "", size);
        luids.push_back(importItem(source, config, data));
    }

    return luids;
}

void LocalTests::updateManyItems(CreateSource createSource, int startIndex, int numItems, int size,
                                 int revision,
                                 std::list<std::string> &luids,
                                 int offset)
{
    CT_ASSERT(!config.m_templateItem.empty());

    restoreStorage(config, client);
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));

    int firstIndex = startIndex;
    if (firstIndex < 0) {
        firstIndex = 1;
    }
    int lastIndex = firstIndex + (numItems >= 1 ? numItems : defNumItems()) - 1;
    std::string revstring = StringPrintf("REVISION #%d", revision);
    std::list<std::string>::const_iterator it = luids.begin();
    for (int i = 0; i < offset && it != luids.end(); i++, ++it) {}
    for (int item = firstIndex;
         item <= lastIndex && it != luids.end();
         item++, ++it) {
        std::string data = createItem(item, revstring, size);
        updateItem(source.get(), data, *it);
    }
    backupStorage(config, client);
}

void LocalTests::removeManyItems(CreateSource createSource, int numItems,
                                 std::list<std::string> &luids,
                                 int offset)
{
    restoreStorage(config, client);
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));

    std::list<std::string>::const_iterator it = luids.begin();
    for (int i = 0; i < offset && it != luids.end(); i++, ++it) {}
    for (int item = 0;
         item < numItems && it != luids.end();
         item++, ++it) {
        removeItem(source.get(), *it);
    }
    backupStorage(config, client);
}

// update every single item in the database
void LocalTests::updateData(CreateSource createSource) {
    // check additional requirements
    CT_ASSERT(config.m_update);

    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));
    BOOST_FOREACH(const string &luid, source->getAllItems()) {
        string item;
        CT_ASSERT_NO_THROW(source->readItemRaw(luid, item));
        CT_ASSERT_NO_THROW(config.m_update(item));
        CT_ASSERT_NO_THROW(source->insertItemRaw(luid, item));
    }
    CT_ASSERT_NO_THROW(source.reset());
}


// creating sync source
void LocalTests::testOpen() {
    // check requirements
    CT_ASSERT(config.m_createSourceA);

    // Intentionally use the plain auto_ptr here and
    // call open directly. That way it is a bit more clear
    // what happens and where it fails, if it fails.
    std::auto_ptr<TestingSyncSource> source;
    CT_ASSERT_NO_THROW(source.reset(createSourceA()));
    // got a sync source?
    CT_ASSERT(source.get() != 0);
    // can it be opened?
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->open());
    // delete it
    CT_ASSERT_NO_THROW(source.reset());
}

// restart scanning of items
void LocalTests::testIterateTwice() {
    // check requirements
    CT_ASSERT(config.m_createSourceA);

    // open source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    SOURCE_ASSERT_MESSAGE(
        "iterating twice should produce identical results",
        source.get(),
        countItems(source.get()) == countItems(source.get()));
}

// deleteItem() must raise 404 for unknown item
void LocalTests::testDelete404() {
    // check requirements
    CT_ASSERT(config.m_createSourceA);

    // open source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    SyncMLStatus status = STATUS_OK;
    try {
        source->deleteItem("no-such-item");
    } catch (const StatusException &ex) {
        status = ex.syncMLStatus();
    }
    CT_ASSERT_EQUAL(STATUS_NOT_FOUND, status);
}

// deleteItem() must raise 404 for unknown item
void LocalTests::testReadItem404() {
    // check requirements
    CT_ASSERT(config.m_createSourceA);

    // open source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    SyncMLStatus status = STATUS_OK;
    try {
        std::string data;
        source->readItem("no-such-item", data);
    } catch (const StatusException &ex) {
        status = ex.syncMLStatus();
    }
    CT_ASSERT_EQUAL(STATUS_NOT_FOUND, status);
}

void LocalTests::doInsert(bool withUID)
{
    // check requirements
    CT_ASSERT(!config.m_insertItem.empty());
    CT_ASSERT(!config.m_createSourceA.empty());

    std::string item = config.m_insertItem;
    if (!withUID) {
        CT_ASSERT_NO_THROW(stripProperty(item, "UID"));
    }
    CT_ASSERT_NO_THROW(insert(createSourceA, item));
}

// insert one contact without clearing the source first
void LocalTests::testSimpleInsert() {
    // Insert the item without the UID. There is no guarantee that the
    // item wasn't already inserted before (database not cleaned by
    // test) and some backends (CalDAV) will catch an attempt to
    // add the same item twice.
    doInsert(false);
}

// delete all items
void LocalTests::testLocalDeleteAll() {
    // check requirements
    CT_ASSERT(!config.m_insertItem.empty());
    CT_ASSERT(config.m_createSourceA);

    // make sure there is something to delete, then delete again
    std::string item = config.m_insertItem;
    CT_ASSERT_NO_THROW(stripProperty(item, "UID"));
    CT_ASSERT_NO_THROW(insert(createSourceA, item));
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
}

// clean database, then insert
void LocalTests::testComplexInsert() {
    CT_ASSERT(config.m_createSourceA);
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    CT_ASSERT_NO_THROW(doInsert());
    CT_ASSERT_NO_THROW(testIterateTwice());
}

// insert the same item (identified by UID) twice => either
// ITEM_NEEDS_MERGE, ITEM_REPLACED or ITEM_MERGED are acceptable
void LocalTests::testInsertTwice() {
    CT_ASSERT(config.m_createSourceA);
    CT_ASSERT(!config.m_insertItem.empty());
    CT_ASSERT(config.m_insertItem.find("\nUID:") != std::string::npos);
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    // create source
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));

    // mangle data once
    std::string data = config.m_mangleItem(config.m_insertItem, false, "");

    // insert new item
    SyncSourceRaw::InsertItemResult first;
    SOURCE_ASSERT_NO_FAILURE(source.get(), first = source->insertItemRaw("", data));
    CT_ASSERT_EQUAL(ITEM_OKAY, first.m_state);

    // and again
    SyncSourceRaw::InsertItemResult second;
    SOURCE_ASSERT_NO_FAILURE(source.get(), second = source->insertItemRaw("", data));
    CLIENT_TEST_LOG("item %s",
                    second.m_state == ITEM_NEEDS_MERGE ? "needs to be merged" :
                    second.m_state == ITEM_REPLACED ? "was replaced" :
                    second.m_state == ITEM_MERGED ? "was merged" :
                    second.m_state == ITEM_OKAY ? "was added, which is broken!" :
                    "unknown result ?!");
    if (config.m_sourceKnowsItemSemantic) {
        CT_ASSERT(second.m_state == ITEM_NEEDS_MERGE || second.m_state == ITEM_REPLACED || second.m_state == ITEM_MERGED);
        CT_ASSERT_EQUAL(first.m_luid, second.m_luid);
    } else {
        CT_ASSERT(second.m_state == ITEM_OKAY);
        CT_ASSERT(first.m_luid != second.m_luid);
    }
    if (second.m_state == ITEM_REPLACED || second.m_state == ITEM_MERGED) {
        CT_ASSERT(first.m_revision != second.m_revision);
    }
}

// clean database, insert item, update it
void LocalTests::testLocalUpdate() {
    // check additional requirements
    CT_ASSERT(!config.m_updateItem.empty());
    CT_ASSERT(config.m_createSourceA);

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    CT_ASSERT_NO_THROW(doInsert());
    CT_ASSERT_NO_THROW(update(createSourceA, config.m_updateItem));
}

// Complex sequence of changes, with one restarted instance of source
// B to observe the changes or multiple instances of it.
// Changes are made both via source A and via source B itself.
void LocalTests::doChanges(bool restart) {
    SyncSourceChanges::Items_t::const_iterator it, it2;

    // check additional requirements
    CT_ASSERT(config.m_createSourceB);
    CT_ASSERT(config.m_createSourceA);

    CLIENT_TEST_LOG("clean via source A");
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    CLIENT_TEST_LOG("insert item via source A");
    CT_ASSERT_NO_THROW(doInsert());

    CLIENT_TEST_LOG("clean changes in sync source B by creating and closing it");
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());

    CLIENT_TEST_LOG("no new changes now in source B");
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    string item;
    string luid;
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getAllItems().begin());
    CT_ASSERT(it != source->getAllItems().end());
    luid = *it;
    // It is not required for incremental syncing that sources must be
    // able to return unchanged items. For example, ActiveSyncSource doesn't support
    // it because it gets only IDs and data of added or updated items.
    // Don't test it.
    // SOURCE_ASSERT_NO_FAILURE(source.get(), source->readItem(*it, item));
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());

    CLIENT_TEST_LOG("delete item again via sync source A");
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    CLIENT_TEST_LOG("check for deleted item via source B");
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getDeletedItems().begin());
    CT_ASSERT(it != source->getDeletedItems().end());
    CT_ASSERT(!it->empty());
    CT_ASSERT_EQUAL(luid, *it);
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());

    // Now make changes via source B directly: these changes are not to be
    // reported back. Google CalDAV is very strict about UID/SEQUENCE,
    // work around that by not reusing a UID inside this test.
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    // add
    std::string mangled = config.m_mangleItem(config.m_insertItem, false, "-B");
    SyncSourceRaw::InsertItemResult res;
    SOURCE_ASSERT_NO_FAILURE(source.get(), res = source->insertItemRaw("", mangled));
    CT_ASSERT(!res.m_luid.empty());
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());
    // update
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    mangled = config.m_mangleItem(config.m_updateItem, false, "-B");
    SOURCE_ASSERT_NO_FAILURE(source.get(), res = source->insertItemRaw(res.m_luid, mangled));
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());
    // delete
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->deleteItem(res.m_luid));
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());

    CLIENT_TEST_LOG("insert another item via source A");
    CT_ASSERT_NO_THROW(insert(createSourceA, config.m_insertItem, false, NULL, "-C"));
    CLIENT_TEST_LOG("check for new item via source B");
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getAllItems().begin());
    CT_ASSERT(it != source->getAllItems().end());
    luid = *it;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->readItem(*it, item));
    string newItem;
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getNewItems().begin());
    CT_ASSERT(it != source->getNewItems().end());
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->readItem(*it, item));
    CT_ASSERT_EQUAL(luid, *it);
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());

    CLIENT_TEST_LOG("update item via source A");
    CT_ASSERT_NO_THROW(update(createSourceA, config.m_updateItem, "-C"));
    CLIENT_TEST_LOG("check for updated item via source B");
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    string updatedItem;
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getUpdatedItems().begin());
    CT_ASSERT(it != source->getUpdatedItems().end());
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->readItem(*it, updatedItem));
    CT_ASSERT_EQUAL(luid, *it);
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());

    CLIENT_TEST_LOG("one item, no changes in source B");
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());

    CLIENT_TEST_LOG("start anew in both sources");
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());
    CLIENT_TEST_LOG("create and update an item in source A");
    // Must use a UID different than the one used before despite that data being gone,
    // to keep Google CalDAV server happy.
    CT_ASSERT_NO_THROW(insert(createSourceA, config.m_insertItem, false, NULL, "-D"));
    CT_ASSERT_NO_THROW(update(createSourceA, config.m_updateItem, "-D"));
    CLIENT_TEST_LOG("should only be listed as new or updated in source B, but not both");
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()) + countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());

    CLIENT_TEST_LOG("start anew once more in both sources");
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    CT_ASSERT_NO_THROW(restart ? source.stopAccess() : source.reset());
    CLIENT_TEST_LOG("create, delete and recreate an item in source A");
    CT_ASSERT_NO_THROW(insert(createSourceA, config.m_insertItem, false, NULL, "-E"));
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    CT_ASSERT_NO_THROW(insert(createSourceA, config.m_insertItem, false, NULL, "-F"));
    CLIENT_TEST_LOG("should only be listed as new or updated in source B, even if\n "
                    "(as for calendar with UID) the same LUID gets reused");
    SOURCE_ASSERT_NO_FAILURE(source.get(), restart ? source.startAccess() : source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()) + countUpdatedItems(source.get()));
    if (countDeletedItems(source.get()) == 1) {
        // It's not nice, but acceptable to send the LUID of a deleted item to a
        // server which has never seen that LUID. The LUID must not be the same as
        // the one we list as new or updated, though.
        SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getDeletedItems().begin());
        CT_ASSERT(it != source->getDeletedItems().end());
        SOURCE_ASSERT_NO_FAILURE(source.get(), it2 = source->getNewItems().begin());
        if (it2 == source->getNewItems().end()) {
            SOURCE_ASSERT_NO_FAILURE(source.get(), it2 = source->getUpdatedItems().begin());
            CT_ASSERT(it2 != source->getUpdatedItems().end());
        }
        CT_ASSERT(*it != *it2);
    } else {
        SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    }

    CT_ASSERT_NO_THROW(source.reset());
}

// complex sequence of changes, with source B instantiated anew
// after each change
void LocalTests::testChanges()
{
    doChanges(false);
}

// complex sequence of changes, with source B only instantiated once
// and restarted multiple times
void LocalTests::testChangesMultiCycles()
{
    doChanges(true);
}

// Make changes in one source and verify that other linked
// sources do not see and report any changes in their view
// of the shared database. Source A of the other sources
// is created once and is restarted, source B is created
// from scratch after each change.
void LocalTests::testLinkedSources()
{
    // make changes in each of the sources (doesn't have t be
    // the current one)
    BOOST_FOREACH (LocalTests *main, m_linkedSources) {
        CLIENT_TEST_LOG("making changes in %s", main->getSourceName().c_str());

        // first delete via *all* sources
        BOOST_FOREACH (LocalTests *test, m_linkedSources) {
            CLIENT_TEST_LOG("clean via source A of %s", test->getSourceName().c_str());
            CT_ASSERT_NO_THROW(test->deleteAll(test->createSourceA));
            // reset change tracking in source B
            TestingSyncSourcePtr sourceB;
            SOURCE_ASSERT_NO_FAILURE(sourceB.get(), sourceB.reset(test->createSourceB()));
        }

        std::map<std::string, TestingSyncSourcePtr> sourcesA;
        BOOST_FOREACH (LocalTests *test, m_linkedSources) {
            if (test == main) {
                continue;
            }
            TestingSyncSourcePtr &source = sourcesA[test->getSourceName()];
            CLIENT_TEST_LOG("creating source A of %s", test->getSourceName().c_str());
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(test->createSourceA()));
            CT_ASSERT_NO_THROW(source.stopAccess());
        }

        // insert one item
        CLIENT_TEST_LOG("inserting into %s", main->getSourceName().c_str());
        CT_ASSERT_NO_THROW(main->doInsert());
        BOOST_FOREACH (LocalTests *test, m_linkedSources) {
            if (test == main) {
                continue;
            }
            TestingSyncSourcePtr &source = sourcesA[test->getSourceName()];
            CLIENT_TEST_LOG("checking %s after insertion into %s",
                            test->getSourceName().c_str(),
                            main->getSourceName().c_str());
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.startAccess());
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.stopAccess());

            TestingSyncSourcePtr sourceB;
            SOURCE_ASSERT_NO_FAILURE(sourceB.get(), sourceB.reset(test->createSourceB()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countItems(sourceB.get()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countNewItems(sourceB.get()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countUpdatedItems(sourceB.get()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countDeletedItems(sourceB.get()));
        }

        // update one item
        CLIENT_TEST_LOG("updating in %s", main->getSourceName().c_str());
        CT_ASSERT_NO_THROW(main->update(main->createSourceA, main->config.m_updateItem));
        BOOST_FOREACH (LocalTests *test, m_linkedSources) {
            if (test == main) {
                continue;
            }
            TestingSyncSourcePtr &source = sourcesA[test->getSourceName()];
            CLIENT_TEST_LOG("checking %s after update into %s",
                            test->getSourceName().c_str(),
                            main->getSourceName().c_str());
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.startAccess());
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.stopAccess());

            TestingSyncSourcePtr sourceB;
            SOURCE_ASSERT_NO_FAILURE(sourceB.get(), sourceB.reset(test->createSourceB()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countItems(sourceB.get()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countNewItems(sourceB.get()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countUpdatedItems(sourceB.get()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countDeletedItems(sourceB.get()));
        }

        // delete one item
        CLIENT_TEST_LOG("deleting in %s", main->getSourceName().c_str());
        CT_ASSERT_NO_THROW(main->deleteAll(main->createSourceA));
        BOOST_FOREACH (LocalTests *test, m_linkedSources) {
            if (test == main) {
                continue;
            }
            TestingSyncSourcePtr &source = sourcesA[test->getSourceName()];
            CLIENT_TEST_LOG("checking %s after delete in %s",
                            test->getSourceName().c_str(),
                            main->getSourceName().c_str());
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.startAccess());
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.stopAccess());

            TestingSyncSourcePtr sourceB;
            SOURCE_ASSERT_NO_FAILURE(sourceB.get(), sourceB.reset(test->createSourceB()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countItems(sourceB.get()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countNewItems(sourceB.get()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countUpdatedItems(sourceB.get()));
            SOURCE_ASSERT_EQUAL(sourceB.get(), 0, countDeletedItems(sourceB.get()));
        }
    }
}

// clean database, import file, then export again and compare
void LocalTests::doImport(const std::string &testcases) {
    // check additional requirements
    CT_ASSERT(config.m_import);
    CT_ASSERT(config.m_dump);
    CT_ASSERT(config.m_compare);
    CT_ASSERT(!testcases.empty());
    CT_ASSERT(config.m_createSourceA);

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    // import via sync source A
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    restoreStorage(config, client);
    std::string actualData;
    std::string importFailures = config.m_import(client, *source.get(), config, testcases, actualData, NULL);
    backupStorage(config, client);
    CT_ASSERT_NO_THROW(source.reset());

    // export again and compare against original file,
    // without relying on change tracking (because
    // Google ActiveSync has problems with Fetch,
    // which would be needed for a data dump whenr
    // using the incremental approach)
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceA(), TestingSyncSourcePtr::SLOW));
    bool equal = compareDatabases(actualData.c_str(), *copy.get(), false);
    CT_ASSERT_NO_THROW(source.reset());

    if (importFailures.empty()) {
        CT_ASSERT_MESSAGE("imported and exported data equal", equal);
    } else {
        CT_ASSERT_EQUAL(std::string(""), importFailures);
    }
}

void LocalTests::testImport() {
    doImport(config.m_testcases);
}

// same as testImport() with immediate delete
void LocalTests::testImportDelete() {
    CT_ASSERT_NO_THROW(testImport());

    // delete again, because it was observed that this did not
    // work right with calendars in SyncEvolution
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
}

// clean database, import file, update with minimized test data (= all
// non-essential properties removed), compare: verifies that updates
// can remove data
void LocalTests::testRemoveProperties() {
    // check additional requirements
    CT_ASSERT(config.m_import);
    CT_ASSERT(config.m_dump);
    CT_ASSERT(config.m_compare);
    CT_ASSERT(!config.m_testcases.empty());
    CT_ASSERT(config.m_createSourceA);

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    // import via sync source A
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    restoreStorage(config, client);
    std::string testcases;
    std::list<std::string> luids;
    std::string importFailures = config.m_import(client, *source.get(), config, config.m_testcases, testcases, &luids);
    backupStorage(config, client);
    CT_ASSERT_NO_THROW(source.reset());

    // don't check for correct importing - that is done in testImport

    // reduce data
    std::list<std::string> items;
    std::string dummy;
    CT_ASSERT_NO_THROW(ClientTest::getItems(testcases, items, dummy));
    static const pcrecpp::RE bodyre("^BEGIN:(VCARD|VEVENT|VTODO|VJOURNAL)\\r?\\n(.*)^(END:\\g1)",
                                    pcrecpp::RE_Options().set_multiline(true).set_dotall(true));
    std::string updated = getCurrentTest();
    updated += ".updated.";
    updated += config.m_sourceName;
    updated += ".dat";
    simplifyFilename(updated);
    ofstream out(updated.c_str());

    BOOST_FOREACH (std::string &item, items) {
        std::string kind;
        pcrecpp::StringPiece body;
        CT_ASSERT(bodyre.PartialMatch(item, &kind, &body));
        static const pcrecpp::RE propre("^((\\S[^;:]*).*\\n(?:\\s.*\\n)*)",
                                        pcrecpp::RE_Options().set_multiline(true));
        pcrecpp::StringPiece input(body);
        pcrecpp::StringPiece prop;
        std::string propname;
        std::list<std::string> result;
        while (propre.Consume(&input, &prop, &propname)) {
            if (config.m_essentialProperties.find(propname) != config.m_essentialProperties.end()) {
                result.push_back(prop.as_string());
            }
        }

        item.replace(body.data() - item.c_str(), body.size(),
                     boost::join(result, ""));
        out << item << "\n";
    }
    out.close();

    // update
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    std::string updateFailures = config.m_import(client, *source.get(), config, updated, dummy, &luids);
    CT_ASSERT_NO_THROW(source.reset());

    // compare
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceA(), TestingSyncSourcePtr::SLOW));
    std::auto_ptr<ScopedEnvChange> envProps;
    if (currentServer() == "googlecontacts") {
        // Google CardDAV server does not remove X- properties when
        // they are not sent at all. TODO (?): send them as empty
        // properties.
        envProps.reset(new ScopedEnvChange("CLIENT_TEST_STRIP_PROPERTIES", "(X-.*)"));
    }
    bool equal = compareDatabases(updated.c_str(), *copy.get(), false);
    CT_ASSERT_NO_THROW(source.reset());

    if (importFailures.empty() && updateFailures.empty()) {
        CT_ASSERT_MESSAGE("imported and exported data equal", equal);
    } else {
        CT_ASSERT_EQUAL(std::string(""), importFailures + updateFailures);
    }
}

// test change tracking with large number of items
void LocalTests::testManyChanges() {
    // check additional requirements
    CT_ASSERT(!config.m_templateItem.empty());

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    // check that everything is empty, also resets change counter of sync source B
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // now insert plenty of items
    int numItems;
    CT_ASSERT_NO_THROW(numItems = insertManyItems(createSourceA).size());

    // check that exactly this number of items is listed as new
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), numItems, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), numItems, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // delete all items
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    // verify again
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), numItems, countDeletedItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());
}

template<class T, class V> int countEqual(const T &container,
                                          const V &value) {
    return count(container.begin(),
                 container.end(),
                 value);
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsParent() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // now insert main item
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));

    // check that exactly the parent is listed as new
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    // delete all items
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    // verify again
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsChild() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // same as above for child item
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsParentChild() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // insert parent first, then child
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (config.m_supportsReccurenceEXDates) {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
        CLIENT_TEST_LOG("retrieve parent as reported to the Synthesis engine, check for X-SYNCEVOLUTION-EXDATE-DETACHED");
        std::string parentDataEngine;
        CT_ASSERT_NO_THROW(source->readItem(parent, parentDataEngine));
        size_t pos = childData.find("RECURRENCE-ID");
        CT_ASSERT(pos != childData.npos);
        size_t end = childData.find_first_of("\r\n", pos);
        CT_ASSERT(end != childData.npos);
        std::string exdate = childData.substr(pos, end - pos);
        boost::replace_first(exdate, "RECURRENCE-ID", "X-SYNCEVOLUTION-EXDATE-DETACHED");
        // not generated because not needed by Synthesis engine
        boost::replace_first(exdate, ";VALUE=DATE", "");
        pos = parentDataEngine.find(exdate);
        CT_ASSERT_MESSAGE(exdate + " not found in:\n" + parentDataEngine, pos != parentDataEngine.npos);
    }

    if (config.m_supportsReccurenceEXDates) {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
        CLIENT_TEST_LOG("retrieve parent as reported to the Synthesis engine, check for X-SYNCEVOLUTION-EXDATE-DETACHED");
        std::string parentDataEngine;
        CT_ASSERT_NO_THROW(source->readItem(parent, parentDataEngine));
        size_t pos = childData.find("RECURRENCE-ID");
        CT_ASSERT(pos != childData.npos);
        size_t end = childData.find_first_of("\r\n", pos);
        CT_ASSERT(end != childData.npos);
        std::string exdate = childData.substr(pos, end - pos);
        boost::replace_first(exdate, "RECURRENCE-ID", "X-SYNCEVOLUTION-EXDATE-DETACHED");
        // not generated because not needed by Synthesis engine
        boost::replace_first(exdate, ";VALUE=DATE", "");
        pos = parentDataEngine.find(exdate);
        CT_ASSERT_MESSAGE(exdate + " not found in:\n" + parentDataEngine, pos != parentDataEngine.npos);
    }

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsChildParent() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // insert child first, then parent
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &parentData));
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], true, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsChildChangesParent() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // insert child first, check changes, then insert the parent
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], true, &parentData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listNewItems(copy.get()), parent));
    }
    // relaxed semantic: the child item might be considered updated now if
    // it had to be modified when inserting the parent
    SOURCE_ASSERT(copy.get(), 1 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsRemoveParentFirst() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // insert both items, remove parent, then child
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    CT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    // deleting the parent may or may not modify the child
    SOURCE_ASSERT(copy.get(), 1 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
    CT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsRemoveNormal() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr source, copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // insert both items, remove child, then parent
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    CT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    // The removal of the child fails with Exchange (BMC #22849).
    // Skip the testing, proceed to full removal.
    if (currentServer() != "exchange") {
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
        if (getCurrentTest().find("::eds_event::") != std::string::npos) {
            bool usingEDSLT36;

#ifdef  EVOLUTION_COMPATIBILITY
            // Need to check at runtime.
            // We are using EDS < 3.6 if (and only if) we found the right libs.
            usingEDSLT36 = EDSAbiHaveEcal;
#elif defined(USE_EDS_CLIENT)
            // Detected at runtime: new EDS API.
            usingEDSLT36 = false;
#else
            // Detected at runtime: old EDS API.
            usingEDSLT36 = true;
#endif
            if (usingEDSLT36) {
                // hack: ignore EDS < 3.6 side effect of adding EXDATE to parent, see http://bugs.meego.com/show_bug.cgi?id=10906
                size_t pos = parentData.rfind("DTSTART");
                parentData.insert(pos,
                                  getCurrentTest().find("LinkedItemsAllDay") == std::string::npos ?
                                  "EXDATE:20080413T090000\n" :
                                  "EXDATE:20080413\n");
            }
        }

        CT_ASSERT_NO_THROW(compareDatabases(*source, &parentData, NULL));
        SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));

        CT_ASSERT_NO_THROW(source.reset());

        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
        SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
        // parent might have been updated
        int updated = false;
        CT_ASSERT_NO_THROW(updated = countUpdatedItems(copy.get()));
        SOURCE_ASSERT(copy.get(), 0 <= updated && updated <= 1);
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
        if (!config.m_sourceLUIDsAreVolatile) {
            SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
        }
        CT_ASSERT_NO_THROW(copy.reset());
    }

    CT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(),
                        // Exchange did not actually remove child above, done now.
                        currentServer() != "exchange" ? 1 : 2,
                        countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertParentTwice() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // add parent twice (should be turned into update)
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertChildTwice() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // add child twice (should be turned into update)
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1]));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), child));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsParentUpdate() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // add parent, then update it
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    CT_ASSERT_NO_THROW(parent = updateItem(createSourceA, config, parent, items[0], &parentData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsUpdateChild() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // add child, then update it
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    CT_ASSERT_NO_THROW(child = updateItem(createSourceA, config, child, items[1], &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), child));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertBothUpdateChild() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // add parent and child, then update child
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    CT_ASSERT_NO_THROW(child = updateItem(createSourceA, config, child, items[1], &childData));

    // child has to be listed as modified, parent may be
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 1 <= countUpdatedItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 2 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), child));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));
    CT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
    CT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertBothUpdateParent() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // add parent and child, then update parent
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    CT_ASSERT_NO_THROW(parent = updateItem(createSourceA, config, parent, items[0], &parentData));

    // parent has to be listed as modified, child may be
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 1 <= countUpdatedItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 2 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), parent));
    }
    CT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));
    CT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
}

// - insert parent and child
// - update child *without* UID and RECURRENCE-ID: source expected to  re-insert them
void LocalTests::testLinkedItemsInsertBothUpdateChildNoIDs() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // add parent and child, then update child
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    // remove UID and RECURRENCE-ID before updating
    std::string reducedChildData = items[1];
    std::string uid;
    CT_ASSERT_NO_THROW(uid = stripProperty(reducedChildData, "UID"));
    std::string rid;
    CT_ASSERT_NO_THROW(rid = stripProperty(reducedChildData, "RECURRENCE-ID"));
    CT_ASSERT_NO_THROW(child = updateItem(createSourceA, config, child, reducedChildData, &childData));

    // compare
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceA()));
    CT_ASSERT_NO_THROW(insertProperty(childData, uid, "END:VEVENT"));
    CT_ASSERT_NO_THROW(insertProperty(childData, rid, "END:VEVENT"));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
}

// - insert child
// - update child *without* UID and RECURRENCE-ID: source expected to re-insert them
void LocalTests::testLinkedItemsUpdateChildNoIDs() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string child;
    std::string childData;
    TestingSyncSourcePtr copy;

    // add child, then update child
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    // remove UID and RECURRENCE-ID before updating
    std::string reducedChildData = items[1];
    std::string uid;
    CT_ASSERT_NO_THROW(uid = stripProperty(reducedChildData, "UID"));
    std::string rid;
    CT_ASSERT_NO_THROW(rid = stripProperty(reducedChildData, "RECURRENCE-ID"));
    CT_ASSERT_NO_THROW(child = updateItem(createSourceA, config, child, reducedChildData, &childData));

    // compare
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceA()));
    CT_ASSERT_NO_THROW(insertProperty(childData, uid, "END:VEVENT"));
    CT_ASSERT_NO_THROW(insertProperty(childData, rid, "END:VEVENT"));
    CT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
}

// insert parent, try to delete or retrieve non-existent child:
// must report 404
void LocalTests::testLinkedItemsSingle404() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;

    // now insert main item
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false));

    // fake subid: works for CalDAV and EDS
    child = parent + "no-such-subitem";

    // read
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset((createSourceA())));
    SyncMLStatus status = STATUS_OK;
    CT_ASSERT_NO_THROW(try {
            std::string data;
            source->readItem(child, data);
        } catch (const StatusException &ex) {
            status = ex.syncMLStatus();
        }
    );
    CT_ASSERT_EQUAL(STATUS_NOT_FOUND, status);

    // delete
    status = STATUS_OK;
    CT_ASSERT_NO_THROW(try {
            source->deleteItem(child);
        } catch (const StatusException &ex) {
            status = ex.syncMLStatus();
        }
    );
    CT_ASSERT_EQUAL(STATUS_NOT_FOUND, status);
}

// insert parent and child, try to delete or retrieve non-existent child:
// must report 404
void LocalTests::testLinkedItemsMany404() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;

    // now insert two items
    CT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false));
    CT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false));

    // fake subid: works for CalDAV and EDS
    child = parent + "no-such-subitem";

    // read
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    SyncMLStatus status = STATUS_OK;
    CT_ASSERT_NO_THROW(try {
            std::string data;
            source->readItem(child, data);
        } catch (const StatusException &ex) {
            status = ex.syncMLStatus();
        }
    );
    CT_ASSERT_EQUAL(STATUS_NOT_FOUND, status);

    // delete
    status = STATUS_OK;
    CT_ASSERT_NO_THROW(try {
            source->deleteItem(child);
        } catch (const StatusException &ex) {
            status = ex.syncMLStatus();
        }
    );
    CT_ASSERT_EQUAL(STATUS_NOT_FOUND, status);
}

// Is run as Client::Source::LinkedItems<testdata>::testSubsetStart<start>Skip<skip>
// where start = first detached recurrence to send and skip = detached recurrences
// to skip before adding the next one (=> 0 = send all).
//
// "Exdate" instead of Skip<skip> is special: it picks the <start>, <start> + 1 and last
// item, which typically leads to an irregular pattern and requires adding EXDATEs
// in the activesyncd.
void LocalTests::testSubset()
{
    ClientTestConfig::LinkedItems_t items = getParentChildData();
    int start, skip;
    std::string test = getCurrentTest();
    pcrecpp::RE re("testSubsetStart(\\d+)(?:Skip(\\d+)|(Exdate))");
    std::string exdate, optSkip;
    CT_ASSERT(re.PartialMatch(test, &start, &optSkip, &exdate));
    if (exdate.empty()) {
        // skip case
        CT_ASSERT(!optSkip.empty());
        skip = atoi(optSkip.c_str());
    } else {
        // EXDATE case
        CT_ASSERT_EQUAL(std::string("Exdate"), exdate);
        skip = -1;
    }
    CT_ASSERT(items.size() > (size_t)start);
    CT_ASSERT(skip >= -1);

    // check that everything is empty, also resets change counter of sync source B
    CT_ASSERT_NO_THROW(deleteAll(createSourceA));
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CT_ASSERT_NO_THROW(copy.reset());

    // insert parent first, then child
    std::list<std::string> sent;
    int i = start;
    while ((size_t)i < items.size() &&
           ((start == 0 && skip == 0) || /* _0_0 really uses all items (stress test) */
            skip == -1 ||                /* _x_e already is limited to 3 items */
            i - start < 5)) {            /* avoid huge number of items per test */
        std::string data;
        std::string message = StringPrintf("start %d, skip %d, at %d of %d",
                                           start, skip, i, (int)items.size());
        CT_ASSERT_NO_THROW_MESSAGE(message, insert(createSourceA, items[i], false, &data));
        sent.push_back(data);

        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
        std::list<std::string> actual(sent);
        if (items.m_testLinkedItemsSubsetAdditional) {
            std::string event = items.m_testLinkedItemsSubsetAdditional(start, skip, i, items.size());
            if (!event.empty()) {
                actual.push_back(event);
            }
        }
        CT_ASSERT_NO_THROW_MESSAGE(message, compareDatabasesRef(*copy, actual));

        if (skip >= 0) {
            // skip intermediate items
            i += skip + 1;
        } else if (i == start) {
            // go to second item
            i++;
        } else if (i == start + 1) {
            // go to last item
            CT_ASSERT((size_t)i != items.size() - 1);
            i = items.size() - 1;
        } else {
            // done with first, second and last item
            break;
        }
    }
}


ClientTestConfig::LinkedItems_t LocalTests::getParentChildData()
{
    // extract suffix and use it as index for our config
    std::string test = getCurrentTest();
    const std::string testname = "LinkedItems";
    size_t off = test.find(testname);
    CT_ASSERT(off != test.npos);
    off += testname.size();
    size_t end = test.find(':', off);
    CT_ASSERT(end != test.npos);
    std::string name = test.substr(off, end - off);
    BOOST_FOREACH(const ClientTestConfig::LinkedItems_t &items, config.m_linkedItems) {
        if (items.m_name == name) {
            return items;
        }
    }
    BOOST_FOREACH(const ClientTestConfig::LinkedItems_t &items, config.m_linkedItemsSubset) {
        if (items.m_name == name) {
            return items;
        }
    }
    CT_ASSERT_MESSAGE("linked items test data not found", false);
    return ClientTestConfig::LinkedItems_t();
}

SyncTests::SyncTests(const std::string &name, ClientTest &cl, std::vector<int> sourceIndices, bool isClientA) :
    CppUnit::TestSuite(name),
    client(cl) {
    sourceArray = new int[sourceIndices.size() + 1];
    int offset = 0;
    for (std::vector<int>::iterator it = sourceIndices.begin();
         it != sourceIndices.end();
         ++it) {
        ClientTest::Config config;
        client.getSyncSourceConfig(*it, config);

        if (!config.m_sourceName.empty()) {
            sourceArray[sources.size()+offset] = *it;
            if (!config.m_subConfigs.empty()) {
                vector<string> subs;
                boost::split (subs, config.m_subConfigs, boost::is_any_of(","));
                offset++;
                ClientTest::Config subConfig;
                BOOST_FOREACH (string sub, subs) {
                client.getSourceConfig (sub, subConfig);
                sources.push_back(std::pair<int,LocalTests *>(*it, cl.createLocalTests(sub, client.getLocalSourcePosition(sub), subConfig)));
                offset--;
                }
            } else {
                sources.push_back(std::pair<int,LocalTests *>(*it, cl.createLocalTests(config.m_sourceName, client.getLocalSourcePosition(config.m_sourceName), config)));
            }
        }
    }
    sourceArray[sources.size()+ offset] = -1;

    // check whether we have a second client
    ClientTest *clientB = cl.getClientB();
    if (clientB) {
        accessClientB = clientB->createSyncTests(name, sourceIndices, false);
    } else {
        accessClientB = 0;
    }
}

SyncTests::~SyncTests() {
    for (source_it it = sources.begin();
         it != sources.end();
         ++it) {
        delete it->second;
    }
    delete [] sourceArray;
    if (accessClientB) {
        delete accessClientB;
    }
}

/** adds the supported tests to the instance itself */
void SyncTests::addTests(bool isFirstSource) {
    if (sources.size()) {
        const ClientTest::Config &config(sources[0].second->config);

        // run this test first, even if it is more complex:
        // if it works, all the following tests will run with
        // the server in a deterministic state
        if (config.m_createSourceA) {
            if (!config.m_insertItem.empty()) {
                ADD_TEST(SyncTests, testDeleteAllRefresh);
            }
        }

        ADD_TEST(SyncTests, testTwoWaySync);
        ADD_TEST(SyncTests, testSlowSync);
        ADD_TEST(SyncTests, testRefreshFromServerSync);
        ADD_TEST(SyncTests, testRefreshFromClientSync);
        ADD_TEST(SyncTests, testRefreshFromRemoteSync);
        ADD_TEST(SyncTests, testRefreshFromLocalSync);
        // testTimeout is independent of the actual peer; all it needs
        // is a SyncML client config. Can't test for that explicitly
        // here, so only rule out the test if we run in server mode.
        if (isFirstSource &&
            (!getenv("CLIENT_TEST_MODE") ||
             strcmp(getenv("CLIENT_TEST_MODE"), "server"))) {
            ADD_TEST(SyncTests, testTimeout);
        }

        if (config.m_compare &&
            !config.m_testcases.empty() &&
            !isServerMode()) {
            ADD_TEST(SyncTests, testConversion);
        }

        if (config.m_createSourceA) {
            if (!config.m_insertItem.empty()) {
                ADD_TEST(SyncTests, testRefreshFromServerSemantic);
                ADD_TEST(SyncTests, testRefreshFromClientSemantic);
                ADD_TEST(SyncTests, testRefreshStatus);

                // This test works regardless whether the peer can
                // restart: if restarts are not possible, it checks
                // that they don't occur. The rest of the tests then
                // only make sense when restarting works.
                ADD_TEST(SyncTests, testTwoWayRestart);
                if (getenv("CLIENT_TEST_PEER_CAN_RESTART")) {
                    ADD_TEST(SyncTests, testSlowRestart);
                    ADD_TEST(SyncTests, testRefreshFromLocalRestart);
                    ADD_TEST(SyncTests, testOneWayFromLocalRestart);
                    ADD_TEST(SyncTests, testRefreshFromRemoteRestart);
                    ADD_TEST(SyncTests, testOneWayFromRemoteRestart);
                    ADD_TEST(SyncTests, testManyRestarts);
                }

                if (accessClientB &&
                    config.m_dump &&
                    config.m_compare) {
                    ADD_TEST(SyncTests, testCopy);
                    ADD_TEST(SyncTests, testDelete);
                    ADD_TEST(SyncTests, testAddUpdate);
                    ADD_TEST(SyncTests, testManyItems);
                    ADD_TEST(SyncTests, testManyDeletes);
                    ADD_TEST(SyncTests, testSlowSyncSemantic);
                    ADD_TEST(SyncTests, testComplexRefreshFromServerSemantic);
                    ADD_TEST(SyncTests, testDeleteBothSides);
                    if (config.m_updateItem.find("UID:") != std::string::npos &&
                        config.m_updateItem.find("LAST-MODIFIED:") != std::string::npos &&
                        sources.size() == 1) {
                        ADD_TEST(SyncTests, testAddBothSides);
                        ADD_TEST(SyncTests, testAddBothSidesRefresh);
                    }

                    // only add when testing individual source,
                    // test data not guaranteed to be available for all sources
                    if (sources.size() == 1 &&
                        !config.m_linkedItems.empty()) {
                        ADD_TEST(SyncTests, testLinkedItemsParentChild);

                        if (config.m_linkedItemsRelaxedSemantic) {
                            ADD_TEST(SyncTests, testLinkedItemsChild);
                            ADD_TEST(SyncTests, testLinkedItemsChildParent);
                        }
                    }

                    if (!config.m_updateItem.empty()) {
                        ADD_TEST(SyncTests, testUpdate);
                    }
                    if (!config.m_complexUpdateItem.empty()) {
                        ADD_TEST(SyncTests, testComplexUpdate);
                    }
                    if (!config.m_mergeItem1.empty() &&
                        !config.m_mergeItem2.empty()) {
                        ADD_TEST(SyncTests, testMerge);
                    }
                    if (config.m_import) {
                        ADD_TEST(SyncTests, testTwinning);
                        ADD_TEST(SyncTests, testItems);
                        ADD_TEST(SyncTests, testItemsXML);
                        if (config.m_update) {
                            ADD_TEST(SyncTests, testExtensions);
                        }
                    }
                    if (!config.m_templateItem.empty()) {
                        ADD_TEST(SyncTests, testMaxMsg);
                        ADD_TEST(SyncTests, testLargeObject);
                        ADD_TEST(SyncTests, testOneWayFromServer);
                        ADD_TEST(SyncTests, testOneWayFromClient);
                        ADD_TEST(SyncTests, testOneWayFromRemote);
                        ADD_TEST(SyncTests, testOneWayFromLocal);
                    }
                }
            }

            // Tests which depend on item manipulation in the peer.
            // These tests get enabled if their testdata is found in
            // testcases/synctests/<server name> and if we are
            // currently testing only a single source. The tests will
            // fail if SyncEvolution was not configured correctly for
            // them (see getPeerConfig()).
            if (sources.size() == 1) {
                const std::string sourceName = config.m_sourceName;
#define ADD_PEER_TEST(_x) if (isDir(getPeerTestdata(sourceName, #_x, ""))) { ADD_TEST(SyncTests, _x); }

                ADD_PEER_TEST(testDownload);
                ADD_PEER_TEST(testUpload);
                ADD_PEER_TEST(testUpdateLocalWins);
                ADD_PEER_TEST(testUpdateRemoteWins);
            }
        }

        if (config.m_retrySync &&
            !config.m_insertItem.empty() &&
            !config.m_updateItem.empty() &&
            accessClientB &&
            config.m_dump &&
            config.m_compare) {
            CppUnit::TestSuite *retryTests = new CppUnit::TestSuite(getName() + "::Retry");
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientAdd);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientRemove);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientUpdate);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerAdd);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerRemove);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerUpdate);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientAddBig);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientUpdateBig);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerAddBig);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerUpdateBig);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeFull);
            addTest(FilterTest(retryTests));
        }

        if (config.m_suspendSync &&
            !config.m_insertItem.empty() &&
            !config.m_updateItem.empty() &&
            accessClientB &&
            config.m_dump &&
            config.m_compare) {
            CppUnit::TestSuite *suspendTests = new CppUnit::TestSuite(getName() + "::Suspend");
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientAdd);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientRemove);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientUpdate);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerAdd);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerRemove);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerUpdate);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientAddBig);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientUpdateBig);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerAddBig);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerUpdateBig);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendFull);
            addTest(FilterTest(suspendTests));
        }

        if (config.m_resendSync &&
            !config.m_insertItem.empty() &&
            !config.m_updateItem.empty() &&
            accessClientB &&
            config.m_dump &&
            config.m_compare) {
            CppUnit::TestSuite *resendTests = new CppUnit::TestSuite(getName() + "::Resend");
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendClientAdd);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendClientRemove);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendClientUpdate);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendServerAdd);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendServerRemove);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendServerUpdate);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendFull);
            addTest(FilterTest(resendTests));
        }

        if (getenv("CLIENT_TEST_RESEND_PROXY") &&
            !config.m_insertItem.empty() &&
            !config.m_updateItem.empty() &&
            accessClientB &&
            config.m_dump &&
            config.m_compare) {
            CppUnit::TestSuite *resendTests = new CppUnit::TestSuite(getName() + "::ResendProxy");
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyClientAdd);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyClientRemove);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyClientUpdate);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyServerAdd);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyServerRemove);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyServerUpdate);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyFull);
            addTest(FilterTest(resendTests));
        }
    }
}

bool SyncTests::compareDatabases(const char *refFileBase, bool raiseAssert) {
    source_it it1;
    source_it it2;
    bool equal = true;

    CT_ASSERT(accessClientB);
    for (it1 = sources.begin(), it2 = accessClientB->sources.begin();
         it1 != sources.end() && it2 != accessClientB->sources.end();
         ++it1, ++it2) {
        TestingSyncSourcePtr copy;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(it2->second->createSourceB()));
        if (refFileBase) {
            std::string refFile = refFileBase;
            refFile += it1->second->config.m_sourceName;
            refFile += ".dat";
            simplifyFilename(refFile);
            if (!it1->second->compareDatabases(refFile.c_str(), *copy.get(), raiseAssert)) {
                equal = false;
            }
        } else {
            if (!it1->second->compareDatabases(NULL, *copy.get(), raiseAssert)) {
                equal = false;
            }
        }
        CT_ASSERT_NO_THROW(copy.reset());
    }
    CT_ASSERT(it1 == sources.end());
    CT_ASSERT(it2 == accessClientB->sources.end());

    CT_ASSERT(!raiseAssert || equal);
    return equal;
}

/** deletes all items locally and on server */
void SyncTests::deleteAll(DeleteAllMode mode) {
    SyncPrefix prefix("deleteall", *this);

    const char *value = getenv ("CLIENT_TEST_DELETE_REFRESH");
    if (value) {
        mode = DELETE_ALL_REFRESH;
    }

    switch(mode) {
     case DELETE_ALL_SYNC:
        // a refresh from server would slightly reduce the amount of data exchanged, but not all servers support it
        CT_ASSERT_NO_THROW(allSourcesDeleteAll());
        doSync(__FILE__, __LINE__, "init", SyncOptions(SYNC_SLOW));
        // now that client and server are in sync, delete locally and sync again
        CT_ASSERT_NO_THROW(allSourcesDeleteAll());
        doSync(__FILE__, __LINE__,
               "twoway",
               SyncOptions(SYNC_TWO_WAY,
                           CheckSyncReport(0,0,0, 0,0,-1, true, SYNC_TWO_WAY)));
        break;
     case DELETE_ALL_REFRESH:
        // delete locally and then tell the server to "copy" the empty databases
        CT_ASSERT_NO_THROW(allSourcesDeleteAll());
        doSync(__FILE__, __LINE__,
               "refreshserver",
               SyncOptions(RefreshFromLocalMode(),
                           CheckSyncReport(0,0,0, 0,0,-1, true, SYNC_REFRESH_FROM_LOCAL)));
        break;
    }
}

/** get both clients in sync with empty server, then copy one item from client A to B */
void SyncTests::doCopy() {
    SyncPrefix copy("copy", *this);

    // check requirements
    CT_ASSERT(accessClientB);

    CT_ASSERT_NO_THROW(deleteAll());
    accessClientB->deleteAll();

    // insert into first database, copy to server
    CT_ASSERT_NO_THROW(allSourcesInsert());
    doSync(__FILE__, __LINE__,
           "send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // copy into second database
    accessClientB->doSync(__FILE__, __LINE__,
                          "recv",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    CT_ASSERT_NO_THROW(compareDatabases());
}

/**
 * Replicate server database locally. Works with all servers
 * (including those which do not support refresh sync) because the
 * Synthesis engine will do a local delete + slow sync (by default)
 * and a real refresh sync only when asked to explicitly (Funambol).
 */
void SyncTests::refreshClient(SyncOptions options) {
    doSync(__FILE__, __LINE__,
           "refresh",
           options
           .setSyncMode(SYNC_REFRESH_FROM_REMOTE)
           .setCheckReport(CheckSyncReport(-1,0,-1, 0,0,0, true, SYNC_REFRESH_FROM_REMOTE)));
}


// delete all items, locally and on server using refresh-from-client sync
void SyncTests::testDeleteAllRefresh() {
    source_it it;

    // start with clean local data
    CT_ASSERT_NO_THROW(allSourcesDeleteAll());

    // copy something to server first; doesn't matter whether it has the
    // item already or not, as long as it exists there afterwards
    CT_ASSERT_NO_THROW(allSourcesInsert());
    doSync(__FILE__, __LINE__, "insert", SyncOptions(SYNC_SLOW));

    // now ensure we can delete it
    deleteAll(DELETE_ALL_REFRESH);

    // nothing stored locally?
    for (it = sources.begin(); it != sources.end(); ++it) {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
        CT_ASSERT_NO_THROW(source.reset());
    }

    // make sure server really deleted everything
    doSync(__FILE__, __LINE__,
           "check",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_SLOW)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
        CT_ASSERT_NO_THROW(source.reset());
    }
}

// refresh-from-server sync, regardless of peer's role
void SyncTests::testRefreshFromServerSync()
{
    doSync(__FILE__, __LINE__,
           SyncOptions(SYNC_REFRESH_FROM_SERVER,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true,
                                       isServerMode() ? SYNC_REFRESH_FROM_LOCAL : SYNC_REFRESH_FROM_REMOTE)));
}

// do a refresh-from-client sync, regardless of peer's role
void SyncTests::testRefreshFromClientSync()
{
    doSync(__FILE__, __LINE__,
           SyncOptions(SYNC_REFRESH_FROM_CLIENT,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true,
                                       isServerMode() ? SYNC_REFRESH_FROM_REMOTE : SYNC_REFRESH_FROM_LOCAL)));
}

// do a refresh-from-remote sync, regardless of peer's role
void SyncTests::testRefreshFromRemoteSync()
{
    doSync(__FILE__, __LINE__,
           SyncOptions(SYNC_REFRESH_FROM_REMOTE,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_REFRESH_FROM_REMOTE)));
}

// do a refresh-from-local sync, regardless of peer's role
void SyncTests::testRefreshFromLocalSync()
{
    doSync(__FILE__, __LINE__,
           SyncOptions(SYNC_REFRESH_FROM_LOCAL,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_REFRESH_FROM_LOCAL)));
}

// delete all items, locally and on server using two-way sync
void SyncTests::testDeleteAllSync()
{
    CT_ASSERT_NO_THROW(deleteAll(DELETE_ALL_SYNC));
}

// test that a refresh sync from an empty server leads to an empty datatbase
// and no changes are sent to server during next two-way sync
void SyncTests::testRefreshFromServerSemantic() {
    source_it it;

    // clean client and server
    CT_ASSERT_NO_THROW(deleteAll());

    // insert item, then refresh from empty server
    CT_ASSERT_NO_THROW(allSourcesInsert());
    doSync(__FILE__, __LINE__,
           "refresh",
           SyncOptions(RefreshFromPeerMode(),
                       CheckSyncReport(0,0,-1, 0,0,0, true, SYNC_REFRESH_FROM_REMOTE)));

    // check
    for (it = sources.begin(); it != sources.end(); ++it) {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
        CT_ASSERT_NO_THROW(source.reset());
    }
    doSync(__FILE__, __LINE__,
           "two-way",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));
}

// test that a refresh sync from an empty client leads to an empty datatbase
// and no changes are sent to server during next two-way sync
void SyncTests::testRefreshFromClientSemantic() {
    source_it it;

    // clean client and server
    CT_ASSERT_NO_THROW(deleteAll());

    // insert item, send to server
    CT_ASSERT_NO_THROW(allSourcesInsert());
    doSync(__FILE__, __LINE__,
           "send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // delete locally
    CT_ASSERT_NO_THROW(allSourcesDeleteAll());

    // refresh from client
    doSync(__FILE__, __LINE__,
           "refresh",
           SyncOptions(RefreshFromLocalMode(),
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_REFRESH_FROM_LOCAL)));

    // check
    doSync(__FILE__, __LINE__,
           "check",
           SyncOptions(RefreshFromPeerMode(),
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_REFRESH_FROM_REMOTE)));
}

// tests the following sequence of events:
// - insert item
// - delete all items
// - insert one other item
// - refresh from client
// => no items should now be listed as new, updated or deleted for this client during another sync
void SyncTests::testRefreshStatus() {
    source_it it;

    CT_ASSERT_NO_THROW(allSourcesInsert(false));
    CT_ASSERT_NO_THROW(allSourcesDeleteAll());
    CT_ASSERT_NO_THROW(allSourcesInsert());
    doSync(__FILE__, __LINE__,
           "refresh-from-client",
           SyncOptions(RefreshFromLocalMode(),
                       CheckSyncReport(0,0,0, -1,-1,-1, /* strictly speaking 1,0,0, but not sure exactly what the server will be told */
                                       true, SYNC_REFRESH_FROM_LOCAL)));
    doSync(__FILE__, __LINE__,
           "two-way",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));
}

static void log(const char *text)
{
    CLIENT_TEST_LOG("%s", text);
}

static void logSyncSourceReport(SyncSource *source)
{
    CLIENT_TEST_LOG("source %s, start of cycle #%d: local new/mod/del/conflict %d/%d/%d/%d, remote %d/%d/%d/%d, mode %s",
                    source->getName().c_str(),
                    source->getRestarts(),
                    source->getItemStat(SyncSourceReport::ITEM_LOCAL, SyncSourceReport::ITEM_ADDED, SyncSource::ITEM_TOTAL),
                    source->getItemStat(SyncSourceReport::ITEM_LOCAL, SyncSourceReport::ITEM_UPDATED, SyncSource::ITEM_TOTAL),
                    source->getItemStat(SyncSourceReport::ITEM_LOCAL, SyncSourceReport::ITEM_REMOVED, SyncSource::ITEM_TOTAL),
                    source->getItemStat(SyncSourceReport::ITEM_LOCAL, SyncSourceReport::ITEM_ANY, SyncSource::ITEM_REJECT),

                    source->getItemStat(SyncSourceReport::ITEM_REMOTE, SyncSourceReport::ITEM_ADDED, SyncSource::ITEM_TOTAL),
                    source->getItemStat(SyncSourceReport::ITEM_REMOTE, SyncSourceReport::ITEM_UPDATED, SyncSource::ITEM_TOTAL),
                    source->getItemStat(SyncSourceReport::ITEM_REMOTE, SyncSourceReport::ITEM_REMOVED, SyncSource::ITEM_TOTAL),
                    source->getItemStat(SyncSourceReport::ITEM_REMOTE, SyncSourceReport::ITEM_ANY, SyncSource::ITEM_REJECT),

                    PrettyPrintSyncMode(source->getFinalSyncMode()).c_str());
}

/**
 * Helper function, to be used inside a SyncOptions start callback
 * to connect all sources instantiated for a sync with the given
 * pre-operation signal.
 */
template<class W, class M, class S>
bool connectSourceSignal(SyncContext &context,
                         W SyncSource::Operations::*operation,
                         M getSignal,
                         const S &slot)
{
    BOOST_FOREACH(const SyncSource *source, *context.getSources())  {
        ((source->getOperations().*operation).*getSignal)().connect(slot);
    }
    return false;
}

void SyncTests::doRestartSync(SyncMode mode)
{
    CT_ASSERT_NO_THROW(deleteAll());
    int startCount = 0;
    bool needToConnect = true;
    typedef std::map<std::string, SyncSourceReport> Reports_t;
    typedef std::map<int, Reports_t> Cycles_t;
    Cycles_t results;

    // Triggered for every m_startDataRead.
    //
    // It records the current source statistics for later checking and
    // logs it.
    //
    // Also requests a restart at the very beginning, once. Must be
    // done before m_endDataWrite, because then it might be too late
    // to restart.
    boost::function<SyncSource::Operations::StartDataRead_t::PreSignal::signature_type> start =
        (boost::lambda::if_then(boost::lambda::bind(&Cycles_t::empty, boost::ref(results)),
                                (boost::lambda::bind(log, "requesting restart"),
                                 boost::lambda::bind(SyncContext::requestAnotherSync))),
         (boost::lambda::var(results)[boost::lambda::bind(&SyncSource::getRestarts, &boost::lambda::_1)]
          [boost::lambda::bind(&SyncSource::getName, &boost::lambda::_1)] =
          boost::lambda::_1),
         boost::lambda::bind(logSyncSourceReport,
                             &boost::lambda::_1)
         );

    // Triggered at the end of each m_endDataWrite.
    //
    // Adds a new item or (in later syncs) updates/deletes
    // it. Because the cycle is other, those changes won't
    // interfere with the cycle. Doing real concurrent
    // changes is something for another tests...
    boost::function<SyncSource::Operations::EndDataWrite_t::PostSignal::signature_type> end =
        boost::bind(boost::function<void ()>(
                                             boost::lambda::if_then(++boost::lambda::var(startCount) == sources.size(),
                                                                    (boost::lambda::bind(log, "inserting one item"),
                                                                     boost::lambda::bind(&SyncTests::allSourcesInsert, this, true)))
                                             ));

    SyncOptions::Callback_t setup =
        (boost::lambda::if_then(boost::lambda::var(needToConnect),
                                (boost::lambda::var(needToConnect) = false,
                                 boost::lambda::bind(connectSourceSignal<SyncSource::Operations::StartDataRead_t,
                                                                         typeof(&SyncSource::Operations::StartDataRead_t::getPreSignal),
                                                                         typeof(start)>,
                                                         boost::lambda::_1,
                                                         &SyncSource::Operations::m_startDataRead,
                                                         &SyncSource::Operations::StartDataRead_t::getPreSignal,
                                                         boost::cref(start)),
                                 boost::lambda::bind(connectSourceSignal<SyncSource::Operations::EndDataWrite_t,
                                                                         typeof(&SyncSource::Operations::EndDataWrite_t::getPostSignal),
                                                                         typeof(end)>,
                                                         boost::lambda::_1,
                                                         &SyncSource::Operations::m_endDataWrite,
                                                         &SyncSource::Operations::EndDataWrite_t::getPostSignal,
                                                         boost::cref(end))
                                )),
         boost::lambda::constant(false)
        );

    bool canRestart = getenv("CLIENT_TEST_PEER_CAN_RESTART") != NULL &&
        !isServerMode();

    CT_ASSERT_NO_THROW(doSync(__FILE__, __LINE__,
                              "add",
                              SyncOptions(mode,
                                          CheckSyncReport(0,
                                                          0,
                                                          // TODO (?): should the item added after the initial refresh-from-remote be deleted in the second cycle?
                                                          // Right now it isn't, because the second sync is
                                                          // a one-way-from-remote.
                                                          mode == SYNC_REFRESH_FROM_REMOTE ? /* 1 */ 0 : 0,

                                                          // nothing transferred when item only exists locally
                                                          // and not transferring to peer
                                                          !canRestart ? 0 :
                                                          (mode == SYNC_ONE_WAY_FROM_REMOTE ||
                                                           mode == SYNC_REFRESH_FROM_REMOTE) ? 0 : 1,
                                                          0,
                                                          0,
                                                          true, mode)
                                          .setRestarts(canRestart ? 1 : 0))
                              .setStartCallback(setup)
                              ));

    // two cycles if restarted, one otherwise
    CT_ASSERT_EQUAL((size_t)(canRestart ? 2 : 1), results.size());

    // nothing transfered before first or second cycle
    BOOST_FOREACH(const Cycles_t::value_type &cycle, results) {
        CT_ASSERT_EQUAL(sources.size(), cycle.second.size());
        BOOST_FOREACH(const Reports_t::value_type &entry, cycle.second) {
            CT_ASSERT_NO_THROW(CheckSyncReport(0,0, 0,
                                               0,0,0)
                               .setRestarts(cycle.first)
                               .check(entry.first, entry.second));
        }
    }

    // one item exists now, in all cases
    // (but see remark about refresh-from-remote!)
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(source_pair.second->createSourceA()));
        CT_ASSERT_EQUAL(1, countItems(source.get()));
    }

    if (mode == SYNC_REFRESH_FROM_REMOTE ||
        !canRestart) {
        // Can't continue testing for refresh-from-remote, because the
        // item was never sent to remote and will be gone locally
        // after the next refresh-from-remote (prevents updating and
        // deleting it locally).
        // Without restart support further tests don't make much sense.
        // We already verified above that a restart request was
        // correctly rejected/ignored.
        return;
    }

    // update item while the sync runs
#ifndef __clang_analyzer__
    needToConnect = true;
#endif
    startCount = 0;
    results.clear();
    end =
        boost::bind(boost::function<void ()>(
                                             boost::lambda::if_then(++boost::lambda::var(startCount) == sources.size(),
                                                                    (boost::lambda::bind(log, "update one item"),
                                                                     boost::lambda::bind(&SyncTests::allSourcesUpdate, this)))
                                             ));

    CT_ASSERT_NO_THROW(doSync(__FILE__, __LINE__,
                              "update",
                              SyncOptions(mode,
                                          CheckSyncReport(0,0,0,

                                                          // refresh-from-local and slow sync transfer existing item
                                                          // in first cycle anew
                                                          (mode == SYNC_REFRESH_FROM_LOCAL ||
                                                           mode == SYNC_SLOW) ? 1 : 0,
                                                          // nothing transferred when item only exists locally
                                                          // and not transferring to peer
                                                          mode == SYNC_ONE_WAY_FROM_REMOTE ? 0 : 1,
                                                          0,
                                                          true, mode)
                                          .setRestarts(1))
                              .setStartCallback(setup)
                              ));

    // two cycles
    CT_ASSERT_EQUAL((size_t)2, results.size());

    // nothing transfered before first or second cycle
    BOOST_FOREACH(const Cycles_t::value_type &cycle, results) {
        CLIENT_TEST_LOG("checking cycle #%d", cycle.first);
        CT_ASSERT_EQUAL(sources.size(), cycle.second.size());
        BOOST_FOREACH(const Reports_t::value_type &entry, cycle.second) {
            CT_ASSERT_NO_THROW(CheckSyncReport(0,0,0,

                                               // refresh-from-local and slow sync transfer existing item
                                               // in first cycle anew
                                               (cycle.first == 1 &&
                                                (mode == SYNC_REFRESH_FROM_LOCAL ||
                                                 mode == SYNC_SLOW)) ? 1 : 0,
                                               0,0)
                               .setRestarts(cycle.first)
                               .check(entry.first, entry.second));
        }
    }

    // one item exists now, in all cases
    // (but see remark about refresh-from-remote!)
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(source_pair.second->createSourceA()));
        CT_ASSERT_EQUAL(1, countItems(source.get()));
    }

    // delete item while the sync runs
#ifndef __clang_analyzer__
    needToConnect = true;
#endif
    startCount = 0;
    results.clear();
    end =
        boost::bind(boost::function<void ()>(
                                             boost::lambda::if_then(++boost::lambda::var(startCount) == sources.size(),
                                                                    (boost::lambda::bind(log, "delete one item"),
                                                                     boost::lambda::bind(&SyncTests::allSourcesDeleteAll, this)))
                                             ));

    CT_ASSERT_NO_THROW(doSync(__FILE__, __LINE__,
                              "delete",
                              SyncOptions(mode,
                                          CheckSyncReport(0,0,0,

                                                          // refresh-from-local and slow sync transfer existing item
                                                          // in first cycle anew
                                                          (mode == SYNC_REFRESH_FROM_LOCAL ||
                                                           mode == SYNC_SLOW) ? 1 : 0,
                                                          0,
                                                          // nothing transferred when item only existed locally
                                                          // and not transferring to peer
                                                          mode == SYNC_ONE_WAY_FROM_REMOTE ? 0 : 1,
                                                          true, mode)
                                          .setRestarts(1))
                              .setStartCallback(setup)
                              ));

    // two cycles
    CT_ASSERT_EQUAL((size_t)2, results.size());

    // nothing transfered before first or second cycle
    BOOST_FOREACH(const Cycles_t::value_type &cycle, results) {
        CT_ASSERT_EQUAL(sources.size(), cycle.second.size());
        BOOST_FOREACH(const Reports_t::value_type &entry, cycle.second) {
            CT_ASSERT_NO_THROW(CheckSyncReport(0,0, 0,
                                               // refresh-from-local and slow sync transfer existing item
                                               // in first cycle anew
                                               (cycle.first == 1 &&
                                                (mode == SYNC_REFRESH_FROM_LOCAL ||
                                                 mode == SYNC_SLOW)) ? 1 : 0,
                                               0,0)
                               .setRestarts(cycle.first)
                               .check(entry.first, entry.second));
        }
    }

    // no item exists now, in all cases
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(source_pair.second->createSourceA()));
        CT_ASSERT_EQUAL(0, countItems(source.get()));
    }
}

// two-way sync when both sides are empty,
// insert item locally while sync runs, restart
// => one item sent to peer
void SyncTests::testTwoWayRestart()
{
    CT_ASSERT_NO_THROW(doRestartSync(SYNC_TWO_WAY));
}

// slow sync when both sides are empty,
// insert item locally while sync runs, restart
// => one item sent to peer
void SyncTests::testSlowRestart()
{
    CT_ASSERT_NO_THROW(doRestartSync(SYNC_SLOW));
}

// refresh-from-local sync when both sides are empty,
// insert item locally while sync runs, restart
// => one item sent to peer
void SyncTests::testRefreshFromLocalRestart()
{
    CT_ASSERT_NO_THROW(doRestartSync(SYNC_REFRESH_FROM_LOCAL));
}

// one-way-from-local sync when both sides are empty,
// insert item locally while sync runs, restart
// => one item sent to peer
void SyncTests::testOneWayFromLocalRestart()
{
    CT_ASSERT_NO_THROW(doRestartSync(SYNC_ONE_WAY_FROM_LOCAL));
}

// refresh-from-remote sync when both sides are empty,
// insert item locally while sync runs, restart
// => *nothing* sent to peer
void SyncTests::testRefreshFromRemoteRestart()
{
    CT_ASSERT_NO_THROW(doRestartSync(SYNC_REFRESH_FROM_REMOTE));
}

// one-way-from-remote sync when both sides are empty,
// insert item locally while sync runs, restart
// => *nothing* sent to peer
void SyncTests::testOneWayFromRemoteRestart()
{
    CT_ASSERT_NO_THROW(doRestartSync(SYNC_ONE_WAY_FROM_REMOTE));
}

// Start with empty database, refresh peer.
// Then add 1, 2, 4, 8 items in four cycles,
// update them the same way, and finally delete them.
// Results in 12 cycles with different changes and
// one empty, final cycle.
void SyncTests::testManyRestarts()
{
    CT_ASSERT_NO_THROW(deleteAll());
    int startCount = 0;
    bool needToConnect = true;
    typedef std::map<std::string, SyncSourceReport> Reports_t;
    typedef std::map<int, Reports_t> Cycles_t;
    Cycles_t results;
    std::map<int, std::list<std::string> > luids;

    // Triggered for every m_startDataRead.
    //
    // It records the current source statistics for later checking,
    // logs it, and does the item changes.
    boost::function<SyncSource::Operations::StartDataRead_t::PreSignal::signature_type> start =
        (boost::lambda::if_then(boost::lambda::var(startCount) % sources.size() == 0,
         (
           boost::lambda::switch_statement(boost::lambda::var(startCount) / sources.size(),
               boost::lambda::case_statement<0>(
                  (boost::lambda::bind(log, "insert 1 item, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesInsertMany, this, 1, 1, boost::ref(luids)),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  )),
               boost::lambda::case_statement<1>(
                  (boost::lambda::bind(log, "insert 2 items, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesInsertMany, this, 2, 2, boost::ref(luids)),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  )),
               boost::lambda::case_statement<2>(
                  (boost::lambda::bind(log, "insert 4 items, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesInsertMany, this, 4, 4, boost::ref(luids)),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  )),
               boost::lambda::case_statement<3>(
                  (boost::lambda::bind(log, "insert 8 items, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesInsertMany, this, 8, 8, boost::ref(luids)),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  )),
               boost::lambda::case_statement<4>(
                  (boost::lambda::bind(log, "update 1 item, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesUpdateMany, this, 1, 1, 1, boost::ref(luids), 0),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  )),
               boost::lambda::case_statement<5>(
                  (boost::lambda::bind(log, "update 2 items, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesUpdateMany, this, 2, 2, 1, boost::ref(luids), 1),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  )),
               boost::lambda::case_statement<6>(
                  (boost::lambda::bind(log, "update 4 items, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesUpdateMany, this, 4, 4, 1, boost::ref(luids), 3),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  )),
               boost::lambda::case_statement<7>(
                  (boost::lambda::bind(log, "update 8 items, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesUpdateMany, this, 8, 8, 1, boost::ref(luids), 7),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  ))
           ),
           // must break up switch statement, it only has a limited number of case slots
           boost::lambda::switch_statement(boost::lambda::var(startCount) / sources.size(),
               boost::lambda::case_statement<8>(
                  (boost::lambda::bind(log, "delete 1 item, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesRemoveMany, this, 1, boost::ref(luids), 0),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  )),
               boost::lambda::case_statement<9>(
                  (boost::lambda::bind(log, "delete 2 items, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesRemoveMany, this, 2, boost::ref(luids), 1),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  )),
               boost::lambda::case_statement<10>(
                  (boost::lambda::bind(log, "delete 4 items, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesRemoveMany, this, 4, boost::ref(luids), 3),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  )),
               boost::lambda::case_statement<11>(
                  (boost::lambda::bind(log, "delete 8 items, restart"),
                   boost::lambda::bind(&SyncTests::allSourcesRemoveMany, this, 8, boost::ref(luids), 7),
                   boost::lambda::bind(SyncContext::requestAnotherSync)
                  ))
           )
          )
         ),
         (boost::lambda::var(results)[boost::lambda::bind(&SyncSource::getRestarts, &boost::lambda::_1)]
          [boost::lambda::bind(&SyncSource::getName, &boost::lambda::_1)] = boost::lambda::_1
         ),
         boost::lambda::bind(logSyncSourceReport,
                             &boost::lambda::_1),
         ++boost::lambda::var(startCount)
         );

    SyncOptions::Callback_t setup =
        (boost::lambda::if_then(boost::lambda::var(needToConnect),
                                (boost::lambda::var(needToConnect) = false,
                                 boost::lambda::bind(connectSourceSignal<SyncSource::Operations::StartDataRead_t,
                                                                         typeof(&SyncSource::Operations::StartDataRead_t::getPreSignal),
                                                                         typeof(start)>,
                                                         boost::lambda::_1,
                                                         &SyncSource::Operations::m_startDataRead,
                                                         &SyncSource::Operations::StartDataRead_t::getPreSignal,
                                                         boost::cref(start))
                                )),
         boost::lambda::constant(false)
        );

    CT_ASSERT_NO_THROW(doSync(__FILE__, __LINE__,
                              SyncOptions(SYNC_TWO_WAY,
                                          CheckSyncReport(0,
                                                          0,
                                                          0,

                                                          15,
                                                          15,
                                                          15,
                                                          true, SYNC_TWO_WAY)
                                          .setRestarts(12))
                              .setStartCallback(setup)
                              ));

    // 13 cycles
    CT_ASSERT_EQUAL((size_t)13, results.size());
    static const int changes[13][3] = {
        {  0,  0,  0 }, // nothing before first cycle
        {  1,  0,  0 }, // result of first cycle
        {  3,  0,  0 }, // statistics are cummulative: first + second
        {  7,  0,  0 },
        { 15,  0,  0 },
        { 15,  1,  0 },
        { 15,  3,  0 },
        { 15,  7,  0 },
        { 15, 15,  0 },
        { 15, 15,  1 },
        { 15, 15,  3 },
        { 15, 15,  7 },
        { 15, 15, 15 }
    };
    BOOST_FOREACH(const Cycles_t::value_type &cycle, results) {
        CT_ASSERT_EQUAL(sources.size(), cycle.second.size());
        BOOST_FOREACH(const Reports_t::value_type &entry, cycle.second) {
            const int *c = changes[cycle.first];
            CLIENT_TEST_LOG("Checking stats before cycle #%d, source %s: expected remote %d/%d/%d",
                            cycle.first, entry.first.c_str(),
                            c[0], c[1], c[2]);
            CT_ASSERT_NO_THROW(CheckSyncReport(0,0,0,
                                               c[0], c[1], c[2])
                               .setRestarts(cycle.first)
                               .check(entry.first, entry.second));
        }
    }

    // no item exists now
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(source_pair.second->createSourceA()));
        CT_ASSERT_EQUAL(0, countItems(source.get()));
    }
}

// test that a two-way sync copies an item from one address book into the other
void SyncTests::testCopy()
{
    CT_ASSERT_NO_THROW(doCopy());
    CT_ASSERT_NO_THROW(compareDatabases());
}

// test that a two-way sync copies updates from database to the other client,
// using simple data commonly supported by servers
void SyncTests::testUpdate() {
    CT_ASSERT(sources.begin() != sources.end());
    CT_ASSERT(!sources.begin()->second->config.m_updateItem.empty());

    // setup client A, B and server so that they all contain the same item
    CT_ASSERT_NO_THROW(doCopy());

    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->update(it->second->createSourceA, it->second->config.m_updateItem));
    }

    doSync(__FILE__, __LINE__,
           "update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));
    accessClientB->doSync(__FILE__, __LINE__,
                          "update",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,1,0, 0,0,0, true, SYNC_TWO_WAY)));

    CT_ASSERT_NO_THROW(compareDatabases());
}

// test that a two-way sync copies updates from database to the other client,
// using data that some, but not all servers support, like adding a second
// phone number to a contact
void SyncTests::testComplexUpdate() {
    // setup client A, B and server so that they all contain the same item
    CT_ASSERT_NO_THROW(doCopy());

    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->update(it->second->createSourceA,
                           /* this test might get executed with some sources which have
                              a complex update item while others don't: use the normal update item
                              for them or even just the same item */
                           !it->second->config.m_complexUpdateItem.empty() ? it->second->config.m_complexUpdateItem :
                           !it->second->config.m_updateItem.empty() ? it->second->config.m_updateItem :
                           it->second->config.m_insertItem
                           );
    }

    doSync(__FILE__, __LINE__,
           "update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));
    accessClientB->doSync(__FILE__, __LINE__,
                          "update",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,1,0, 0,0,0, true, SYNC_TWO_WAY)));

    CT_ASSERT_NO_THROW(compareDatabases());
}


// test that a two-way sync deletes the copy of an item in the other database
void SyncTests::testDelete() {
    // setup client A, B and server so that they all contain the same item
    CT_ASSERT_NO_THROW(doCopy());

    // delete it on A
    CT_ASSERT_NO_THROW(allSourcesDeleteAll());

    // transfer change from A to server to B
    doSync(__FILE__, __LINE__,
           "delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    accessClientB->doSync(__FILE__, __LINE__,
                          "delete",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,0,1, 0,0,0, true, SYNC_TWO_WAY)));

    // check client B: shouldn't have any items now
    for (source_it it = sources.begin(); it != sources.end(); ++it) {
        TestingSyncSourcePtr copy;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
        CT_ASSERT_NO_THROW(copy.reset());
    }
}

// test what the server does when it finds that different
// fields of the same item have been modified
void SyncTests::testMerge() {
    // setup client A, B and server so that they all contain the same item
    CT_ASSERT_NO_THROW(doCopy());

    // update in client A
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->update(it->second->createSourceA, it->second->config.m_mergeItem1));
    }

    // update in client B
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->update(it->second->createSourceA, it->second->config.m_mergeItem2));
    }

    // send change to server from client A (no conflict)
    doSync(__FILE__, __LINE__,
           "update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));
    // Now the changes from client B (conflict!).
    // There are several possible outcomes:
    // - client item completely replaces server item
    // - server item completely replaces client item (update on client)
    // - server merges and updates client
    accessClientB->doSync(__FILE__, __LINE__,
                          "conflict",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_TWO_WAY)));

    // figure out how the conflict during ".conflict" was handled
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        TestingSyncSourcePtr copy;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(it->second->createSourceA()));
        int numItems = 0;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), numItems = countItems(copy.get()));
        CT_ASSERT(numItems >= 1);
        CT_ASSERT(numItems <= 2);
        std::cerr << " \"" << it->second->config.m_sourceName << ": " << (numItems == 1 ? "conflicting items were merged" : "both of the conflicting items were preserved") << "\" ";
        std::cerr.flush();
        CT_ASSERT_NO_THROW(copy.reset());        
    }

    // now pull the same changes into client A
    doSync(__FILE__, __LINE__,
           "refresh",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(-1,-1,-1, 0,0,0, true, SYNC_TWO_WAY)));

    // client A and B should have identical data now
    CT_ASSERT_NO_THROW(compareDatabases());

    // Furthermore, it should be identical with the server.
    // Be extra careful and pull that data anew and compare once more.
    doSync(__FILE__, __LINE__,
           "check",
           SyncOptions(RefreshFromPeerMode(),
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_REFRESH_FROM_REMOTE)));
    CT_ASSERT_NO_THROW(compareDatabases());
}

// test what the server does when it has to execute a slow sync
// with identical data on client and server:
// expected behaviour is that nothing changes
void SyncTests::testTwinning() {
    // clean server and client A
    CT_ASSERT_NO_THROW(deleteAll());

    // import test data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->testImport());
    }

    // send to server
    doSync(__FILE__, __LINE__, "send", SyncOptions(SYNC_TWO_WAY));

    // ensure that client has the same data, thus ignoring data conversion
    // issues (those are covered by testItems())
    CT_ASSERT_NO_THROW(refreshClient());

    // copy to client B to have another copy
    CT_ASSERT_NO_THROW(accessClientB->refreshClient());

    // slow sync should not change anything
    doSync(__FILE__, __LINE__, "twinning", SyncOptions(SYNC_SLOW));

    // check
    CT_ASSERT_NO_THROW(compareDatabases());
}

// tests one-way sync from peer:
// - get both clients and server in sync with no items anywhere
// - add one item on first client, copy to server
// - add a different item on second client, one-way-from-server
// - two-way sync with first client
// => one item on first client, two on second
// - delete on first client, sync that to second client
//   via two-way sync + one-way-from-server
// => one item left on second client (the one inserted locally)
void SyncTests::doOneWayFromRemote(SyncMode oneWayFromRemote) {
    // no items anywhere
    CT_ASSERT_NO_THROW(deleteAll());
    CT_ASSERT_NO_THROW(accessClientB->refreshClient());

    // check that everything is empty, also resets change tracking
    // in second sources of each client
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add one item on first client, copy to server, and check change tracking via second source
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->insertManyItems(it->second->createSourceA, 200, 1));
    }
    doSync(__FILE__, __LINE__,
           "send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add a different item on second client, one-way-from-server
    // => one item added locally, none sent to server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->insertManyItems(it->second->createSourceA, 2, 1));

        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }
    accessClientB->doSync(__FILE__, __LINE__,
                          "recv",
                          SyncOptions(oneWayFromRemote,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_ONE_WAY_FROM_REMOTE)));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 2, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }

    // two-way sync with first client for verification
    // => no changes
    doSync(__FILE__, __LINE__,
           "check",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }

    // delete items on clientA, sync to server
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->deleteAll(it->second->createSourceA));

        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }
    doSync(__FILE__, __LINE__,
           "delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }

    // sync the same change to second client
    // => one item left (the one inserted locally)
    accessClientB->doSync(__FILE__, __LINE__,
                          "delete",
                          SyncOptions(oneWayFromRemote,
                                      CheckSyncReport(0,0,1, 0,0,0, true, SYNC_ONE_WAY_FROM_REMOTE)));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }
}

// one-way-from-remote test with one-way-from-client/server, depending
// on role of remote side
void SyncTests::testOneWayFromServer()
{
    CT_ASSERT_NO_THROW(doOneWayFromRemote(OneWayFromPeerMode()));
}

void SyncTests::testOneWayFromRemote()
{
    CT_ASSERT_NO_THROW(doOneWayFromRemote(SYNC_ONE_WAY_FROM_REMOTE));
}

// tests one-way sync from local side:
// - get both clients and server in sync with no items anywhere
// - add one item on first client, copy to server
// - add a different item on second client, one-way-from-client
// - two-way sync with first client
// => two items on first client, one on second
// - delete on second client, sync that to first client
//   via one-way-from-client, two-way
// => one item left on first client (the one inserted locally)
void SyncTests::doOneWayFromLocal(SyncMode oneWayFromLocal) {
    // no items anywhere
    CT_ASSERT_NO_THROW(deleteAll());
    CT_ASSERT_NO_THROW(accessClientB->deleteAll());

    // check that everything is empty, also resets change tracking
    // in second sources of each client
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add one item on first client, copy to server, and check change tracking via second source
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->insertManyItems(it->second->createSourceA, 1, 1));
    }
    doSync(__FILE__, __LINE__,
           "send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add a different item on second client, one-way-from-client
    // => no item added locally, one sent to server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->insertManyItems(it->second->createSourceA, 2, 1));

        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }
    accessClientB->doSync(__FILE__, __LINE__,
                          "send",
                          SyncOptions(oneWayFromLocal,
                                      CheckSyncReport(0,0,0, 1,0,0, true, SYNC_ONE_WAY_FROM_LOCAL)));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }

    // two-way sync with client A for verification
    // => receive one item
    doSync(__FILE__, __LINE__,
           "check",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 2, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }

    // delete items on client B, sync to server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->deleteAll(it->second->createSourceA));

        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }
    accessClientB->doSync(__FILE__, __LINE__,
                          "delete",
                          SyncOptions(oneWayFromLocal,
                                      CheckSyncReport(0,0,0, 0,0,1, true, SYNC_ONE_WAY_FROM_LOCAL)));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }

    // sync the same change to client A
    // => one item left (the one inserted locally)
    doSync(__FILE__, __LINE__,
           "delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,1, 0,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }
}

// do a two-way sync without additional checks,
// may or may not actually be done in two-way mode
void SyncTests::testTwoWaySync()
{
    doSync(__FILE__, __LINE__, SyncOptions(SYNC_TWO_WAY));
}

void SyncTests::testSlowSync()
{
    doSync(__FILE__, __LINE__,
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_SLOW)));
}

// one-way-from-local test with one-way-from-client/server, depending
// on role of local side
void SyncTests::testOneWayFromClient()
{
    CT_ASSERT_NO_THROW(doOneWayFromLocal(OneWayFromLocalMode()));
}

// do a slow sync without additional checks
void SyncTests::testOneWayFromLocal()
{
    CT_ASSERT_NO_THROW(doOneWayFromLocal(SYNC_ONE_WAY_FROM_LOCAL));
}

// get engine ready, then use it to convert our test items
// to and from the internal field list
void SyncTests::testConversion() {
    bool success = false;
    SyncOptions::Callback_t callback = boost::bind(&SyncTests::doConversionCallback, this, &success, _1, _2);

    doSync(__FILE__, __LINE__,
           SyncOptions(SYNC_TWO_WAY, CheckSyncReport(-1,-1,-1, -1,-1,-1, false))
           .setStartCallback(callback));
    CT_ASSERT(success);
}

bool SyncTests::doConversionCallback(bool *success,
                                     SyncContext &syncClient,
                                     SyncOptions &options) {
    *success = false;

    for (source_it it = sources.begin(); it != sources.end(); ++it) {
        const ClientTest::Config *config = &it->second->config;
        TestingSyncSource *source = static_cast<TestingSyncSource *>(syncClient.findSource(config->m_sourceName));
        CT_ASSERT(source);

        std::string type = source->getNativeDatatypeName();
        if (type.empty()) {
            continue;
        }

        std::list<std::string> items;
        std::string testcases;
        ClientTest::getItems(config->m_testcases, items, testcases);
        std::string converted = getCurrentTest();
        converted += ".converted.";
        converted += config->m_sourceName;
        converted += ".dat";
        simplifyFilename(converted);
        std::ofstream out(converted.c_str());
        BOOST_FOREACH(const string &item, items) {
            string convertedItem = item;
            if(!sysync::DataConversion(syncClient.getSession().get(),
                                       type.c_str(),
                                       type.c_str(),
                                       convertedItem)) {
                SE_LOG_ERROR(NULL, "failed parsing as %s:\n%s",
                             type.c_str(),
                             item.c_str());
            } else {
                out << convertedItem << "\n";
            }
        }
        out.close();

        // The test used peer-specific test cases, but the actual
        // result does not depend on the peer because we haven't
        // received the peer's DevInf at the point where we
        // import/export the test cases (=> don't apply peer-specific
        // synccompare workarounds).
        //
        // Due to the lack of DevInf, properties and parameters which
        // need to be enabled via DevInf get lost (= filter them out).
        ScopedEnvChange env("CLIENT_TEST_SERVER", "");
        ScopedEnvChange envParams("CLIENT_TEST_STRIP_PARAMETERS", "X-EVOLUTION-UI-SLOT");
        CT_ASSERT(config->m_compare(client, testcases, converted));
    }

    // abort sync after completing the test successfully (no exception so far!)
    *success = true;
    return true;
}

// imports test data, transmits it from client A to the server to
// client B and then compares which of the data has been transmitted
void SyncTests::testItems() {
    // clean server and first test database
    CT_ASSERT_NO_THROW(deleteAll());

    // import data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->testImport());
    }

    // transfer from client A to server to client B
    doSync(__FILE__, __LINE__, "send", SyncOptions(SYNC_TWO_WAY).setWBXML(true));
    CT_ASSERT_NO_THROW(accessClientB->refreshClient(SyncOptions().setWBXML(true)));

    CT_ASSERT_NO_THROW(compareDatabases());
}

// creates several items, transmits them back and forth and
// then compares which of them have been preserved
void SyncTests::testItemsXML() {
    // clean server and first test database
    CT_ASSERT_NO_THROW(deleteAll());

    // import data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->testImport());
    }

    // transfer from client A to server to client B using the non-default XML format
    doSync(__FILE__, __LINE__, "send", SyncOptions(SYNC_TWO_WAY).setWBXML(false));
    CT_ASSERT_NO_THROW(accessClientB->refreshClient(SyncOptions().setWBXML(false)));

    CT_ASSERT_NO_THROW(compareDatabases());
}

// imports test data, transmits it from client A to the server to
// client B, update on B and transfers back to the server,
// then compares against reference data that has the same changes
// applied on A
void SyncTests::testExtensions() {
    // clean server and first test database
    CT_ASSERT_NO_THROW(deleteAll());

    // import data and create reference data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->testImport());

        string refDir = getCurrentTest() + "." + it->second->config.m_sourceName + ".ref.dat";
        simplifyFilename(refDir);
        rm_r(refDir);
        mkdir_p(refDir);

        TestingSyncSourcePtr source;
        int counter = 0;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
        BOOST_FOREACH(const string &luid, source->getAllItems()) {
            string item;
            source->readItemRaw(luid, item);
            CT_ASSERT_NO_THROW(it->second->config.m_update(item));
            ofstream out(StringPrintf("%s/%d", refDir.c_str(), counter).c_str());
            out.write(item.c_str(), item.size());
            counter++;
        }
        CT_ASSERT_NO_THROW(source.reset());
    }

    // transfer from client A to server to client B
    doSync(__FILE__, __LINE__, "send", SyncOptions(SYNC_TWO_WAY));
    CT_ASSERT_NO_THROW(accessClientB->refreshClient(SyncOptions()));

    // update on client B
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->updateData(it->second->createSourceB));
    }

    // send back
    accessClientB->doSync(__FILE__, __LINE__, "update", SyncOptions(SYNC_TWO_WAY));
    doSync(__FILE__, __LINE__, "patch", SyncOptions(SYNC_TWO_WAY));

    bool equal = true;
    for (it = sources.begin(); it != sources.end(); ++it) {
        string refDir = getCurrentTest() + "." + it->second->config.m_sourceName + ".ref.dat";
        simplifyFilename(refDir);
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
        // Compare data in source A against reference data *without*
        // telling synccompare to ignore known data loss for the
        // server. CLIENT_TEST_SERVER is relevant for finding the
        // right source config and thus setting it must come after the
        // createSourceB() call.
        ScopedEnvChange env("CLIENT_TEST_SERVER", "");
        ScopedEnvChange envParams("CLIENT_TEST_STRIP_PARAMETERS", "X-EVOLUTION-UI-SLOT"); 
        // X-EVOLUTION-FILE-AS is not preserved correctly with EDS due
        // to setting it on incoming items. It is not always set as already stored
        // locally, so when writing back, the new, artificially generated value overwrites
        // the one chosen by the user. Not ideal, but the alternative (not setting it on
        // incoming items) is worse.
        ScopedEnvChange envProps("CLIENT_TEST_STRIP_PROPERTIES", "(PHOTO|FN|X-EVOLUTION-FILE-AS)");
        if (!it->second->compareDatabases(refDir.c_str(), *source, false)) {
            equal = false;
        }
    }
    CT_ASSERT(equal);
}

// tests the following sequence of events:
// - both clients in sync with server
// - client 1 adds item
// - client 1 updates the same item
// - client 2 gets item: the client should be asked to add the item
//
// However it has been observed that sometimes the item was sent as "update"
// for a non-existant local item. This is a server bug, the client does not
// have to handle that. See
// http://forge.objectweb.org/tracker/?func=detail&atid=100096&aid=305018&group_id=96
void SyncTests::testAddUpdate() {
    // clean server and both test databases
    CT_ASSERT_NO_THROW(deleteAll());
    accessClientB->refreshClient();

    // add item
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA, it->second->config.m_insertItem, false));
    }
    doSync(__FILE__, __LINE__,
           "add",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // update it
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->update(it->second->createSourceB, it->second->config.m_updateItem));
    }
    doSync(__FILE__, __LINE__,
           "update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));

    // now download the updated item into the second client
    accessClientB->doSync(__FILE__, __LINE__,
                          "recv",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    // compare the two databases
    CT_ASSERT_NO_THROW(compareDatabases());
}

// test copying with maxMsg and no large object support
void SyncTests::testMaxMsg()
{
    CT_ASSERT_NO_THROW(doVarSizes(true, false));
}

// test copying with maxMsg and large object support
void SyncTests::testLargeObject()
{
    CT_ASSERT_NO_THROW(doVarSizes(true, true));
}

//
// stress tests: execute some of the normal operations,
// but with large number of artificially generated items
//

// two-way sync with clean client/server,
// followed by slow sync and comparison
// via second client
void SyncTests::testManyItems() {
    // clean server and client A
    CT_ASSERT_NO_THROW(deleteAll());

    // import artificial data: make them large to generate some
    // real traffic and test buffer handling
    source_it it;
    int num_items = defNumItems();
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->insertManyItems(it->second->createSourceA, 0, num_items, 2000));
    }

    // send data to server
    doSync(__FILE__, __LINE__,
           "send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, num_items,0,0, true, SYNC_TWO_WAY),
                       SyncOptions::DEFAULT_MAX_MSG_SIZE,
                       SyncOptions::DEFAULT_MAX_OBJ_SIZE, 
                       true));

    // ensure that client has the same data, ignoring data conversion
    // issues (those are covered by testItems())
    CT_ASSERT_NO_THROW(refreshClient());

    // also copy to second client
    accessClientB->refreshClient();

    // slow sync now should not change anything
    doSync(__FILE__, __LINE__,
           "twinning",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_SLOW),
                       SyncOptions::DEFAULT_MAX_MSG_SIZE,
                       SyncOptions::DEFAULT_MAX_OBJ_SIZE, 
                       true));

    // compare
    CT_ASSERT_NO_THROW(compareDatabases());
}

/**
 * Tell server to delete plenty of items.
 */
void SyncTests::testManyDeletes() {
    // clean server and client A
    CT_ASSERT_NO_THROW(deleteAll());

    // import artificial data: make them small, we just want
    // many of them
    source_it it;
    int num_items = defNumItems();
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->insertManyItems(it->second->createSourceA, 0, num_items, 100));
    }

    // send data to server
    doSync(__FILE__, __LINE__,
           "send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, num_items,0,0, true, SYNC_TWO_WAY),
                       64 * 1024, 64 * 1024, true));

    // ensure that client has the same data, ignoring data conversion
    // issues (those are covered by testItems())
    CT_ASSERT_NO_THROW(refreshClient());

    // also copy to second client
    accessClientB->refreshClient();

    // slow sync now should not change anything
    doSync(__FILE__, __LINE__,
           "twinning",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_SLOW),
                       64 * 1024, 64 * 1024, true));

    // compare
    CT_ASSERT_NO_THROW(compareDatabases());

    // delete everything locally
    CT_ASSERT_NO_THROW(allSourcesDeleteAll());
    doSync(__FILE__, __LINE__,
           "delete-server",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,num_items, true, SYNC_TWO_WAY),
                       10 * 1024));

    // Reporting locally deleted items depends on sync mode
    // recognition, see SyncContext.cpp.
    const char* checkSyncModeStr = getenv("CLIENT_TEST_NOCHECK_SYNCMODE");    

    // update second client
    accessClientB->doSync(__FILE__, __LINE__,
                          "delete-client",
                          SyncOptions(RefreshFromPeerMode(),
                                      checkSyncModeStr ? CheckSyncReport() :
                                      CheckSyncReport(0,0,num_items, 0,0,0, true, SYNC_REFRESH_FROM_REMOTE),
                                      10 * 1024));
}

/**
 * - get client A, server, client B in sync with one item
 * - force slow sync in A: must not duplicate items, but may update it locally
 * - refresh client B (in case that the item was updated)
 * - delete item in B and server via two-way sync
 * - refresh-from-server in B to check that item is gone
 * - two-way in A: must delete the item
 */
void SyncTests::testSlowSyncSemantic()
{
    // set up one item everywhere
    CT_ASSERT_NO_THROW(doCopy());

    // slow in A
    doSync(__FILE__, __LINE__,
           "slow",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(0,-1,0, -1,-1,0, true, SYNC_SLOW)));

    // refresh B, delete item
    accessClientB->doSync(__FILE__, __LINE__,
                          "refresh",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,-1,0, 0,0,0, true, SYNC_TWO_WAY)));
    CT_ASSERT_NO_THROW(accessClientB->allSourcesDeleteAll());
    accessClientB->doSync(__FILE__, __LINE__,
                          "delete",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    accessClientB->doSync(__FILE__, __LINE__,
                          "check",
                          SyncOptions(RefreshFromPeerMode(),
                                      CheckSyncReport(0,0,0, 0,0,0, true, SYNC_REFRESH_FROM_REMOTE)));

    // now the item should also be deleted on A
    doSync(__FILE__, __LINE__,
           "delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,1, 0,0,0, true, SYNC_TWO_WAY)));
}

/**
 * check that refresh-from-server works correctly:
 * - create the same item on A, server, B via testCopy()
 * - refresh B (one item deleted, one created)
 * - delete item on A and server
 * - refresh B (one item deleted)
 */
void SyncTests::testComplexRefreshFromServerSemantic()
{
    CT_ASSERT_NO_THROW(testCopy());

    // Reporting locally deleted items depends on sync mode
    // recognition, see SyncContext.cpp.
    const char* checkSyncModeStr = getenv("CLIENT_TEST_NOCHECK_SYNCMODE");    

    // check refresh with one item on server
    const char *value = getenv ("CLIENT_TEST_NOREFRESH");
    // If refresh_from_server or refresh_from_client (depending on this is a
    // server or client) is not supported, we can still test via slow sync.
    if (value) {
        accessClientB->refreshClient();
    } else {
        accessClientB->doSync(__FILE__, __LINE__,
                              "refresh-one",
                              SyncOptions(RefreshFromPeerMode(),
                                          checkSyncModeStr ? CheckSyncReport() :
                                          CheckSyncReport(1,0,1, 0,0,0, true, SYNC_REFRESH_FROM_REMOTE)));
    }

    // delete that item via A, check again
    CT_ASSERT_NO_THROW(allSourcesDeleteAll());
    doSync(__FILE__, __LINE__,
           "delete-item",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    if (value) {
        accessClientB->refreshClient();
    } else {
        accessClientB->doSync(__FILE__, __LINE__,
                              "refresh-none",
                              SyncOptions(RefreshFromPeerMode(),
                                          checkSyncModeStr ? CheckSyncReport() :
                                          CheckSyncReport(0,0,1, 0,0,0, true, SYNC_REFRESH_FROM_REMOTE)));
    }
}

/**
 * - create the same item on A, server, B via testCopy()
 * - delete on both sides
 * - sync A
 * - sync B
 *
 * Must not fail, even though the Synthesis engine will ask the backends
 * for deletion of an already deleted item.
 */
void SyncTests::testDeleteBothSides()
{
    CT_ASSERT_NO_THROW(testCopy());

    CT_ASSERT_NO_THROW(allSourcesDeleteAll());
    CT_ASSERT_NO_THROW(accessClientB->allSourcesDeleteAll());

    doSync(__FILE__, __LINE__,
           "delete-item-A",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }


    // it is undefined whether the item is meant to be reported as deleted again here:
    // a SyncML client test will mark it as deleted, local sync as server won't
    accessClientB->doSync(__FILE__, __LINE__,
                          "delete-item-B",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,0,0, 0,0,-1, true, SYNC_TWO_WAY)));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CT_ASSERT_NO_THROW(source.reset());
        }
    }
}

/**
 * - clean A, server, B
 * - create an item on A
 * - sync A
 * - create a modified version of the item on B
 * - sync B
 *
 * Depends on UID and LAST-MODIFIED in item data, i.e., iCalendar 2.0.
 * Uses the normal "insertItem" test case. Only works for a single source.
 *
 * The server must not duplicate the item *and* preserve the modified
 * properties.
 *
 * Temporary: because conflict resolution is server-dependent, such a strict
 * test fails. For example, with SyncEvolution 1.2 as server, DESCRIPTION and
 * LOCATION end up being concatenated (merge=lines mode). The test now avoids
 * using different data, with the expected outcome that only one item
 * is present at the end and no unnecessary data transfers happen (only true
 * for SyncEvolution server).
 *
 * A similar situation occurs on the client side, but it is harder to
 * trigger: the updated item must be added to the client's database
 * after it has reported its changes. Because if it happens earlier,
 * it would send an Add to the server and the server would have to
 * resolve the add<->add conflict, as in this test here.
 */

// using updated item data makes the test harder to pass:
// server must use exactly the right item, which currently
// is not the case for SyncEvolution
bool addBothSidesUsesUpdateItem = true;

// SyncEvolution passes with addBothSidesUsesUpdateItem == true
// if we avoid changes to those properties in the iCalendar test
// set which currently use merge=lines.
bool addBothSidesNoMergeLines=true;

// if true, relax expectations for updates from server:
// may or may not send one
bool addBothSidesMayUpdate = false;

// if true, then accept that the Synthesis server mode counts
// Add commands as "added items" even if they are turned into updates
bool addBothSidesAddStatsBroken = false;

// if true, then the peer is a SyncML server which does not
// support UID/RECURRENCE-ID and thus doesn't detect
// duplicates itself; the client needs to do that
bool addBothSidesServerIsDumb = getenv("CLIENT_TEST_ADD_BOTH_SIDES_SERVER_IS_DUMB") != NULL;

static void testAddBothSidesFixUpdateItem(std::string &updateItem)
{
    if (addBothSidesNoMergeLines) {
        // VEVENT
        boost::replace_all(updateItem, "LOCATION:big meeting room", "LOCATION:my office");
        boost::replace_all(updateItem, "DESCRIPTION:nice to see you", "DESCRIPTION:let's talk<<REVISION>>");
        // VJOURNAL
        boost::replace_all(updateItem, "DESCRIPTION:Summary\\nBody text", "DESCRIPTION:Summary Modified\\nBody text");
        // VTODO
        boost::replace_all(updateItem, "DESCRIPTION:to be done", "DESCRIPTION:to be done<<REVISION>>");
    }
}

void SyncTests::testAddBothSides()
{
    CT_ASSERT_NO_THROW(deleteAll());
    accessClientB->deleteAll();

    std::string insertItem = sources[0].second->config.m_insertItem;
    std::string updateItem = sources[0].second->config.m_updateItem;
    testAddBothSidesFixUpdateItem(updateItem);

    CT_ASSERT_NO_THROW(sources[0].second->insert(sources[0].second->createSourceA,
                                                      insertItem));

    doSync(__FILE__, __LINE__,
           "send-old",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // insert updated item data on B
    std::string data;
    CT_ASSERT_NO_THROW(accessClientB->sources[0].second->insert(accessClientB->sources[0].second->createSourceA,
                                                                addBothSidesUsesUpdateItem ?
                                                                updateItem:
                                                                insertItem,
                                                                false,
                                                                &data));

    // As far as the client knows, it is adding an item;
    // server not expected to send back an update (our data was more recent
    // and completely overwrites the server's data).
    // When acting as server, we do the duplicate detection and thus know
    // more about the actual outcome.
    accessClientB->doSync(__FILE__, __LINE__,
                          "send-update",
                          SyncOptions(SYNC_TWO_WAY,
                                      isServerMode() ?
                                      CheckSyncReport(addBothSidesAddStatsBroken ? -1 : 0,0,0,

                                                      0,
                                                      addBothSidesMayUpdate ? -1 :
                                                      addBothSidesUsesUpdateItem ? 1 : 0,
                                                      0,

                                                      true, SYNC_TWO_WAY) :
                                      addBothSidesServerIsDumb ?
                                      CheckSyncReport(addBothSidesServerIsDumb ? 1 : 0,
                                                      addBothSidesMayUpdate ? -1 : 0,
                                                      0,

                                                      // client got one redundant item from
                                                      // server, had to receive it, match against
                                                      // its own copy, then tell the server to
                                                      // update one copy and delete the other;
                                                      // no update necessary on server because
                                                      // it already had the latest copy
                                                      1,0,1, true, SYNC_TWO_WAY).setRestarts(1) :
                                      CheckSyncReport(0,
                                                      addBothSidesMayUpdate ? -1 : 0,
                                                      0,

                                                      // client doesn't know that the add
                                                      // was an update, in contrast to server
                                                      1,0,0, true, SYNC_TWO_WAY)));

    // update sent to client A
    doSync(__FILE__, __LINE__,
           "update",
           SyncOptions(SYNC_TWO_WAY,
                       (!isServerMode() && addBothSidesServerIsDumb) ?
                       // server had to be told to update old item
                       // and delete redundant one, which is what it now
                       // also tells us here
                       CheckSyncReport(1,0,1,
                                       0,0,0, true, SYNC_TWO_WAY) :
                       CheckSyncReport(0,
                                       addBothSidesMayUpdate ? -1 :
                                       addBothSidesUsesUpdateItem ? 1 : 0,
                                       0,

                                       0,0,0, true, SYNC_TWO_WAY)));

    // nothing necessary for client B
    accessClientB->doSync(__FILE__, __LINE__,
                          "nop",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    // now compare client A against reference data
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(sources[0].second->createSourceB()));
    sources[0].second->compareDatabases(*copy, &data, (void *)NULL);
    CT_ASSERT_NO_THROW(copy.reset());
}

/**
 * compared to testAddBothSides the age of the items is reversed now;
 * a server which always copies the client's data passes testAddBothSides
 * but fails here
 */
void SyncTests::testAddBothSidesRefresh()
{
    CT_ASSERT_NO_THROW(deleteAll());
    accessClientB->deleteAll();

    std::string insertItem = sources[0].second->config.m_insertItem;
    std::string updateItem = sources[0].second->config.m_updateItem;
    testAddBothSidesFixUpdateItem(updateItem);

    // insert initial item data on B
    CT_ASSERT_NO_THROW(accessClientB->sources[0].second->insert(accessClientB->sources[0].second->createSourceA,
                                                                     insertItem));

    // sleep one second to ensure that it's mangled LAST-MODIFIED is older than
    // the one from the next item, inserted on A
    sleep(1);

    // more recent data sent to server first
    std::string data;
    CT_ASSERT_NO_THROW(sources[0].second->insert(sources[0].second->createSourceA,
                                                      addBothSidesUsesUpdateItem ?
                                                      updateItem :
                                                      insertItem,
                                                      false,
                                                      &data));
    doSync(__FILE__, __LINE__,
           "send-new",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // As far as the client knows, it is adding an item;
    // server expected to send back an update (client's data was out-dated);
    // When acting as server, we do the duplicate detection and thus
    // know more about the actual outcome.
    accessClientB->doSync(__FILE__, __LINE__,
                          "send-old",
                          SyncOptions(SYNC_TWO_WAY,
                                      isServerMode() ?
                                      CheckSyncReport(addBothSidesAddStatsBroken ? -1 : 0,
                                                      addBothSidesMayUpdate ? -1 :
                                                      addBothSidesUsesUpdateItem ? 1 : 0,
                                                      0,

                                                      0,
                                                      addBothSidesMayUpdate ? -1 : 0,
                                                      0,

                                                      true, SYNC_TWO_WAY) :
                                      // When the server is dumb, it
                                      // will just accept the added
                                      // item and send us an <Add>
                                      // with an item that has the
                                      // same UID as the one it just
                                      // received. The client then
                                      // must start a second sync and
                                      // fix the server by sending an update
                                      // (of the old version) and a delete (of the
                                      // new one)
                                      addBothSidesServerIsDumb ?
                                      CheckSyncReport(1,0,0,
                                                      1,1,1, true, SYNC_TWO_WAY).setRestarts(1) :
                                      CheckSyncReport(0,
                                                      addBothSidesMayUpdate ? -1 :
                                                      addBothSidesUsesUpdateItem ? 1 : 0,
                                                      0,

                                                      // client doesn't know that add was
                                                      // an update
                                                      1,0,0, true, SYNC_TWO_WAY)));

    // potentially send update to A
    doSync(__FILE__, __LINE__,
           "nopA",
           SyncOptions(SYNC_TWO_WAY,
                       (!isServerMode() && addBothSidesServerIsDumb) ?
                       // receives extra changes because dumb server had to be fixed
                       CheckSyncReport(1,0,1, 0,0,0, true, SYNC_TWO_WAY) :
                       CheckSyncReport(0,addBothSidesMayUpdate ? -1 : 0,0, 0,0,0, true, SYNC_TWO_WAY)));

    // nothing necessary for client B (already synchronized completely above in one sync)
    accessClientB->doSync(__FILE__, __LINE__,
                          "nopB",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    // now compare client A against reference data
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(sources[0].second->createSourceB()));
    sources[0].second->compareDatabases(*copy, &data, (void *)NULL);
    CT_ASSERT_NO_THROW(copy.reset());
}

/**
 * - adds parent on client A
 * - syncs A
 * - adds unrelated item via client B (necessary to trigger corner cases in
 *   change tracking, see BMC #22329)
 * - syncs B
 * - adds child on client A
 * - syncs A and B
 * - compares
 */
void SyncTests::testLinkedItemsParentChild()
{
    source_it it;

    // clean server, client A and client B
    CT_ASSERT_NO_THROW(deleteAll());
    accessClientB->refreshClient();

    // create and copy parent item
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT(!it->second->config.m_linkedItems.empty());
        CT_ASSERT(it->second->config.m_linkedItems[0].size() >= 2);
        TestingSyncSourcePtr source;
        CT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                              it->second->config.m_linkedItems[0][0],
                                              false));
    }
    doSync(__FILE__, __LINE__,
           "send-parent",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // create independent item, refresh client B and server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        CT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                              it->second->config.m_insertItem,
                                              false));
    }
    accessClientB->doSync(__FILE__, __LINE__,
                          "recv-parent",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // add child on client A
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT(!it->second->config.m_linkedItems.empty());
        CT_ASSERT(it->second->config.m_linkedItems[0].size() >= 2);
        TestingSyncSourcePtr source;
        CT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                              it->second->config.m_linkedItems[0][1],
                                              false));
    }
    // parent may or may not be considered updated
    doSync(__FILE__, __LINE__,
           "send-child",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(1,0,0, 1,-1,0, true, SYNC_TWO_WAY)));
    // parent may or may not be considered updated here
    accessClientB->doSync(__FILE__, __LINE__,
                          "recv-child",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,-1,0, 0,0,0, true, SYNC_TWO_WAY)));

    // final comparison
    CT_ASSERT_NO_THROW(compareDatabases());
}

/**
 * - adds child on client A
 * - syncs A
 * - syncs B
 * - compare
 */
void SyncTests::testLinkedItemsChild()
{
    source_it it;

    // clean server, client A and client B
    CT_ASSERT_NO_THROW(deleteAll());
    accessClientB->refreshClient();

    // create and copy child item
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT(!it->second->config.m_linkedItems.empty());
        CT_ASSERT(it->second->config.m_linkedItems[0].size() >= 2);
        TestingSyncSourcePtr source;
        CT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                              it->second->config.m_linkedItems[0][1],
                                              false));
    }
    doSync(__FILE__, __LINE__,
           "send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));
    accessClientB->doSync(__FILE__, __LINE__,
                          "recv",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    // final comparison
    CT_ASSERT_NO_THROW(compareDatabases());
}


/**
 * - adds child on client A
 * - syncs A and B
 * - adds parent on client A
 * - syncs A and B
 * - compares
 */
void SyncTests::testLinkedItemsChildParent()
{
    source_it it;

    // clean server, client A and client B
    CT_ASSERT_NO_THROW(deleteAll());
    accessClientB->refreshClient();

    // create and copy child item
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT(!it->second->config.m_linkedItems[0].empty());
        CT_ASSERT(it->second->config.m_linkedItems[0].size() >= 2);
        TestingSyncSourcePtr source;
        CT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                              it->second->config.m_linkedItems[0][1],
                                              false));
    }
    doSync(__FILE__, __LINE__,
           "send-child",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));
    accessClientB->doSync(__FILE__, __LINE__,
                          "recv-child",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    // add parent on client A
    for (it = sources.begin(); it != sources.end(); ++it) {
        CT_ASSERT(!it->second->config.m_linkedItems.empty());
        CT_ASSERT(it->second->config.m_linkedItems[0].size() >= 2);
        TestingSyncSourcePtr source;
        // relaxed change checks because child event is also modified
        CT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                              it->second->config.m_linkedItems[0][0],
                                              true));
    }
    // child may or may not be considered updated
    doSync(__FILE__, __LINE__,
           "send-parent",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,-1,0, true, SYNC_TWO_WAY)));
    // child may or may not be considered updated here
    accessClientB->doSync(__FILE__, __LINE__,
                          "recv-parent",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,-1,0, 0,0,0, true, SYNC_TWO_WAY)));

    // final comparison
    CT_ASSERT_NO_THROW(compareDatabases());
}

/**
 * implements testMaxMsg(), testLargeObject(), testLargeObjectEncoded()
 * using a sequence of items with varying sizes
 */
void SyncTests::doVarSizes(bool withMaxMsgSize,
                           bool withLargeObject) {
    int maxMsgSize = 8 * 1024;
    const char* maxItemSize = getenv("CLIENT_TEST_MAX_ITEMSIZE");
    int tmpSize = maxItemSize ? atoi(maxItemSize) : 0;
    if(tmpSize > 0) 
        maxMsgSize = tmpSize;

    // clean server and client A
    CT_ASSERT_NO_THROW(deleteAll());

    // insert items, doubling their size, then restart with small size
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        int item = 1;
        restoreStorage(it->second->config, client);
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        for (int i = 0; i < 2; i++ ) {
            int size = 1;
            while (size < 2 * maxMsgSize) {
                CT_ASSERT_NO_THROW(it->second->insertManyItems(source.get(), item, 1, it->second->config.m_templateItem.size() + 10 + size));
                size *= 2;
                item++;
            }
        }
        backupStorage(it->second->config, client);
    }

    // transfer to server
    doSync(__FILE__, __LINE__,
           "send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, -1,0,0, true, SYNC_TWO_WAY), // number of items sent to server depends on source
                       withMaxMsgSize ? SyncOptions::DEFAULT_MAX_MSG_SIZE: 0,
                       withMaxMsgSize ? SyncOptions::DEFAULT_MAX_OBJ_SIZE : 0,
                       withLargeObject));

    // copy to second client
    const char *value = getenv ("CLIENT_TEST_NOREFRESH");
    // If refresh_from_server or refresh_from_client (depending on this is a
    // server or client) is not supported, we can still test via slow sync.
    if (value) {
        accessClientB->refreshClient();
    } else {
        accessClientB->doSync(__FILE__, __LINE__,
                "recv",
                SyncOptions(RefreshFromPeerMode(),
                    CheckSyncReport(-1,0,-1, 0,0,0, true, SYNC_REFRESH_FROM_REMOTE), // number of items received from server depends on source
                    withLargeObject ? maxMsgSize : withMaxMsgSize ? maxMsgSize * 100 /* large enough so that server can sent the largest item */ : 0,
                    withMaxMsgSize ? maxMsgSize * 100 : 0,
                    withLargeObject));
    }
    // compare
    CT_ASSERT_NO_THROW(compareDatabases());
}

/**
 * Send message to server, then pretend that we timed out at exactly
 * one specific message, specified via m_interruptAtMessage.  The
 * caller is expected to resend the message, without aborting the
 * session. That resend and all following message will get through
 * again.
 *
 * Each send() is counted as one message, starting at 1 for the first
 * message.
 */
class TransportResendInjector : public TransportWrapper{
private:
    int timeout;
public:
    TransportResendInjector()
         :TransportWrapper() {
             const char *s = getenv("CLIENT_TEST_RESEND_TIMEOUT");
             timeout = s ? atoi(s) : 0;
    }

    ~TransportResendInjector() {
    }

    virtual int getResendFailureThreshold() { return 0; }

    virtual void send(const char *data, size_t len)
    {
        m_messageCount++;
        if (m_interruptAtMessage >= 0 &&
                m_messageCount == m_interruptAtMessage+1) {
            m_wrappedAgent->send(data, len);
            m_status = m_wrappedAgent->wait();
            //trigger client side resend
            sleep (timeout);
            m_status = TIME_OUT;
        }
        else 
        {
            m_wrappedAgent->send(data, len);
            m_status = m_wrappedAgent->wait();
        }
    }

    virtual void getReply(const char *&data, size_t &len, std::string &contentType) {
        if (m_status == FAILED) {
            data = "";
            len = 0;
        } else {
            m_wrappedAgent->getReply(data, len, contentType);
        }
    }
};

/**
 * Stop sending at m_interruptAtMessage. The caller is forced to abort
 * the current session and will recover by retrying in another
 * session.
 *
 * Each send() increments the counter by two, so that 1 aborts before
 * the first message and 2 after it.
 */
class TransportFaultInjector : public TransportWrapper{
public:
    TransportFaultInjector()
         :TransportWrapper() {
    }

    ~TransportFaultInjector() {
    }

    virtual void send(const char *data, size_t len)
    {
        if (m_interruptAtMessage == m_messageCount) {
            SE_LOG_DEBUG(NULL, "TransportFaultInjector: interrupt before sending message #%d", m_messageCount);
        }
        m_messageCount++;
        if (m_interruptAtMessage >= 0 &&
            m_messageCount > m_interruptAtMessage) {
            throw string("TransportFaultInjector: interrupt before send");
        }
    
        m_wrappedAgent->send(data, len);

        m_status = m_wrappedAgent->wait();
        
        if (m_interruptAtMessage == m_messageCount) {
            SE_LOG_DEBUG(NULL, "TransportFaultInjector: interrupt after receiving reply #%d", m_messageCount);
        }
        m_messageCount++;
        if (m_interruptAtMessage >= 0 &&
            m_messageCount > m_interruptAtMessage) {
            m_status = FAILED;
        }
    }

    virtual void getReply(const char *&data, size_t &len, std::string &contentType) {
        if (m_status == FAILED) {
            data = "";
            len = 0;
        } else {
            m_wrappedAgent->getReply(data, len, contentType);
        }
    }
};

/**
 * Swallow data at various points:
 * - between "client sent data" and "server receives data"
 * - after "server received data" and before "server sends reply"
 * - after "server has sent reply"
 *
 * The client deals with it by resending. This is similar to
 * TransportResendInjector and the ::Resend tests, but more thorough,
 * and stresses the HTTP server more (needs to deal with "reply not
 * delivered" error).
 *
 * Each send() increments the counter by three, so that 0 aborts
 * before the first message, 1 after sending it, and 2 after receiving
 * its reply.
 *
 * Swallowing data is implemented via the proxy.py script. This is
 * necessary because the wrapped agent has no API to trigger the second
 * error scenario. The wrapped agent is told to use a specific port
 * on localhost, with the base port passing message and reply through,
 * "base + 1" intercepting the message, etc.
 *
 * Because of the use of a proxy, this cannot be used to test servers
 * where a real proxy is needed.
 */
class TransportResendProxy : public TransportWrapper {
private:
    int port;
public:
    TransportResendProxy() : TransportWrapper() {
        const char *s = getenv("CLIENT_TEST_RESEND_PROXY");
        port = s ? atoi(s) : 0;
    }

    virtual int getResendFailureThreshold() { return 2; }

    virtual void send(const char *data, size_t len)
    {
        HTTPTransportAgent *agent = dynamic_cast<HTTPTransportAgent *>(m_wrappedAgent.get());
        CT_ASSERT(agent);

        m_messageCount += 3;
        if (m_interruptAtMessage >= 0 &&
            m_interruptAtMessage < m_messageCount &&
            m_interruptAtMessage >= m_messageCount - 3) {
            int offset = m_interruptAtMessage - m_messageCount + 4;
            SE_LOG_DEBUG(NULL, "TransportResendProxy: interrupt %s",
                         offset == 1 ? "before sending message" :
                         offset == 2 ? "directly after sending message" :
                         "after receiving reply");
            agent->setProxy(StringPrintf("http://127.0.0.1:%d",
                                         offset + port));
        } else {
            agent->setProxy("");
        }
        agent->send(data, len);
        m_status = agent->wait();
    }

    virtual void getReply(const char *&data, size_t &len, std::string &contentType) {
        if (m_status == FAILED) {
            data = "";
            len = 0;
        } else {
            m_wrappedAgent->getReply(data, len, contentType);
        }
    }
};


/**
 * Emulates a user suspend just after receving response 
 * from server.
 */
class UserSuspendInjector : public TransportWrapper{
public:
    UserSuspendInjector()
         :TransportWrapper() {
    }

    ~UserSuspendInjector() {
    }

    virtual void send(const char *data, size_t len)
    {
        m_wrappedAgent->send(data, len);
        m_status = m_wrappedAgent->wait();
    }

    virtual void getReply(const char *&data, size_t &len, std::string &contentType) {
        if (m_status == FAILED) {
            data = "";
            len = 0;
        } else {
            if (m_interruptAtMessage == m_messageCount) {
                 SE_LOG_DEBUG(NULL, "UserSuspendInjector: user suspend after getting reply #%d", m_messageCount);
            }
            m_messageCount++;
            if (m_interruptAtMessage >= 0 &&
                    m_messageCount > m_interruptAtMessage) {
                m_options->m_isSuspended = SuspendFlags::getSuspendFlags().suspend();
            }
            m_wrappedAgent->getReply(data, len, contentType);
        }
    }
};

/**
 * This function covers different error scenarios that can occur
 * during real synchronization. To pass, clients must either force a
 * slow synchronization after a failed synchronization or implement
 * the error handling described in the design guide (track server's
 * status for added/updated/deleted items and resend unacknowledged
 * changes).
 *
 * The items used during these tests are synthetic. They are
 * constructed so that normally a server should be able to handle
 * twinning during a slow sync correctly.
 *
 * Errors are injected into a synchronization by wrapping the normal
 * HTTP transport agent. The wrapper enumerates messages sent between
 * client and server (i.e., one message exchange increments the
 * counter by two), starting from zero. It "cuts" the connection before
 * sending out the next message to the server respectively after the 
 * server has replied, but before returning the reply to the client.
 * The first case simulates a lost message from the client to the server
 * and the second case a lost message from the server to the client.
 *
 * The expected result is the same as in an uninterrupted sync, which
 * is done once at the beginning.
 *
 * Each test goes through the following steps:
 * - client A and B reset local data store
 * - client A creates 3 new items, remembers LUIDs
 * - refresh-from-client A sync
 * - refresh-from-client B sync
 * - client B creates 3 different items, remembers LUIDs
 * - client B syncs
 * - client A syncs => A, B, server are in sync
 * - client A modifies his items (depends on test) and
 *   sends changes to server => server has changes for B
 * - client B modifies his items (depends on test)
 * - client B syncs, transport wrapper simulates lost message n
 * - client B syncs again, resuming synchronization if possible or
 *   slow sync otherwise (responsibility of the client!)
 * - client A syncs (not tested yet: A should be sent exactly the changes made by B)
 * - test that A and B contain same items
 * - test that A contains the same items as the uninterrupted reference run
 * - repeat the steps above ranging starting with lost message 0 until no
 *   message got lost
 *
 * Set the CLIENT_TEST_INTERRUPT_AT env variable to a message number
 * >= 0 to execute one uninterrupted run and then interrupt at that
 * message. Set to -1 to just do the uninterrupted run.
 */
void SyncTests::doInterruptResume(int changes, 
                  boost::shared_ptr<TransportWrapper> wrapper)
{
    int interruptAtMessage = -1;
    const char *t = getenv("CLIENT_TEST_INTERRUPT_AT");
    int requestedInterruptAt = t ? atoi(t) : -2;
    const char *s = getenv("CLIENT_TEST_INTERRUPT_SLEEP");
    int sleep_t = s ? atoi(s) : 0;
    size_t i;
    std::string refFileBase = getCurrentTest() + ".ref.";
    bool equal = true;
    bool resend = wrapper->getResendFailureThreshold() != -1;
    bool suspend = dynamic_cast <UserSuspendInjector *> (wrapper.get()) != NULL;
    bool interrupt = dynamic_cast <TransportFaultInjector *> (wrapper.get()) != NULL;

    // better be large enough for complete DevInf, 20000 is already a
    // bit small when running with many stores
    size_t maxMsgSize = 20000;
    size_t changedItemSize = (changes & BIG) ?
        5 * maxMsgSize / 2 : // large enough to be split over three messages
        0;

    // After running the uninterrupted sync, we remember the number
    // of sent messages. We never interrupt between sending our
    // own last message and receiving the servers last reply,
    // because the server is unable to detect that we didn't get
    // the reply. It will complete the session whereas the client
    // suspends, leading to an unexpected slow sync the next time.
    int maxMsgNum = 0;

    while (true) {
        char buffer[80];
        sprintf(buffer, "%d", interruptAtMessage);
        const char *prefix = interruptAtMessage == -1 ? "complete" : buffer;
        SyncPrefix prefixA(prefix, *this);
        SyncPrefix prefixB(prefix, *accessClientB);

        std::vector< std::list<std::string> > clientAluids;
        std::vector< std::list<std::string> > clientBluids;

        // create new items in client A and sync to server
        clientAluids.resize(sources.size());
        for (i = 0; i < sources.size(); i++) {
            sources[i].second->deleteAll(sources[i].second->createSourceA);
            clientAluids[i] =
                sources[i].second->insertManyItems(sources[i].second->createSourceA,
                                                   1, 3, 0);
        }
        doSync(__FILE__, __LINE__, "fromA", SyncOptions(RefreshFromLocalMode()));

        // init client B and add its items to server and client A
        accessClientB->doSync(__FILE__, __LINE__, "initB", SyncOptions(RefreshFromPeerMode()));
        clientBluids.resize(sources.size());
        for (i = 0; i < sources.size(); i++) {
            clientBluids[i] =
                accessClientB->sources[i].second->insertManyItems(accessClientB->sources[i].second->createSourceA,
                                                                  11, 3, 0);
        }
        accessClientB->doSync(__FILE__, __LINE__, "fromB", SyncOptions(SYNC_TWO_WAY));
        doSync(__FILE__, __LINE__, "updateA", SyncOptions(SYNC_TWO_WAY));

        // => client A, B and server in sync with a total of six items

        // make changes as requested on client A and sync to server
        for (i = 0; i < sources.size(); i++) {
            if (changes & SERVER_ADD) {
                sources[i].second->insertManyItems(sources[i].second->createSourceA,
                                                   4, 1, changedItemSize);
            }
            if (changes & SERVER_REMOVE) {
                // remove second item
                removeItem(sources[i].second->createSourceA,
                           *(++clientAluids[i].begin()));
            }
            if (changes & SERVER_UPDATE) {
                // update third item
                updateItem(sources[i].second->createSourceA,
                           sources[i].second->config,
                           *(++ ++clientAluids[i].begin()),
                           sources[i].second->createItem(3, "updated", changedItemSize).c_str());
            }
        }

        // send using the same mode as in the interrupted sync with client B
        if (changes & (SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE)) {
            doSync(__FILE__, __LINE__, "changesFromA", SyncOptions(SYNC_TWO_WAY).setMaxMsgSize(maxMsgSize));
        }

        // make changes as requested on client B
        for (i = 0; i < sources.size(); i++) {
            if (changes & CLIENT_ADD) {
                accessClientB->sources[i].second->insertManyItems(accessClientB->sources[i].second->createSourceA,
                                                                  14, 1, changedItemSize);
            }
            if (changes & CLIENT_REMOVE) {
                // remove second item
                removeItem(accessClientB->sources[i].second->createSourceA,
                           *(++clientBluids[i].begin()));
            }
            if (changes & CLIENT_UPDATE) {
                // update third item
                updateItem(accessClientB->sources[i].second->createSourceA,
                           accessClientB->sources[i].second->config,
                           *(++ ++clientBluids[i].begin()),
                           accessClientB->sources[i].second->createItem(13, "updated", changedItemSize).c_str());
            }
        }

        // Now do an interrupted sync between B and server.
        // The explicit delete of the TransportAgent is suppressed
        // by overloading the delete operator.
        int wasInterrupted;
        {
            CheckSyncReport check(-1, -1, -1, -1, -1, -1, false);
            if (resend && interruptAtMessage > wrapper->getResendFailureThreshold()) {
                // resend tests must succeed, except for the first
                // message in the session, which is not resent
                check.mustSucceed = true;
            }
            SyncOptions options(SYNC_TWO_WAY, check);
            options.setTransportAgent(wrapper);
            options.setMaxMsgSize(maxMsgSize);
            // disable resending completely or shorten the resend
            // interval to speed up testing
            options.setRetryInterval(resend ? 10 : 0);
            wrapper->setInterruptAtMessage(interruptAtMessage);
            accessClientB->doSync(__FILE__, __LINE__, "changesFromB", options);
            wasInterrupted = interruptAtMessage != -1 &&
                wrapper->getMessageCount() <= interruptAtMessage;
            if (!maxMsgNum) {
                maxMsgNum = wrapper->getMessageCount();
            }
            wrapper->rewind();
        }

        if (interruptAtMessage != -1) {
            if (wasInterrupted) {
                // uninterrupted sync, done
                break;
            }

            // continue, wait until server timeout
            if(sleep_t) 
                sleep (sleep_t);

            // no need for resend tests, unless they were interrupted at the first message
            if (!resend || interruptAtMessage <= wrapper->getResendFailureThreshold()) {
                SyncReport report;
                accessClientB->doSync(__FILE__, __LINE__,
                                      "retryB",
                                      SyncOptions(SYNC_TWO_WAY,
                                                  CheckSyncReport().setMode(SYNC_TWO_WAY).setReport(&report)));
                // Suspending at first and last message doesn't need a
                // resume, everything else does. When multiple sources
                // are involved, some may suspend, some may not, so we
                // cannot check.
                if (suspend &&
                    interruptAtMessage != 0 &&
                    interruptAtMessage + 1 != maxMsgNum &&
                    report.size() == 1) {
                    BOOST_FOREACH(const SyncReport::SourceReport_t &sourceReport, report) {
                        CT_ASSERT(sourceReport.second.isResumeSync());
                    }
                }
            }
        }

        // copy changes to client A
        doSync(__FILE__, __LINE__, "toA", SyncOptions(SYNC_TWO_WAY));

        // compare client A and B
        if (interruptAtMessage != -1 &&
            !compareDatabases(refFileBase.c_str(), false)) {
            equal = false;
            std::cerr << "====> comparison of client B against reference file(s) failed after interrupting at message #" <<
                interruptAtMessage << std::endl;
            std::cerr.flush();
        }
        if (!compareDatabases(NULL, false)) {
            equal = false;
            std::cerr << "====> comparison of client A and B failed after interrupting at message #" <<
                interruptAtMessage << std::endl;
            std::cerr.flush();
        }

        // save reference files from uninterrupted run?
        if (interruptAtMessage == -1) {
            for (source_it it = sources.begin();
                 it != sources.end();
                 ++it) {
                std::string refFile = refFileBase;
                refFile += it->second->config.m_sourceName;
                refFile += ".dat";
                simplifyFilename(refFile);
                TestingSyncSourcePtr source;
                SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
                SOURCE_ASSERT_EQUAL(source.get(), 0, it->second->config.m_dump(client, *source.get(), refFile.c_str()));
                CT_ASSERT_NO_THROW(source.reset());
            }
        }

        // pick next iteration
        if (requestedInterruptAt == -1) {
            // user requested to stop after first iteration
            break;
        } else if (requestedInterruptAt >= 0) {
            // only do one interrupted run of the test
            if (requestedInterruptAt == interruptAtMessage) {
                break;
            } else {
                interruptAtMessage = requestedInterruptAt;
            }
        } else {
            // interrupt one message later than before
            interruptAtMessage++;
            if (interrupt &&
                interruptAtMessage + 1 >= maxMsgNum) {
                // Don't interrupt before the server's last reply,
                // because then the server thinks we completed the
                // session when we think we didn't, which leads to a
                // slow sync. Testing that is better done with a
                // specific test.
                break;
            }
            if (interruptAtMessage >= maxMsgNum) {
                // next run would not interrupt at all, stop now
                break;
            }
        }
    }

    CT_ASSERT(equal);
}

void SyncTests::testInterruptResumeClientAdd()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_ADD, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testInterruptResumeClientRemove()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testInterruptResumeClientUpdate()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testInterruptResumeServerAdd()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_ADD, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testInterruptResumeServerRemove()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testInterruptResumeServerUpdate()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testInterruptResumeClientAddBig()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_ADD|BIG, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testInterruptResumeClientUpdateBig()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_UPDATE|BIG, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testInterruptResumeServerAddBig()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_ADD|BIG, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testInterruptResumeServerUpdateBig()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_UPDATE|BIG, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testInterruptResumeFull()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_ADD|CLIENT_REMOVE|CLIENT_UPDATE|
                                         SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector())));
}

void SyncTests::testUserSuspendClientAdd()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_ADD, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testUserSuspendClientRemove()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_REMOVE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testUserSuspendClientUpdate()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_UPDATE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testUserSuspendServerAdd()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_ADD, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testUserSuspendServerRemove()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_REMOVE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testUserSuspendServerUpdate()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testUserSuspendClientAddBig()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_ADD|BIG, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testUserSuspendClientUpdateBig()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_UPDATE|BIG, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testUserSuspendServerAddBig()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_ADD|BIG, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testUserSuspendServerUpdateBig()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_UPDATE|BIG, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testUserSuspendFull()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_ADD|CLIENT_REMOVE|CLIENT_UPDATE|
                                         SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector())));
}

void SyncTests::testResendClientAdd()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_ADD, boost::shared_ptr<TransportWrapper> (new TransportResendInjector())));
}

void SyncTests::testResendClientRemove()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportResendInjector())));
}

void SyncTests::testResendClientUpdate()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportResendInjector())));
}

void SyncTests::testResendServerAdd()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_ADD, boost::shared_ptr<TransportWrapper> (new TransportResendInjector())));
}

void SyncTests::testResendServerRemove()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportResendInjector())));
}

void SyncTests::testResendServerUpdate()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportResendInjector())));
}

void SyncTests::testResendFull()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_ADD|CLIENT_REMOVE|CLIENT_UPDATE|
                                         SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE, 
                                         boost::shared_ptr<TransportWrapper> (new TransportResendInjector())));
}

void SyncTests::testResendProxyClientAdd()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_ADD, boost::shared_ptr<TransportWrapper> (new TransportResendProxy())));
}

void SyncTests::testResendProxyClientRemove()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportResendProxy())));
}

void SyncTests::testResendProxyClientUpdate()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportResendProxy())));
}

void SyncTests::testResendProxyServerAdd()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_ADD, boost::shared_ptr<TransportWrapper> (new TransportResendProxy())));
}

void SyncTests::testResendProxyServerRemove()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportResendProxy())));
}

void SyncTests::testResendProxyServerUpdate()
{
    CT_ASSERT_NO_THROW(doInterruptResume(SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportResendProxy())));
}

void SyncTests::testResendProxyFull()
{
    CT_ASSERT_NO_THROW(doInterruptResume(CLIENT_ADD|CLIENT_REMOVE|CLIENT_UPDATE|
                                         SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE, 
                                         boost::shared_ptr<TransportWrapper> (new TransportResendProxy())));
}

static bool setDeadSyncURL(SyncContext &context,
                           SyncOptions &options,
                           int port,
                           bool *skipped)
{
    vector<string> urls = context.getSyncURL();
    string url;
    if (urls.size() == 1) {
        url = urls.front();
    }

    // use IPv4 localhost address, that's what we listen on
    string fakeURL = StringPrintf("http://127.0.0.1:%d/foobar", port);

    if (boost::starts_with(url, "http")) {
        context.setSyncURL(fakeURL, true);
        context.setSyncUsername("foo", true);
        context.setSyncPassword("bar", true);
        return false;
    } else if (boost::starts_with(url, "local://")) {
        FullProps props = context.getConfigProps();
        string target = url.substr(strlen("local://"));
        props[target].m_syncProps["syncURL"] = fakeURL;
        props[target].m_syncProps["retryDuration"] = InitStateString("10", true);
        props[target].m_syncProps["retryInterval"] = InitStateString("10", true);
        context.setConfigProps(props);
        return false;
    } else {
        // cannot run test, tell parent
        *skipped = true;
        return true;
    }
}

void SyncTests::testTimeout()
{
    // Create a dead listening socket, then run a sync with a sync URL
    // which points towards localhost at that port. Do this with no
    // message resending and a very short overall timeout. The
    // expectation is that the transmission timeout strikes.
    time_t start = time(NULL);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CT_ASSERT(fd != -1);
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    int res = bind(fd, (sockaddr *)&servaddr, sizeof(servaddr));
    CT_ASSERT_EQUAL(0, res);
    socklen_t len = sizeof(servaddr);
    res = getsockname(fd, (sockaddr *)&servaddr, &len);
    CT_ASSERT_EQUAL(0, res);
    res = listen(fd, 10);
    CT_ASSERT_EQUAL(0, res);
    bool skipped = false;
    SyncReport report;
    doSync(__FILE__, __LINE__,
           "timeout",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(-1, -1, -1, -1, -1, -1,
                                       false).setReport(&report))
           .setPrepareCallback(boost::bind(setDeadSyncURL, _1, _2, ntohs(servaddr.sin_port), &skipped))
           .setRetryDuration(20)
           .setRetryInterval(20));
    time_t end = time(NULL);
    close(fd);
    if (!skipped) {
        CT_ASSERT_EQUAL(STATUS_TRANSPORT_FAILURE, report.getStatus());
        CT_ASSERT(end - start >= 19);
        CT_ASSERT(end - start < 40); // needs to be sufficiently larger than 20s timeout
                                     // because under valgrind the startup time is considerable
    }
}

static void UpdateLocal(const std::string &config, const std::string &source,
                        const std::string &actualLocalData,
                        const std::string &localModified,
                        const std::string &modifyLocal)
{
    // The local side also uses the Cmdline class because then we only
    // need to implement one way of updating items. But first we need to
    // get the actual data.
    std::auto_ptr<Cmdline> cmdline;

    rm_r(actualLocalData);
    mkdir_p(actualLocalData);
    cmdline.reset(new TestCmdline("--daemon=no",
                                  "--export",
                                  actualLocalData.c_str(),
                                  config.c_str(),
                                  source.c_str(),
                                  (const char *)NULL));
    CT_ASSERT(cmdline->parse());
    CT_ASSERT_MESSAGE("export " + currentServer() + "_1 " +  source, cmdline->run());

    CT_ASSERT(!system(StringPrintf("%s %s %s",
                                   modifyLocal.c_str(),
                                   actualLocalData.c_str(),
                                   localModified.c_str()).c_str()));
    CT_ASSERT(isDir(localModified));
    cmdline.reset(new TestCmdline("--daemon=no",
                                  "--update",
                                  localModified.c_str(),
                                  config.c_str(),
                                  source.c_str(),
                                  (const char *)NULL));
    CT_ASSERT(cmdline->parse());
    CT_ASSERT_MESSAGE("update " + config + " " +  source, cmdline->run());
}

void SyncTests::testUpload()
{
    const std::string testname = "testUpload";

    CT_ASSERT_EQUAL(sources.size(), 1);
    const ClientTest::Config &config(sources[0].second->config);
    StringPair peerConfig = getPeerConfig(config.m_sourceName);
    const std::string &peer = peerConfig.first;
    const std::string &peerSource = peerConfig.second;
    CT_ASSERT(!peer.empty());
    CT_ASSERT(!peerSource.empty());

    std::string localTestdata = getPeerTestdata(config.m_sourceName, testname, "local");
    CT_ASSERT_MESSAGE(localTestdata, !access(localTestdata.c_str(), R_OK));
    std::string remoteTestdata = getPeerTestdata(config.m_sourceName, testname, "remote");
    CT_ASSERT_MESSAGE(remoteTestdata, !access(remoteTestdata.c_str(), R_OK));
    std::string modifyRemote = getPeerTestdata(config.m_sourceName, testname, "modify-remote");
    CT_ASSERT_MESSAGE(modifyRemote, !access(modifyRemote.c_str(), R_OK|X_OK));
    std::string localSyncedTestdata = getPeerTestdata(config.m_sourceName, testname, "local-synced");
    CT_ASSERT_MESSAGE(localSyncedTestdata, !access(localSyncedTestdata.c_str(), R_OK));

    std::auto_ptr<Cmdline> cmdline;

    // Import locally into empty database.
    sources[0].second->deleteAll(sources[0].second->createSourceA);
    sources[0].second->doImport(localTestdata);

    // Sync to remote.
    doSync(__FILE__, __LINE__,
           "upload",
           SyncOptions(RefreshFromLocalMode(),
                       CheckSyncReport(0,0,0, -1,0,0, true, SYNC_REFRESH_FROM_LOCAL)));

    // Export from remote directly.
    std::string actualData = getCurrentTest() + ".remote.test.dat";
    simplifyFilename(actualData);
    mkdir_p(actualData);
    cmdline.reset(new TestCmdline("--daemon=no",
                                  "--export",
                                  actualData.c_str(),
                                  peer.c_str(),
                                  peerSource.c_str(),
                                  (const char *)NULL));
    CT_ASSERT(cmdline->parse());
    CT_ASSERT_MESSAGE(peer + " " + peerSource, cmdline->run());

    // Compare against expected result. We use the compare operation
    // of the local source and apply it to data from the remote one.
    // This typically works if the data has the same format.
    {
        ScopedEnvChange fullSyncCompare("CLIENT_TEST_SERVER", "none");
        CT_ASSERT(sources[0].second->compareDatabases(remoteTestdata, actualData));
    }

    // Modify remotely.
    std::string remoteModified = getCurrentTest() + ".remote.modified.test.dat";
    simplifyFilename(remoteModified);
    CT_ASSERT(!system(StringPrintf("%s %s %s",
                                   modifyRemote.c_str(),
                                   actualData.c_str(),
                                   remoteModified.c_str()).c_str()));
    CT_ASSERT(isDir(remoteModified));
    cmdline.reset(new TestCmdline("--daemon=no",
                                  "--update",
                                  remoteModified.c_str(),
                                  peer.c_str(),
                                  peerSource.c_str(),
                                  (const char *)NULL));
    CT_ASSERT(cmdline->parse());
    CT_ASSERT_MESSAGE("update " + peer + " " + peerSource, cmdline->run());

    // Sync between both sides to update the local data.
    doSync(__FILE__, __LINE__,
           "two-way",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,-1,0, 0,0,0, true, SYNC_TWO_WAY)));

    // Compare against expected result.
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(sources[0].second->createSourceA(), TestingSyncSourcePtr::SLOW));
    {
        ScopedEnvChange fullSyncCompare("CLIENT_TEST_SERVER", "none");
        CT_ASSERT(sources[0].second->compareDatabases(localSyncedTestdata.c_str(), *copy));
    }
}

void SyncTests::testDownload()
{
    const std::string testname = "testDownload";

    CT_ASSERT_EQUAL(sources.size(), 1);
    const ClientTest::Config &config(sources[0].second->config);
    StringPair peerConfig = getPeerConfig(config.m_sourceName);
    const std::string &peer = peerConfig.first;
    const std::string &peerSource = peerConfig.second;
    CT_ASSERT(!peer.empty());
    CT_ASSERT(!peerSource.empty());

    std::string localTestdata = getPeerTestdata(config.m_sourceName, testname, "local");
    CT_ASSERT_MESSAGE(localTestdata, !access(localTestdata.c_str(), R_OK));
    std::string remoteTestdata = getPeerTestdata(config.m_sourceName, testname, "remote");
    CT_ASSERT_MESSAGE(remoteTestdata, !access(remoteTestdata.c_str(), R_OK));
    std::string modifyLocal = getPeerTestdata(config.m_sourceName, testname, "modify-local");
    CT_ASSERT_MESSAGE(modifyLocal, !access(modifyLocal.c_str(), R_OK|X_OK));
    std::string remoteSyncedTestdata = getPeerTestdata(config.m_sourceName, testname, "remote-synced");
    CT_ASSERT_MESSAGE(remoteSyncedTestdata, !access(remoteSyncedTestdata.c_str(), R_OK));

    std::auto_ptr<Cmdline> cmdline;

    // Wipe remote directly, then import.
    cmdline.reset(new TestCmdline("--daemon=no",
                                  "--delete-items",
                                  peer.c_str(),
                                  peerSource.c_str(),
                                  "*",
                                  (const char *)NULL));
    CT_ASSERT(cmdline->parse());
    CT_ASSERT_MESSAGE(peer + " " + peerSource, cmdline->run());
    cmdline.reset(new TestCmdline("--daemon=no",
                                  "--import",
                                  remoteTestdata.c_str(),
                                  peer.c_str(),
                                  peerSource.c_str(),
                                  (const char *)NULL));
    CT_ASSERT(cmdline->parse());
    CT_ASSERT_MESSAGE(peer + " " + peerSource, cmdline->run());

    // Sync into local database.
    doSync(__FILE__, __LINE__,
           "download",
           SyncOptions(SYNC_REFRESH_FROM_REMOTE,
                       CheckSyncReport(-1,0,-1, 0,0,0, true, SYNC_REFRESH_FROM_REMOTE)));

    // Compare against expected result.
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(sources[0].second->createSourceA(), TestingSyncSourcePtr::SLOW));
    {
        ScopedEnvChange fullSyncCompare("CLIENT_TEST_SERVER", "none");
        CT_ASSERT(sources[0].second->compareDatabases(localTestdata.c_str(), *copy));
    }

    // Modify locally.
    std::string actualLocalData = getCurrentTest() + ".local.test.dat";
    simplifyFilename(actualLocalData);
    std::string localModified = getCurrentTest() + ".local.modified.test.dat";
    simplifyFilename(localModified);
    CT_ASSERT_NO_THROW(UpdateLocal(currentServer() + "_1", config.m_sourceName,
                                   actualLocalData, localModified,
                                   modifyLocal));

    // Sync between both sides to update the remote.
    doSync(__FILE__, __LINE__,
           "two-way",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,-1,0, true, SYNC_TWO_WAY)));

    // Check remote.
    std::string syncedRemoteData = getCurrentTest() + ".remote.test.dat";
    simplifyFilename(syncedRemoteData);
    rm_r(syncedRemoteData);
    mkdir_p(syncedRemoteData);
    cmdline.reset(new TestCmdline("--daemon=no",
                                  "--export",
                                  syncedRemoteData.c_str(),
                                  peer.c_str(),
                                  peerSource.c_str(),
                                  (const char *)NULL));
    CT_ASSERT(cmdline->parse());
    CT_ASSERT_MESSAGE("export " + peer + " " + peerSource, cmdline->run());

    // Compare against expected result. We use the compare operation
    // of the local source and apply it to data from the remote one.
    // This typically works if the data has the same format.
    {
        ScopedEnvChange fullSyncCompare("CLIENT_TEST_SERVER", "none");
        CT_ASSERT(sources[0].second->compareDatabases(remoteSyncedTestdata, syncedRemoteData));
    }
}

void SyncTests::doUpdateConflict(const std::string &testname, bool localWins)
{
    CT_ASSERT_EQUAL(sources.size(), 1);
    const ClientTest::Config &config(sources[0].second->config);
    StringPair peerConfig = getPeerConfig(config.m_sourceName);
    const std::string &peer = peerConfig.first;
    const std::string &peerSource = peerConfig.second;
    CT_ASSERT(!peer.empty());
    CT_ASSERT(!peerSource.empty());

    std::string localTestdata = getPeerTestdata(config.m_sourceName, testname, "local");
    CT_ASSERT_MESSAGE(localTestdata, !access(localTestdata.c_str(), R_OK));
    std::string localSyncedTestdata = getPeerTestdata(config.m_sourceName, testname, "local-synced");
    CT_ASSERT_MESSAGE(localSyncedTestdata, !access(localSyncedTestdata.c_str(), R_OK));
    std::string remoteSyncedTestdata = getPeerTestdata(config.m_sourceName, testname, "remote-synced");
    CT_ASSERT_MESSAGE(remoteSyncedTestdata, !access(remoteSyncedTestdata.c_str(), R_OK));
    std::string modifyLocal = getPeerTestdata(config.m_sourceName, testname, "modify-local");
    CT_ASSERT_MESSAGE(modifyLocal, !access(modifyLocal.c_str(), R_OK|X_OK));
    std::string modifyRemote = getPeerTestdata(config.m_sourceName, testname, "modify-remote");
    CT_ASSERT_MESSAGE(modifyRemote, !access(modifyRemote.c_str(), R_OK|X_OK));

    // Import locally into empty database.
    sources[0].second->deleteAll(sources[0].second->createSourceA);
    sources[0].second->doImport(localTestdata);

    // Sync to remote.
    doSync(__FILE__, __LINE__,
           "upload",
           SyncOptions(RefreshFromLocalMode(),
                       CheckSyncReport(0,0,0, -1,0,0, true, SYNC_REFRESH_FROM_LOCAL)));

    // Export from remote directly.
    std::string actualRemoteData = getCurrentTest() + ".remote.test.dat";
    simplifyFilename(actualRemoteData);
    std::auto_ptr<Cmdline> cmdline;
    rm_r(actualRemoteData);
    mkdir_p(actualRemoteData);
    cmdline.reset(new TestCmdline("--daemon=no",
                                  "--export",
                                  actualRemoteData.c_str(),
                                  peer.c_str(),
                                  peerSource.c_str(),
                                  (const char *)NULL));
    CT_ASSERT(cmdline->parse());
    CT_ASSERT_MESSAGE("export " + peer + " " + peerSource, cmdline->run());

    // Modify all items on both sides. In both cases the modification
    // is done with a shell script which must make a copy of the data.
    // The shell scripts can be used to cause one or the other side
    // to have a modified version of an item or both at the same time,
    // which will trigger merging in the engine. The shell script
    // needs to create an entry for each item which is meant to be
    // updated, using the same file name (= luid) as in the input
    // directory.
    //
    // The order and timing of updating matters for the test because
    // the engine will look at time stamps (REV resp. LAST-MODIFIED)
    // to determine which side has the more recent change.

    for (int i = 0; i < 2; i++) {
        if (localWins ? i == 0 : i == 1) {
            // The remote side can use the data downloaded earlier.
            std::string remoteModified = getCurrentTest() + ".remote.modified.test.dat";
            simplifyFilename(remoteModified);
            CT_ASSERT(!system(StringPrintf("%s %s %s",
                                           modifyRemote.c_str(),
                                           actualRemoteData.c_str(),
                                           remoteModified.c_str()).c_str()));
            CT_ASSERT(isDir(remoteModified));
            cmdline.reset(new TestCmdline("--daemon=no",
                                          "--update",
                                          remoteModified.c_str(),
                                          peer.c_str(),
                                          peerSource.c_str(),
                                          (const char *)NULL));
            CT_ASSERT(cmdline->parse());
            CT_ASSERT_MESSAGE("update " + peer + " " + peerSource, cmdline->run());

            // Check remote after update.
            std::string remoteActualModified = getCurrentTest() + ".remote.actual.test.dat";
            simplifyFilename(remoteActualModified);
            rm_r(remoteActualModified);
            mkdir_p(remoteActualModified);
            cmdline.reset(new TestCmdline("--daemon=no",
                                          "--export",
                                          remoteActualModified.c_str(),
                                          peer.c_str(),
                                          peerSource.c_str(),
                                          (const char *)NULL));
            CT_ASSERT(cmdline->parse());
            CT_ASSERT_MESSAGE("export " + peer + " " + peerSource, cmdline->run());

            // Copy all unmodified items before the comparison.
            ReadDir dir(remoteModified);
            std::set<std::string> modified(dir.begin(), dir.end());
            BOOST_FOREACH(const std::string &luid, ReadDir(actualRemoteData)) {
                if (modified.find(luid) == modified.end()) {
                    std::string content;
                    CT_ASSERT(ReadFile(actualRemoteData + "/" + luid, content));
                    std::ofstream((remoteModified + "/" + luid).c_str()).write(content.c_str(), content.size());
                }
            }

            // Compare against expected result. We use the compare operation
            // of the local source and apply it to data from the remote one.
            // This typically works if the data has the same format.
            {
                ScopedEnvChange fullSyncCompare("CLIENT_TEST_SERVER", "none");
                CT_ASSERT(sources[0].second->compareDatabases(remoteModified, remoteActualModified));
            }
        } else {
            std::string actualLocalData = getCurrentTest() + ".local.test.dat";
            simplifyFilename(actualLocalData);
            std::string localModified = getCurrentTest() + ".local.modified.test.dat";
            simplifyFilename(localModified);
            CT_ASSERT_NO_THROW(UpdateLocal(currentServer() + "_1", config.m_sourceName,
                                           actualLocalData, localModified,
                                           modifyLocal));
        }

        // System time must be synchronized with the remote side for the
        // test to pass reliably. Wait here and/or check that
        // loosing side's time is in the past (TODO).
        sleep(5);
    }

    // Sync between both sides.
    doSync(__FILE__, __LINE__,
           "two-way",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,-1,-1, 0,-1,0, true, SYNC_TWO_WAY)));

    // Check remote.
    std::string syncedRemoteData = getCurrentTest() + ".remote.test.dat";
    simplifyFilename(syncedRemoteData);
    rm_r(syncedRemoteData);
    mkdir_p(syncedRemoteData);
    cmdline.reset(new TestCmdline("--daemon=no",
                                  "--export",
                                  syncedRemoteData.c_str(),
                                  peer.c_str(),
                                  peerSource.c_str(),
                                  (const char *)NULL));
    CT_ASSERT(cmdline->parse());
    CT_ASSERT_MESSAGE("export " + peer + " " + peerSource, cmdline->run());

    // Compare against expected result. We use the compare operation
    // of the local source and apply it to data from the remote one.
    // This typically works if the data has the same format.
    {
        ScopedEnvChange fullSyncCompare("CLIENT_TEST_SERVER", "none");
        CT_ASSERT(sources[0].second->compareDatabases(remoteSyncedTestdata, syncedRemoteData));
    }

    // Check local.
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(sources[0].second->createSourceA(), TestingSyncSourcePtr::SLOW));
    {
        ScopedEnvChange fullSyncCompare("CLIENT_TEST_SERVER", "none");
        CT_ASSERT(sources[0].second->compareDatabases(localSyncedTestdata.c_str(), *copy));
    }
}

void SyncTests::testUpdateRemoteWins()
{
    // Local side gets updated first, then remote -> remote wins during merge conflict.
    doUpdateConflict("testUpdateRemoteWins", false);
}

void SyncTests::testUpdateLocalWins()
{
    // Remote side gets updated first, then local -> local wins during merge conflict.
    doUpdateConflict("testUpdateLocalWins", true);
}

void SyncTests::doSync(const SyncOptions &options)
{
    int res = 0;
    static int syncCounter = 0;
    static std::string lastTest;
    std::stringstream logstream;

    // reset counter when switching tests
    if (lastTest != getCurrentTest()) {
        syncCounter = 0;
        lastTest = getCurrentTest();
    }

    std::string prefix;
    prefix.reserve(80);
    for (std::list<std::string>::iterator it = logPrefixes.begin();
         it != logPrefixes.end();
         ++it) {
        prefix += ".";
        prefix += *it;
    }
    if (!prefix.empty()) {
        printf(" %s", prefix.c_str() + 1);
        fflush(stdout);
    }

    logstream /* << std::setw(4) << std::setfill('0') << syncCounter << "_" */ << getCurrentTest()
                 << prefix
                 << ".client." << (accessClientB ? "A" : "B");
    std::string logname = logstream.str();
    simplifyFilename(logname);
    syncCounter++;

    SE_LOG_DEBUG(NULL, "%d. starting %s with sync mode %s",
                 syncCounter, logname.c_str(), PrettyPrintSyncMode(options.m_syncMode).c_str());

    try {
        CT_ASSERT_NO_THROW(res = client.doSync(sourceArray,
                                               logname,
                                               options));
    } catch (...) {
        postSync(res, logname);
        throw;
    }
    CT_ASSERT_NO_THROW(postSync(res, logname));
}

void SyncTests::postSync(int res, const std::string &logname)
{
    client.postSync(res, logname);
}

void SyncTests::allSourcesInsert(bool withUID)
{
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        CT_ASSERT_NO_THROW(source_pair.second->doInsert(withUID));
    }
}

void SyncTests::allSourcesUpdate()
{
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        CT_ASSERT_NO_THROW(source_pair.second->update(source_pair.second->createSourceA,
                                                      source_pair.second->config.m_updateItem));
    }
}

void SyncTests::allSourcesDeleteAll()
{
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        CT_ASSERT_NO_THROW(source_pair.second->deleteAll(source_pair.second->createSourceA));
    }
}

void SyncTests::allSourcesInsertMany(int startIndex, int numItems,
                                     std::map<int, std::list<std::string> > &luids)
{
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        std::list<std::string> l;
        CT_ASSERT_NO_THROW(l = source_pair.second->insertManyItems(source_pair.second->createSourceA,
                                                                   startIndex,
                                                                   numItems,
                                                                   0));
        CT_ASSERT_EQUAL((size_t)numItems, l.size());
        // append instead of overwriting - useful when multiple
        // insertMany calls share the same luid buffer
        luids[source_pair.first].insert(luids[source_pair.first].end(), l.begin(), l.end());
    }
}

void SyncTests::allSourcesUpdateMany(int startIndex, int numItems,
                                     int revision,
                                     std::map<int, std::list<std::string> > &luids,
                                     int offset)
{
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        CT_ASSERT_NO_THROW(source_pair.second->updateManyItems(source_pair.second->createSourceA,
                                                               startIndex,
                                                               numItems,
                                                               0,
                                                               revision,
                                                               luids[source_pair.first],
                                                               offset));
    }
}

void SyncTests::allSourcesRemoveMany(int numItems,
                                     std::map<int, std::list<std::string> > &luids,
                                     int offset)
{
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        CT_ASSERT_NO_THROW(source_pair.second->removeManyItems(source_pair.second->createSourceA,
                                                               numItems,
                                                               luids[source_pair.first],
                                                               offset));
    }
}


/** generates tests on demand based on what the client supports */
class ClientTestFactory : public CppUnit::TestFactory {
public:
    ClientTestFactory(ClientTest &c) :
        client(c) {}

    virtual CppUnit::Test *makeTest() {
        int source;
        CppUnit::TestSuite *alltests = new CppUnit::TestSuite("Client");
        CppUnit::TestSuite *tests;

        // create local source tests
        typedef std::map<std::string, LocalTests *> ConfigMap;
        ConfigMap configs;
        tests = new CppUnit::TestSuite(alltests->getName() + "::Source");
        for (source=0; source < client.getNumLocalSources(); source++) {
            ClientTest::Config config;
            client.getLocalSourceConfig(source, config);
            if (!config.m_sourceName.empty()) {
                LocalTests *sourcetests =
                    client.createLocalTests(tests->getName() + "::" + config.m_sourceName, source, config);
                sourcetests->addTests();
                tests->addTest(FilterTest(sourcetests));
                configs[config.m_sourceName] = sourcetests;
            }
        }
        // link configs of sources which share the same database
        BOOST_FOREACH (const ConfigMap::value_type &entry, configs) {
            LocalTests *sourcetests = entry.second;
            const ClientTest::Config &config = sourcetests->config;
            if (!config.m_linkedSources.empty()) {
                sourcetests->m_linkedSources.push_back(sourcetests);
                BOOST_FOREACH (const std::string &source, config.m_linkedSources) {
                    sourcetests->m_linkedSources.push_back(configs[source]);
                }
            }
        }
        alltests->addTest(FilterTest(tests));

        // create sync tests with just one source
        tests = new CppUnit::TestSuite(alltests->getName() + "::Sync");
        for (source=0; source < client.getNumSyncSources(); source++) {
            ClientTest::Config config;
            client.getSyncSourceConfig(source, config);
            if (!config.m_sourceName.empty()) {
                std::vector<int> sources;
                sources.push_back(source);
                SyncTests *synctests =
                    client.createSyncTests(tests->getName() + "::" + config.m_sourceName, sources);
                synctests->addTests(source == 0);
                tests->addTest(FilterTest(synctests));
            }
        }

        // create sync tests with all sources enabled, unless we only have one:
        // that would be identical to the test above
        std::vector<int> sources;
        std::string name, name_reversed;
        for (source=0; source < client.getNumSyncSources(); source++) {
            ClientTest::Config config;
            client.getSyncSourceConfig(source, config);
            if (!config.m_sourceName.empty()) {
                sources.push_back(source);
                if (name.size() > 0) {
                    name += "_";
                    name_reversed = std::string("_") + name_reversed;
                }
                name += config.m_sourceName;
                name_reversed = config.m_sourceName + name_reversed;
            }
        }
        if (sources.size() > 1) {
            SyncTests *synctests =
                client.createSyncTests(tests->getName() + "::" + name, sources);
            synctests->addTests();
            tests->addTest(FilterTest(synctests));
            synctests = 0;

            if (getenv("CLIENT_TEST_REVERSE_SOURCES")) {
                // now also in reversed order - who knows, it might make a difference;
                // typically it just makes the whole run slower, so not enabled
                // by default
                std::reverse(sources.begin(), sources.end());
                synctests =
                    client.createSyncTests(tests->getName() + "::" + name_reversed, sources);
                synctests->addTests();
                tests->addTest(FilterTest(synctests));
                synctests = 0;
            }
        }

        alltests->addTest(FilterTest(tests));
        tests = 0;

        return alltests;
    }

private:
    ClientTest &client;
};

void ClientTest::registerTests()
{
    freeFactory();
    factory = (void *)new ClientTestFactory(*this);
    CppUnit::TestFactoryRegistry::getRegistry().registerFactory((CppUnit::TestFactory *)factory);
}

ClientTest::ClientTest(int serverSleepSec, const std::string &serverLog) :
    serverSleepSeconds(serverSleepSec),
    serverLogFileName(serverLog),
    factory(NULL)
{
}

void ClientTest::freeFactory()
{
    if(factory) {
        CppUnit::TestFactoryRegistry::getRegistry().unregisterFactory((CppUnit::TestFactory *)factory);
        delete (CppUnit::TestFactory *)factory;
        factory = NULL;
    }
}

ClientTest::~ClientTest()
{
    freeFactory();
}

void ClientTest::registerCleanup(Cleanup_t cleanup)
{
    cleanupSet.insert(cleanup);
}

void ClientTest::shutdown()
{
    BOOST_FOREACH(Cleanup_t cleanup, cleanupSet) {
        cleanup();
    }
}

LocalTests *ClientTest::createLocalTests(const std::string &name, int sourceParam, ClientTest::Config &co)
{
    return new LocalTests(name, *this, sourceParam, co);
}

SyncTests *ClientTest::createSyncTests(const std::string &name, std::vector<int> sourceIndices, bool isClientA)
{
    return new SyncTests(name, *this, sourceIndices, isClientA);
}

int ClientTest::dump(ClientTest &client, TestingSyncSource &source, const std::string &file)
{
    BackupReport report;
    boost::shared_ptr<ConfigNode> node(new VolatileConfigNode);

    rm_r(file);
    mkdir_p(file);
    CT_ASSERT(source.getOperations().m_backupData);
    source.getOperations().m_backupData(SyncSource::Operations::ConstBackupInfo(),
                                        SyncSource::Operations::BackupInfo(SyncSource::Operations::BackupInfo::BACKUP_OTHER, file, node),
                                        report);
    return 0;
}

void ClientTest::getItems(const std::string &file, list<string> &items, std::string &testcases)
{
    items.clear();

    // import the file, trying a .tem file (base file plus patch)
    // first
    std::ifstream input;
    string server = currentServer();
    testcases = file + '.' + server +".tem";
    input.open(testcases.c_str());

    if (input.fail()) {
        // try server-specific file (like eds_event.ics.local)
        testcases = file + '.' + server;
        input.open(testcases.c_str());
    }

    if (input.fail()) {
        // try base file
        testcases = file;
        input.open(testcases.c_str());
    }
    CT_ASSERT(!input.bad());
    CT_ASSERT(input.is_open());
    std::string data, line;
    while (input) {
        bool wasend = false;
        do {
            getline(input, line);
            CT_ASSERT(!input.bad());
            // empty lines directly after line which starts with END mark end of record;
            // check for END necessary becayse vCard 2.1 ENCODING=BASE64 may have empty lines in body of VCARD!
            if ((line != "\r" && line.size() > 0) || !wasend) {
                data += line;
                data += "\n";
            } else {
                if (!data.empty()) {
                    items.push_back(data);
                }
                data = "";
            }
            wasend = !line.compare(0, 4, "END:");
        } while(!input.eof());
    }
    if (data != "" && data != "\r\n" && data != "\n") {
        items.push_back(data);
    }
}

std::string ClientTest::import(ClientTest &client, TestingSyncSource &source, const ClientTestConfig &config,
                               const std::string &file, std::string &realfile,
                               std::list<std::string> *luids)
{
    list<string> items;
    getItems(file, items, realfile);
    SE_LOG_DEBUG(NULL, "importing %d test cases from file %s", (int)items.size(), realfile.c_str());
    std::string failures;
    bool doImport = !luids || luids->empty();
    std::list<std::string>::const_iterator it;
    if (!doImport) {
        it = luids->begin();
    }
    BOOST_FOREACH(string &data, items) {
        std::string luid;
        try {
            if (doImport) {
                luid = importItem(&source, config, data);
                CT_ASSERT(!luid.empty());
                if (luids) {
                    luids->push_back(luid);
                }
            } else {
                CT_ASSERT(it != luids->end());
                luid = *it;
                ++it;
                // Did import already fail? If yes, then don't try to
                // update because it will also fail.
                if (!luid.empty()) {
                    // TODO: should fail for status = 6 in eas
                    updateItem(&source, data, luid);
                }
            }
        } catch (...) {
            std::string explanation;
            Exception::handle(explanation);
            failures += "Failed to ";
            if (doImport) {
                failures += "import:\n";
            } else {
                failures += "update " + luid + ":\n";
            }
            failures += data;
            failures += "\n";
            failures += explanation;
            failures += "\n";
            if (doImport && luids) {
                luids->push_back("");
            }
        }
    }
    return failures;
}

bool ClientTest::compare(ClientTest &client, const std::string &fileA, const std::string &fileB)
{
    setenv("CLIENT_TEST_HEADER", "\n\n", 1);
    setenv("CLIENT_TEST_LEFT_NAME", fileA.c_str(), 1);
    setenv("CLIENT_TEST_RIGHT_NAME", fileB.c_str(), 1);
    setenv("CLIENT_TEST_REMOVED", "only in left file", 1);
    setenv("CLIENT_TEST_ADDED", "only in right file", 1);
    bool success = false;
    const char* compareLog = getenv("CLIENT_TEST_COMPARE_LOG");
    if(compareLog && strlen(compareLog))
    {
       std::string cmdstr = std::string("synccompare ") + fileA + " " + fileB;
       string tmpfile = "____compare.log";
       cmdstr =string("bash -c 'set -o pipefail;") + cmdstr;
       cmdstr += " 2>&1|tee " +tmpfile+"'";
       success = system(cmdstr.c_str()) == 0;
    } else {
        // Shortcut: run synccompare directly, without shell in the middle
        // (reduces overhead and output when running under valgrind).
        pid_t child = fork();
        switch (child) {
        case -1:
            perror("fork");
            break;
        case 0:
            // child
            execl("synccompare", "synccompare", fileA.c_str(), fileB.c_str(), (char *)NULL);
            perror("synccompare");
            exit(1);
            break;
        default:
            // parent
            int status;
            child = waitpid(child, &status, 0);
            if (child == -1) {
                perror("wait for synccompare");
            } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                success = true;
            }
            break;
        }
    }
    if (!success) {
        printf("failed: env CLIENT_TEST_SERVER=%s 'CLIENT_TEST_STRIP_PARAMETERS=%s' 'CLIENT_TEST_STRIP_PROPERTIES=%s'synccompare %s %s\n",
               getEnv("CLIENT_TEST_SERVER", ""),
               getEnv("CLIENT_TEST_STRIP_PARAMETERS", ""),
               getEnv("CLIENT_TEST_STRIP_PROPERTIES", ""),
               fileA.c_str(), fileB.c_str());
    }
    return success;
}

void ClientTest::update(std::string &item)
{
    const static char *props[] = {
        "\nSUMMARY",
        "\nNOTE",
        NULL
    };

    for (const char **prop = props; *prop; prop++) {
        size_t pos;
        pos = item.find(*prop);
        if (pos != item.npos) {
            // Modify existing property. Fast-forward to : (works as
            // long as colon is not in parameters).
            pos = item.find(':', pos);
        }
        if (pos != item.npos) {
            item.insert(pos + 1, "MOD-");
        } else if (!strcmp(*prop, "\nNOTE") && (pos = item.find("END:VCARD")) != item.npos) {
            // add property, but only if it is allowed in the item
            item.insert(pos, "NOTE:MOD\n");
        }
    }
}

void ClientTest::postSync(int res, const std::string &logname)
{
#ifdef WIN32
    Sleep(serverSleepSeconds * 1000);
#else
    sleep(serverSleepSeconds);

    // make a copy of the server's log (if found), then truncate it
    if (serverLogFileName.size()) {
        int fd = open(serverLogFileName.c_str(), O_RDWR);

        if (fd >= 0) {
            int out = open((logname + ".server.log").c_str(), O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
            if (out >= 0) {
                char buffer[4096];
                bool cont = true;
                ssize_t len;
                while (cont && (len = read(fd, buffer, sizeof(buffer))) > 0) {
                    ssize_t total = 0;
                    while (cont && total < len) {
                        ssize_t written = write(out, buffer, len);
                        if (written < 0) {
                            perror(("writing " + logname + ".server.log").c_str());
                            cont = false;
                        } else {
                            total += written;
                        }
                    }
                }
                if (len < 0) {
                    perror(("reading " + serverLogFileName).c_str());
                }
                close(out);
            }
            if (ftruncate(fd, 0)) {
                perror("truncating log file");
            }
            close(fd);
        } else {
            perror(serverLogFileName.c_str());
        }
    }
#endif
}

static string mangleGeneric(const std::string &data, bool update, const std::string &uniqueUIDSuffix)
{
    std::string item = data;
    if (update) {
        boost::replace_first(item, "NOTE:", "NOTE:U ");
    }
    return item;
}

static string mangleICalendar20(const std::string &data, bool update, const std::string &uniqueUIDSuffix)
{
    std::string item = data;
    std::string type;
    static const pcrecpp::RE re("BEGIN:(VEVENT|VJOURNAL|VTODO)\n");
    re.PartialMatch(data, &type);

    if (update) {
        if (type == "VJOURNAL") {
            // Need to modify first line of description and summary
            // consistently for a note because in plain text
            // representation, these lines are expected to be
            // identical.
            boost::replace_first(item, "SUMMARY:", "SUMMARY:U ");
        }
        boost::replace_first(item, "DESCRIPTION:", "DESCRIPTION:U ");
    }

    if (getenv("CLIENT_TEST_NO_UID")) {
        stripProperty(item, "UID");
    } else if (getenv("CLIENT_TEST_SIMPLE_UID")) {
        boost::replace_all(item, "UID:1234567890!@#$%^&*()<>@dummy", "UID:1234567890@dummy");
    }

    const char *uniqueUID = getenv("CLIENT_TEST_UNIQUE_UID");
    if (uniqueUID) {
        // Making UID unique per test to avoid issues
        // when the source already holds older copies.
        // Might still be an issue in real life?!
        static time_t start;
        static std::string test;
        if (test != getCurrentTest()) {
            start = time(NULL);
            test = getCurrentTest();
        }
        std::string unique = StringPrintf("UID:UNIQUE-UID-%llu-", (long long unsigned)start);
        boost::replace_all(item, "UID:", unique);
        if (atoi(uniqueUID) > 1) {
            // Also avoid reusing the same UID inside the same test.
            // Required by Google CalDAV in calendar testChanges, because
            // they keep even deleted items around and check the SEQUENCE
            // number against their old data.
            boost::replace_all(item, "UNIQUE-UID", "UNIQUE-UID" + uniqueUIDSuffix);
        }
    } else if (getenv("CLIENT_TEST_LONG_UID")) {
        boost::replace_all(item, "UID:", "UID:this-is-a-ridiculously-long-uid-");
    }

    size_t offset = item.find("\nLAST-MODIFIED:");
    static const size_t len = strlen("\nLAST-MODIFIED:20100131T235959Z");
    if (offset != item.npos) {
        // Special semantic for iCalendar 2.0: LAST-MODIFIED should be
        // incremented in updated items. Emulate that by inserting the
        // current time.
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        std::string mod = StringPrintf("\nLAST-MODIFIED:%04d%02d%02dT%02d%02d%02dZ",
                                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                       tm.tm_hour, tm.tm_min, tm.tm_sec);
        item.replace(offset, len, mod);
    }

    const static string sequence("\nSEQUENCE:XXX");
    offset = item.find(sequence);
    if (offset != item.npos) {
        if (getenv("CLIENT_TEST_INCREASE_SEQUENCE")) {
            // Increment sequence number in steps of 100 to ensure that our
            // new item is considered more recent than any corresponding
            // item in the source. Some storages (Google CalDAV) check that.
            static int counter = 100;
            item.replace(offset, sequence.size(), StringPrintf("\nSEQUENCE:%d", counter));
            counter += 100;
        } else {
            item.replace(offset, sequence.size(), "\nSEQUENCE:1");
        }
    }

    return item;
}

static std::string additionalYearly(const std::string &single,
                                    const std::string &many,
                                    int start, int skip, int index, int total)
{
    int startYear = 2012 + start - 1;
    std::string event;

    if (start == 0) {
        // no missing parent, nothing to add
    } else if (start == index) {
        // inserting a single detached recurrence
        event = StringPrintf(single.c_str(), startYear);
    } else {
        // many detached recurrences
        int endYear = startYear + index - start;
        std::string exdates;
        for (int year = startYear; year <= endYear; year++) {
            // a gap?
            if ((year - startYear) % (skip + 1)) {
                exdates +=
                    StringPrintf("EXDATE;TZID=Standard Timezone:%04d0101T120000\n",
                                 year);
            }
        }
        event = StringPrintf(many.c_str(), startYear, endYear, exdates.c_str());
    }


    SE_LOG_DEBUG(NULL, "additional yearly: start %d, skip %d, index %d/%d:\n%s",
                 start, skip, index, total,
                 event.c_str());
    return event;
}

static std::string additionalMonthly(const std::string &single,
                                     const std::string &many,
                                     int day,
                                     int start, int skip, int index, int total)
{
    int startMonth = 1 + start - 1;
    std::string event;
    int endMonth = startMonth + index - start;
    int time = (endMonth >= 4 && endMonth <= 10) ? 10 : 11;

    if (start == 0) {
    } else if (start == index) {
        event = StringPrintf(single.c_str(), startMonth, day, time);
    } else {
        // Monthly recurrence uses INTERVAL instead of
        // EXDATEs, in contrast to yearly recurrence
        // (where Exchange somehow didn't grok the
        // INTERVAL). So EXDATEs are only necessary
        // for the first, second, last case.
        if (skip == -1 ) {
            std::string exdates;
            for (int month = startMonth; month <= endMonth; month++) {
                int step = month - startMonth;
                // a gap?
                if (step > 1 && step < total - start - 1) {
                    exdates +=
                        StringPrintf("EXDATE;TZID=Standard Timezone:2012%02d01T120000\n",
                                     month);
                }
            }
            event = StringPrintf(many.c_str(), startMonth, day, endMonth, time, 1, exdates.c_str());
        } else {
            event = StringPrintf(many.c_str(), startMonth, day, endMonth, time, skip + 1, "");
        }
    }

    SE_LOG_DEBUG(NULL, "additional monthly: start %d, skip %d, index %d/%d:\n%s",
                 start, skip, index, total,
                 event.c_str());
    return event;
}

// instead of trying to determine the dates of all Sundays in 2012
// algorithmically, hard-code them...
static const struct {
    int m_month, m_day;
} sundays[] = {
    { 1, 1 },
    { 1, 8 },
    { 1, 15 },
    { 1, 22 },
    { 1, 29 },
    { 2, 5 },
    { 2, 12 },
    { 2, 19 },
    { 2, 26 },
    { 3, 4 },
    { 3, 11 },
    { 3, 18 },
    // winter time ends on March 25th, week 12 (counting from zero)
#define SUNDAYS_2012_WINTER_TIME_ENDS 12
    { 3, 25 },
    { 4, 1 },
    { 4, 8 },
    { 4, 15 },
    { 4, 22 },
    { 4, 29 },
    { 5, 6 },
    { 5, 13 },
    { 5, 20 },
    { 5, 27 },
    { 6, 3 },
    { 6, 10 },
    { 6, 17 },
    { 6, 24 },
    { 7, 1 },
    { 7, 8 },
    { 7, 15 },
    { 7, 22 },
    { 7, 29 },
    { 8, 5 },
    { 8, 12 },
    { 8, 19 },
    { 8, 26 },
    { 9, 2 },
    { 9, 9 },
    { 9, 16 },
    { 9, 23 },
    { 9, 30 },
    { 10, 7 },
    { 10, 14 },
    { 10, 21 },
    // winter time start on October 28th, week 43 (counting from zero)
#define SUNDAYS_2012_WINTER_TIME_STARTS 43
    { 10, 28 },
    { 11, 4 },
    { 11, 11 },
    { 11, 18 },
    { 11, 25 },
    { 12, 2 },
    { 12, 9 },
    { 12, 16 },
    { 12, 23 },
    { 12, 30 },
    { 0, 0 }
};

static std::string additionalWeekly(const std::string &single,
                                    const std::string &many,
                                    int start, int skip, int index, int total)
{
    int startWeek = start - 1; // numbered from zero in "sundays" array
    if (startWeek < 0) {
        startWeek = 0;
    }
    std::string event;
    int endWeek = startWeek + index - start;
    int time = (endWeek >= SUNDAYS_2012_WINTER_TIME_ENDS &&
                endWeek < SUNDAYS_2012_WINTER_TIME_STARTS) ? 12 : 13;
    int startMonth = sundays[startWeek].m_month;
    int startDay = sundays[startWeek].m_day;

    if (start == 0) {
    } else if (start == index) {
        event = StringPrintf(single.c_str(), startMonth, startDay, time);
    } else {
        int endMonth = sundays[endWeek].m_month;
        int endDay = sundays[endWeek].m_day;

        // Weekly recurrence uses INTERVAL instead of
        // EXDATEs, in contrast to yearly recurrence
        // (where Exchange somehow didn't grok the
        // INTERVAL). So EXDATEs are only necessary
        // for the first, second, last case.
        std::string exdates;
        if (skip == -1 ) {
            for (int week = startWeek; week <= endWeek; week++) {
                int step = week - startWeek;
                // a gap?
                if (step > 1 && step < total - start - 1) {
                    exdates +=
                        StringPrintf("EXDATE;TZID=Standard Timezone:2012%02d%02dT140000\n",
                                                             sundays[week].m_month,
                                     sundays[week].m_day);
                }
            }
            event = StringPrintf(many.c_str(),
                                 startMonth, startDay,
                                 endMonth, endDay,
                                 time, 1, exdates.c_str());
        } else {
            event = StringPrintf(many.c_str(),
                                 startMonth, startDay,
                                 endMonth, endDay,
                                 time, skip + 1, "");
        }
    }

    SE_LOG_DEBUG(NULL, "additional weekly: start %d, skip %d, index %d/%d:\n%s",
                 start, skip, index, total,
                 event.c_str());
    return event;
}

static void addMonthly(size_t &index, ClientTestConfig::MultipleLinkedItems_t &subset,
                       const std::string &pre, const std::string &post,
                       const char *suffix, int day, int months)
{
    index++;
    subset.resize(index + 1);
    ClientTestConfig::LinkedItems_t *items = &subset[index];
    items->m_name = std::string("Monthly") + suffix;

    /* month varies */
    std::string parent =
        pre +
        "BEGIN:VEVENT\n"
        "UID:monthly\n"
        "DTSTAMP:20110101T120000Z\n"
        "DTSTART;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T120000\n"
        "DTEND;TZID=Standard Timezone:2012"  "%1$02d" "%2$02d" "T121000\n"
        "SUMMARY:monthly " + suffix + " Berlin\n"
        "RRULE:BYMONTHDAY=1;COUNT=12;FREQ=MONTHLY\n"
        "TRANSP:TRANSPARENT\n"
        "END:VEVENT\n" +
        post;
    std::string child =
        pre +
        "BEGIN:VEVENT\n"
        "UID:monthly\n"
        "DTSTAMP:20110101T120000Z\n"
        "DTSTART;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T120000\n"
        "DTEND;TZID=Standard Timezone:2012"  "%1$02d" "%2$02d" "T121000\n"
        "SUMMARY:%1$04d monthly " + suffix + " Berlin\n"
        "RECURRENCE-ID;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T120000\n"
        "TRANSP:TRANSPARENT\n"
        "END:VEVENT\n" +
        post;
    items->push_back(StringPrintf(parent.c_str(), 1, day));
    for (int month = 1; month <= months; month++) {
        items->push_back(StringPrintf(child.c_str(), month, day));
    }

    if (currentServer() == "exchange") {
        /* month of event varies and UTC time of UNTIL clause (11 during winter time, 10 during summer) */
        std::string single =
            pre +
            "BEGIN:VEVENT\n"
            "SUMMARY:[[activesyncd pseudo event - ignore me]]\n"
            "DTSTART;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T120000\n"
            "DTEND;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T120000\n"
            "RRULE:FREQ=YEARLY;UNTIL=2012" "%1$02d" "%2$02d" "T" "%3$02d" "0000Z;BYMONTHDAY=1;BYMONTH=%1$d\n"
            "UID:monthly\n"
            "TRANSP:TRANSPARENT\n"
            "END:VEVENT\n" +
            post;
        /* first month, last month, UTC time, INTERVAL and sometimes EXDATE varies */
        std::string many =
            pre +
            "BEGIN:VEVENT\n"
            "SUMMARY:[[activesyncd pseudo event - ignore me]]\n"
            "DTSTART;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T120000\n"
            "DTEND;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T120000\n"
            "RRULE:BYMONTHDAY=1;FREQ=MONTHLY;INTERVAL=%5$d;UNTIL=2012" "%3$02d" "%2$02d" "T" "%4$02d" "0000Z\n"
            "%6$s"
            "UID:monthly\n"
            "TRANSP:TRANSPARENT\n"
            "END:VEVENT\n" +
            post;

        items->m_testLinkedItemsSubsetAdditional = boost::bind(additionalMonthly,
                                                               single, many, day,
                                                               _1, _2, _3, _4);
    }
}


void ClientTest::getTestData(const char *type, Config &config)
{
    std::string server = currentServer();

    config = Config();
    char *env = getenv("CLIENT_TEST_RETRY");
    config.m_retrySync = (env && !strcmp (env, "t")) ?true :false;
    env = getenv("CLIENT_TEST_RESEND");
    config.m_resendSync = (env && !strcmp (env, "t")) ?true :false;
    env = getenv("CLIENT_TEST_SUSPEND");
    config.m_suspendSync = (env && !strcmp (env, "t")) ?true :false;
    config.m_sourceKnowsItemSemantic = true;
    config.m_linkedItemsRelaxedSemantic = true;
    config.m_itemType = "";
    config.m_import = import;
    config.m_dump = dump;
    config.m_compare = compare;
    // Sync::*::testExtensions not enabled by default.
    config.m_update = 0;
    config.m_genericUpdate = update;

    // redirect requests for "eds_event" towards "eds_event_noutc"?
    bool noutc = false;
    env = getenv ("CLIENT_TEST_NOUTC");
    if (env && !strcmp (env, "t")) {
        noutc = true;
    }

    config.m_mangleItem = mangleGeneric;

    // True for most item kinds, exceptions set below.
    config.m_uniqueID = true;

    static std::set<std::string> vCardEssential =
        boost::assign::list_of("FN")("N")("UID")("VERSION"),
        iCalEssential =
        boost::assign::list_of("DTSTART")("DTEND")("DTSTAMP")("SUMMARY")("UID")("RRULE")("RECURRENCE-ID")("VERSION");
    // RRULE is not essential for a valid item, but removing it has implications
    // for other properties (EXDATE) and other items (detached recurrences) and
    // thus cannot be tested in testRemoveProperties (because it doesn't know about
    // these inter-depdendencies).

    if (!strcmp(type, "eds_contact")) {
        config.m_sourceName = "eds_contact";
        config.m_sourceNameServerTemplate = "addressbook";
        config.m_uri = "card3"; // ScheduleWorld
        config.m_type = "text/vcard";
        config.m_essentialProperties = vCardEssential;
        config.m_uniqueID = false;
        config.m_insertItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "UID:25741c35e5431f054444fdf4571219c3\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "X-EVOLUTION-FILE-AS:Doe\\, John\n"
            "X-MOZILLA-HTML:FALSE\n"
            "END:VCARD\n";
        config.m_updateItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:Joan Doe\n"
            "N:Doe;Joan;;;\n"
            "UID:25741c35e5431f054444fdf4571219c3\n"
            "X-EVOLUTION-FILE-AS:Doe\\, Joan\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 2\n"
            "BDAY:2006-01-08\n"
            "X-MOZILLA-HTML:TRUE\n"
            "END:VCARD\n";
        /* adds a second phone number: */
        config.m_complexUpdateItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:Joan Doe\n"
            "N:Doe;Joan;;;\n"
            "UID:25741c35e5431f054444fdf4571219c3\n"
            "X-EVOLUTION-FILE-AS:Doe\\, Joan\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "TEL;TYPE=HOME;TYPE=VOICE:home 2\n"
            "BDAY:2006-01-08\n"
            "X-MOZILLA-HTML:TRUE\n"
            "END:VCARD\n";
        /* add a telephone number, email and X-AIM to initial item */
        config.m_mergeItem1 =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "UID:25741c35e5431f054444fdf4571219c3\n"
            "X-EVOLUTION-FILE-AS:Doe\\, John\n"
            "X-MOZILLA-HTML:FALSE\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "EMAIL:john.doe@work.com\n"
            "X-AIM:AIM JOHN\n"
            "END:VCARD\n";
        config.m_mergeItem2 =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:developer\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "UID:25741c35e5431f054444fdf4571219c3\n"
            "TEL;TYPE=WORK;TYPE=VOICE:123456\n"
            "X-EVOLUTION-FILE-AS:Doe\\, John\n"
            "X-MOZILLA-HTML:TRUE\n"
            "BDAY:2006-01-08\n"
            "END:VCARD\n";
        // use NOTE and N to make the item unique
        config.m_templateItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "N:Doe;<<UNIQUE>>;<<REVISION>>;;\n"
            "FN:<<UNIQUE>> Doe\n"
            "UID:<<UNIQUE>>-25741c35e5431f054444fdf4571219c3\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "X-EVOLUTION-FILE-AS:Doe\\, <<UNIQUE>>\n"
            "X-MOZILLA-HTML:FALSE\n"
            "NOTE:<<REVISION>>\n"
            "END:VCARD\n";  
        config.m_uniqueProperties = "";
        config.m_sizeProperty = "NOTE";
        config.m_testcases = "testcases/eds_contact.vcf";

        if (server == "exchange" ||
            server == "googleeas") {
            // X-MOZILLA-HTML not supported by ActiveSync.
            boost::replace_first(config.m_updateItem, "X-MOZILLA-HTML:TRUE", "X-MOZILLA-HTML:FALSE");
            boost::replace_first(config.m_complexUpdateItem, "X-MOZILLA-HTML:TRUE", "X-MOZILLA-HTML:FALSE");
            boost::replace_first(config.m_mergeItem2, "X-MOZILLA-HTML:TRUE", "X-MOZILLA-HTML:FALSE");
        }
    } else if (!strcmp(type, "eds_event") && !noutc) {
        config.m_sourceName = "eds_event";
        config.m_sourceNameServerTemplate = "calendar";
        config.m_uri = "cal2"; // ScheduleWorld
        config.m_type = "text/x-vcalendar";
        config.m_essentialProperties = iCalEssential;
        if (server == "exchange") {
            // currently cannot remove EXDATE properties, see BMC #24290
            config.m_essentialProperties.insert("EXDATE");
        }
        config.m_mangleItem = mangleICalendar20;
        config.m_insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting - old\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummyVEVENT\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "LOCATION:my office\n"
            "DESCRIPTION:let's talk<<REVISION>>\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:XXX\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.m_updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:meeting on site - updated\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummyVEVENT\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "SEQUENCE:XXX\n"
            "LOCATION:big meeting room\n"
            "DESCRIPTION:nice to see you\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        /* change location and description of insertItem in testMerge(), add alarm */
        config.m_mergeItem1 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummyVEVENT\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "SEQUENCE:XXX\n"
            "LOCATION:calling from home\n"
            "DESCRIPTION:let's talk\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "BEGIN:VALARM\n"
            "DESCRIPTION:alarm\n"
            "ACTION:DISPLAY\n"
            "TRIGGER;VALUE=DURATION;RELATED=START:-PT15M\n"
            "END:VALARM\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        /* change location to something else, add category */
        config.m_mergeItem2 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummyVEVENT\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "SEQUENCE:XXX\n"
            "LOCATION:my office\n"
            "CATEGORIES:WORK\n"
            "DESCRIPTION:what the heck\\, let's even shout a bit\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";

        // Servers have very different understandings of how
	// recurrence interacts with time zones and RRULE.
	// Must use different test cases for some servers to
	// avoid having the linkedItems test cases fail
	// because of that.
	// default: time zones + UNTIL in UTC, with VALARM
        config.m_linkedItems.resize(1);
        config.m_linkedItems[0].m_name = "Default";
        config.m_linkedItems[0].resize(2);
        config.m_linkedItems[0][0] =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:Europe/Berlin\n"
            "X-LIC-LOCATION:Europe/Berlin\n"
            "BEGIN:DAYLIGHT\n"
            "TZOFFSETFROM:+0100\n"
            "TZOFFSETTO:+0200\n"
            "TZNAME:CEST\n"
            "DTSTART:19700329T020000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=3\n"
            "END:DAYLIGHT\n"
            "BEGIN:STANDARD\n"
            "TZOFFSETFROM:+0200\n"
            "TZOFFSETTO:+0100\n"
            "TZNAME:CET\n"
            "DTSTART:19701025T030000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VEVENT\n"
            "UID:20080407T193125Z-19554-727-1-50@dummyVEVENT\n"
            "DTSTAMP:20080407T193125Z\n"
            "DTSTART;TZID=Europe/Berlin:20080406T090000\n"
            "DTEND;TZID=Europe/Berlin:20080406T093000\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:XXX\n"
            "SUMMARY:Recurring\n"
            "DESCRIPTION:recurs each Monday\\, 10 times\n"
            "CLASS:PUBLIC\n"
            "RRULE:FREQ=WEEKLY;UNTIL=20080608T070000Z;INTERVAL=1;BYDAY=SU\n"
            "CREATED:20080407T193241Z\n"
            "LAST-MODIFIED:20080407T193241Z\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.m_linkedItems[0][1] =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:Europe/Berlin\n"
            "X-LIC-LOCATION:Europe/Berlin\n"
            "BEGIN:DAYLIGHT\n"
            "TZOFFSETFROM:+0100\n"
            "TZOFFSETTO:+0200\n"
            "TZNAME:CEST\n"
            "DTSTART:19700329T020000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=3\n"
            "END:DAYLIGHT\n"
            "BEGIN:STANDARD\n"
            "TZOFFSETFROM:+0200\n"
            "TZOFFSETTO:+0100\n"
            "TZNAME:CET\n"
            "DTSTART:19701025T030000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VEVENT\n"
            "UID:20080407T193125Z-19554-727-1-50@dummyVEVENT\n"
            "DTSTAMP:20080407T193125Z\n"
            "DTSTART;TZID=Europe/Berlin:20080413T090000\n"
            "DTEND;TZID=Europe/Berlin:20080413T093000\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:XXX\n"
            "SUMMARY:Recurring: Modified\n"
            "CLASS:PUBLIC\n"
            "CREATED:20080407T193241Z\n"
            "LAST-MODIFIED:20080407T193647Z\n"
            "RECURRENCE-ID;TZID=Europe/Berlin:20080413T090000\n"
            "DESCRIPTION:second instance modified\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";

        bool recurringAllDay = false;
        bool recurringNoTZ = false;
        bool subsets = false;

	if (server == "funambol") {
	    // converts UNTIL into floating time - broken?!
            config.m_linkedItems[0].m_name = "UntilFloatTime";
	    config.m_linkedItems[0][0] =
	        "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VTIMEZONE\n"
                "TZID:Europe/Berlin\n"
                "X-LIC-LOCATION:Europe/Berlin\n"
                "BEGIN:DAYLIGHT\n"
                "TZOFFSETFROM:+0100\n"
                "TZOFFSETTO:+0200\n"
                "TZNAME:CEST\n"
                "DTSTART:19700329T020000\n"
                "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=3\n"
                "END:DAYLIGHT\n"
                "BEGIN:STANDARD\n"
                "TZOFFSETFROM:+0200\n"
                "TZOFFSETTO:+0100\n"
                "TZNAME:CET\n"
                "DTSTART:19701025T030000\n"
                "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10\n"
                "END:STANDARD\n"
                "END:VTIMEZONE\n"
                "BEGIN:VEVENT\n"
                "UID:20080407T193125Z-19554-727-1-50@dummyVEVENT\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART;TZID=Europe/Berlin:20080406T090000\n"
                "DTEND;TZID=Europe/Berlin:20080406T093000\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring\n"
                "DESCRIPTION:recurs each Monday\\, 10 times\n"
                "CLASS:PUBLIC\n"
                "RRULE:FREQ=WEEKLY;UNTIL=20080608T090000;INTERVAL=1;BYDAY=SU\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193241Z\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
        } else if (server == "mobical") {
	    // UTC time
            config.m_linkedItems[0].m_name = "UTC";
	    config.m_linkedItems[0][0] =
	        "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20080407T193125Z-19554-727-1-50@dummyVEVENT\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART:20080406T070000Z\n"
                "DTEND:20080406T073000Z\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring\n"
                "DESCRIPTION:recurs each Monday\\, 10 times\n"
                "CLASS:PUBLIC\n"
                "RRULE:FREQ=WEEKLY;UNTIL=20080608T070000Z;INTERVAL=1;BYDAY=SU\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193241Z\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
            config.m_linkedItems[0][1] =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20080407T193125Z-19554-727-1-50@dummyVEVENT\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART:20080413T070000Z\n"
                "DTEND:20080413T073000Z\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring: Modified\n"
                "CLASS:PUBLIC\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193647Z\n"
                "RECURRENCE-ID:20080413T070000Z\n"
                "DESCRIPTION:second instance modified\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
        } else if (server == "memotoo") {
	    // local floating time, always, regardless what the original
            // time zone might have been (TZID, UTC, floating)
            config.m_linkedItems[0].m_name = "LocalTime";
	    config.m_linkedItems[0][0] =
	        "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20080407T193125Z-19554-727-1-50@dummyVEVENT\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART:20080406T070000\n"
                "DTEND:20080406T073000\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring\n"
                "DESCRIPTION:recurs each Monday\\, 10 times\n"
                "CLASS:PUBLIC\n"
                "RRULE:FREQ=WEEKLY;UNTIL=20080608T070000;INTERVAL=1;BYDAY=SU\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193241Z\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
            config.m_linkedItems[0][1] =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20080407T193125Z-19554-727-1-50@dummyVEVENT\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART:20080413T070000\n"
                "DTEND:20080413T073000\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring: Modified\n"
                "CLASS:PUBLIC\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193647Z\n"
                "RECURRENCE-ID:20080413T070000\n"
                "DESCRIPTION:second instance modified\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";

            // also affects normal test items
            std::string *items[] = { &config.m_insertItem,
                                     &config.m_updateItem,
                                     &config.m_mergeItem1,
                                     &config.m_mergeItem2 };
            BOOST_FOREACH(std::string *item, items) {
                static const pcrecpp::RE times("^(DTSTART|DTEND)(.*)Z$",
                                               pcrecpp::RE_Options().set_multiline(true));
                times.GlobalReplace("\\1\\2", item);
            }
        } else if (server == "exchange") {
            config.m_linkedItems[0].m_name = "StandardTZ";
            BOOST_FOREACH(std::string &item, config.m_linkedItems[0]) {
                // time zone name changes on server to "Standard Timezone",
                // with some information stripped
                boost::replace_all(item,
                                   "Europe/Berlin",
                                   "Standard Timezone");
                // some properties are not stored/supported
                boost::replace_all(item, "TZNAME:CET\n", "");
                boost::replace_all(item, "TZNAME:CEST\n", "");
                boost::replace_all(item, "X-LIC-LOCATION:Standard Timezone\n", "");
            }

            recurringAllDay = true;
            subsets = true;
        } else {
            // in particular for Google Calendar: also try with
            // VALARM, because testing showed that the server works
            // differently with and without VALARM data included
            config.m_linkedItems.resize(2);
            config.m_linkedItems[1].m_name = "WithVALARM";
            config.m_linkedItems[1].resize(2);
            const std::string valarm =
                "BEGIN:VALARM\n"
                "ACTION:DISPLAY\n"
                "DESCRIPTION:This is an event reminder\n"
                "TRIGGER;VALUE=DURATION;RELATED=START:-PT1H\n"
                "X-EVOLUTION-ALARM-UID:foo@bar\n"
                "END:VALARM\nEND:VEVENT";
            config.m_linkedItems[1][0] = config.m_linkedItems[0][0];
            boost::replace_first(config.m_linkedItems[1][0], "END:VEVENT", valarm);
            config.m_linkedItems[1][1] = config.m_linkedItems[0][1];
            boost::replace_first(config.m_linkedItems[1][1], "END:VEVENT", valarm);

            // also enable other linked item variants
            recurringAllDay = true;
            recurringNoTZ = true;
        }

        if (boost::starts_with(server, "google")) {
            // converts local time into time zone of the user,
            // which breaks the test
            recurringNoTZ = false;
        }

        // test is fairly slow, only test with some CalDAV servers
        if (boost::starts_with(server, "apple")) {
            subsets = true;
        }

        if (recurringAllDay) {
            // also test recurring all-day events with exceptions
            size_t index = config.m_linkedItems.size();
            config.m_linkedItems.resize(index + 1);
            config.m_linkedItems[index].m_name = "AllDay";
            config.m_linkedItems[index].resize(2);
            config.m_linkedItems[index][0] =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20110829T130000Z-19554-727-1-50@dummyVEVENT\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART;VALUE=DATE:20080406\n"
                "DTEND;VALUE=DATE:20080407\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring all day event\n"
                "DESCRIPTION:recurs each Monday\\, 3 times\n"
                "CLASS:PUBLIC\n"
                "RRULE:FREQ=WEEKLY;UNTIL=20080420;INTERVAL=1;BYDAY=SU\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193241Z\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";

            // workaround for http://code.google.com/p/google-caldav-issues/issues/detail?id=63
            // Google CalDAV inserts a time into the UNTIL clause, do the same in the
            // reference data.
            if (boost::starts_with(server, "google")) {
                config.m_linkedItems[index].m_name = "AllDayGoogle";
                boost::replace_first(config.m_linkedItems[index][0],
                                     "UNTIL=20080420",
                                     "UNTIL=20080420T070000Z");
            }

            config.m_linkedItems[index][1] =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20110829T130000Z-19554-727-1-50@dummyVEVENT\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART;VALUE=DATE:20080413\n"
                "DTEND;VALUE=DATE:20080414\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring: Modified second instance\n"
                "CLASS:PUBLIC\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193647Z\n"
                "RECURRENCE-ID;VALUE=DATE:20080413\n"
                "DESCRIPTION:second instance modified\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
	}

        if (recurringNoTZ) {
            // also test recurring event with no timezone
            size_t index = config.m_linkedItems.size();
            config.m_linkedItems.resize(index + 1);
            config.m_linkedItems[index].m_name = "NoTZ";
            config.m_linkedItems[index].resize(2);
            config.m_linkedItems[index][0] = config.m_linkedItems[0][0];
            config.m_linkedItems[index][1] = config.m_linkedItems[0][1];
            stripComponent(config.m_linkedItems[index][0], "VTIMEZONE");
            stripParameters(config.m_linkedItems[index][0], "TZID");
            stripComponent(config.m_linkedItems[index][1], "VTIMEZONE");
            stripParameters(config.m_linkedItems[index][1], "TZID");
        }

        if (subsets) {
            static const std::string pre =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VTIMEZONE\n"
                // Actually, this is Europe/Berlin.
                // Was renamed to fit the simplified activesyncd naming
                // and DTSTART was adapted.
                "TZID:Standard Timezone\n"
                "BEGIN:STANDARD\n"
                "DTSTART:19701025T030000\n"
                "RRULE:FREQ=YEARLY;BYDAY=-1SU;BYMONTH=10\n"
                "TZOFFSETFROM:+0200\n"
                "TZOFFSETTO:+0100\n"
                "END:STANDARD\n"
                "BEGIN:DAYLIGHT\n"
                "DTSTART:19700329T020000\n"
                "RRULE:FREQ=YEARLY;BYDAY=-1SU;BYMONTH=3\n"
                "TZOFFSETFROM:+0100\n"
                "TZOFFSETTO:+0200\n"
                "END:DAYLIGHT\n"
                "END:VTIMEZONE\n";
            static const std::string post =
                "END:VCALENDAR\n";

            size_t index = config.m_linkedItemsSubset.size();
            config.m_linkedItemsSubset.resize(index + 1);
            ClientTestConfig::LinkedItems_t *items = &config.m_linkedItemsSubset[index];
            items->m_name = "Yearly";
            /* year varies */
            std::string parent =
                pre +
                "BEGIN:VEVENT\n"
                "UID:yearly\n"
                "DTSTAMP:20110101T120000Z\n"
                "DTSTART;TZID=Standard Timezone:" "%1$04d" "0101T120000\n"
                "DTEND;TZID=Standard Timezone:"  "%1$04d" "0101T121000\n"
                "SUMMARY:yearly Berlin\n"
                "RRULE:BYMONTH=1;BYMONTHDAY=1;UNTIL=20140101T110000Z;FREQ=YEARLY\n"
                "TRANSP:TRANSPARENT\n"
                "END:VEVENT\n" +
                post;
            std::string child =
                pre +
                "BEGIN:VEVENT\n"
                "UID:yearly\n"
                "DTSTAMP:20110101T120000Z\n"
                "DTSTART;TZID=Standard Timezone:" "%1$04d" "0101T120000\n"
                "DTEND;TZID=Standard Timezone:"  "%1$04d" "0101T121000\n"
                "SUMMARY:" "%1$04d" "yearly Berlin\n"
                "RECURRENCE-ID;TZID=Standard Timezone:" "%1$04d" "0101T120000\n"
                "TRANSP:TRANSPARENT\n"
                "END:VEVENT\n" +
                post;
            boost::assign::push_back(config.m_linkedItemsSubset[index])
                (StringPrintf(parent.c_str(), 2012))
                (StringPrintf(child.c_str(), 2012))
                (StringPrintf(child.c_str(), 2013))
                (StringPrintf(child.c_str(), 2014))
                ;
            if (server == "exchange") {
                /* only year of event varies */
                std::string single =
                    pre +
                    "BEGIN:VEVENT\n"
                    "SUMMARY:[[activesyncd pseudo event - ignore me]]\n"
                    "DTSTART;TZID=Standard Timezone:" "%1$04d" "0101T120000\n"
                    "DTEND;TZID=Standard Timezone:" "%1$04d" "0101T120000\n"
                    "RRULE:FREQ=YEARLY;UNTIL=" "%1$04d" "0101T110000Z;BYMONTHDAY=1;BYMONTH=1\n"
                    "UID:yearly\n"
                    "TRANSP:TRANSPARENT\n"
                    "END:VEVENT\n" +
                    post;
                /* first year, last year and EXDATE varies */
                std::string many =
                    pre +
                    "BEGIN:VEVENT\n"
                    "SUMMARY:[[activesyncd pseudo event - ignore me]]\n"
                    "DTSTART;TZID=Standard Timezone:" "%1$04d" "0101T120000\n"
                    "DTEND;TZID=Standard Timezone:" "%1$04d" "0101T120000\n"
                    "RRULE:FREQ=YEARLY;UNTIL=" "%2$04d" "0101T110000Z;BYMONTHDAY=1;BYMONTH=1\n"
                    "%3$s"
                    "UID:yearly\n"
                    "TRANSP:TRANSPARENT\n"
                    "END:VEVENT\n" +
                    post;

                items->m_testLinkedItemsSubsetAdditional = boost::bind(additionalYearly,
                                                                       single, many,
                                                                       _1, _2, _3, _4);
            }

            addMonthly(index, config.m_linkedItemsSubset, pre, post, "First", 1, 12);
            addMonthly(index, config.m_linkedItemsSubset, pre, post, "Middle", 1, 6);

            index++;
            config.m_linkedItemsSubset.resize(index + 1);
            items = &config.m_linkedItemsSubset[index];
            items->m_name = "Weekly";
            items->push_back(pre +
                             "BEGIN:VEVENT\n"
                             "UID:weekly\n"
                             "DTSTAMP:20110101T120000Z\n"
                             "DTSTART;TZID=Standard Timezone:20120101T140000\n"
                             "DTEND;TZID=Standard Timezone:20120101T141000\n"
                             "SUMMARY:weekly Sunday Berlin\n"
                             "RRULE:FREQ=WEEKLY;COUNT=54;BYDAY=SU\n"
                             "TRANSP:TRANSPARENT\n"
                             "END:VEVENT\n" +
                             post);
            for (int i = 0; sundays[i].m_month; i++) {
                items->push_back(StringPrintf((pre +
                                               "BEGIN:VEVENT\n"
                                               "UID:weekly\n"
                                               "DTSTAMP:20110101T120000Z\n"
                                               "DTSTART;TZID=Standard Timezone:2012" "%1$02d%2$02d" "T140000\n"
                                               "DTEND;TZID=Standard Timezone:2012" "%1$02d%2$02d" "T141000\n"
                                               "SUMMARY:2012-%1$02d-%2$02d %3$d. weekly Sunday Berlin\n"
                                               "RECURRENCE-ID;TZID=Standard Timezone:2012" "%1$02d%2$02d" "T140000\n"
                                               "TRANSP:TRANSPARENT\n"
                                               "END:VEVENT\n" +
                                               post).c_str(),
                                              sundays[i].m_month,
                                              sundays[i].m_day,
                                              i + 1));
            }

            if (server == "exchange") {
                /* date varies and UTC time of UNTIL clause (11 during winter time, 10 during summer) */
                std::string single =
                    pre +
                    "BEGIN:VEVENT\n"
                    "SUMMARY:[[activesyncd pseudo event - ignore me]]\n"
                    "DTSTART;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T140000\n"
                    "DTEND;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T140000\n"
                    "RRULE:FREQ=YEARLY;UNTIL=2012" "%1$02d" "%2$02d" "T" "%3$02d" "0000Z;BYMONTHDAY=%2$d;BYMONTH=%1$d\n"
                    "UID:weekly\n"
                    "TRANSP:TRANSPARENT\n"
                    "END:VEVENT\n" +
                    post;
                /* first month, last month, UTC time, INTERVAL and sometimes EXDATE varies */
                std::string many =
                    pre +
                    "BEGIN:VEVENT\n"
                    "SUMMARY:[[activesyncd pseudo event - ignore me]]\n"
                    "DTSTART;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T140000\n"
                    "DTEND;TZID=Standard Timezone:2012" "%1$02d" "%2$02d" "T140000\n"
                    "RRULE:BYDAY=SU;FREQ=WEEKLY;INTERVAL=%6$d;UNTIL=2012" "%3$02d" "%4$02d" "T" "%5$02d" "0000Z\n"
                    "%7$s"
                    "UID:weekly\n"
                    "TRANSP:TRANSPARENT\n"
                    "END:VEVENT\n" +
                    post;

                items ->m_testLinkedItemsSubsetAdditional = boost::bind(additionalWeekly,
                                                                        single, many,
                                                                        _1, _2, _3, _4);
            }
        }

        config.m_templateItem = config.m_insertItem;
        config.m_uniqueProperties = "SUMMARY:UID:LOCATION";
        config.m_sizeProperty = "DESCRIPTION";
        config.m_testcases = "testcases/eds_event.ics";
    } else if (!strcmp(type, "eds_event_noutc") ||
               (!strcmp(type, "eds_event") && noutc)) {
        config.m_sourceName = "eds_event";
        config.m_sourceNameServerTemplate = "calendar";
        config.m_uri = "cal2"; // ScheduleWorld
        config.m_type = "text/x-vcalendar";
        config.m_essentialProperties = iCalEssential;
        config.m_mangleItem = mangleICalendar20;
        config.m_insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:Asia/Shanghai\n"
            "BEGIN:STANDARD\n"
            "DTSTART:19670101T000000\n"
            "TZOFFSETFROM:+0800\n"
            "TZOFFSETTO:+0800\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai\n"
            "X-LIC-LOCATION:Asia/Shanghai\n"
            "BEGIN:STANDARD\n"
            "TZNAME:CST\n"
            "DTSTART:19700914T230000\n"
            "TZOFFSETFROM:+0800\n"
            "TZOFFSETTO:+0800\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND;TZID=/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai:20060406T163000\n"
            "DTSTART;TZID=/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai:20060406T160000\n"
            "UID:1234567890!@#$%^&*()<>@dummyVEVENT\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "LOCATION:my office\n"
            "DESCRIPTION:let's talk<<REVISION>>\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:XXX\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.m_updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:Asia/Shanghai\n"
            "BEGIN:STANDARD\n"
            "DTSTART:19670101T000000\n"
            "TZOFFSETFROM:+0800\n"
            "TZOFFSETTO:+0800\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai\n"
            "X-LIC-LOCATION:Asia/Shanghai\n"
            "BEGIN:STANDARD\n"
            "TZNAME:CST\n"
            "DTSTART:19700914T230000\n"
            "TZOFFSETFROM:+0800\n"
            "TZOFFSETTO:+0800\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:meeting on site\n"
            "DTEND;TZID=/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai:20060406T163000\n"
            "DTSTART;TZID=/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai:20060406T160000\n"
            "UID:1234567890!@#$%^&*()<>@dummyVEVENT\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "LOCATION:big meeting room\n"
            "DESCRIPTION:nice to see you\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:XXX\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        /* change location and description of insertItem in testMerge(), add alarm */
        config.m_mergeItem1 = "";
        config.m_mergeItem2 = "";
        config.m_templateItem = config.m_insertItem;
        config.m_uniqueProperties = "SUMMARY:UID:LOCATION";
        config.m_sizeProperty = "DESCRIPTION";
        config.m_testcases = "testcases/eds_event.ics";
    } else if(!strcmp(type, "eds_task")) {
        config.m_sourceName = "eds_task";
        config.m_sourceNameServerTemplate = "todo";
        config.m_uri = "task2"; // ScheduleWorld
        config.m_type = "text/x-vcalendar";
        config.m_essentialProperties = iCalEssential;
        config.m_mangleItem = mangleICalendar20;
        config.m_insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@dummyVTODO\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me\n"
            "DESCRIPTION:to be done<<REVISION>>\n"
            "PRIORITY:0\n"
            "STATUS:NEEDS-ACTION\n"
            "CREATED:20060417T173712Z\n"
            "LAST-MODIFIED:20060417T173712Z\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        config.m_updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@dummyVTODO\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me ASAP\n"
            "DESCRIPTION:to be done\n"
            "PRIORITY:1\n"
            "STATUS:NEEDS-ACTION\n"
            "CREATED:20060417T173712Z\n"
            "LAST-MODIFIED:20060417T173712Z\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        /* change summary in insertItem in testMerge() */
        config.m_mergeItem1 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@dummyVTODO\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me please\\, please\n"
            "DESCRIPTION:to be done\n"
            "PRIORITY:0\n"
            "STATUS:NEEDS-ACTION\n"
            "CREATED:20060417T173712Z\n"
            "LAST-MODIFIED:20060417T173712Z\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        config.m_mergeItem2 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@dummyVTODO\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me\n"
            "DESCRIPTION:to be done\n"
            "PRIORITY:7\n"
            "STATUS:NEEDS-ACTION\n"
            "CREATED:20060417T173712Z\n"
            "LAST-MODIFIED:20060417T173712Z\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        config.m_templateItem = config.m_insertItem;
        config.m_uniqueProperties = "SUMMARY:UID";
        config.m_sizeProperty = "DESCRIPTION";
        config.m_testcases = "testcases/eds_task.ics";
    } else if(!strcmp(type, "eds_memo")) {
        // The "eds_memo" test uses iCalendar 2.0 VJOURNAL
        // as format because synccompare doesn't handle
        // plain text. A backend which wants to use this
        // test data must support importing/exporting
        // the test data in that format, see EvolutionMemoSource
        // for an example.
        config.m_uri = "note"; // ScheduleWorld
        config.m_sourceName = "eds_memo";
        config.m_sourceNameServerTemplate = "memo";
        config.m_type = "memo";
        config.m_itemType = "text/calendar";
        config.m_essentialProperties = iCalEssential;
        config.m_mangleItem = mangleICalendar20;

        // Although iCalendar 2.0 is used in EDS, uniqueness is not
        // really enforced when syncing. The test data does not have
        // UID set and thus would not pass testInsertTwice.
        config.m_uniqueID = false;

        config.m_insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary\n"
            "DESCRIPTION:Summary\\nBody text\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";
        config.m_updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary Modified\n"
            "DESCRIPTION:Summary Modified\\nBody text\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";
        /* change summary, as in updateItem, and the body in the other merge item */
        config.m_mergeItem1 = config.m_updateItem;
        config.m_mergeItem2 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary\n"
            "DESCRIPTION:Summary\\nBody modified\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";                
        config.m_templateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary\n"
            "DESCRIPTION:Summary\\nBody text <<REVISION>>\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";
        config.m_uniqueProperties = "SUMMARY:DESCRIPTION";
        config.m_sizeProperty = "DESCRIPTION";
        config.m_testcases = "testcases/eds_memo.ics";
    }else if (!strcmp (type, "calendar+todo")) {
        config.m_uri="";
        config.m_sourceNameServerTemplate = "calendar+todo";
    }
}

void CheckSyncReport::check(SyncMLStatus status, SyncReport &report) const
{
    if (m_report) {
        *m_report = report;
    }

    stringstream str;

    str << report;
    str << "----------|--------CLIENT---------|--------SERVER---------|\n";
    str << "          |  NEW  |  MOD  |  DEL  |  NEW  |  MOD  |  DEL  |\n";
    str << "----------|-----------------------------------------------|\n";
    str << StringPrintf("Expected  |  %3d  |  %3d  |  %3d  |  %3d  |  %3d  |  %3d  |\n",
                        clientAdded, clientUpdated, clientDeleted,
                        serverAdded, serverUpdated, serverDeleted);
    str << "Expected sync mode: " << PrettyPrintSyncMode(syncMode) << "\n";
    str << "Expected cycles: " << restarts + 1 << "\n";
    SE_LOG_INFO(NULL, "sync report:\n%s\n", str.str().c_str());

    if (mustSucceed) {
        // both STATUS_OK and STATUS_HTTP_OK map to the same
        // string, so check the formatted status first, then
        // the numerical one
        CT_ASSERT_EQUAL(string("no error (remote, status 0)"), Status2String(status));
        CT_ASSERT_EQUAL(STATUS_OK, status);
    }

    BOOST_FOREACH(SyncReport::value_type &entry, report) {
        const std::string &name = entry.first;
        const SyncSourceReport &source = entry.second;

        SE_LOG_DEBUG(NULL, "Checking sync source %s...", name.c_str());
        if (mustSucceed) {
            CLIENT_TEST_EQUAL(name, STATUS_OK, source.getStatus());
        }
        check(name, source);
    }
    SE_LOG_DEBUG(NULL, "Done with checking sync report.");
}

void CheckSyncReport::check(const std::string &name, const SyncSourceReport &source) const
{
    // this code is intentionally duplicated to produce nicer CPPUNIT asserts
    CLIENT_TEST_EQUAL(name, 0, source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                                  SyncSourceReport::ITEM_ANY,
                                                  SyncSourceReport::ITEM_REJECT));
    CLIENT_TEST_EQUAL(name, 0, source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                                  SyncSourceReport::ITEM_ANY,
                                                  SyncSourceReport::ITEM_REJECT));

    const char* checkSyncModeStr = getenv("CLIENT_TEST_NOCHECK_SYNCMODE");
    bool checkSyncMode = true;
    bool checkSyncStats = getenv ("CLIENT_TEST_NOCHECK_SYNCSTATS") ? false : true;
    if (checkSyncModeStr && 
        (!strcmp(checkSyncModeStr, "1") || !strcasecmp(checkSyncModeStr, "t"))) {
        checkSyncMode = false;
    }

    if (syncMode != SYNC_NONE && checkSyncMode) {
        CLIENT_TEST_EQUAL(name, syncMode, source.getFinalSyncMode());
    }

    CLIENT_TEST_EQUAL(name, restarts + 1, source.getRestarts() + 1);

    if (clientAdded != -1 && checkSyncStats) {
        CLIENT_TEST_EQUAL(name, clientAdded,
                          source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                             SyncSourceReport::ITEM_ADDED,
                                             SyncSourceReport::ITEM_TOTAL));
    }
    if (clientUpdated != -1 && checkSyncStats) {
        CLIENT_TEST_EQUAL(name, clientUpdated,
                          source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                             SyncSourceReport::ITEM_UPDATED,
                                             SyncSourceReport::ITEM_TOTAL));
    }
    if (clientDeleted != -1 && checkSyncStats) {
        CLIENT_TEST_EQUAL(name, clientDeleted,
                          source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                             SyncSourceReport::ITEM_REMOVED,
                                             SyncSourceReport::ITEM_TOTAL));
    }

    if (serverAdded != -1 && checkSyncStats) {
        CLIENT_TEST_EQUAL(name, serverAdded,
                          source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                             SyncSourceReport::ITEM_ADDED,
                                             SyncSourceReport::ITEM_TOTAL));
    }
    if (serverUpdated != -1 && checkSyncStats) {
        CLIENT_TEST_EQUAL(name, serverUpdated,
                          source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                             SyncSourceReport::ITEM_UPDATED,
                                             SyncSourceReport::ITEM_TOTAL));
    }
    if (serverDeleted != -1 && checkSyncStats) {
        CLIENT_TEST_EQUAL(name, serverDeleted,
                          source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                             SyncSourceReport::ITEM_REMOVED,
                                             SyncSourceReport::ITEM_TOTAL));
    }
}

/** @} */
/** @endcond */
#endif // ENABLE_INTEGRATION_TESTS

SE_END_CXX
