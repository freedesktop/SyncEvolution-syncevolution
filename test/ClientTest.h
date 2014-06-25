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

#ifndef INCL_TESTSYNCCLIENT
#define INCL_TESTSYNCCLIENT

#include <string>
#include <vector>
#include <list>

#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>

#include <SyncML.h>
#include <TransportAgent.h>
#include <SyncSource.h>
#include <syncevo/SuspendFlags.h>

#include "test.h"
#include "ClientTestAssert.h"

#ifdef ENABLE_INTEGRATION_TESTS

#include <cppunit/TestSuite.h>
#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>

#include <syncevo/Logging.h>
#include <syncevo/util.h>

#include <boost/utility.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class SyncContext;
class EvolutionSyncSource;
class TransportWrapper;
class TestingSyncSource;

/**
 * This class encapsulates logging and checking of a SyncReport.
 * When constructed with default parameters, no checking will be done.
 * Otherwise the sync report has to contain exactly the expected result.
 * When multiple sync sources are active, @b all of them have to behave
 * alike (which is how the tests are constructed).
 *
 * No item is ever supposed to fail.
 */
class CheckSyncReport {
  public:
    CheckSyncReport(int clAdded = -1, int clUpdated = -1, int clDeleted = -1,
                    int srAdded = -1, int srUpdated = -1, int srDeleted = -1,
                    bool mstSucceed = true, SyncMode mode = SYNC_NONE) :
        clientAdded(clAdded),
        clientUpdated(clUpdated),
        clientDeleted(clDeleted),
        serverAdded(srAdded),
        serverUpdated(srUpdated),
        serverDeleted(srDeleted),
        restarts(0),
        mustSucceed(mstSucceed),
        syncMode(mode),
        m_report(NULL)
        {}

    int clientAdded, clientUpdated, clientDeleted,
        serverAdded, serverUpdated, serverDeleted;
    int restarts;
    bool mustSucceed;
    SyncMode syncMode;

    // if set, then the report is copied here
    SyncReport *m_report;

    CheckSyncReport &setMode(SyncMode mode) { syncMode = mode; return *this; }
    CheckSyncReport &setReport(SyncReport *report) { m_report = report; return *this; }
    CheckSyncReport &setRestarts(int r) { restarts = r; return *this; }

    /**
     * checks that the sync completed as expected and throws
     * CPPUnit exceptions if something is wrong
     *
     * @param res     return code from SyncClient::sync()
     * @param report  the sync report stored in the SyncClient
     */
    void check(SyncMLStatus status, SyncReport &report) const;

    /**
     * checks that the source report matches with expectations
     */
    void check(const std::string &name, const SyncSourceReport &report) const;
};

/**
 * parameters for running a sync
 */
struct SyncOptions {
    /** default maximum message size */
    static const long DEFAULT_MAX_MSG_SIZE = 128 * 1024;
    /** default maximum object size */
    static const long DEFAULT_MAX_OBJ_SIZE = 1024 * 1024 * 1024;
    /** sync mode chosen by client */
    SyncMode m_syncMode;
    /**
     * has to be called after a successful or unsuccessful sync,
     * will dump the report and (optionally) check the result;
     * beware, the later may throw exceptions inside CPPUNIT macros
     */
    CheckSyncReport m_checkReport;

    /** maximum message size supported by client */
    long m_maxMsgSize;
    /** maximum object size supported by client */
    long m_maxObjSize;
    /** enabled large object support */
    bool m_loSupport;
    /** enabled WBXML (default) */
    bool m_isWBXML;
    /** overrides resend properties */
    int m_retryDuration;
    int m_retryInterval;

    boost::shared_ptr<SuspendFlags::StateBlocker> m_isSuspended;
    boost::shared_ptr<SuspendFlags::StateBlocker> m_isAborted;

    /**
     * Callback to be invoked after setting up local sources, but
     * before running the engine. May throw exception to indicate
     * error and return true to stop sync without error.
     */
    typedef boost::function<bool (SyncContext &,
                                  SyncOptions &)> Callback_t;
    Callback_t m_startCallback;

    /**
     * called while configuration is prepared for sync, see
     * SyncContext::prepare()
     */
    Callback_t m_prepareCallback;

    boost::shared_ptr<TransportAgent> m_transport;

