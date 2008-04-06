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

#include <config.h>

#include <base/test.h>
#include <test/ClientTest.h>

#include <cppunit/extensions/HelperMacros.h>
#include <exception>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#ifdef HAVE_VALGRIND_VALGRIND_H
# include <valgrind/valgrind.h>
#endif
#ifdef HAVE_EXECINFO_H
# include <execinfo.h>
#endif

#include "EvolutionSyncClient.h"
#include "EvolutionSyncSource.h"

/**
 * a wrapper class which automatically does an open() in the constructor and a close() in the destructor
 * and ensures that the sync mode is "none" = testing mode
 */
class TestEvolutionSyncSource : public SyncSource {
public:
    TestEvolutionSyncSource(const string &type, const EvolutionSyncSourceParams &params) :
        SyncSource(params.m_name.c_str(), NULL)
    {
        PersistentEvolutionSyncSourceConfig config(params.m_name, params.m_nodes);
        config.setSourceType(type);
        m_source.reset(EvolutionSyncSource::createSource(params));
        m_source->setSyncMode(SYNC_NONE);
    }

    virtual int beginSync() {
        CPPUNIT_ASSERT_NO_THROW(m_source->open());
        CPPUNIT_ASSERT(!m_source->hasFailed());
        return m_source->beginSync();
    }

    virtual int endSync() {
        int res = m_source->endSync();
        CPPUNIT_ASSERT_NO_THROW(m_source->close());
        CPPUNIT_ASSERT(!m_source->hasFailed());
        return res;
    }

    virtual SyncItem* getFirstItem() { return m_source->getFirstItem(); }
    virtual SyncItem* getNextItem() { return m_source->getNextItem(); }
    virtual SyncItem* getFirstNewItem() { return m_source->getFirstNewItem(); }
    virtual SyncItem* getNextNewItem() { return m_source->getNextNewItem(); }
    virtual SyncItem* getFirstUpdatedItem() { return m_source->getFirstUpdatedItem(); }
    virtual SyncItem* getNextUpdatedItem() { return m_source->getNextUpdatedItem(); }
    virtual SyncItem* getFirstDeletedItem() { return m_source->getFirstDeletedItem(); }
    virtual SyncItem* getNextDeletedItem() { return m_source->getNextDeletedItem(); }
    virtual SyncItem* getFirstItemKey() { return m_source->getFirstItemKey(); }
    virtual SyncItem* getNextItemKey() { return m_source->getNextItemKey(); }
    virtual void setItemStatus(const char *key, int status) { m_source->setItemStatus(key, status); }
    virtual int addItem(SyncItem& item) { return m_source->addItem(item); }
    virtual int updateItem(SyncItem& item) { return m_source->updateItem(item); }
    virtual int deleteItem(SyncItem& item) { return m_source->deleteItem(item); }
    const char *getName() { return m_source->getName(); }
    virtual ArrayElement* clone() { return NULL; }

    auto_ptr<EvolutionSyncSource> m_source;
};

class EvolutionLocalTests : public LocalTests {
public:
    EvolutionLocalTests(const std::string &name, ClientTest &cl, int sourceParam, ClientTest::Config &co) :
        LocalTests(name, cl, sourceParam, co)
        {}

    virtual void addTests() {
        LocalTests::addTests();

#ifdef ENABLE_MAEMO
        if (config.createSourceA &&
            config.createSourceB &&
            config.templateItem &&
            strstr(config.templateItem, "BEGIN:VCARD") &&
            config.uniqueProperties) {
            ADD_TEST(EvolutionLocalTests, testOssoDelete);
        }
#endif
    }

private:

    // insert am item,
    // overwrite it with an additional X-OSSO-CONTACT-STATE:DELETED as Maemoe address book does,
    // iterate again and check that our own code deleted the item
    void testOssoDelete() {
        // get into clean state with one template item added
        deleteAll(createSourceA);
        insert(createSourceA, config.templateItem);

        // add X-OSSO-CONTACT-STATE:DELETED
        string item = config.templateItem;
        const char *comma = strchr(config.uniqueProperties, ':');
        size_t offset = item.find(config.uniqueProperties, 0,
                                  comma ? comma - config.uniqueProperties : strlen(config.uniqueProperties));
        CPPUNIT_ASSERT(offset != item.npos);
        item.insert(offset, "X-OSSO-CONTACT-STATE:DELETED\n");
        update(createSourceA, item.c_str(), false);

        // opening and preparing the source should delete the item
        std::auto_ptr<SyncSource> source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
        SOURCE_ASSERT(source.get(), source->beginSync() == 0 );
        CPPUNIT_ASSERT_EQUAL(0, countItemsOfType(source.get(), TOTAL_ITEMS));
        CPPUNIT_ASSERT_EQUAL(0, countItemsOfType(source.get(), NEW_ITEMS));
        CPPUNIT_ASSERT_EQUAL(0, countItemsOfType(source.get(), UPDATED_ITEMS));
        CPPUNIT_ASSERT_EQUAL(1, countItemsOfType(source.get(), DELETED_ITEMS));
    }
};

class TestEvolution : public ClientTest {
public:
    /**
     * can be instantiated as client A with id == "1" and client B with id == "2"
     */
    TestEvolution(const string &id) :
        ClientTest(getenv("CLIENT_TEST_DELAY") ? atoi(getenv("CLIENT_TEST_DELAY")) : 0,
                   getenv("CLIENT_TEST_LOG") ? getenv("CLIENT_TEST_LOG") : ""),
        clientID(id) {
        const char *server = getenv("CLIENT_TEST_SERVER");

        if (id == "1") {
            clientB.reset(new TestEvolution("2"));
        }

        /* check server */
        if (!server) {
            server = "funambol";
            setenv("CLIENT_TEST_SERVER", "funambol", 1);
        }

        /* override Evolution database names? */
        const char *evoprefix = getenv("CLIENT_TEST_EVOLUTION_PREFIX");
        evoPrefix = evoprefix ? evoprefix :  "SyncEvolution_Test_";

        /* check sources */
        const char *sourcelist = getenv("CLIENT_TEST_SOURCES");
        if (!sourcelist) {
            sourcelist = "vcard21,vcard30,ical20,text,itodo20,sqlite,addressbook";
        }
        numSources = 0;
        for (SourceType sourceType = (SourceType)0; sourceType < TEST_MAX_SOURCE; sourceType = (SourceType)((int)sourceType + 1) ) {
            string name = getSourceName(sourceType);

#ifndef ENABLE_EBOOK
            if (sourceType == TEST_CONTACT21_SOURCE || sourceType == TEST_CONTACT30_SOURCE) {
                continue;
            }
#endif
#ifndef ENABLE_ECAL
            if (sourceType == TEST_CALENDAR_SOURCE ||
                sourceType == TEST_TASK_SOURCE ||
                sourceType == TEST_MEMO_SOURCE) {
                continue;
            }
#endif
#ifndef ENABLE_SQLITE
            if (sourceType == TEST_SQLITE_CONTACT_SOURCE) {
                continue;
            }
#endif
#ifndef ENABLE_ADDRESSBOOK
            if (sourceType == TEST_ADDRESS_BOOK_SOURCE) {
                continue;
            }
#endif
            if (strstr(sourcelist, name.c_str())) {
                enabledSources[numSources++] = sourceType;
            }
        }

        // get configuration and set obligatory fields
        LOG.setLevel(LOG_LEVEL_DEBUG);
        std::string root = std::string("evolution/") + server + "_" + id;
        EvolutionSyncConfig config(string(server) + "_" + id);
        if (!config.exists()) {
            // no configuration yet
            config.setDefaults();
            config.setDevID(id == "1" ? "sc-api-nat" : "sc-pim-ppc");
        }
        for (SourceType sourceType = (SourceType)0; sourceType < TEST_MAX_SOURCE; sourceType = (SourceType)((int)sourceType + 1) ) {
            ClientTest::Config testconfig;
            getSourceConfig(sourceType, testconfig);
            CPPUNIT_ASSERT(testconfig.type);

            boost::shared_ptr<EvolutionSyncSourceConfig> sc = config.getSyncSourceConfig(testconfig.sourceName);
            if (!sc || !sc->exists()) {
                // no configuration yet
                config.setSourceDefaults(testconfig.sourceName);
                sc = config.getSyncSourceConfig(testconfig.sourceName);
                CPPUNIT_ASSERT(sc);
                sc->setURI(testconfig.uri);
                sc->setSourceType(testconfig.type);
            }

            // always set this property: the name might have changes since last test run
            string database = getDatabaseName(sourceType);
            sc->setDatabaseID(database);
        }
        config.flush();
    }