    SyncOptions(SyncMode syncMode = SYNC_NONE,
                const CheckSyncReport &checkReport = CheckSyncReport(),
                long maxMsgSize = DEFAULT_MAX_MSG_SIZE, // 128KB = large enough that normal tests should run with a minimal number of messages
                long maxObjSize = DEFAULT_MAX_OBJ_SIZE, // 1GB = basically unlimited...
                bool loSupport = false,
                bool isWBXML = defaultWBXML(),
                Callback_t startCallback = EmptyCallback,
                boost::shared_ptr<TransportAgent> transport =
                boost::shared_ptr<TransportAgent>()) :
        m_syncMode(syncMode),
        m_checkReport(checkReport),
        m_maxMsgSize(maxMsgSize),
        m_maxObjSize(maxObjSize),
        m_loSupport(loSupport),
        m_isWBXML(isWBXML),
        m_retryDuration(300),
        m_retryInterval(60),
        m_startCallback(startCallback),
        m_transport (transport)
    {}

    SyncOptions &setSyncMode(SyncMode syncMode) { m_syncMode = syncMode; return *this; }
    SyncOptions &setCheckReport(const CheckSyncReport &checkReport) { m_checkReport = checkReport; return *this; }
    SyncOptions &setMaxMsgSize(long maxMsgSize) { m_maxMsgSize = maxMsgSize; return *this; }
    SyncOptions &setMaxObjSize(long maxObjSize) { m_maxObjSize = maxObjSize; return *this; }
    SyncOptions &setLOSupport(bool loSupport) { m_loSupport = loSupport; return *this; }
    SyncOptions &setWBXML(bool isWBXML) { m_isWBXML = isWBXML; return *this; }
    SyncOptions &setRetryDuration(int retryDuration) { m_retryDuration = retryDuration; return *this; }
    SyncOptions &setRetryInterval(int retryInterval) { m_retryInterval = retryInterval; return *this; }
    SyncOptions &setStartCallback(const Callback_t &callback) { m_startCallback = callback; return *this; }
    SyncOptions &setPrepareCallback(const Callback_t &callback) { m_prepareCallback = callback; return *this; }
    SyncOptions &setTransportAgent(const boost::shared_ptr<TransportAgent> transport)
                                  {m_transport = transport; return *this;}

    static bool EmptyCallback(SyncContext &,
                              SyncOptions &) { return false; }

    /** if CLIENT_TEST_XML=1, then XML, otherwise WBXML */
    static bool defaultWBXML();
};

class LocalTests;
class SyncTests;

/**
 * This is the interface expected by the testing framework for sync
 * clients.  It defines several methods that a derived class must
 * implement if it wants to use that framework. Note that this class
 * itself is not derived from SyncClient. This gives a user of this
 * framework the freedom to implement it in two different ways:
 * - implement a class derived from both SyncClient and ClientTest
 * - add testing of an existing subclass of SyncClient by implementing
 *   a ClientTest which uses that subclass
 *
 * The client is expected to support change tracking for multiple
 * servers. Although the framework always always tests against the
 * same server, for most tests it is necessary to access the database
 * without affecting the next synchronization with the server. This is
 * done by asking the client for two different sync sources via
 * Config::createSourceA and Config::createSourceB which have to
 * create them in a suitable way - pretty much as if the client was
 * synchronized against different server. A third, different change
 * tracking is needed for real synchronizations of the data.
 *
 * Furthermore the client is expected to support multiple data sources
 * of the same kind, f.i. two different address books. This is used to
 * test full client A <-> server <-> client B synchronizations in some
 * tests or to check server modifications done by client A with a
 * synchronization against client B. In those tests client A is mapped
 * to the first data source and client B to the second one.
 *
 * Handling configuration and creating classes is entirely done by the
 * subclass of ClientTest, the frameworks makes no assumptions
 * about how this is done. Instead it queries the ClientTest for
 * properties (like available sync sources) and then creates several
 * tests.
 */
class ClientTest : private boost::noncopyable {
  public:
    ClientTest(int serverSleepSec = 0, const std::string &serverLog= "");
    virtual ~ClientTest();

    /** a unique string - "1" or "2" in practice */
    virtual std::string getClientID() const = 0;

    /** set up before running a test */
    virtual void setup() { }

    /** cleanup function to be called when shutting down testing */
    typedef void (*Cleanup_t)(void);

    /**
     * Call this to register another shutdown cleanup functions.
     * Every unique function will be called exactly once.
     */
    static void registerCleanup(Cleanup_t cleanup);

    /**
     * Call cleanup functions.
     */
    static void shutdown();

    /**
     * This function registers tests using this instance of ClientTest for
     * later use during a test run.
     *
     * The instance must remain valid until after the tests were
     * run. To run them use a separate test runner, like the one from
     * client-test-main.cpp.
     */
    virtual void registerTests();

    typedef ClientTestConfig Config;