    virtual LocalTests *createLocalTests(const std::string &name, int sourceParam, ClientTest::Config &co) {
        return new EvolutionLocalTests(name, *this, sourceParam, co);
    }

    enum SourceType {
        TEST_CONTACT21_SOURCE,
        TEST_CONTACT30_SOURCE,
        TEST_CALENDAR_SOURCE,
        TEST_TASK_SOURCE,
        TEST_MEMO_SOURCE,
        TEST_SQLITE_CONTACT_SOURCE,
        TEST_ADDRESS_BOOK_SOURCE,
        TEST_MAX_SOURCE
    };

    virtual int getNumSources() {
        return numSources;
    }

    virtual void getSourceConfig(SourceType sourceType, Config &config) {
        memset(&config, 0, sizeof(config));
        
        switch (sourceType) {
         case TEST_CONTACT21_SOURCE:
            // we cannot use the C++ client libraries' vcard21 test
            // data because Evolution only imports vCard 3.0 items,
            // but we can use the vcard30 test data and synchronize
            // it as vCard 2.1
            getTestData("vcard30", config);
            config.sourceName = "vcard21";
            config.uri = "card"; // Funambol
            config.type = "evolution-contacts:text/x-vcard";
            break;
         case TEST_CONTACT30_SOURCE:
            getTestData("vcard30", config);
            config.type = "Evolution Address Book:text/vcard";
            break;
         case TEST_CALENDAR_SOURCE:
            getTestData("ical20", config);
            config.type = "evolution-calendar";
            break;
         case TEST_TASK_SOURCE:
            getTestData("itodo20", config);
            config.type = "evolution-todo";
            break;
         case TEST_MEMO_SOURCE:
            config.sourceName = "text";
            config.uri = "note"; // ScheduleWorld
            config.type = "Evolution Memos"; // use an alias here to test that
            config.insertItem =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "METHOD:PUBLISH\n"
                "BEGIN:VJOURNAL\n"
                "SUMMARY:Summary\n"
                "DESCRIPTION:Summary\\nBody text\n"
                "END:VJOURNAL\n"
                "END:VCALENDAR\n";
            config.updateItem =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "METHOD:PUBLISH\n"
                "BEGIN:VJOURNAL\n"
                "SUMMARY:Summary Modified\n"
                "DESCRIPTION:Summary Modified\\nBody text\n"
                "END:VJOURNAL\n"
                "END:VCALENDAR\n";
            /* change summary, as in updateItem, and the body in the other merge item */
            config.mergeItem1 = config.updateItem;
            config.mergeItem2 =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "METHOD:PUBLISH\n"
                "BEGIN:VJOURNAL\n"
                "SUMMARY:Summary\n"
                "DESCRIPTION:Summary\\nBody modified\n"
                "END:VJOURNAL\n"
                "END:VCALENDAR\n";                
            config.templateItem = config.insertItem;
            config.uniqueProperties = "SUMMARY:DESCRIPTION";
            config.sizeProperty = "DESCRIPTION";
            config.import = ClientTest::import;
            config.dump = dumpMemoSource;
            config.testcases = "testcases/imemo20.ics";
            break;
         case TEST_SQLITE_CONTACT_SOURCE:
            getTestData("vcard21", config);
            config.sourceName = "sqlite";
            config.type = "sqlite-contacts";
            config.testcases = "testcases/vcard21_sqlite.vcf";
            break;
         case TEST_ADDRESS_BOOK_SOURCE:
            getTestData("vcard30", config);
            config.sourceName = "addressbook";
            config.type = "apple-contacts";
            break;
         default:
            CPPUNIT_ASSERT(sourceType < TEST_MAX_SOURCE);
            break;
        }
        config.createSourceA = createSource;
        config.createSourceB = createSource;
        config.compare = compare;
    }