    /**
     * Creates an instance of LocalTests (default implementation) or a
     * class derived from it.  LocalTests provides tests which cover
     * the SyncSource interface and can be executed without a SyncML
     * server. It also contains utility functions for working with
     * SyncSources.
     *
     * A ClientTest implementation can, but doesn't have to extend
     * these tests by instantiating a derived class here.
     */
    virtual LocalTests *createLocalTests(const std::string &name, int sourceParam, ClientTest::Config &co);

    /**
     * Creates an instance of SyncTests (default) or a class derived
     * from it.  SyncTests provides tests which cover the actual
     * interaction with a SyncML server.
     *
     * A ClientTest implementation can, but doesn't have to extend
     * these tests by instantiating a derived class here.
     */
    virtual SyncTests *createSyncTests(const std::string &name, std::vector<int> sourceIndices, bool isClientA = true);

    /**
     * utility function for dumping items which are C strings with blank lines as separator
     */
    static int dump(ClientTest &client, TestingSyncSource &source, const std::string &file);

    /**
     * utility function for splitting file into items with blank lines as separator
     *
     * @retval realfile       If <file>.<server>.tem exists, then it is used instead
     *                        of the generic version. The caller gets the name of the
     *                        file that was opened here.
     */
    static void getItems(const std::string &file, std::list<std::string> &items, std::string &realfile);

    /**
     * utility function for importing items with blank lines as separator,
     * for ClientTestConfig::m_import
     */
    static std::string import(ClientTest &client, TestingSyncSource &source,
                              const ClientTestConfig &config,
                              const std::string &file, std::string &realfile,
                              std::list<std::string> *luids);

    /**
     * utility function for comparing vCard and iCal files with the external
     * synccompare.pl Perl script
     */
    static bool compare(ClientTest &client, const std::string &fileA, const std::string &fileB);

    /**
     * utility function: update a vCard or iCalendar item by inserting "MOD-" into
     * FN, N, resp. SUMMARY; used for ClientTestConfig::update
     */
    static void update(std::string &item);

    struct ClientTestConfig config;

    /**
     * A derived class can use this call to get default test
     * cases, but still has to add callbacks which create sources
     * and execute a sync session.
     *
     * Some of the test cases are compiled into the library, other
     * depend on the auxiliary files from the "test" directory.
     * Currently supported types:
     * - eds_contact = vCard 3.0 contacts, with Evolution extensions
     * - eds_event = iCalendar 2.0 events, as used by Evolution
     * - eds_task = iCalendar 2.0 tasks, as used by Evolution
     * - eds_memo = iCalendar 2.0 journals, as used by Evolution
     */
    static void getTestData(const char *type, Config &config);

    /**
     * Data sources are enumbered from 0 to n-1 for the purpose of
     * testing. This call returns n.
     */
    virtual int getNumLocalSources() = 0;
    virtual int getNumSyncSources() = 0;

    /**
     * Called to fill the given test source config with information
     * about a sync source identified by its index. It's okay to only
     * fill in the available pieces of information and set everything
     * else to zero.
     * Two kinds of source config indexs are maintained, used for localSources
     * and SyncSources, this is because virtual datasoures should be visible as
     * a whole to the synccontext while should be viewed as a list of sub
     * datasoures for Localtests.
     */
    virtual void getLocalSourceConfig(int source, Config &config) = 0;
    virtual void getSyncSourceConfig(int source, Config &config) = 0;

    /**
     * Find the correspoding test source config via config name.
     */
    virtual void getSourceConfig(const string &configName, Config &config) =0;
    /*
     * Give me a test source config name, return the index in localSyncSources.
     * */
    virtual int getLocalSourcePosition (const string &configName) =0;

    /**
     * The instance to use as second client. Returning NULL disables
     * all checks which require a second client. The returned pointer
     * must remain valid throughout the life time of the tests.
     *
     * The second client must be configured to access the same server
     * and have data sources which match the ones from the primary
     * client.
     */
    virtual ClientTest *getClientB() = 0;

    /**
     * Execute a synchronization with the selected sync sources
     * and the selected synchronization options. The log file
     * in LOG has been set up already for the synchronization run
     * and should not be changed by the client.
     *
     * @param activeSources a -1 terminated array of sync source indices
     * @param logbase      basename for logging: can be used for directory or as file (by adding .log suffix)
     * @param options      sync options to be used
     * @return return code of SyncClient::sync()
     */
    virtual SyncMLStatus doSync(
        const int *activeSources,
        const std::string &logbase,
        const SyncOptions &options) = 0;


    /**
     * This is called after successful sync() calls (res == 0) as well
     * as after unsuccessful ones (res != 1). The default implementation
     * sleeps for the number of seconds specified when constructing this
     * instance and copies the server log if one was named.
     *
     * @param res       result of sync()
     * @param logname   base name of the current sync log (without ".client.[AB].log" suffix)
     */
    virtual void postSync(int res, const std::string &logname);

  protected:
    /**
     * time to sleep in postSync()
     */
    int serverSleepSeconds;

    /**
     * server log file which is copied by postSync() and then
     * truncated (Unix only, Windows does not allow such access
     * to an open file)
     */
    std::string serverLogFileName;

  private:
    /**
     * really a CppUnit::TestFactory, but declared as void * to avoid
     * dependencies on the CPPUnit header files: created by
     * registerTests() and remains valid until the client is deleted
     */
    void *factory;
    void freeFactory();
};

/**
 * helper class to encapsulate ClientTest::Config::createsource_t
 * pointer and the corresponding parameters
 */
class CreateSource {
public:
     CreateSource(const ClientTest::Config::createsource_t &createSourceParam, ClientTest &clientParam, int sourceParam, bool isSourceAParam) :
        createSource(createSourceParam),
        client(clientParam),
        source(sourceParam),
        isSourceA(isSourceAParam) {}

    TestingSyncSource *operator() () {
        CPPUNIT_ASSERT(createSource);
        return createSource(client, client.getClientID(), source, isSourceA);
    }

    const ClientTest::Config::createsource_t createSource;
    ClientTest &client;
    const int source;
    const bool isSourceA;
};


/**
 * local test of one sync source and utility functions also used by
 * sync tests
 */
class LocalTests : public CppUnit::TestSuite, public CppUnit::TestFixture {
public:
    /** the client we are testing */
    ClientTest &client;

    /** number of the source we are testing in that client */
    const int source;

    /** configuration that corresponds to source */
    const ClientTest::Config config;

    /** shortcut for config.m_sourceName */
    const std::string &getSourceName() const { return config.m_sourceName; }

    /**
     * A list of config pointers which share the same
     * database. Normally, sources are tested in isolation, but for
     * such linked sources we also need to test interdependencies, in
     * particular regarding change tracking and item listing.
     *
     * This includes *all* configs, not just the other ones.
     */
    std::list<LocalTests *> m_linkedSources;

    /** helper funclets to create sources */
    CreateSource createSourceA, createSourceB;

    LocalTests(const std::string &name, ClientTest &cl, int sourceParam, ClientTest::Config &co) :
        CppUnit::TestSuite(name),
        client(cl),
        source(sourceParam),
        config(co),
        createSourceA(co.m_createSourceA, cl, sourceParam, true),
        createSourceB(co.m_createSourceB, cl, sourceParam, false)
        {}

    /** set up before running a test */
    virtual void setUp() { client.setup(); }

    /**
     * adds the supported tests to the instance itself;
     * this is the function that a derived class can override
     * to add additional tests
     */
    virtual void addTests();

    /**
     * opens source and inserts the given item; can be called
     * regardless whether the data source already contains items or not
     *
     * @param relaxed   if true, then disable some of the additional checks after adding the item
     * @retval inserted    actual data that was inserted, optional
     * @param uniqueUIDSuffix    gets added to UID of the inserted item if unique UIDs are necessary
     * @return the LUID of the inserted item
     */
    virtual std::string insert(CreateSource createSource, const std::string &data, bool relaxed = false, std::string *inserted = NULL, const std::string &uniqueUIDSuffix = "");

    /**
     * assumes that exactly one element is currently inserted and updates it with the given item
     *
     * @param check     if true, then reopen the source and verify that the reported items are as expected
     * @param uniqueUIDSuffix    same UID suffix as when creating the item
     */
    virtual void update(CreateSource createSource, const std::string &data, bool check = true, const std::string &uniqueUIDSuffix = "");

    /**
     * updates one item identified by its LUID with the given item
     *
     * The type of the item is cleared, as in insert() above.
     */
    virtual void update(CreateSource createSource, const std::string &data, const std::string &luid);

    /** deletes all items locally via sync source */
    virtual void deleteAll(CreateSource createSource);

    /**
     * takes two databases, exports them,
     * then compares them using synccompare
     *
     * @param refFile      existing file with source reference items, NULL uses a dump of sync source A instead
     * @param copy         a sync source which contains the copied items, begin/endSync will be called
     * @param raiseAssert  raise assertion if comparison yields differences (defaults to true)
     * @return true if the two databases are equal
     */
    virtual bool compareDatabases(const char *refFile, TestingSyncSource &copy, bool raiseAssert = true);