    virtual void getSourceConfig(int source, Config &config) {
        getSourceConfig(enabledSources[source], config);
    }

    virtual ClientTest *getClientB() {
        return clientB.get();
    }

    virtual bool isB64Enabled() {
        return false;
    }

    virtual int sync(
        const int *sources,
        SyncMode syncMode,
        const CheckSyncReport &checkReport,
        long maxMsgSize = 0,
        long maxObjSize = 0,
        bool loSupport = false,
        const char *encoding = NULL) {
        set<string> activeSources;
        for(int i = 0; sources[i] >= 0; i++) {
            activeSources.insert(getSourceName(enabledSources[sources[i]]));
        }

        string server = getenv("CLIENT_TEST_SERVER") ? getenv("CLIENT_TEST_SERVER") : "funambol";
        server += "_";
        server += clientID;
        
        class ClientTest : public EvolutionSyncClient {
        public:
            ClientTest(const string &server,
                           const set<string> &activeSources,
                           SyncMode syncMode,
                           long maxMsgSize,
                           long maxObjSize,
                           bool loSupport,
                           const char *encoding) :
                EvolutionSyncClient(server, false, activeSources),
                m_syncMode(syncMode),
                m_maxMsgSize(maxMsgSize),
                m_maxObjSize(maxObjSize),
                m_loSupport(loSupport),
                m_encoding(encoding)
                {}

        protected:
            virtual void prepare(SyncSource **sources) {
                for (SyncSource **source = sources;
                     *source;
                     source++) {
                    ((EvolutionSyncSource *)*source)->setEncoding(m_encoding ? m_encoding : "", true);
                    (*source)->setPreferredSyncMode(m_syncMode);
                }
                setLoSupport(m_loSupport, true);
                setMaxObjSize(m_maxObjSize, true);
                setMaxMsgSize(m_maxMsgSize, true);
                EvolutionSyncClient::prepare(sources);
            }

        private:
            const SyncMode m_syncMode;
            const long m_maxMsgSize;
            const long m_maxObjSize;
            const bool m_loSupport;
            const char *m_encoding;
        } client(server, activeSources, syncMode, maxMsgSize, maxObjSize, loSupport, encoding);

        int res = client.sync();
        CPPUNIT_ASSERT(client.getSyncReport());
        checkReport.check(res, *client.getSyncReport());
        return res;
    }

    static bool compare(ClientTest &client, const char *fileA, const char *fileB) {
        std::string cmdstr = std::string("./synccompare ") + fileA + " " + fileB;
        return system(cmdstr.c_str()) == 0;
    }
    
private:
    string clientID;
    std::auto_ptr<TestEvolution> clientB;
    string addressBookConfigPath;

    /** prefix to be used for Evolution databases */
    string evoPrefix;

    /** all sources that are active in the current test run */
    SourceType enabledSources[TEST_MAX_SOURCE];
    /** number of active sources */
    int numSources;

    /** returns the name corresponding to the type, using the same strings as the C++ client testing system */
    static string getSourceName(SourceType type) {
        switch (type) {
         case TEST_CONTACT21_SOURCE:
            return "vcard21";
            break;
         case TEST_CONTACT30_SOURCE:
            return "vcard30";
            break;
         case TEST_CALENDAR_SOURCE:
            return "ical20";
            break;
         case TEST_TASK_SOURCE:
            return "itodo20";
            break;
         case TEST_MEMO_SOURCE:
            return "text";
            break;
         case TEST_SQLITE_CONTACT_SOURCE:
            return "sqlite";
            break;
        case TEST_ADDRESS_BOOK_SOURCE:
            return "addressbook";
            break;
         default:
            CPPUNIT_ASSERT(type >= 0 && type < TEST_MAX_SOURCE);
            return "";
            break;
        }
    }