    /**
     * compares data using synccompare
     *
     * @param refFile      existing file with source reference items
     * @param actualFile   existing file with actual items which are expected to match
     * @param raiseAssert  raise assertion if comparison yields differences (defaults to true)
     * @return true if the two databases are equal
     */
    bool compareDatabases(const std::string &refFile, const std::string &actualFile, bool raiseAssert = true);

    /**
     * compare data in source with set of items
     */
    void compareDatabasesRef(TestingSyncSource &copy,
                             const std::list<std::string> &items);

    /**
     * compare data in source with vararg list of std::string pointers, NULL terminated
     */
    void compareDatabases(TestingSyncSource &copy, ...);

    /**
     * insert artificial items, number of them determined by TEST_EVOLUTION_NUM_ITEMS
     * unless passed explicitly
     *
     * @param createSource    a factory for the sync source that is to be used
     * @param startIndex      IDs are generated starting with this value
     * @param numItems        number of items to be inserted if non-null, otherwise TEST_EVOLUTION_NUM_ITEMS is used
     * @param size            minimum size for new items
     * @return number of items inserted
     */
    virtual std::list<std::string> insertManyItems(CreateSource createSource, int startIndex = 1, int numItems = 0, int size = -1);
    virtual std::list<std::string> insertManyItems(TestingSyncSource *source, int startIndex = 1, int numItems = 0, int size = -1);

    /**
     * Update existing items. Must match a corresponding previous call to
     * insertManyItems().
     *
     * @param revision    revision number, used to distinguish different generations of each item
     * @param luids       result from corresponding insertManyItems() call
     * @param offset      skip that many items at the start of luids before updating the following ones
     */
    void updateManyItems(CreateSource createSource, int startIndex, int numItems, int size,
                         int revision,
                         std::list<std::string> &luids,
                         int offset);

    /**
     * Delete items. Skips offset items in luids before deleting numItems.
     */
    void removeManyItems(CreateSource createSource, int numItems,
                         std::list<std::string> &luids,
                         int offset);

    /**
     * update every single item, using config.update
     */
    virtual void updateData(CreateSource createSource);

    /**
     * create an artificial item for the current database
     *
     * @param item      item number: items with different number should be
     *                  recognized as different by SyncML servers
     * @param revision  differentiates items with the same item number (= updates of an older item)
     * @param size      if > 0, then create items at least that large (in bytes)
     * @return created item
     */
    std::string createItem(int item, const std::string &revision, int size);
    std::string createItem(int item, int revision, int size) {
        char buffer[32];
        sprintf(buffer, "%d", revision);
        return createItem(item, std::string(buffer), size);
    }

    /* for more information on the different tests see their implementation */

    virtual void testOpen();
    virtual void testIterateTwice();
    virtual void testDelete404();
    virtual void testReadItem404();
    void doInsert(bool withUID = true);
    virtual void testSimpleInsert();
    virtual void testLocalDeleteAll();
    virtual void testComplexInsert();
    virtual void testInsertTwice();
    virtual void testLocalUpdate();
    void doChanges(bool restart);
    virtual void testChanges();
    virtual void testChangesMultiCycles();
    virtual void testLinkedSources();
    void doImport(const std::string &testcases);
    virtual void testImport();
    virtual void testImportDelete();
    virtual void testRemoveProperties();
    virtual void testManyChanges();
    virtual void testLinkedItemsParent();
    virtual void testLinkedItemsChild();
    virtual void testLinkedItemsParentChild();
    virtual void testLinkedItemsChildParent();
    virtual void testLinkedItemsChildChangesParent();
    virtual void testLinkedItemsRemoveParentFirst();
    virtual void testLinkedItemsRemoveNormal();
    virtual void testLinkedItemsInsertParentTwice();
    virtual void testLinkedItemsInsertChildTwice();
    virtual void testLinkedItemsParentUpdate();
    virtual void testLinkedItemsUpdateChild();
    virtual void testLinkedItemsInsertBothUpdateChild();
    virtual void testLinkedItemsInsertBothUpdateParent();
    virtual void testLinkedItemsInsertBothUpdateChildNoIDs();
    virtual void testLinkedItemsUpdateChildNoIDs();
    virtual void testLinkedItemsSingle404();
    virtual void testLinkedItemsMany404();

    virtual void testSubset();

    /** retrieve right set of items for running test */
    ClientTestConfig::LinkedItems_t getParentChildData();
};

std::list<std::string> listItemsOfType(TestingSyncSource *source, int state);

/**
 * Tests synchronization with one or more sync sources enabled.
 * When testing multiple sources at once only the first config
 * is checked to see which tests can be executed.
 */
class SyncTests : public CppUnit::TestSuite, public CppUnit::TestFixture {
public:
    /** the client we are testing */
    ClientTest &client;

    SyncTests(const std::string &name, ClientTest &cl, std::vector<int> sourceIndices, bool isClientA = true);
    ~SyncTests();

    /**
     * adds the supported tests to the instance itself
     * @param isFirstSource     the tests are getting generated for a single source,
     *                          the one which was listed first; some tests are the
     *                          same for all sources and should only be run once
     */
    virtual void addTests(bool isFirstSource = false);

    /** set up before running a test */
    virtual void setUp() { client.setup(); }

protected:
    /** list with all local test classes for manipulating the sources and their index in the client */
    typedef std::vector< std::pair<int, LocalTests *> > source_array_t;
    source_array_t sources;
    typedef source_array_t::iterator source_it;

    /**
     * Stack of log file prefixes which are to be appended to the base name,
     * which already contains the current test name. Add a new prefix by
     * instantiating SyncPrefix. Its destructor takes care of popping
     * the prefix.
     */
    std::list<std::string> logPrefixes;

    class SyncPrefix {
        SyncTests &m_tests;
    public:
        SyncPrefix(const std::string &prefix, SyncTests &tests) :
        m_tests(tests) {
            tests.logPrefixes.push_back(prefix);
        }
        ~SyncPrefix() {
            m_tests.logPrefixes.pop_back();
        }
    };
    friend class SyncPrefix;

    /** the indices from sources, terminated by -1 (for sync()) */
    int *sourceArray;

    /** utility functions for second client */
    SyncTests *accessClientB;

    enum DeleteAllMode {
        DELETE_ALL_SYNC,   /**< make sure client and server are in sync,
                              delete locally,
                              sync again */
        DELETE_ALL_REFRESH /**< delete locally, refresh server */
    };

    /**
     * Compare databases second client with either reference file(s)
     * or first client.  The reference file(s) must follow the naming
     * scheme <reFileBase><source name>.dat
     */
    virtual bool compareDatabases(const char *refFileBase = NULL,
                                  bool raiseAssert = true);

    /** deletes all items locally and on server */
    virtual void deleteAll(DeleteAllMode mode = DELETE_ALL_SYNC);

    /** get both clients in sync with empty server, then copy one item from client A to B */
    virtual void doCopy();

    /**
     * replicate server database locally: same as SYNC_REFRESH_FROM_SERVER,
     * but done with explicit local delete and then a SYNC_SLOW because some
     * servers do no support SYNC_REFRESH_FROM_SERVER
     */
    virtual void refreshClient(SyncOptions options = SyncOptions());

    /* for more information on the different tests see their implementation */

    virtual void testTwoWaySync();
    virtual void testSlowSync();
    virtual void testRefreshFromServerSync();
    virtual void testRefreshFromClientSync();
    virtual void testRefreshFromRemoteSync();
    virtual void testRefreshFromLocalSync();

    virtual void testDeleteAllSync();

    virtual void testDeleteAllRefresh();
    virtual void testRefreshFromClientSemantic();
    virtual void testRefreshFromServerSemantic();
    virtual void testRefreshStatus();

    void doRestartSync(SyncMode mode);
    void testTwoWayRestart();
    void testSlowRestart();
    void testRefreshFromLocalRestart();
    void testOneWayFromLocalRestart();
    void testRefreshFromRemoteRestart();
    void testOneWayFromRemoteRestart();
    void testManyRestarts();

    void testCopy();

    virtual void testUpdate();
    virtual void testComplexUpdate();
    virtual void testDelete();
    virtual void testMerge();
    virtual void testTwinning();
    void doOneWayFromRemote(SyncMode oneWayFromRemote);
    void testOneWayFromServer();
    void testOneWayFromRemote();
    void doOneWayFromLocal(SyncMode oneWayFromLocal);
    void testOneWayFromClient();
    void testOneWayFromLocal();
    bool doConversionCallback(bool *success,
                              SyncContext &client,
                              SyncOptions &options);
    virtual void testConversion();
    virtual void testItems();
    virtual void testItemsXML();
    virtual void testExtensions();
    virtual void testAddUpdate();

    void testMaxMsg();
    void testLargeObject();

    virtual void testManyItems();
    virtual void testManyDeletes();
    virtual void testSlowSyncSemantic();
    virtual void testComplexRefreshFromServerSemantic();
    virtual void testDeleteBothSides();
    virtual void testAddBothSides();
    virtual void testAddBothSidesRefresh();
    virtual void testLinkedItemsParentChild();
    virtual void testLinkedItemsChild();
    virtual void testLinkedItemsChildParent();