    /** returns the name of the Evolution database */
    string getDatabaseName(SourceType type) {
        return evoPrefix + getSourceName(type) + "_" + clientID;
    }
    
    static SyncSource *createSource(ClientTest &client, int source, bool isSourceA) {
        string changeID = "SyncEvolution Change ID #";
        SourceType type = ((TestEvolution &)client).enabledSources[source];
        changeID += isSourceA ? "1" : "2";
        string database = ((TestEvolution &)client).getDatabaseName(type);
        SyncSource *ss = NULL;

        EvolutionSyncConfig config("client-test-changes");
        string name = ((TestEvolution &)client).getSourceName(type);
        SyncSourceNodes nodes = config.getSyncSourceNodes(name,
                                                          string("_") + ((TestEvolution &)client).clientID +
                                                          "_" + (isSourceA ? "A" : "B"));

        // always set this property: the name might have changes since last test run
        nodes.m_configNode->setProperty("evolutionsource", database.c_str());

        EvolutionSyncSourceParams params(name,
                                         nodes,
                                         changeID);

        switch (type) {
         case TEST_CONTACT21_SOURCE:
         case TEST_CONTACT30_SOURCE:
            ss = new TestEvolutionSyncSource("evolution-contacts:text/vcard", params);
            break;
         case TEST_CALENDAR_SOURCE:
            ss = new TestEvolutionSyncSource("evolution-calendar", params);
            break;
         case TEST_TASK_SOURCE:
            ss = new TestEvolutionSyncSource("evolution-tasks", params);
            break;
         case TEST_MEMO_SOURCE:
            ss = new TestEvolutionSyncSource("evolution-memos", params);
            break;
         case TEST_SQLITE_CONTACT_SOURCE:
            ss = new TestEvolutionSyncSource("SQLite Address Book", params);
            break;
         case TEST_ADDRESS_BOOK_SOURCE:
            ss = new TestEvolutionSyncSource("apple-contacts", params);
            break;
         default:
            CPPUNIT_ASSERT(type >= 0 && type < TEST_MAX_SOURCE);
        }

        return ss;
    }

    /**
     * dump memos in iCalendar 2.0 format for synccompare: ClientTest::dump() would
     * dump the plain text
     */
    static int dumpMemoSource(ClientTest &client, SyncSource &source, const char *file) {
        std::ofstream out(file);

        ((TestEvolutionSyncSource &)source).m_source->exportData(out);

        out.close();
        return out.bad();
    }

};

static void handler(int sig)
{
    void *buffer[100];
    int size;
    
    fprintf(stderr, "\ncaught signal %d\n", sig);
    fflush(stderr);
#ifdef HAVE_EXECINFO_H
    size = backtrace(buffer, sizeof(buffer)/sizeof(buffer[0]));
    backtrace_symbols_fd(buffer, size, 2);
#endif
#ifdef HAVE_VALGRIND_VALGRIND_H
    VALGRIND_PRINTF_BACKTRACE("\ncaught signal %d\n", sig);
#endif
    /* system("objdump -l -C -d client-test >&2"); */
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_DFL;
    sigaction(SIGABRT, &act, NULL);
    abort();
}

static class RegisterTestEvolution {
public:
    RegisterTestEvolution() :
        testClient("1") {
        struct sigaction act;

        memset(&act, 0, sizeof(act));
        act.sa_handler = handler;
        sigaction(SIGABRT, &act, NULL);
        sigaction(SIGSEGV, &act, NULL);
        sigaction(SIGILL, &act, NULL);

#if defined(HAVE_GLIB) && defined(HAVE_EDS)
        // this is required on Maemo and does not harm either on a normal
        // desktop system with Evolution
        g_type_init();
#endif
        testClient.registerTests();
    }

private:
    TestEvolution testClient;
    
} testEvolution;