    virtual void doInterruptResume(int changes,
                  boost::shared_ptr<TransportWrapper> wrapper); 

    /**
     * CLIENT_ = change made on client B before interrupting
     * SERVER_ = change made on client A and applied to server before interrupting
     *           while sending to B
     * _ADD = new item added
     * _REMOVE = existing item deleted
     * _UPDATE = existing item replaced
     * BIG = when adding or updating, make the new item so large that it does
     *       not fit into a single message
     */
    enum {
        CLIENT_ADD = (1<<0),
        CLIENT_REMOVE = (1<<1),
        CLIENT_UPDATE = (1<<2),
        SERVER_ADD = (1<<3),
        SERVER_REMOVE = (1<<4),
        SERVER_UPDATE = (1<<5),
        BIG = (1<<6)
    };
    virtual void testInterruptResumeClientAdd();
    virtual void testInterruptResumeClientRemove();
    virtual void testInterruptResumeClientUpdate();
    virtual void testInterruptResumeServerAdd();
    virtual void testInterruptResumeServerRemove();
    virtual void testInterruptResumeServerUpdate();
    virtual void testInterruptResumeClientAddBig();
    virtual void testInterruptResumeClientUpdateBig();
    virtual void testInterruptResumeServerAddBig();
    virtual void testInterruptResumeServerUpdateBig();
    virtual void testInterruptResumeFull();

    virtual void testUserSuspendClientAdd();
    virtual void testUserSuspendClientRemove();
    virtual void testUserSuspendClientUpdate();
    virtual void testUserSuspendServerAdd();
    virtual void testUserSuspendServerRemove();
    virtual void testUserSuspendServerUpdate();
    virtual void testUserSuspendClientAddBig();
    virtual void testUserSuspendClientUpdateBig();
    virtual void testUserSuspendServerAddBig();
    virtual void testUserSuspendServerUpdateBig();
    virtual void testUserSuspendFull();

    virtual void testResendClientAdd();
    virtual void testResendClientRemove();
    virtual void testResendClientUpdate();
    virtual void testResendServerAdd();
    virtual void testResendServerRemove();
    virtual void testResendServerUpdate();
    virtual void testResendFull();

    virtual void testResendProxyClientAdd();
    virtual void testResendProxyClientRemove();
    virtual void testResendProxyClientUpdate();
    virtual void testResendProxyServerAdd();
    virtual void testResendProxyServerRemove();
    virtual void testResendProxyServerUpdate();
    virtual void testResendProxyFull();

    virtual void testTimeout();

    virtual void testUpload();
    virtual void testDownload();

    void doUpdateConflict(const std::string &testname, bool localWins);
    virtual void testUpdateLocalWins();
    virtual void testUpdateRemoteWins();

    /**
     * implements testMaxMsg(), testLargeObject(), testLargeObjectEncoded()
     * using a sequence of items with varying sizes
     */
    virtual void doVarSizes(bool withMaxMsgSize,
                            bool withLargeObject);

    /**
     * executes a sync with the given options,
     * checks the result and (optionally) the sync report
     */
    virtual void doSync(const SyncOptions &options);
    virtual void doSync(const char *logPrefix,
                        const SyncOptions &options) {
        SyncPrefix prefix(logPrefix, *this);
        doSync(options);
    }
    void doSync(const char *file, int line,
                const char *logPrefix,
                const SyncOptions &options) {
        CT_WRAP_ASSERT(file, line, doSync(logPrefix, options), CT_ASSERT_EXE_CLANG_BUILD(doSync(logPrefix, options)));
    }
    void doSync(const char *file, int line,
                const SyncOptions &options) {
        CT_WRAP_ASSERT(file, line, doSync(options), CT_ASSERT_EXE_CLANG_BUILD(doSync(options)));
    }
    virtual void postSync(int res, const std::string &logname);

 private:
    void allSourcesInsert(bool withUID = true);
    void allSourcesUpdate();
    void allSourcesDeleteAll();
    void allSourcesInsertMany(int startIndex, int numItems,
                              std::map<int, std::list<std::string> > &luids);
    void allSourcesUpdateMany(int startIndex, int numItems,
                              int revision,
                              std::map<int, std::list<std::string> > &luids,
                              int offset);
    void allSourcesRemoveMany(int numItems,
                              std::map<int, std::list<std::string> > &luids,
                              int offset);
};

/*
 * A transport wraper wraps a real transport impl and gives user 
 * possibility to do additional work before/after transport operation.
 * We use TransportFaultInjector to emulate a network failure;
 * We use UserSuspendInjector to emulate a user suspend after receving
 * a response.
 */
class TransportWrapper : public TransportAgent {
protected:
    int m_interruptAtMessage, m_messageCount;
    boost::shared_ptr<TransportAgent> m_wrappedAgent;
    Status m_status;
    SyncOptions *m_options;
public:
    TransportWrapper() {
        m_messageCount = 0;
        m_interruptAtMessage = -1;
        m_wrappedAgent = boost::shared_ptr<TransportAgent>();
        m_status = INACTIVE;
        m_options = NULL;
    }
    ~TransportWrapper() {
    }

    /**
     * -1 for wrappers which are meant to be used without message resending,
     * otherwise the number x for which "interrupt" <= x will lead to
     * an aborted sync (0 for TransportResendInjector, 2 for TransportResendProxy)
     */
    virtual int getResendFailureThreshold() { return -1; }

    virtual int getMessageCount() { return m_messageCount; }

    virtual void setURL(const std::string &url) { m_wrappedAgent->setURL(url); }
    virtual void setContentType(const std::string &type) { m_wrappedAgent->setContentType(type); }
    virtual void setAgent(boost::shared_ptr<TransportAgent> agent) {m_wrappedAgent = agent;}
    virtual void setSyncOptions(SyncOptions *options) {m_options = options;}
    virtual void setInterruptAtMessage (int interrupt) {m_interruptAtMessage = interrupt;}
    virtual void cancel() { m_wrappedAgent->cancel(); }
    virtual void shutdown() { m_wrappedAgent->shutdown(); }

    virtual void rewind() {
        m_messageCount = 0;
        m_interruptAtMessage = -1;
        m_status = INACTIVE;
        m_options = NULL;
        m_wrappedAgent.reset();
    }
    virtual Status wait(bool noReply = false) { return m_status; }
    virtual void setTimeout(int seconds) { m_wrappedAgent->setTimeout(seconds); }
};

/** write log message into *.log file of a test */
#define CLIENT_TEST_LOG(_format, _args...) \
    SE_LOG_DEBUG(NULL, "\n%s:%d *** " _format, \
                 getBasename(__FILE__).c_str(), __LINE__, \
                 ##_args)

/** assert equality, include string in message if unequal */
#define CLIENT_TEST_EQUAL( _prefix, \
                           _expected, \
                           _actual ) \
    CT_ASSERT_EQUAL_MESSAGE( std::string(_prefix) + ": " + #_expected + " == " + #_actual, \
                             _expected, \
                             _actual )

/** execute _x and then check the status of the _source pointer */
#define SOURCE_ASSERT_NO_FAILURE(_source, _x) \
{ \
    CT_ASSERT_NO_THROW(_x); \
    CT_ASSERT((_source)); \
}

/** check _x for true and then the status of the _source pointer */
#define SOURCE_ASSERT(_source, _x) \
{ \
    CT_ASSERT(_x); \
    CT_ASSERT((_source)); \
}

/** check that _x evaluates to a specific value and then the status of the _source pointer */
#define SOURCE_ASSERT_EQUAL(_source, _value, _x) \
{ \
    CT_ASSERT_EQUAL(_value, _x); \
    CT_ASSERT((_source)); \
}

/** same as SOURCE_ASSERT() with a specific failure message */
#define SOURCE_ASSERT_MESSAGE(_message, _source, _x)     \
{ \
    CT_ASSERT_MESSAGE((_message), (_x)); \
    CT_ASSERT((_source)); \
}

/**
 * convenience macro for adding a test name like a function,
 * to be used inside addTests() of an instance of that class
 *
 * @param _class      class which contains the function
 * @param _function   a function without parameters in that class
 */
#define ADD_TEST(_class, _function) \
    ADD_TEST_TO_SUITE(this, _class, _function)

#define ADD_TEST_TO_SUITE(_suite, _class, _function) \
    _suite->addTest(FilterTest(new CppUnit::TestCaller<_class>(_suite->getName() + "::" #_function, &_class::_function, *this)))

#define ADD_TEST_TO_SUITE_SUFFIX(_suite, _class, _function, _suffix) \
    _suite->addTest(FilterTest(new CppUnit::TestCaller<_class>(_suite->getName() + "::" #_function + _suffix, &_class::_function, *this)))

SE_END_CXX

#endif // ENABLE_INTEGRATION_TESTS
#endif // INCL_TESTSYNCCLIENT
