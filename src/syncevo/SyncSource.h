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

#ifndef INCL_SYNCSOURCE
#define INCL_SYNCSOURCE

#include <syncevo/SyncConfig.h>
#include <syncevo/SyncML.h>
#include <syncevo/Timespec.h>

#include <synthesis/sync_declarations.h>
#include <synthesis/syerror.h>
#include <synthesis/blobs.h>

#include <boost/function.hpp>
#include <boost/signals2.hpp>
#include <boost/variant.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class SyncSource;
struct SDKInterface;

/**
 * This set of parameters always has to be passed when constructing
 * SyncSource instances.
 */
struct SyncSourceParams {
    /**
     * @param    name        the name needed by SyncSource
     * @param    nodes       a set of config nodes to be used by this source
     * @param    context     Additional non-source config settings.
     *                       When running as part of a normal sync, these are the
     *                       settings for the peer. When running in a local sync,
     *                       these settings come from the "target-config" peer
     *                       config inside the config context of the source.
     *                       Testing uses "target-config@client-test". On the
     *                       command line, this is the config chosen by the
     *                       user, which may or may not have peer-specific settings!
     * @param    contextName optional name of context in which the source is defined,
     *                       needed to disambiguates "name" when sources from
     *                       different contexts are active in a sync
     */
    SyncSourceParams(const string &name,
                     const SyncSourceNodes &nodes,
                     const boost::shared_ptr<SyncConfig> &context,
                     const string &contextName = "") :
        m_name(name),
        m_nodes(nodes),
        m_context(context),
        m_contextName(contextName)
    {}

    std::string getDisplayName() const { return m_contextName.empty() ? m_name : m_contextName + "/" + m_name; }

    string m_name;
    SyncSourceNodes m_nodes;
    boost::shared_ptr<SyncConfig> m_context;
    string m_contextName;
};

/**
 * The SyncEvolution core has no knowledge of existing SyncSource
 * implementations. Implementations have to register themselves
 * by instantiating this class exactly once with information
 * about themselves.
 *
 * It is also possible to add configuration options. For that define a
 * derived class. In its constructor use
 * SyncSourceConfig::getRegistry() resp. SyncConfig::getRegistry() to
 * define new configuration properties. The advantage of registering
 * them is that the user interface will automatically handle them like
 * the predefined ones. The namespace of these configuration options
 * is shared by all sources and the core.
 *
 * For properties with arbitrary names use the
 * SyncSourceNodes::m_trackingNode.
 */
class RegisterSyncSource
{
 public:
    /**
     * Users select a SyncSource and its data format via the "type"
     * config property. Backends have to add this kind of function to
     * the SourceRegistry_t in order to be considered by the
     * SyncSource creation mechanism.
     *
     * The function will be called to check whether the backend was
     * meant by the user. It should return a new instance which will
     * be freed by the caller or NULL if it does not support the
     * selected type.
     * 
     * Inactive sources should return the special InactiveSource
     * instance (created with InactiveSource() below) if they
     * recognize without a doubt that the user wanted to instantiate
     * them: for example, an inactive EvolutionContactSource will
     * return NULL for "addressbook" but InactiveSource for
     * "evolution-contacts".
     */
    typedef SyncSource *(*Create_t)(const SyncSourceParams &params);

    /** create special result of Create_t: a source which just throws errors when used */
    static SyncSource *InactiveSource(const SyncSourceParams &params);

    /**
     * @param shortDescr     a few words identifying the data to be synchronized,
     *                       e.g. "Evolution Calendar"
     * @param enabled        true if the sync source can be instantiated,
     *                       false if it was not enabled during compilation or is
     *                       otherwise not functional
     * @param create         factory function for sync sources of this type
     * @param typeDescr      multiple lines separated by \n which get appended to
     *                       the the description of the type property, e.g.
     *                       "Evolution Memos = memo = evolution-memo\n"
     *                       "   plain text in UTF-8 (default) = text/plain\n"
     *                       "   iCalendar 2.0 = text/calendar\n"
     *                       "   The later format is not tested because none of the\n"
     *                       "   supported SyncML servers accepts it.\n"
     * @param typeValues     the config accepts multiple names for the same internal
     *                       type string; this list here is added to that list of
     *                       aliases. It should contain at least one unique string
     *                       the can be used to pick  this sync source among all
     *                       SyncEvolution sync sources (testing, listing backends, ...).
     *                       Example: Values() + (Aliases("Evolution Memos") + "evolution-memo")
     */
    RegisterSyncSource(const string &shortDescr,
                       bool enabled,
                       Create_t create,
                       const string &typeDescr,
                       const Values &typeValues);
 public:
    const string m_shortDescr;
    const bool m_enabled;
    const Create_t m_create;
    const string m_typeDescr;
    const Values m_typeValues;
};
    
typedef list<const RegisterSyncSource *> SourceRegistry;
class ClientTest;
class TestingSyncSource;

/**
 * Information about a data source. For the sake of simplicity all
 * items pointed to are owned by the ClientTest and must
 * remain valid throughout a test session. Not setting a pointer
 * is okay, but it will disable all tests that need the
 * information.
 */
struct ClientTestConfig {
    /**
     * The name is used in test names and has to be set.
     */
    std::string m_sourceName;

    /**
     * A default URI to be used when creating a client config.
     */
    std::string m_uri;

    /**
     * A corresponding source name in the default server template,
     * this is used to copy corresponding uri set in the server template
     * instead of the uri field above (which is the same for all servers).
     */
    std::string m_sourceNameServerTemplate;

    /**
     * A member function of a subclass which is called to create a
     * sync source referencing the data. This is used in tests of
     * the SyncSource API itself as well as in tests which need to
     * modify or check the data sources used during synchronization.
     *
     * The test framework will call beginSync() and then some of
     * the functions it wants to test. After a successful test it
     * will call endSync() which is then expected to store all
     * changes persistently. Creating a sync source again
     * with the same call should not report any
     * new/updated/deleted items until such changes are made via
     * another sync source.
     *
     * The instance will be deleted by the caller. Because this
     * may be in the error case or in an exception handler,
     * the sync source's desctructor should not thow exceptions.
     *
     * @param client    the same instance to which this config belongs
     * @param clientID  the unique ID of the client, "1" resp. "2" in practice (can also be obtained as
     *                  client->getClientID(), but not all implementers have (or want) access to the
     *                  class definition)
     * @param source    index of the data source (from 0 to ClientTest::getNumSources() - 1)
     * @param isSourceA true if the requested SyncSource is the first one accessing that
     *                  data, otherwise the second
     */
    typedef boost::function<TestingSyncSource *(ClientTest &, const std::string &, int, bool)> createsource_t;

    /**
     * Creates a sync source which references the primary database;
     * it may report the same changes as the sync source used during
     * sync tests.
     */
    createsource_t m_createSourceA;

    /**
     * A second sync source also referencing the primary data
     * source, but configured so that it tracks changes
     * independently from the the primary sync source.
     *
     * In local tests the usage is like this:
     * - add item via first SyncSource
     * - iterate over new items in second SyncSource
     * - check that it lists the added item
     *
     * In tests with a server the usage is:
     * - do a synchronization with the server
     * - iterate over items in second SyncSource
     * - check that the total number and number of
     *   added/updated/deleted items is as expected
     */
    createsource_t m_createSourceB;

    /**
     * The framework can generate vCard and vCalendar/iCalendar items
     * automatically by copying a template item and modifying certain
     * properties.
     *
     * This is the template for these automatically generated items.
     * It must contain the string <<REVISION>> which will be replaced
     * with the revision parameter of the createItem() method.
     */
    std::string m_templateItem;

    /**
     * This is a colon (:) separated list of properties which need
     * to be modified in templateItem.
     */
    std::string m_uniqueProperties;

    /**
     * This is a single property in templateItem which can be extended
     * to increase the size of generated items.
     */
    std::string m_sizeProperty;

    /**
     * Type to be set when importing any of the items into the
     * corresponding sync sources. Use "" if sync source doesn't
     * need this information.
     *
     * Not currently used! All items are assumed to be in the raw,
     * internal format (see SyncSourceRaw and SyncSourceSerialize).
     */
    std::string m_itemType;

    /**
     * callback which is invoked with a specific item as paramter
     * to do data type specific conversions before actually
     * using the test item; default is a NOP function
     *
     * @param update     modify item content so that it can be
     *                   used as an update of the old data
     * @param uniqueUIDSuffix   beyond not reusing UIDs between tests, also embed this suffix
     *                          in test items inside the running test to avoid reuse
     *                          if necessary (Google CalDAV + UID/SEQUENCE => 409 error)
     */
    boost::function<std::string (const std::string &data, bool update, const std::string &uniqueUIDSuffix)> m_mangleItem;

    /**
     * Items have a UID which really has to be unique among all items
     * in the database. True for iCalendar 2.0, false for vCard 3.0
     * (there is a UID, but its uniqueness is not enforced).
     */
    bool m_uniqueID;

    /**
     * A very simple item that is inserted during basic tests. Ideally
     * it only contains properties supported by all servers.
     */
    std::string m_insertItem;

    /**
     * A slightly modified version of insertItem. If the source has UIDs
     * embedded into the item data, then both must have the same UID.
     * Again all servers should better support these modified properties.
     */
    std::string m_updateItem;

    /**
     * A more heavily modified version of insertItem. Same UID if necessary,
     * but can test changes to items only supported by more advanced
     * servers.
     */
    std::string m_complexUpdateItem;

    /**
     * To test merge conflicts two different updates of insertItem are
     * needed. This is the first such update.
     */
    std::string m_mergeItem1;

    /**
     * The second merge update item. To avoid true conflicts it should
     * update different properties than mergeItem1, but even then servers
     * usually have problems perfectly merging items. Therefore the
     * test is run without expecting a certain merge result.
     */
    std::string m_mergeItem2;

    /**
     * The items in the inner vector are related: the first one the is
     * main one, the other(s) is/are a subordinate ones. The semantic
     * is that the main item is complete on it its own, while the
     * other normally should only be used in combination with the main
     * one.
     *
     * Because SyncML cannot express such dependencies between items,
     * a SyncSource has to be able to insert, updated and remove
     * both items independently. However, operations which violate
     * the semantic of the related items (like deleting the parent, but
     * not the child) may have unspecified results (like also deleting
     * the child). See linkedItemsRelaxedSemantic and sourceKnowsItemSemantic.
     *
     * One example for main and subordinate items are a recurring
     * iCalendar 2.0 event and a detached recurrence.
     */
    typedef class LinkedItems : public std::vector<std::string> {
    public:
        std::string m_name; /**< used as Client::Source::LinkedItems<m_name> */
        StringMap m_options; /**< used to pass additional parameters to the test */
        /** for testLinkedItemsSubset: create the additional VEVENT that is added when talking to Exchange;
            parameters are start, skip, index and total number of items in that test */
        boost::function<std::string (int, int, int, int)> m_testLinkedItemsSubsetAdditional;
    } LinkedItems_t;

    /**
     * The linked items may exist in different variations (outer vector).
     */
    typedef std::vector<LinkedItems_t> MultipleLinkedItems_t;

    MultipleLinkedItems_t m_linkedItems;

    /**
     * Another set of linked items for the LinkedItems*::testItemsAll/Second/Third/... tests.
     */
    MultipleLinkedItems_t m_linkedItemsSubset;

    /**
     * Backends atomic modification tests
     */
    Bool m_atomicModification;

    /**
     * set to false to disable tests which slightly violate the
     * semantic of linked items by inserting children
     * before/without their parent
     */
    Bool m_linkedItemsRelaxedSemantic;

    /**
     * setting this to false disables tests which depend
     * on the source's support for linked item semantic
     * (testLinkedItemsInsertParentTwice, testLinkedItemsInsertChildTwice)
     *
     * Matches SynthesisInfo::m_globalIDs.
     */
    Bool m_sourceKnowsItemSemantic;

    /**
     * Set this to true if the backend does not have IDs which are the
     * same for all clients and across slow syncs. For example, when
     * testing the ActiveSync backend this field needs to be true,
     * because items are renumbered as 1:x with x = 1, 2, ... for each
     * clients when a sync anchor is assigned to it.
     */
    Bool m_sourceLUIDsAreVolatile;

    /**
     * Set this to true if the backend supports
     * X-SYNCEVOLUTION-EXDATE-DETACHED, see CalDAVSource.cpp
     * CalDAVSource::readSubItem().
     */
    Bool m_supportsReccurenceEXDates;

    /**
     * called to dump all items into a file, required by tests which need
     * to compare items
     *
     * ClientTest::dump can be used: it will simply dump all items of the source
     * with a blank line as separator.
     *
     * @param source     sync source A already created and with beginSync() called
     * @param file       a file name
     * @return error code, 0 for success
     */
    boost::function<int (ClientTest &, TestingSyncSource &, const std::string &)> m_dump;

    /**
     * import test items: which these are is determined entirely by
     * the implementor, but tests work best if several complex items are
     * imported
     *
     * ClientTest::import can be used if the file contains items separated by
     * empty lines.
     *
     * @param source     sync source A already created and with beginSync() called
     * @param file       the name of the file to import
     * @retval realfile  the name of the file that was really imported;
     *                   this may depend on the current server that is being tested
     * @param luids      optional; if empty, then fill with luids (empty string for failed items);
     *                   if not empty, then update instead of adding the items
     * @return error string, empty for success
     */
    boost::function<std::string (ClientTest &, TestingSyncSource &, const ClientTestConfig &,
                                 const std::string &, std::string &, std::list<std::string> *)> m_import;

    /**
     * a function which compares two files with items in the format used by "dump"
     *
     * @param fileA      first file name
     * @param fileB      second file name
     * @return true if the content of the files is considered equal
     */
    boost::function<bool (ClientTest &, const std::string &, const std::string &)> m_compare;

    /**
     * A file with test cases in the format expected by import and compare.
     * The file should contain data as supported by the local storage.
     *
     * It is used in "Source::*::testImport" test, which verifies that
     * the backend can import and export that data.
     *
     * It is also used in "Sync::*::testItems", which verifies that
     * the peer can store and export it. Often local extensions are
     * not supported by peers. This can be handled in different ways:
     * - Patch synccompare to ignore such changes on a per-peer basis.
     * - Create a <testcases>.<peer>.tem file in the src/testcases
     *   build directory where <testcases> is the string here ("eds_event.ics"),
     *   and <peer> the value of CLIENT_TEST_SERVER ("funambol").
     *   That file then will be used in testItems instead of the base
     *   version. See the src/Makefile.am for rules that maintain such files.
     */
    std::string m_testcases;

    /**
     * the item type normally used by the source (not used by the tests
     * themselves; client-test.cpp uses it to initialize source configs)
     */
    std::string m_type;

    /**
     * a list of sub configs separated via , if this is a super datastore
     */
    std::string m_subConfigs;

    /**
     * TRUE if the source supports recovery from an interrupted
     * synchronization. Enables the Client::Sync::*::Retry group
     * of tests.
     */
    Bool m_retrySync;
    Bool m_suspendSync;
    Bool m_resendSync;

    /**
     * Set this to a list of properties which must *not* be removed
     * from the test items. Leave empty to disable the testRemoveProperties
     * test. Test items must be in vCard 3.0/iCalendar 2.0 format.
     */
    std::set<std::string> m_essentialProperties;

    /**
     * Set this to test if the source supports preserving local data extensions.
     * Uses the "testcases" data. See Sync::*::testExtensions.
     *
     * The function must modify a single item such that re-importing
     * it locally will be seen as updating it. It is empty by default
     * because not all backends necessarily pass this test.
     *
     * genericUpdate works for vCard and iCalendar by updating FN, N, resp. SUMMARY
     * and can be used as implementation of update.
     */
    boost::function<void (std::string &)> m_update;
    boost::function<void (std::string &)> m_genericUpdate;

    /**
     * A list of m_sourceName values of other ClientTestConfigs
     * which share the same database. Normally, sources are tested in
     * isolation, but for such linked sources we also need to test
     * interdependencies, in particular regarding change tracking and
     * item listing.
     */
    std::list<std::string> m_linkedSources;
};

/**
 * In addition to registering the sync source itself by creating an
 * instance of RegisterSyncSource, configurations for testing it can
 * also be registered. A sync source which supports more than one data
 * exchange format can register one configuration for each format, but
 * not registering any configuration is also okay.
 *
 * *Using* the registered tests depends on the CPPUnit test framework.
 * *Registering* does not. Therefore backends should always register *
 * *themselves for testing and leave it to the test runner
 * "client-test" whether tests are really executed.
 *
 * Unit tests are different. They create hard dependencies on CPPUnit
 * inside the code that contains them, and thus should be encapsulated
 * inside #ifdef ENABLE_UNIT_TESTS checks.
 *
 * Sync sources have to work stand-alone without a full SyncClient
 * configuration for all local tests. The minimal configuration prepared
 * for the source includes:
 * - a tracking node (as used f.i. by TrackingSyncSource) which
 *   points towards "~/.config/syncevolution/client-test-changes"
 * - a unique change ID (as used f.i. by EvolutionContactSource)
 * - a valid "evolutionsource" property in the config node, starting
 *   with the CLIENT_TEST_EVOLUTION_PREFIX env variable or (if that
 *   wasn't set) the "SyncEvolution_Test_" prefix
 * - "evolutionuser/password" if CLIENT_TEST_EVOLUTION_USER/PASSWORD
 *   are set
 *
 * No other properties are set, which implies that currently sync sources
 * which require further parameters cannot be tested.
 *
 * @warning There is a potential problem with the registration
 * mechanism. Both the sync source tests as well as the CPPUnit tests
 * derived from them are registrered when global class instances are
 * initialized. If the RegisterTestEvolution instance in
 * client-test-app.cpp is initialized *before* the sync source tests,
 * then those won't show up in the test list. Currently the right
 * order seems to be used, so everything works as expected.
 */
class RegisterSyncSourceTest
{
 public:
    /**
     * Invoked after all global constructors are run.
     * May add further RegisterSyncSourceTests to the
     * global registry.
     */
    virtual void init() const {}

    /**
     * This call is invoked after setting up the config with default
     * values for the test cases selected via the constructor's
     * testCaseName parameter (one of eds_contact, eds_contact, eds_event, eds_task;
     * see ClientTest in the Funambol client library for the current
     * list).
     *
     * This call can then override any of the values or (if there
     * are no predefined test cases) add them.
     *
     * The "type" property must select your sync source and the
     * data format for the test.
     *
     * @retval config        change any field whose default is not suitable
     */
    virtual void updateConfig(ClientTestConfig &config) const = 0;

    /**
     * @param configName     a unique string: the predefined names known by
     *                       ClientTest::getTestData() are already used for the initial
     *                       set of Evolution sync sources, for new sync sources
     *                       build a string by combining them with the sync source name
     *                       (e.g., "sqlite_eds_contact")
     * @param testCaseName   a string recognized by ClientTest::getTestData() or an
     *                       empty string if there are no predefined test cases
     */
    RegisterSyncSourceTest(const string &configName,
                           const string &testCaseName);
    virtual ~RegisterSyncSourceTest() {}

    const string m_configName;
    const string m_testCaseName;

    /**
     * A list of m_configName values of other RegisterSyncSourceTest
     * which share the same database. Normally, sources are tested in
     * isolation, but for such linked sources we also need to test
     * interdependencies, in particular regarding change tracking and
     * item listing.
     */
    std::list<std::string> m_linkedSources;
};

class TestRegistry : public vector<const RegisterSyncSourceTest *>
{
 public:
    // TODO: using const RegisterSyncSourceTest * operator [] (int);
    const RegisterSyncSourceTest * operator [] (const string &configName) const {
        BOOST_FOREACH(const RegisterSyncSourceTest *test, *this) {
            if (test->m_configName == configName) {
                return test;
            }
        }
        throw out_of_range(string("test config registry: ") + configName);
        return NULL;
    }
};

/**
 * a container for Synthesis XML config fragments
 *
 * Backends can define their own field lists, profiles, datatypes and
 * remote rules. The name of each of these entities have to be unique:
 * either prefix each name with the name of the backend or coordinate
 * with other developers (e.g. regarding shared field lists).
 *
 * To add new items, add them to the respective hash in your backend's
 * getDatastoreXML() or getSynthesisInfo() implementation. Both
 * methods have default implementations: getSynthesisInfo() is called
 * by the default getDatastoreXML() to provide some details and
 * provides them based on the "type" configuration option.
 *
 * The default config XML contains several predefined items:
 * - field lists: contacts, calendar, Note, bookmarks
 * - profiles: vCard, vCalendar, Note, vBookmark
 * - datatypes: vCard21, vCard30, vCalendar10, iCalendar20,
 *              note10/11 (no difference except the versioning!),
 *              vBookmark10
 * - remote rule: EVOLUTION
 *
 * These items do not appear in the hashes, so avoid picking the same
 * names. The entries of each hash has to be a well-formed XML
 * element, their keys the name encoded in each XML element.
 */
struct XMLConfigFragments {
    class mapping : public std::map<std::string, std::string> {
    public:
        string join() {
            string res;
            size_t len = 0;
            BOOST_FOREACH(const value_type &entry, *this) {
                len += entry.second.size() + 1;
            }
            res.reserve(len);
            BOOST_FOREACH(const value_type &entry, *this) {
                res += entry.second;
                res += "\n";
            }
            return res;
        }
    } m_fieldlists,
        m_profiles,
        m_datatypes,
        m_remoterules;
};

/**
 * used in SyncSource::Operations post-operation signal
 */
enum OperationExecution {
    OPERATION_SKIPPED,      /**< operation was skipped because pre-operation slot threw an exception */
    OPERATION_EXCEPTION,    /**< operation itself failed with an exception (may also return error code) */
    OPERATION_FINISHED,     /**< operation finished normally (but might have returned an error code) */
    OPERATION_EMPTY         /**< operation not implemented */
};

/**
 * Helper class for looking up a pending operation by a Synthesis parameter.
 * KeyH (add, replace) and item ID (delete) are supported.
 */
template<class A1> struct KeyConverter;

/**
 * For KeyH we make the assumption that the key exists as long
 * as the pending operation, and thus its address can be used as
 * unique identifier for the operation.
 */
template<> struct KeyConverter<sysync::KeyH>
{
    typedef void * key_type;
    static key_type toKey(sysync::KeyH key) { return static_cast<void *>(key); }
};

/**
 * For cItemID we just use the item ID as string.
 */
template<> struct KeyConverter<sysync::cItemID>
{
    typedef std::string key_type;
    static key_type toKey(sysync::cItemID id) { return id->item; }
};

/**
 * To be returned by a function wrapped by OperationWrapper
 * when the function is not done yet and wants to be called again
 * for the same item.
 */
template <class F> class ContinueOperation : public boost::function<F>
{
 public:
    ContinueOperation()
    {}

    ContinueOperation(const boost::function<F> &callback) : 
        boost::function<F>(callback)
    {}
};

/**
 * Interface expected by SyncSourceBase helper class.
 * Needed to break a cyclic dependency.
 */
class SyncSourceName
{
 public:
    /**
     * the name of the sync source (for example, "addressbook"),
     * unique in the context of its own configuration
     **/
    virtual std::string getName() const { return "uninitialized SyncSourceBase"; }

    /**
     * the name of the sync source as it should be displayed to users
     * in debug messages; typically the same as getName(), but may
     * also include a context ("@foobar/addressbook") to disambiguate
     * the name when "addressbook" is used multiple times in a sync (as
     * with local sync)
     */
    virtual std::string getDisplayName() const { return "uninitialized SyncSourceBase"; }
};

/**
 * Implements the "call all slots, error if any failed" semantic of
 * the pre- and post-signals described below.
 */
class OperationSlotInvoker {
    SyncSourceName &m_source;

 public:
    OperationSlotInvoker(SyncSourceName &source) :
        m_source(source)
    {}

    typedef sysync::TSyError result_type;
    template<typename InputIterator>
        result_type operator() (InputIterator first, InputIterator last) const
        {
            result_type res = sysync::LOCERR_OK;
            while (first != last) {
                SyncMLStatus status;
                try {
                    status = *first;
                } catch (...) {
                    status = Exception::handle(m_source.getDisplayName());
                }
                if (res == sysync::LOCERR_OK) {
                    res = static_cast<result_type>(status);
                }
                ++first;
            }
            return res;
        }
};


/**
 * Helper class, needs to be specialized based on number of parameters
 * and return type.
 */
template<class F, int arity, class R> class OperationWrapperSwitch;

/** one parameter, sysync::TSyError type */
template<class F> class OperationWrapperSwitch<F, 0, sysync::TSyError>
{
 public:
    typedef sysync::TSyError result_type;
    typedef boost::function<F> OperationType;

    /**
     * The pre-signal is invoked with the same parameters as
     * the operations, plus a reference to the sync source as
     * initial parameter. Slots may return something other than
     * STATUS_OK or throw exceptions, which
     * will skip the actual implementation. However, all slots
     * will be invoked exactly once even if one of them throws
     * an exception. The result of the operation then is the
     * error code extracted from the first exception (see
     * OperationSlotInvoker).
     */
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &),
        OperationSlotInvoker> PreSignal;

    /**
     * The post-signal is invoked exactly once, regardless
     * whether the implementation was skipped, executed or
     * doesn't exist at all. This information is passed as the
     * second parameter, followed by the result of the
     * operation or the pre-signals, followed by the
     * parameters of the operation.
     *
     * As with the pre-signal, any slot may return something other
     * than STATUS_OK or throw an exception
     * to override the final result, but this won't interrupt
     * calling the rest of the slots.
     */
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, OperationExecution, sysync::TSyError),
        OperationSlotInvoker> PostSignal;

    /**
     * invokes signals and implementation of operation,
     * combines all return codes into one
     */
    sysync::TSyError operator () () const throw ()
    {
        sysync::TSyError res;
        OperationExecution exec;
        res = m_pre(dynamic_cast<SyncSource &>(m_source));
        if (res != sysync::LOCERR_OK) {
            exec = OPERATION_SKIPPED;
        } else {
            if (m_operation) {
                try {
                    res = m_operation();
                    exec = OPERATION_FINISHED;
                } catch (...) {
                    res = Exception::handle(m_source.getDisplayName());
                    exec = OPERATION_EXCEPTION;
                }
            } else {
                res = sysync::LOCERR_NOTIMP;
                exec = OPERATION_EMPTY;
            }
        }
        sysync::TSyError newres = m_post(dynamic_cast<SyncSource &>(m_source), exec, res);
        if (newres != sysync::LOCERR_OK) {
            res = newres;
        }
        return res == STATUS_FATAL ? STATUS_DATASTORE_FAILURE : res;
    }

    /**
     * Anyone may connect code to the signals via
     * getOperations().getPre/PostSignal(), although strictly
     * speaking this modifies the behavior of the
     * implementation.
     */
    PreSignal &getPreSignal() const { return const_cast<PreSignal &>(m_pre); }
    PostSignal &getPostSignal() const { return const_cast<PostSignal &>(m_post); }


    OperationWrapperSwitch(SyncSourceName &source) :
        m_source(source),
        m_pre(OperationSlotInvoker(source)),
        m_post(OperationSlotInvoker(source))
    {
    }

 protected:
    OperationType m_operation;

 private:
    SyncSourceName &m_source;
    PreSignal m_pre;
    PostSignal m_post;
};

template<class F> class OperationWrapperSwitch<F, 1, sysync::TSyError>
{
 public:
    typedef sysync::TSyError result_type;
    typedef boost::function<F> OperationType;
    typedef typename boost::function<F>::arg1_type arg1_type;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, arg1_type a1),
        OperationSlotInvoker> PreSignal;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, OperationExecution, sysync::TSyError,
                                                  arg1_type a1),
        OperationSlotInvoker> PostSignal;

    sysync::TSyError operator () (arg1_type a1) const throw ()
    {
        sysync::TSyError res;
        OperationExecution exec;
        res = m_pre(dynamic_cast<SyncSource &>(m_source), a1);
        if (res != sysync::LOCERR_OK) {
            exec = OPERATION_SKIPPED;
        } else {
            if (m_operation) {
                try {
                    res = m_operation(a1);
                    exec = OPERATION_FINISHED;
                } catch (...) {
                    res = Exception::handle(m_source.getDisplayName());
                    exec = OPERATION_EXCEPTION;
                }
            } else {
                res = sysync::LOCERR_NOTIMP;
                exec = OPERATION_EMPTY;
            }
        }
        sysync::TSyError newres = m_post(dynamic_cast<SyncSource &>(m_source), exec, res, a1);
        if (newres != sysync::LOCERR_OK) {
            res = newres;
        }
        return res == STATUS_FATAL ? STATUS_DATASTORE_FAILURE : res;
    }

    PreSignal &getPreSignal() const { return const_cast<PreSignal &>(m_pre); }
    PostSignal &getPostSignal() const { return const_cast<PostSignal &>(m_post); }

    OperationWrapperSwitch(SyncSourceName &source) :
        m_source(source),
        m_pre(OperationSlotInvoker(source)),
        m_post(OperationSlotInvoker(source))
    {
    }

 protected:
    OperationType m_operation;

 private:
    SyncSourceName &m_source;
    PreSignal m_pre;
    PostSignal m_post;
};

template<class F, class V> class OperationWrapperSwitch<F, 1, V>
{
 public:
    typedef sysync::TSyError result_type;
    typedef boost::function<F> OperationType;
    typedef typename boost::function<F>::arg1_type arg1_type;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, arg1_type a1),
        OperationSlotInvoker> PreSignal;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, OperationExecution, sysync::TSyError,
                                          arg1_type a1),
        OperationSlotInvoker> PostSignal;
    typedef KeyConverter<arg1_type> Converter;
    typedef ContinueOperation<sysync::TSyError (arg1_type)> Continue;
    typedef std::map<typename Converter::key_type, Continue> Pending;

    sysync::TSyError operator () (arg1_type a1) const throw ()
    {
        sysync::TSyError res;
        OperationExecution exec;

        // Marking m_pending "volatile" didn't work, find() not defined for that.
        typename Pending::iterator it = const_cast<Pending &>(m_pending).find(Converter::toKey(a1));
        bool continuing = it != m_pending.end();

        res = continuing ? sysync::LOCERR_OK : m_pre(dynamic_cast<SyncSource &>(m_source), a1);
        if (res != sysync::LOCERR_OK) {
            exec = OPERATION_SKIPPED;
        } else {
            if (continuing) {
                res = it->second(a1);
                if (res != sysync::LOCERR_AGAIN) {
                    const_cast<Pending &>(m_pending).erase(it);
                }
            } else if (m_operation) {
                try {
                    V newres = m_operation(a1);
                    sysync::TSyError *completed = boost::get<sysync::TSyError>(&newres);
                    if (completed) {
                        res = *completed;
                    } else {
                        res = sysync::LOCERR_AGAIN;
                        Continue cont = boost::get<Continue>(newres);
                        const_cast<Pending &>(m_pending).insert(std::make_pair(Converter::toKey(a1), cont));
                    }
                    exec = OPERATION_FINISHED;
                } catch (...) {
                    res = Exception::handle(m_source.getDisplayName());
                    exec = OPERATION_EXCEPTION;
                }
            } else {
                res = sysync::LOCERR_NOTIMP;
                exec = OPERATION_EMPTY;
            }
        }
        if (res != sysync::LOCERR_AGAIN) {
            sysync::TSyError newres = m_post(dynamic_cast<SyncSource &>(m_source), exec, res, a1);
            if (newres != sysync::LOCERR_OK) {
                res = newres;
            }
        }
        return res == STATUS_FATAL ? STATUS_DATASTORE_FAILURE : res;
    }

    PreSignal &getPreSignal() const { return const_cast<PreSignal &>(m_pre); }
    PostSignal &getPostSignal() const { return const_cast<PostSignal &>(m_post); }

    OperationWrapperSwitch(SyncSourceName &source) :
        m_source(source),
        m_pre(OperationSlotInvoker(source)),
        m_post(OperationSlotInvoker(source))
    {
    }

 protected:
    OperationType m_operation;

 private:
    SyncSourceName &m_source;
    PreSignal m_pre;
    PostSignal m_post;
    Pending m_pending;
};

template<class F> class OperationWrapperSwitch<F, 2, sysync::TSyError>
{
 public:
    typedef sysync::TSyError result_type;
    typedef boost::function<F> OperationType;
    typedef typename boost::function<F>::arg1_type arg1_type;
    typedef typename boost::function<F>::arg2_type arg2_type;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, arg1_type a1, arg2_type a2),
        OperationSlotInvoker> PreSignal;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, OperationExecution, sysync::TSyError,
                                                  arg1_type a1, arg2_type a2),
        OperationSlotInvoker> PostSignal;

    sysync::TSyError operator () (arg1_type a1, arg2_type a2) const throw ()
    {
        sysync::TSyError res;
        OperationExecution exec;
        res = m_pre(dynamic_cast<SyncSource &>(m_source), a1, a2);
        if (res != sysync::LOCERR_OK) {
            exec = OPERATION_SKIPPED;
        } else {
            if (m_operation) {
                try {
                    res = m_operation(a1, a2);
                    exec = OPERATION_FINISHED;
                } catch (...) {
                    res = Exception::handle(m_source.getDisplayName());
                    exec = OPERATION_EXCEPTION;
                }
            } else {
                res = sysync::LOCERR_NOTIMP;
                exec = OPERATION_EMPTY;
            }
        }
        sysync::TSyError newres = m_post(dynamic_cast<SyncSource &>(m_source), exec, res, a1, a2);
        if (newres != sysync::LOCERR_OK) {
            res = newres;
        }
        return res == STATUS_FATAL ? STATUS_DATASTORE_FAILURE : res;
    }

    PreSignal &getPreSignal() const { return const_cast<PreSignal &>(m_pre); }
    PostSignal &getPostSignal() const { return const_cast<PostSignal &>(m_post); }

    OperationWrapperSwitch(SyncSourceName &source) :
        m_source(source),
        m_pre(OperationSlotInvoker(source)),
        m_post(OperationSlotInvoker(source))
    {
    }

 protected:
    OperationType m_operation;

 private:
    SyncSourceName &m_source;
    PreSignal m_pre;
    PostSignal m_post;
};

template<class F, class V> class OperationWrapperSwitch<F, 2, V>
{
 public:
    typedef sysync::TSyError result_type;
    typedef boost::function<F> OperationType;
    typedef typename boost::function<F>::arg1_type arg1_type;
    typedef typename boost::function<F>::arg2_type arg2_type;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, arg1_type a1, arg2_type a2),
        OperationSlotInvoker> PreSignal;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, OperationExecution, sysync::TSyError,
                                                  arg1_type a1, arg2_type a2),
        OperationSlotInvoker> PostSignal;
    typedef KeyConverter<arg1_type> Converter;
    typedef ContinueOperation<sysync::TSyError (arg1_type, arg2_type)> Continue;
    typedef std::map<typename Converter::key_type, Continue> Pending;

    sysync::TSyError operator () (arg1_type a1, arg2_type a2) const throw ()
    {
        sysync::TSyError res;
        OperationExecution exec;

        // Marking m_pending "volatile" didn't work, find() not defined for that.
        typename Pending::iterator it = const_cast<Pending &>(m_pending).find(Converter::toKey(a1));
        bool continuing = it != m_pending.end();

        res = continuing ? sysync::LOCERR_OK : m_pre(dynamic_cast<SyncSource &>(m_source), a1, a2);
        if (res != sysync::LOCERR_OK) {
            exec = OPERATION_SKIPPED;
        } else {
            if (continuing) {
                res = it->second(a1, a2);
                if (res != sysync::LOCERR_AGAIN) {
                    const_cast<Pending &>(m_pending).erase(it);
                }
            } else if (m_operation) {
                try {
                    V newres = m_operation(a1, a2);
                    sysync::TSyError *completed = boost::get<sysync::TSyError>(&newres);
                    if (completed) {
                        res = *completed;
                    } else {
                        res = sysync::LOCERR_AGAIN;
                        Continue cont = boost::get<Continue>(newres);
                        const_cast<Pending &>(m_pending).insert(std::make_pair(Converter::toKey(a1), cont));
                    }
                    exec = OPERATION_FINISHED;
                } catch (...) {
                    res = Exception::handle(m_source.getDisplayName());
                    exec = OPERATION_EXCEPTION;
                }
            } else {
                res = sysync::LOCERR_NOTIMP;
                exec = OPERATION_EMPTY;
            }
        }
        if (res != sysync::LOCERR_AGAIN) {
            sysync::TSyError newres = m_post(dynamic_cast<SyncSource &>(m_source), exec, res, a1, a2);
            if (newres != sysync::LOCERR_OK) {
                res = newres;
            }
        }
        return res == STATUS_FATAL ? STATUS_DATASTORE_FAILURE : res;
    }

    PreSignal &getPreSignal() const { return const_cast<PreSignal &>(m_pre); }
    PostSignal &getPostSignal() const { return const_cast<PostSignal &>(m_post); }

    OperationWrapperSwitch(SyncSourceName &source) :
        m_source(source),
        m_pre(OperationSlotInvoker(source)),
        m_post(OperationSlotInvoker(source))
    {
    }

 protected:
    OperationType m_operation;

 private:
    SyncSourceName &m_source;
    PreSignal m_pre;
    PostSignal m_post;
    Pending m_pending;
};

template<class F> class OperationWrapperSwitch<F, 3, sysync::TSyError>
{
 public:
    typedef sysync::TSyError result_type;
    typedef boost::function<F> OperationType;
    typedef typename boost::function<F>::arg1_type arg1_type;
    typedef typename boost::function<F>::arg2_type arg2_type;
    typedef typename boost::function<F>::arg3_type arg3_type;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, arg1_type a1, arg2_type a2, arg3_type a3),
        OperationSlotInvoker> PreSignal;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, OperationExecution, sysync::TSyError,
                                                  arg1_type a1, arg2_type a2, arg3_type a3),
        OperationSlotInvoker> PostSignal;

    sysync::TSyError operator () (arg1_type a1, arg2_type a2, arg3_type a3) const throw ()
    {
        sysync::TSyError res;
        OperationExecution exec;
        res = m_pre(dynamic_cast<SyncSource &>(m_source), a1, a2, a3);
        if (res != sysync::LOCERR_OK) {
            exec = OPERATION_SKIPPED;
        } else {
            if (m_operation) {
                try {
                    res = m_operation(a1, a2, a3);
                    exec = OPERATION_FINISHED;
                } catch (...) {
                    res = Exception::handle(m_source.getDisplayName());
                        exec = OPERATION_EXCEPTION;
                }
            } else {
                res = sysync::LOCERR_NOTIMP;
                exec = OPERATION_EMPTY;
            }
        }
        sysync::TSyError newres = m_post(dynamic_cast<SyncSource &>(m_source), exec, res, a1, a2, a3);
        if (newres != sysync::LOCERR_OK) {
            res = newres;
        }
        return res == STATUS_FATAL ? STATUS_DATASTORE_FAILURE : res;
    }

    PreSignal &getPreSignal() const { return const_cast<PreSignal &>(m_pre); }
    PostSignal &getPostSignal() const { return const_cast<PostSignal &>(m_post); }

    OperationWrapperSwitch(SyncSourceName &source) :
        m_source(source),
        m_pre(OperationSlotInvoker(source)),
        m_post(OperationSlotInvoker(source))
    {
    }

 protected:
    OperationType m_operation;

 private:
    SyncSourceName &m_source;
    PreSignal m_pre;
    PostSignal m_post;

};

template<class F, class V> class OperationWrapperSwitch<F, 3, V>
{
 public:
    typedef sysync::TSyError result_type;
    typedef boost::function<F> OperationType;
    typedef typename boost::function<F>::arg1_type arg1_type;
    typedef typename boost::function<F>::arg2_type arg2_type;
    typedef typename boost::function<F>::arg3_type arg3_type;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, arg1_type a1, arg2_type a2, arg3_type a3),
        OperationSlotInvoker> PreSignal;
    typedef boost::signals2::signal<SyncMLStatus (SyncSource &, OperationExecution, sysync::TSyError,
                                                  arg1_type a1, arg2_type a2, arg3_type a3),
        OperationSlotInvoker> PostSignal;
    typedef KeyConverter<arg1_type> Converter;
    typedef ContinueOperation<sysync::TSyError (arg1_type, arg2_type, arg3_type)> Continue;
    typedef std::map<typename Converter::key_type, Continue> Pending;

    sysync::TSyError operator () (arg1_type a1, arg2_type a2, arg3_type a3) const throw ()
    {
        sysync::TSyError res;
        OperationExecution exec;

        // Marking m_pending "volatile" didn't work, find() not defined for that.
        typename Pending::iterator it = const_cast<Pending &>(m_pending).find(Converter::toKey(a1));
        bool continuing = it != m_pending.end();

        res = continuing ? sysync::LOCERR_OK : m_pre(dynamic_cast<SyncSource &>(m_source), a1, a2, a3);
        if (res != sysync::LOCERR_OK) {
            exec = OPERATION_SKIPPED;
        } else {
            if (continuing) {
                res = it->second(a1, a2, a3);
                if (res != sysync::LOCERR_AGAIN) {
                    const_cast<Pending &>(m_pending).erase(it);
                }
            } else if (m_operation) {
                try {
                    V newres = m_operation(a1, a2, a3);
                    sysync::TSyError *completed = boost::get<sysync::TSyError>(&newres);
                    if (completed) {
                        res = *completed;
                    } else {
                        res = sysync::LOCERR_AGAIN;
                        Continue cont = boost::get<Continue>(newres);
                        const_cast<Pending &>(m_pending).insert(std::make_pair(Converter::toKey(a1), cont));
                    }
                    exec = OPERATION_FINISHED;
                } catch (...) {
                    res = Exception::handle(m_source.getDisplayName());
                    exec = OPERATION_EXCEPTION;
                }
            } else {
                res = sysync::LOCERR_NOTIMP;
                exec = OPERATION_EMPTY;
            }
        }
        if (res != sysync::LOCERR_AGAIN) {
            sysync::TSyError newres = m_post(dynamic_cast<SyncSource &>(m_source), exec, res, a1, a2, a3);
            if (newres != sysync::LOCERR_OK) {
                res = newres;
            }
        }
        return res == STATUS_FATAL ? STATUS_DATASTORE_FAILURE : res;
    }

    PreSignal &getPreSignal() const { return const_cast<PreSignal &>(m_pre); }
    PostSignal &getPostSignal() const { return const_cast<PostSignal &>(m_post); }

    OperationWrapperSwitch(SyncSourceName &source) :
        m_source(source),
        m_pre(OperationSlotInvoker(source)),
        m_post(OperationSlotInvoker(source))
    {
    }

 protected:
    OperationType m_operation;

 private:
    SyncSourceName &m_source;
    PreSignal m_pre;
    PostSignal m_post;
    Pending m_pending;
};

/**
 * This mimics a boost::function with the same signature. The function
 * signature F must have a sysync::TSyError error return code, as in most
 * of the Synthesis DB API, or a boost::variant of sysync::TSyError and
 * ContinueOperation<F>.
 *
 * Specializations of this class for operations with different number
 * of parameters provide a call operator which invokes a pre- and
 * post-signal around the actual implementation. See
 * OperationWrapperSwitch<F, 0> for details.
 *
 * If the function returns a variant with a ContinueOperation<F> inside,
 * the OperationWrapper will store that callback for the current
 * set of parameters (currently using only the first one as key),
 * then when called again, skip the pre-signal and invoke the callback
 * instead of the original operation. That callback may return LOCERR_AGAIN
 * to request being called again the same way. The post-signal is
 * called when the operation finally completes.
 *
 * Additional operations could be wrapped by providing more
 * specializations (different return code, more parameters). The
 * number or parameters in the operation cannot exceed six, because
 * adding three more parameters in the post-signal would push the
 * total number of parameters in that signal beyond the limit of nine
 * supported arguments in boost::signals2/boost::function.
 */
template<class F> class OperationWrapper :
public OperationWrapperSwitch<F, boost::function<F>::arity, typename boost::function<F>::result_type>
{
    typedef OperationWrapperSwitch<F, boost::function<F>::arity, typename boost::function<F>::result_type> inherited;
 public:
    OperationWrapper(SyncSourceName &source): inherited(source) {}

    /** operation implemented? */
    operator bool () const { return inherited::m_operation; }

    /**
     * Only usable by derived classes via read/write m_operations:
     * sets the actual implementation of the operation.
     */
    void operator = (const boost::function<F> &operation)
    {
        inherited::m_operation = operation;
    }
};

/**
 * abstract base class for SyncSource with some common functionality
 * and no data
 *
 * Used to implement and call that functionality in multiple derived
 * classes, including situations where a derived class is derived from
 * this base via different intermediate classes, therefore the
 * need to keep it abstract.
 */
class SyncSourceBase : public SyncSourceName {
 public:
    virtual ~SyncSourceBase() {}

    /**
     * Convenience function, to be called inside a catch() block of
     * (or for) the sync source.
     *
     * Rethrows the exception to determine what it is, then logs it
     * as an error and returns a suitable error code (usually a general
     * STATUS_DATASTORE_FAILURE).
     *
     * @param flags     influence behavior of the method
     */
    SyncMLStatus handleException(HandleExceptionFlags flags = HANDLE_EXCEPTION_FLAGS_NONE);

    /**
     * throw an exception after an operation failed
     *
     * output format: <source name>: <action>: <error string>
     *
     * @param action   a string describing the operation or object involved
     * @param error    the errno error code for the failure
     */
    void throwError(const SourceLocation &where, const string &action, int error) SE_NORETURN;

    /**
     * throw an exception after an operation failed and
     * remember that this instance has failed
     *
     * output format: <source name>: <failure>
     *
     * @param action     a string describing what was attempted *and* how it failed
     */
    void throwError(const SourceLocation &where, const string &failure) SE_NORETURN;

    /**
     * throw an exception with a specific status code after an operation failed and
     * remember that this instance has failed
     *
     * output format: <source name>: <failure>
     *
     * @param status     a more specific status code; other throwError() variants use STATUS_FATAL
     * @param action     a string describing what was attempted *and* how it failed
     */
    void throwError(const SourceLocation &where, SyncMLStatus status, const string &failure) SE_NORETURN;

    /**
     * The Synthesis engine only counts items which are deleted by the
     * peer. Items deleted locally at the start of a
     * refresh-from-server sync are not counted (and cannot be counted
     * in all cases).
     *
     * Sync sources which want to have those items included in the
     * sync statistics should count *all* deleted items using these
     * methods. SyncContext will use this number for
     * refresh-from-server syncs.
     */
    /**@{*/
    virtual long getNumDeleted() const = 0;
    virtual void setNumDeleted(long num) = 0;
    virtual void incrementNumDeleted() = 0;
    /**@}*/

    /**
     * Return Synthesis <datastore> XML fragment for this sync source.
     * Must *not* include the <datastore> element; it is created by
     * the caller.
     *
     * The default implementation returns a configuration for the
     * SynthesisDBPlugin, which invokes SyncSource::Operations. Items
     * are exchanged with the SyncsSource in the format defined by
     * getSynthesisInfo(). The format used with the SyncML side is
     * negotiated via the peer's capabilities, with the type defined
     * in the configuration being the preferred one of the data store.
     *
     * See SyncContext::getConfigXML() for details about
     * predefined <datatype> entries that can be referenced here.
     *
     * @retval xml         put content of <datastore>...</datastore> here
     * @retval fragments   the necessary definitions for the datastore have to be added here
     */
    virtual void getDatastoreXML(string &xml, XMLConfigFragments &fragments);

    /**
     * Synthesis <datatype> name which matches the format used
     * for importing and exporting items (exportData()).
     * This is not necessarily the same format that is given
     * to the Synthesis engine. If this internal format doesn't
     * have a <datatype> in the engine, then an empty string is
     * returned.
     */
    virtual string getNativeDatatypeName();

    /**
     * return Synthesis API pointer, if one currently is available
     * (between SyncEvolution_Module_CreateContext() and
     * SyncEvolution_Module_DeleteContext())
     */
    virtual SDKInterface *getSynthesisAPI() const = 0;

    /**
     * Prepare the sync source for usage inside a SyncML server.  To
     * be called directly after creating the source, if at all.
     */
    virtual void enableServerMode() = 0;
    virtual bool serverModeEnabled() const = 0;

    /**
     * The optional operations.
     *
     * All of them are guaranteed to happen between open() and
     * close().
     *
     * They are all allowed to throw exceptions: the operations called
     * by SyncEvolution then abort whatever SyncEvolution was doing
     * and end in the normal exception handling. For the Synthesis
     * operations, the bridge code in SynthesisDBPlugin code catches
     * exceptions, logs them and translates them into Synthesis error
     * codes, which are returned to the Synthesis engine.
     *
     * Monitoring of most DB operations is possible via the pre- and
     * post-signals managed by OperationWrapper.
     */
    struct Operations {
        Operations(SyncSourceName &source);

        /**
         * The caller determines where item data is stored (m_dirname)
         * and where meta information about them (m_node). The callee
         * then can use both arbitrarily. As an additional hint,
         * m_mode specifies why and when the backup is made, which
         * is useful to determine whether information can be reused.
         */
        struct BackupInfo {
            enum Mode {
                BACKUP_BEFORE,   /**< directly at start of sync */
                BACKUP_AFTER,    /**< directly after sync */
                BACKUP_OTHER
            } m_mode;
            string m_dirname;
            boost::shared_ptr<ConfigNode> m_node;
            BackupInfo() {}
            BackupInfo(Mode mode,
                       const string &dirname,
                       const boost::shared_ptr<ConfigNode> &node) :
                m_mode(mode),
                m_dirname(dirname),
                m_node(node)
            {}
        };
        struct ConstBackupInfo {
            BackupInfo::Mode m_mode;
            string m_dirname;
            boost::shared_ptr<const ConfigNode> m_node;
            ConstBackupInfo() {}
            ConstBackupInfo(BackupInfo::Mode mode,
                            const string &dirname,
                            const boost::shared_ptr<const ConfigNode> &node) :
                m_mode(mode),
                m_dirname(dirname),
                m_node(node)
            {}
        };

        /**
         * Dump all data from source unmodified into the given backup location.
         * Information about the created backup is added to the
         * report.
         *
         * Required for the backup/restore functionality in
         * SyncEvolution, not for syncing itself. But typically it is
         * called before syncing (can be turned off by users), so
         * implementations can reuse the information gathered while
         * making a backup in later operations.
         *
         * @param previous     the most recent backup, empty m_dirname if none
         * @param next         the backup which is to be created, directory and node are empty
         * @param report       to be filled with information about backup (number of items, etc.)
         */
        typedef void (BackupData_t)(const ConstBackupInfo &oldBackup,
                                    const BackupInfo &newBackup,
                                    BackupReport &report);
        boost::function<BackupData_t> m_backupData;

        /**
         * Restore database from data stored in backupData().
         * If possible don't touch items which are the same as in the
         * backup, to mimimize impact on future incremental syncs.
         *
         * @param oldBackup    the backup which is to be restored
         * @param dryrun       pretend to restore and fill in report, without
         *                     actually touching backend data
         * @param report       to be filled with information about restore
         *                     (number of total items and changes)
         */
        typedef void (RestoreData_t)(const ConstBackupInfo &oldBackup,
                                     bool dryrun,
                                     SyncSourceReport &report);
        boost::function<RestoreData_t> m_restoreData;

        /**
         * initialize information about local changes and items
         * as in beginSync() with all parameters set to true,
         * but without changing the state of the underlying database
         *
         * This method will be called to check for local changes without
         * actually running a sync, so there is no matching end call.
         *
         * There might be sources which don't support non-destructive
         * change tracking (in other words, checking changes permanently
         * modifies the state of the source and cannot be repeated).
         * Such sources should leave the functor empty.
         */
        typedef void (CheckStatus_t)(SyncSourceReport &local);
        boost::function<CheckStatus_t> m_checkStatus;

        /**
         * A quick check whether the source currently has data.
         *
         * If this cannot be determined easily, don't provide the
         * operation. The information is currently only used to
         * determine whether a slow sync should be allowed. If
         * the operation is not provided, the assumption is that
         * there is local data, which disables the "allow slow
         * sync for empty databases" heuristic and forces the user
         * to choose.
         */
        typedef bool (IsEmpty_t)();
        boost::function<IsEmpty_t> m_isEmpty;

        /**
         * Synthesis DB API callbacks. For documentation see the
         * Synthesis API specification (PDF and/or sync_dbapi.h).
         *
         * Implementing this is necessary for SyncSources which want
         * to be part of a sync session.
         */
        /**@{*/
        typedef OperationWrapper<sysync::TSyError (const char *, const char *)> StartDataRead_t;
        StartDataRead_t m_startDataRead;

        typedef OperationWrapper<sysync::TSyError ()> EndDataRead_t;
        EndDataRead_t m_endDataRead;

        typedef OperationWrapper<sysync::TSyError ()> StartDataWrite_t;
        StartDataWrite_t m_startDataWrite;

        typedef OperationWrapper<sysync::TSyError (sysync::cItemID aID, sysync::ItemID updID)> FinalizeLocalID_t;
        FinalizeLocalID_t m_finalizeLocalID;

        typedef OperationWrapper<sysync::TSyError (bool success, char **newToken)> EndDataWrite_t;
        EndDataWrite_t m_endDataWrite;

        /** the SynthesisDBPlugin is configured so that this operation
            doesn't have to (and cannot) return the item data */
        typedef OperationWrapper<sysync::TSyError (sysync::ItemID aID,
                                                   sysync::sInt32 *aStatus, bool aFirst)> ReadNextItem_t;
        ReadNextItem_t m_readNextItem;

        typedef OperationWrapper<sysync::TSyError (sysync::cItemID aID, sysync::KeyH aItemKey)> ReadItemAsKey_t;
        ReadItemAsKey_t m_readItemAsKey;

        typedef ContinueOperation<sysync::TSyError (sysync::KeyH aItemKey, sysync::ItemID newID)> InsertItemAsKeyContinue_t;
        typedef boost::variant<sysync::TSyError, InsertItemAsKeyContinue_t> InsertItemAsKeyResult_t;
        typedef OperationWrapper<InsertItemAsKeyResult_t (sysync::KeyH aItemKey, sysync::ItemID newID)> InsertItemAsKey_t;
        InsertItemAsKey_t m_insertItemAsKey;

        typedef ContinueOperation<sysync::TSyError (sysync::KeyH aItemKey, sysync::cItemID aID, sysync::ItemID updID)> UpdateItemAsKeyContinue_t;
        typedef boost::variant<sysync::TSyError, UpdateItemAsKeyContinue_t> UpdateItemAsKeyResult_t;
        typedef OperationWrapper<UpdateItemAsKeyResult_t (sysync::KeyH aItemKey, sysync::cItemID aID, sysync::ItemID updID)> UpdateItemAsKey_t;
        UpdateItemAsKey_t m_updateItemAsKey;

        typedef ContinueOperation<sysync::TSyError (sysync::cItemID aID)> DeleteItemContinue_t;
        typedef boost::variant<sysync::TSyError, DeleteItemContinue_t> DeleteItemResult_t;
        typedef OperationWrapper<DeleteItemResult_t (sysync::cItemID aID)> DeleteItem_t;
        DeleteItem_t m_deleteItem;
        /**@}*/


        /**
         * Synthesis administration callbacks. For documentation see the
         * Synthesis API specification (PDF and/or sync_dbapi.h).
         *
         * Implementing this is *optional* in clients. In the Synthesis client
         * engine, the "binfiles" module provides these calls without SyncEvolution
         * doing anything.
         *
         * In the Synthesis server engine, the
         * SyncSource::enableServerMode() call must install an
         * implementation, like the one from SyncSourceAdmin.
         */
        /**@{*/
        typedef OperationWrapper<sysync::TSyError (const char *aLocDB,
                                                   const char *aRemDB,
                                                   char **adminData)> LoadAdminData_t;
        LoadAdminData_t m_loadAdminData;

        typedef OperationWrapper<sysync::TSyError (const char *adminData)> SaveAdminData_t;
        SaveAdminData_t m_saveAdminData;

        // not currently wrapped because it has a different return type;
        // templates could be adapted to handle that
        typedef bool (ReadNextMapItem_t)(sysync::MapID mID, bool aFirst);
        boost::function<ReadNextMapItem_t> m_readNextMapItem;

        typedef OperationWrapper<sysync::TSyError (sysync::cMapID mID)> InsertMapItem_t;
        InsertMapItem_t m_insertMapItem;

        typedef OperationWrapper<sysync::TSyError (sysync::cMapID mID)> UpdateMapItem_t;
        UpdateMapItem_t m_updateMapItem;

        typedef OperationWrapper<sysync::TSyError (sysync::cMapID mID)> DeleteMapItem_t;
        DeleteMapItem_t m_deleteMapItem;

        // not wrapped, too many parameters
        typedef boost::function<sysync::TSyError (sysync::cItemID aID, const char *aBlobID,
                                                  void **aBlkPtr, size_t *aBlkSize,
                                                  size_t *aTotSize,
                                                  bool aFirst, bool *aLast)> ReadBlob_t;
        ReadBlob_t m_readBlob;

        typedef boost::function<sysync::TSyError (sysync::cItemID aID, const char *aBlobID,
                                                  void *aBlkPtr, size_t aBlkSize,
                                                  size_t aTotSize,
                                                  bool aFirst, bool aLast)> WriteBlob_t;
        WriteBlob_t m_writeBlob;

        typedef OperationWrapper<sysync::TSyError (sysync::cItemID aID, const char *aBlobID)> DeleteBlob_t;
        DeleteBlob_t m_deleteBlob;
        /**@}*/
    };

    /**
     * Read-only access to operations.
     */
    virtual const Operations &getOperations() const = 0;

    /**
     * Start flushing item modifications which were not executed right
     * away. Item modifications (add/update/delete) can be delayed by
     * returning LOCERR_AGAIN or, when using for example
     * SyncSourceSerialize aka TrackingSyncSource, by returning a "check"
     * function instead of the final result.
     *
     * The sync engine calls this method after processing each incoming
     * SyncML message.
     */
    virtual void flushItemChanges() {}

    /**
     * Called after flush() to ensure that all pending modifications
     * have completed. Called when the engine needs the results.
     *
     * Called by the sync engine when the SyncML peer ran out of new
     * item changes. At that time we would start sending back and forth
     * empty messages, unless we can provide results.
     */
    virtual void finishItemChanges() {}

    /**
     * In some usage scenarios, change tracking is not necessary.
     * This includes local caching (where local data is not expected
     * to changed outside of sync) or item manipulation.
     *
     * The user of a source must explicitly disable change tracking,
     * see SyncSource.
     */
    virtual bool needChanges() { return true; }

    enum ReadAheadOrder {
        READ_ALL_ITEMS,      /** all items as reported by the ReadNextItem operation */
        READ_CHANGED_ITEMS,  /** read updated or added items as reported by the ReadNextItem operation */
        READ_SELECTED_ITEMS, /** read items as given in an explicit list */
        READ_NONE            /** remove previous hint */
    };

    typedef std::vector<std::string> ReadAheadItems;
    /**
     * Provides a hint to the source which items are going to be read
     * next. A source may use this to implement read-ahead. This
     * is just a hint, the source must also work if reads turn out to
     * ask for other items.
     */
    virtual void setReadAheadOrder(ReadAheadOrder order,
                                   const ReadAheadItems &luids = ReadAheadItems())
    { }
    virtual void getReadAheadOrder(ReadAheadOrder &order,
                                   ReadAheadItems &luids)
    {
        order = READ_NONE;
        luids.clear();
    }

 protected:
    struct SynthesisInfo {
        /**
         * name to use for MAKE/PARSETEXTWITHPROFILE,
         * leave empty when acessing the field list directly
         */
        std::string m_profile;

        /**
         * the second parameter for MAKE/PARSETEXTWITHPROFILE
         * which specifies a remote rule to be applied when
         * converting to and from the backend
         */
        std::string m_backendRule;
    
        /** list of supported datatypes in "<use .../>" format */
        std::string m_datatypes;

        /** native datatype (see getNativeDatatypeName()) */
        std::string m_native;

        /** name of the field list used by the datatypes */
        std::string m_fieldlist;

        /**
         * One or more Synthesis script statements, separated
         * and terminated with a semicolon. Can be left empty.
         *
         * If not empty, then these statements are executed directly
         * before converting the current item fields into
         * a single string with MAKETEXTWITHPROFILE() in the sync source's
         * <beforewritescript> (see SyncSourceBase::getDatastoreXML()).
         *
         * This value is currently only used by sync sources which
         * set m_profile.
         */
        std::string m_beforeWriteScript;

        /**
         * Same as m_beforeWriteScript, but used directly after
         * converting a string into fields with PARSETEXTWITHPROFILE()
         * in <afterreadscript>.
         */
        std::string m_afterReadScript;

        /**
         * Arbitrary configuration options, can override the ones above
         * because they are added to the <datastore></datastore>
         * XML configuration directly before the closing element.
         *
         * One example is adding <updateallfields>: this is necessary
         * in backends which depend on getting complete items (= for example,
         * vCard 3.0 strings) from the engine. Note that any source
         * derived from SyncSourceSerialize (= the majority of the backends)
         * have this set by default.
         */
        std::string m_datastoreOptions;

        /**
         * If true, then the StartDataRead call (aka SyncSourceSession::beginSync)
         * is invoked before the first message exchange with the peer. Otherwise
         * it is invoked only if the peer could be reached and accepts the credentials.
         *
         * See SyncSourceSession::beginSync for further comments.
         */
        Bool m_earlyStartDataRead;

        /**
         * If true, then the storage is considered read-only by the
         * engine. All write requests by the peer will be silently
         * discarded. This is necessary for slow syncs, where the peer
         * might send back modified items.
         */
        Bool m_readOnly;

        /**
         * If true, then the storage preserves and supports UID and
         * (in iCalendar 2.0) RECURRENCE-ID with the "globally unique"
         * semantic from iCalendar 2.0 (id assigned once when item is
         * created). If both sides in a sync support this, then the
         * engine can rely on these properties to find matching items
         * during a slow sync.
         *
         * Matches ClientTestConfig::m_sourceKnowsItemSemantic.
         */
        Bool m_globalIDs;
    };

    /**
     * helper function for getDatastoreXML(): fill in information
     * as necessary
     *
     * @retval fragments   the necessary definitions for the other
     *                     return values have to be added here
     */
    virtual void getSynthesisInfo(SynthesisInfo &info,
                                  XMLConfigFragments &fragments) = 0;

    /**
     * utility code: creates Synthesis <use datatype=...>
     * statements, using the predefined vCard21/vCard30/vcalendar10/icalendar20
     * types. Throws an error if no suitable result can be returned (empty or invalid type)
     *
     * @param type         the format specifier as used in SyncEvolution configs, with and without version
     *                     (text/x-vcard:2.1, text/x-vcard, text/x-vcalendar, text/calendar, text/plain, ...);
     *                     see SourceType::m_format
     * @param forceFormat  if true, then don't allow alternative formats (like vCard 3.0 in addition to 2.1);
     *                     see SourceType::m_force
     * @return generated XML fragment
     */
    std::string getDataTypeSupport(const std::string &type,
                                   bool forceFormat);
};

/**
 * SyncEvolution accesses all sources through this interface.
 *
 * Certain functionality is optional or can be implemented in
 * different ways. These methods are accessed through functors
 * (function objects) which may be unset. The expected usage is that
 * derived classes fill in the pieces that they provide by binding the
 * functors to normal methods. For example, TrackingSyncSource
 * provides a normal base class with pure virtual functions which have
 * to be provided by users of that class.
 *
 * Error reporting is done via the Log class.
 */
class SyncSource : virtual public SyncSourceBase, public SyncSourceConfig, public SyncSourceReport
{
 public:
    SyncSource(const SyncSourceParams &params);
    virtual ~SyncSource() {}

    /** true in sources which are not meant to be used, see RegisterSyncSource::InactiveSource() */
    virtual bool isInactive() const { return false; }

    /**
     * SyncSource implementations must register themselves here via
     * RegisterSyncSource
     */
    static SourceRegistry &getSourceRegistry();

    /**
     * SyncSource tests are registered here by the constructor of
     * RegisterSyncSourceTest
     */
    static TestRegistry &getTestRegistry();

    struct Database {
    Database(const string &name, const string &uri, bool isDefault = false, bool isReadOnly = false) :
        m_name( name ), m_uri( uri ), m_isDefault(isDefault), m_isReadOnly(isReadOnly) {}
        string m_name;
        string m_uri;
        bool m_isDefault;
        bool m_isReadOnly;
    };
    typedef vector<Database> Databases;
    
    /**
     * returns a list of all know data sources for the kind of items
     * supported by this sync source
     */
    virtual Databases getDatabases() = 0;

    /**
     * Creates a new database.
     * The default implementation just throws an error.
     *
     * @param database     At least the name should be set. Some backends
     *                     may also be able to create the database with
     *                     a specific URI.
     * @return description of the new database
     */
    virtual Database createDatabase(const Database &database) { throwError(SE_HERE, "creating databases is not supported by backend " + getBackend()); return Database("", ""); }

    /**
     * Removing a database primarily removes the meta data about the
     * database. The data itself may still exist in a trash folder.
     * The enum tells the deleteDatabase() call what the intention of
     * the caller is.
     */
    enum RemoveData {
        REMOVE_DATA_DEFAULT,    /**< do whatever makes most sense for the backend */
        REMOVE_DATA_FORCE,      /**< force immediate purging of the data, fail if not possible */
        REMOVE_DATA_KEEP        /**< keep data, only remove access to it */
    };

    /**
     * Removes a database. To map a "database" property to a uri,
     * instantiate the source with the desired config, open() it and
     * then call getDatabase().
     *
     * @param uri              unique identifier for the database
     * @param removeData       describes what to do about the database content
     */
    virtual void deleteDatabase(const std::string &uri, RemoveData removeData) { throwError(SE_HERE, "deleting databases is not supported by backend " + getBackend()); }

    /**
     * Actually opens the data source specified in the constructor,
     * will throw the normal exceptions if that fails. Should
     * not modify the state of the sync source.
     *
     * The expectation is that this call is fairly light-weight, but
     * does enough checking to determine whether the source is
     * usable. More expensive operations (like determining changes)
     * should be done in the m_startDataRead callback (bound to
     * beginSync() in some of the utility classes).
     *
     * In clients, it will be called for all sources before
     * the sync starts. In servers, it is called for each source once
     * the client asks for it, but not sooner.
     */
    virtual void open() = 0;

    /**
     * Checks whether the source as currently configured can access
     * data. open() must have been called first.
     *
     * The default implementation calls Operations::m_isEmpty; this
     * assumes that m_isEmpty really does something with the
     * database. Derived classes which neither check their config in
     * open() nor during m_isEmpty() should provide their own
     * implementation of isUsable(). It might also be possible to
     * check usability more efficiently.
     *
     * It is also possible to suppress the usability check by
     * providing an implementation which always returns true, for
     * example when checking would be too expensive or lead to
     * unexpected operations (like accessing a remote server).
     *
     * If unusable, the implementation should print an INFO message
     * explaining why the source is unusable. Whether that is an
     * error will be determined by the caller.
     *
     * @return false if the source is definitely unusable, true if it
     * is usable or might be
     */
    virtual bool isUsable();

    /**
     * Returns the actual database that is in use. open() must
     * have been called first.
     *
     * Useful because the "database" property might be empty or
     * be interpreted in different ways by different backends.
     *
     * Needed for deleting databases. Not implemented in all
     * backends. The default implementation returns an empty
     * structure.
     *
     * @return Database structure with at least m_uri set if
     *         the actual database is known.
     */
    Database getDatabase() const { return m_database; }

    /**
     * To be called by derived implementation of open().
     */
    void setDatabase(const Database &database) { m_database = database; }

    /**
     * Read-only access to operations.  Derived classes can modify
     * them via m_operations.
     */
    virtual const Operations &getOperations() const { return m_operations; }

    /**
     * closes the data source so that it can be reopened
     *
     * Just as open() it should not affect the state of
     * the database unless some previous action requires
     * it.
     */
    virtual void close() = 0;

    /**
     * A hint to the source that syncing will stop processing data
     * for a while (freeze = true) or resume processing (freeze =
     * false).
     *
     * If the source needs to change its own state to accomodate
     * the new freeze state of the sync and that change fails, then
     * the source must throw an error in setFreeze(). The caller will
     * not interact with the source while frozen and thus would not
     * notice the failure if no error was thrown.
     */
    virtual void setFreeze(bool freeze) {}

    /**
     * Number of InsertItem operations, regardless whether the
     * operation succeeded or failed. Operations which get suspended
     * are counted again each time they are resumed.
     */
    int32_t getAdded() const { return m_added; }
    int32_t getUpdated() const { return m_updated; }
    int32_t getDeleted() const { return m_deleted; }

    /**
     * return Synthesis API pointer, if one currently is available
     * (between SyncEvolution_Module_CreateContext() and
     * SyncEvolution_Module_DeleteContext())
     */
    virtual SDKInterface *getSynthesisAPI() const;

    /**
     * change the Synthesis API that is used by the source
     */
    void pushSynthesisAPI(sysync::SDK_InterfaceType *synthesisAPI);

    /**
     * remove latest Synthesis API and return to previous one (if any)
     */
    void popSynthesisAPI();

    /**
     * If called while a sync session runs (i.e. after m_startDataRead
     * (aka beginSync()) and before m_endDataWrite (aka endSync())),
     * the engine will finish the session and then immediately try to
     * run another session where any source in which requestAnotherSync()
     * was called is active again. There is no guarantee that this
     * will be possible.
     *
     * The source must be prepared to correctly handle another sync
     * session. m_endDataWrite will be called and then the sequence
     * of calls starts again at m_startDataRead.
     *
     * The sync mode will switch to an incremental sync in the same
     * direction as the initial sync (one-way to client or server,
     * two-way).
     *
     * Does nothing when called at the wrong time. There's no
     * guarantee either that restarting is possible.
     *
     * Currently only supported when a single source is active in
     * the initial sync.
     */
    void requestAnotherSync();

    /**
     * factory function for a SyncSource that provides the
     * source type specified in the params.m_nodes.m_configNode
     *
     * @param error    throw a runtime error describing what the problem is if no matching source is found
     * @param config   optional, needed for intantiating virtual sources
     * @return valid instance, NULL if no source can handle the given type (only when error==false)
     */
    static SyncSource *createSource(const SyncSourceParams &params,
                                    bool error = true,
                                    SyncConfig *config = NULL);

    /**
     * Factory function for a SyncSource with the given name
     * and handling the kind of data specified by "type" (e.g.
     * "Evolution Contacts:text/x-vcard").
     *
     * The source is instantiated with dummy configuration nodes under
     * the pseudo server name "testing". This function is used for
     * testing sync sources, not for real syncs. If the prefix is set,
     * then <prefix>_<name>_1 is used as database, just as in the
     * Client::Sync and Client::Source tests. Otherwise the default
     * database is used.
     *
     * @param error    throw a runtime error describing what the problem is if no matching source is found
     * @return NULL if no source can handle the given type
     */
    static SyncSource *createTestingSource(const string &name, const string &type, bool error,
                                           const char *prefix = getenv("CLIENT_TEST_EVOLUTION_PREFIX"));

    /**
     * Some information about available backends.
     * Multiple lines, formatted for users of the
     * command line.
     */
    static string backendsInfo();
    /**
     * Debug information about backends.
     */
    static string backendsDebug();

    /**
     * Mime type a backend communicates with the remote peer by default,
     * this is used to alert the remote peer in SAN during server alerted sync.
     */
    virtual std::string getPeerMimeType() const =0;

    /* implementation of SyncSourceBase */
    virtual std::string getName() const { return SyncSourceConfig::getName(); }
    virtual std::string getDisplayName() const { return m_name.c_str(); }
    virtual void setDisplayName(const std::string &name) { m_name = name; }
    virtual long getNumDeleted() const { return m_numDeleted; }
    virtual void setNumDeleted(long num) { m_numDeleted = num; }
    virtual void incrementNumDeleted() { m_numDeleted++; }
    virtual bool needChanges() { return m_needChanges; }
    void setNeedChanges(bool needChanges) { m_needChanges = needChanges; }


    /**
     * Set to true in SyncContext::initSAN() when a SyncML server has
     * to force a client into slow sync mode. This is necessary because
     * the server cannot request that mode (missing in the standard).
     * Forcing the slow sync mode is done via a FORCESLOWSYNC() macro
     * call in an <alertscript>.
     */
    void setForceSlowSync(bool forceSlowSync) { m_forceSlowSync = forceSlowSync; }
    bool getForceSlowSync() const { return m_forceSlowSync; }

 protected:
    Operations m_operations;

  private:
    /**
     * Counter for items deleted in the source. Has to be incremented
     * by RemoveAllItems() and DeleteItem(). This counter is used to
     * update the Synthesis engine counter in those cases where the
     * engine does not (refresh from server) or cannot
     * (RemoveAllItems()) count the removals itself.
     */
    long m_numDeleted;

    /**
     * Counter for InsertItem operations. Updated by hooking into the operation.
     */
    int32_t m_added, m_updated, m_deleted;

    bool m_forceSlowSync;

    /**
     * Interface pointer for this sync source, allocated for us by the
     * Synthesis engine and registered here by
     * SyncEvolution_Module_CreateContext(). Only valid until
     * SyncEvolution_Module_DeleteContext(), in other words, while
     * the engine is running.
     */
    std::vector<sysync::SDK_InterfaceType *> m_synthesisAPI;

    /** database in use after open(), to be set via setDatabase() by derived class */
    Database m_database;

    /** actual name of the source */
    std::string m_name;

    /** change tracking enabled? */
    bool m_needChanges;
};

/**
 * A SyncSource with no pure virtual functions.
 */
class DummySyncSource : public SyncSource
{
 public:
    DummySyncSource(const SyncSourceParams &params) :
       SyncSource(params) {}

     DummySyncSource(const std::string &name, const std::string &contextName) :
       SyncSource(SyncSourceParams(name, SyncSourceNodes(), boost::shared_ptr<SyncConfig>(), contextName)) {}

    virtual Databases getDatabases() { return Databases(); }
    virtual void open() {}
    virtual void close() {}
    virtual void getSynthesisInfo(SynthesisInfo &info,
                                  XMLConfigFragments &fragments) {}
    virtual void enableServerMode() {}
    virtual bool serverModeEnabled() const { return false; }
    virtual std::string getPeerMimeType() const {return "";} 
};

/**
 * A special source which combines one or more real sources.
 * Most of the special handling for that is in SyncContext.cpp.
 *
 * This class can be instantiated, opened and closed if and only if
 * the underlying sources also support that.
 */
class VirtualSyncSource : public DummySyncSource 
{
    std::vector< boost::shared_ptr<SyncSource> > m_sources;
    bool isEmpty();

public:
    /**
     * @param config   optional: when given, the constructor will instantiate the
     *                 referenced underlying sources and check them in open()
     */
    VirtualSyncSource(const SyncSourceParams &params, SyncConfig *config = NULL);

    /** opens underlying sources and checks config by calling getDataTypeSupport() */
    virtual void open();
    virtual void close();

    /**
     * returns array with source names that are referenced by this
     * virtual source
     */
    std::vector<std::string> getMappedSources();

    /**
     * returns <use datatype=...> statements for XML config,
     * throws error if not configured correctly
     */
    std::string getDataTypeSupport();
    using SyncSourceBase::getDataTypeSupport;


   /*
    * If any of the sub datasource has no databases associated, return an empty
    * database list to indicate a possibly error condition; otherwise return a
    * dummy database to identify "calendar+todo" combined datasource.
    **/
    virtual Databases getDatabases();
};

/**
 * Hooks up the Synthesis DB Interface start sync (BeginDataRead) and
 * end sync (EndDataWrite) calls with virtual methods. Ensures that
 * sleepSinceModification() is called.
 *
 * Inherit from this class in your sync source and call the init()
 * method to use it.
 */
class SyncSourceSession : virtual public SyncSourceBase {
 public:
    /**
     * called before Synthesis engine starts to ask for changes and item data
     *
     * If SynthesisInfo::m_earlyStartDataRead is true, then this call is
     * invoked before the first message exchange with a peer and it
     * may throw a STATUS_SLOW_SYNC_508 StatusException if an
     * incremental sync is not possible. In that case, preparations
     * for a slow sync must have completed successfully inside the
     * beginSync() call. It is not going to get called again.
     *
     * If SynthesisInfo::m_earlyStartDataRead is false (the default),
     * then this is called only if the peer was reachable and accepted
     * the credentials. This mode of operation is preferred if a fallback
     * to slow sync is not needed, because it allows deferring expensive
     * operations until really needed. For example, the engine does
     * database dumps at the time when StartDataRead is called.
     *
     * See StartDataRead for details.
     *
     * @param lastToken     identifies the last completed sync
     * @param resumeToken   identifies a more recent sync which needs to be resumed;
     *                      if not empty, then report changes made after that sync
     *                      instead of the last completed sync
     */
    virtual void beginSync(const std::string &lastToken, const std::string &resumeToken) = 0;

    /**
     * called after completing or suspending the current sync
     *
     * See EndDataWrite for details.
     *
     * @return a token identifying this sync session for a future beginSync()
     */
    virtual std::string endSync(bool success) = 0;

    /** set Synthesis DB Interface operations */
    void init(SyncSource::Operations &ops);
 private:
    sysync::TSyError startDataRead(const char *lastToken, const char *resumeToken);
    sysync::TSyError endDataWrite(bool success, char **newToken);
};


/**
 * Implements the Synthesis DB Interface for reporting item changes
 * (ReadNextItemAsKey) *without* actually delivering the item data.
 */
class SyncSourceChanges : virtual public SyncSourceBase {
 public:
    SyncSourceChanges();

    enum State {
        ANY,
        NEW,
        UPDATED,
        DELETED,
        MAX
    };

    /**
     * Add the LUID of a NEW/UPDATED/DELETED item.
     * If unspecified, the luid is added to the list of
     * all items. This must be done *in addition* to adding
     * the luid with a specific state.
     *
     * For example, the luid of an updated item should be added with
     * addItem(luid [, ANY]) and again with addItem(luid, DELETED).
     *
     * The Synthesis engine does not need the list of deleted items
     * and does not distinguish between added and updated items, so
     * for syncing, adding DELETED items is optional and all items
     * which are different from the last sync can be added as
     * UPDATED. The client-test program expects that the informationb
     * is provided precisely.
     *
     * @return true if the luid was already listed
     */
    bool addItem(const string &luid, State state = ANY);

    /**
     * Wipe out all added items, returning true if any were found.
     */
    bool reset();

    typedef std::set<std::string> Items_t;
    const Items_t &getItems(State state) { return m_items[state]; }
    const Items_t &getAllItems() const { return m_items[ANY]; }
    const Items_t &getNewItems() const { return m_items[NEW]; }
    const Items_t &getUpdatedItems() const { return m_items[UPDATED]; }
    const Items_t &getDeletedItems() const { return m_items[DELETED]; }

    /** set Synthesis DB Interface operations */
    void init(SyncSource::Operations &ops);

 private:
    Items_t m_items[MAX];
    bool m_first;
    Items_t::const_iterator m_it;

    sysync::TSyError iterate(sysync::ItemID aID,
                             sysync::sInt32 *aStatus,
                             bool aFirst);
};

/**
 * Implements the Synthesis DB Interface for deleting an item
 * (DeleteItem). Increments number of deleted items in
 * SyncSourceBase.
 */
class SyncSourceDelete : virtual public SyncSourceBase {
 public:
    virtual void deleteItem(const string &luid) = 0;

    /** set Synthesis DB Interface operations */
    void init(SyncSource::Operations &ops);

 private:
    sysync::TSyError deleteItemSynthesis(sysync::cItemID aID);
};

enum InsertItemResultState {
    /**
     * Operation not complete, invoke callback in ItemResult to check
     * for progress.
     */
    ITEM_AGAIN,

    /**
     * item added or updated as requested
     */
    ITEM_OKAY,

    /**
     * When a backend is asked to add an item and recognizes
     * that the item matches an already existing item, it may
     * replace that item instead of creating a duplicate. In this
     * case it must return ITEM_REPLACED and set the luid/revision
     * of that updated item.
     *
     * This can happen when such an item was added concurrently to
     * the running sync or, more likely, was reported as new by
     * the backend and the engine failed to find the match because
     * it doesn't know about some special semantic, like iCalendar
     * 2.0 UID).
     *
     * Note that depending on the age of the items, the older data
     * will replace the more recent one when always using item
     * replacement.
     */
    ITEM_REPLACED,

    /**
     * Same as ITEM_REPLACED, except that the backend did some
     * modifications to the data that was sent to it before
     * storing it, like merging it with the existing item. The
     * engine will treat the updated item as modified and send
     * back the update to the peer as soon as possible. In server
     * mode that will be in the same sync session, in a client in
     * the next session (client cannot send changes after having
     * received data from the server).
     */
    ITEM_MERGED,

    /**
     * As before, a match against an existing item was detected.
     * By returning this state and the luid of the matched item
     * (revision not needed) the engine is instructed to do the
     * necessary data comparison and merging itself. Useful when a
     * backend can't do the necessary merging itself.
     */
    ITEM_NEEDS_MERGE
};

/**
 * an interface for reading and writing items in the internal
 * format; see SyncSourceSerialize for an explanation
 */
class SyncSourceRaw : virtual public SyncSourceBase {
 public:
    class InsertItemResult {
    public:
        InsertItemResult() :
            m_state(ITEM_OKAY)
            {}
        
        /**
         * @param luid      the LUID after the operation; during an update the LUID must
         *                  not be changed, so return the original one here
         * @param revision  the revision string after the operation; leave empty if not used
         * @param state     report about what was done with the data
         */
        InsertItemResult(const string &luid,
                         const string &revision,
                         InsertItemResultState state) :
        m_luid(luid),
            m_revision(revision),
            m_state(state)
            {}

        /**
         * Constructor for the case where the final result is not available yet.
         *
         * @param check   will be called again later to poll for completion
         */
        InsertItemResult(const boost::function<InsertItemResult ()> &check) :
        m_state(ITEM_AGAIN),
            m_continue(check)
        {}

        string m_luid;
        string m_revision;
        InsertItemResultState m_state;
        typedef ContinueOperation<InsertItemResult ()> Continue_t;
        Continue_t m_continue;
    };

    /** same as SyncSourceSerialize::insertItem(), but with internal format */
    virtual InsertItemResult insertItemRaw(const std::string &luid, const std::string &item) = 0;

    /** same as SyncSourceSerialize::readItem(), but with internal format */
    virtual void readItemRaw(const std::string &luid, std::string &item) = 0;
};

/**
 * Implements the Synthesis DB Interface for importing/exporting item
 * data (ReadItemAsKey, InsertItemAsKey, UpdateItemAsKey) in such a
 * way that the sync source only has to deal with a text
 * representation of an item.
 *
 * There may be two such representations:
 * - "engine format" is the one exchanged with the Synthesis engine
 * - "internal or raw format" is a format that might better capture
 *   the internal representation and can be used for backup/restore
 *   and testing
 *
 * To give an example, the EvolutionMemoSource uses plain text as
 * engine format and iCalendar 2.0 as raw format.
 *
 * The BackupData_t and RestoreData_t operations are implemented by
 * this class using the internal format.
 *
 * The engine format must be something that the Synthesis engine can
 * parse and generate, in other words, there must be a corresponding
 * profile in the XML configuration. This class uses information
 * provided by the sync source (mime type and version) and from the
 * configuration (format selected by user) to generate the required
 * XML configuration parts for common configurations (vCard,
 * vCalendar, iCalendar, text). Special representations can be added
 * to the global XML configuration by overriding default
 * implementations provided in this class.
 *
 * InsertItemAsKey and UpdateItemAsKey are mapped to the same
 * insertItem() call because in practice it can happen that a request
 * to add an item must be turned into an update. For example, a
 * meeting was imported both into the server and the client. A request
 * to add the item again should be treated as an update, based on the
 * unique iCalendar 2.0 LUID.
 */
class SyncSourceSerialize : virtual public SyncSourceBase, virtual public SyncSourceRaw {
 public:
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

    /**
     * returns the backend selection and configuration
     */
    virtual InitState<SourceType> getSourceType() const = 0;

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
     * @param item     contains the new content of the item, using the engine format
     * @return the result of inserting the item
     */
    virtual InsertItemResult insertItem(const std::string &luid, const std::string &item) = 0;

    /**
     * Return item data in engine format.
     *
     * @param luid     identifies the item
     * @retval item    item data
     */
    virtual void readItem(const std::string &luid, std::string &item) = 0;

    /* implement SyncSourceRaw under the assumption that the internal and engine format are identical */
    virtual InsertItemResult insertItemRaw(const std::string &luid, const std::string &item);
    virtual void readItemRaw(const std::string &luid, std::string &item);

    /** set Synthesis DB Interface operations */
    void init(SyncSource::Operations &ops);

 protected:
    /**
     * used getMimeType(), getMimeVersion() and getSourceType()
     * to provide the information necessary for automatic
     * conversion to the sync source's internal item representation
     */
    virtual void getSynthesisInfo(SynthesisInfo &info,
                                  XMLConfigFragments &fragments);
 private:
    sysync::TSyError readItemAsKey(sysync::cItemID aID, sysync::KeyH aItemKey);
    SyncSource::Operations::InsertItemAsKeyResult_t insertItemAsKey(sysync::KeyH aItemKey, sysync::ItemID newID);
    SyncSource::Operations::UpdateItemAsKeyResult_t updateItemAsKey(sysync::KeyH aItemKey, sysync::cItemID aID, sysync::ItemID newID);
    sysync::TSyError insertContinue(sysync::ItemID newID, const InsertItemResult::Continue_t &cont);
};

/**
 * Mapping from Hash() value to file.
 * Used by SyncSourceRevisions, but may be of use for
 * other backup implementations.
 */
class ItemCache
{
public:
#ifdef USE_SHA256
    typedef std::string Hash_t;
    Hash_t hashFunc(const std::string &data) { return SHA_256(data); }
#else
    typedef unsigned long Hash_t;
    Hash_t hashFunc(const std::string &data) { return Hash(data); }
#endif
    typedef unsigned long Counter_t;

    /** mark the algorithm used for the hash via different suffices */
    static const char *m_hashSuffix;

    /**
     * Collect information about stored hashes. Provides
     * access to file name via hash.
     *
     * If no hashes were written (as in an old SyncEvoltion
     * version), we could read the files to recreate the
     * hashes. This is not done because it won't occur
     * often enough.
     *
     * Hashes are also not verified. Users should better
     * not edit them or file contents...
     *
     * @param oldBackup     existing backup to read; may be empty
     * @param newBackup     new backup to be created
     * @param legacy        legacy mode includes a bug
     *                      which cannot be fixed without breaking on-disk format
     */
    void init(const SyncSource::Operations::ConstBackupInfo &oldBackup,
              const SyncSource::Operations::BackupInfo &newBackup,
              bool legacy);

    /**
     * create file name for a specific hash, empty if no such hash
     */
    string getFilename(Hash_t hash);

    /**
     * add a new item, reusing old one if possible
     *
     * @param item       new item data
     * @param uid        its unique ID
     * @param rev        revision string
     */
    void backupItem(const std::string &item,
                    const std::string &uid,
                    const std::string &rev);

    /** to be called after init() and all backupItem() calls */
    void finalize(BackupReport &report);

    /** can be used to restart creating the backup after an intermediate failure */
    void reset();

private:
    typedef std::map<Hash_t, Counter_t> Map_t;
    Map_t m_hash2counter;
    string m_dirname;
    SyncSource::Operations::BackupInfo m_backup;
    bool m_legacy;
    unsigned long m_counter;
};

/**
 * Implements change tracking based on a "revision" string, a string
 * which is guaranteed to change automatically each time an item is
 * modified. Backup/restore is optionally implemented by this class if
 * pointers to SyncSourceRaw and SyncSourceDelete interfaces are
 * passed to init(). For backup only the former is needed, for restore
 * both.
 *
 * Potential implementations of the revision string are:
 * - a modification time stamp
 * - a hash value of a textual representation of the item
 *   (beware, such a hash might change as the textual representation
 *    changes even though the item is unchanged)
 *
 * Sync sources which want to use this functionality have to provide
 * the following functionality by implementing the pure virtual
 * functions below:
 * - enumerate all existing items
 * - provide LUID and the revision string
 *   The LUID must remain *constant* when making changes to an item,
 *   whereas the revision string must *change* each time the item is
 *   changed by anyone.
 *   Both can be arbitrary strings, but keeping them simple (printable
 *   ASCII, no white spaces, no equal sign) makes debugging simpler
 *   because they can be stored as they are as key/value pairs in the
 *   sync source's change tracking config node (the .other.ini files when
 *   using file-based configuration). More complex strings use escape
 *   sequences introduced with an exclamation mark for unsafe characters.
 *
 * Most of the functionality of this class must be activated
 * explicitly as part of the life cycle of the sync source instance by
 * calling detectChanges(), updateRevision() and deleteRevision().
 *
 * If the required interfaces are provided to init(), then backup/restore
 * operations are set. init() also hooks into the session life cycle
 * with an end callback that ensures that enough time passes at the end
 * of the sync. This is important for sync sources which use time stamps
 * as revision string. "enough time" is defined by a parameter to the
 * init call.
 */
class SyncSourceRevisions : virtual public SyncSourceChanges, virtual public SyncSourceBase {
 public:
    typedef map<string, string> RevisionMap_t;

    /**
     * Fills the complete mapping from UID to revision string of all
     * currently existing items.
     *
     * Usually both UID and revision string must be non-empty. The
     * only exception is a refresh-from-client: in that case the
     * revision string may be empty. The implementor of this call
     * cannot know whether empty strings are allowed, therefore it
     * should not throw errors when it cannot create a non-empty
     * string. The caller of this method will detect situations where
     * a non-empty string is necessary and none was provided.
     *
     * An source must set the revision string for all items or
     * none at all.
     *
     * This call is typically only invoked only once during the
     * lifetime of a source, at the time when detectChanges() needs
     * the information. The result returned in that invocation is
     * used throught the session.
     *
     * When detectChanges() is called with CHANGES_NONE, listAllItems()
     * is avoided. Instead the cached information is used. Sources
     * may need to know that information, so in this case setAllItems()
     * is called as part of detectChanges().
     */
    virtual void listAllItems(RevisionMap_t &revisions) = 0;

    /**
     * Called by SyncSourceRevisions::detectChanges() to tell
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
     * Tells detectChanges() how to do its job.
     */
    enum ChangeMode {
        /**
         * Call listAllItems() and use the list of previous items
         * to calculate changes.
         */
        CHANGES_FULL,

        /**
         * Don't rely on previous information. Will call
         * listAllItems() and generate a full list of items based on
         * the result.
         */
        CHANGES_SLOW,

        /**
         * Caller has already determined that a) no items have changed
         * and that b) the list of previous items is valid. For example,
         * some backends have a way of getting a revision string for
         * the whole database and can compare that against the value
         * from the end of the previous sync.
         *
         * In this mode, listAllItems() doesn't have to be called.
         * A list of all items will be created, with no items marked
         * as added/updated/deleted.
         */
        CHANGES_NONE
    };

    /**
     * calculate changes, call when sync source is ready for
     * listAllItems() and before changes are needed
     *
     * The trackingNode must be provided by the caller. It will
     * be updated by each of the calls and must be stored by
     * the caller.
     *
     * @param trackingNode     a config node for exclusive use by this class
     * @param mode             determines how changes are detected; if unsure,
     *                         use CHANGES_FULL, which will always produce
     *                         the required information, albeit more slowly
     *                         than the other modes
     * @return true if change detection could only provide a list of currently
     *         existing items, but not the list of added/updated/deleted items
     */
    bool detectChanges(ConfigNode &trackingNode, ChangeMode mode);

    /**
     * record that an item was added or updated
     *
     * @param old_luid         empty for add, old LUID otherwise
     * @param new_luid         normally LUIDs must not change, but this call allows it
     * @param revision         revision string after change
     */
    void updateRevision(ConfigNode &trackingNode,
                        const std::string &old_luid,
                        const std::string &new_luid,
                        const std::string &revision);

    /**
     * record that we deleted an item
     *
     * @param luid        the obsolete LUID
     */
    void deleteRevision(ConfigNode &trackingNode,
                        const std::string &luid);

    /**
     * set Synthesis DB Interface and backup/restore operations
     * @param raw           needed for backups; if NULL, no backups are made
     * @param del           needed for restores; if NULL, only backups are possible
     * @param granularity   time that has to pass between making a modification
     *                      and checking for changes; this class ensures that
     *                      at least this amount of time has passed before letting
     *                      the session terminate. Delays in different source do
     *                      not add up.
     */
    void init(SyncSourceRaw *raw, SyncSourceDelete *del,
              int granularity,
              SyncSource::Operations &ops);

 private:
    SyncSourceRaw *m_raw;
    SyncSourceDelete *m_del;
    int m_revisionAccuracySeconds;

    /** buffers the result of the initial listAllItems() call */
    RevisionMap_t m_revisions;
    bool m_revisionsSet;
    bool m_firstCycle;
    void initRevisions();

    /**
     * Dump all data from source unmodified into the given directory.
     * The ConfigNode can be used to store meta information needed for
     * restoring that state. Both directory and node are empty.
     */
    void backupData(const SyncSource::Operations::ConstBackupInfo &oldBackup,
                    const SyncSource::Operations::BackupInfo &newBackup,
                    BackupReport &report);

    /**
     * Restore database from data stored in backupData(). Will be
     * called inside open()/close() pair. beginSync() is *not* called.
     */
    void restoreData(const SyncSource::Operations::ConstBackupInfo &oldBackup,
                     bool dryrun,
                     SyncSourceReport &report);

    /**
     * Increments the time stamp of the latest database modification,
     * called automatically whenever revisions change.
     */
    void databaseModified();

    /** time stamp of latest database modification, for sleepSinceModification() */
    Timespec m_modTimeStamp;
    SyncMLStatus sleepSinceModification();
};


/**
 * Common logging for sync sources.
 *
 * This class wraps the Synthesis DB functors that were set before
 * calling its init() method. The wrappers then log a single line
 * describing what is happening (adding/updating/removing)
 * to which item (with a short item specific description extracted
 * from the incoming item data or the backend).
 */
class SyncSourceLogging : public virtual SyncSourceBase
{
 public:
    /**
     * wrap Synthesis DB Interface operations
     *
     * @param fields     list of fields to read in getDescription()
     * @param sep        separator between non-empty fields
     */
    void init(const std::list<std::string> &fields,
              const std::string &sep,
              SyncSource::Operations &ops);

    /**
     * Extract short description from Synthesis item data.
     * The default implementation reads a list of fields
     * as strings and concatenates the non-empty ones
     * with a separator.
     *
     * @param aItemKey     key for reading fields
     * @return description, empty string will cause the ID of the item to be printed
     */
    virtual std::string getDescription(sysync::KeyH aItemKey);

    /**
     * Extract short description from backend.
     * Necessary for deleted items. The default implementation
     * returns an empty string, so that implementing this
     * is optional.
     *
     * @param luid          LUID of the item to be deleted in the backend
     * @return description, empty string will cause the ID of the item to be printed
     */
    virtual std::string getDescription(const string &luid);

 private:
    std::list<std::string> m_fields;
    std::string m_sep;

    SyncMLStatus insertItemAsKey(sysync::KeyH aItemKey, sysync::ItemID newID);
    SyncMLStatus updateItemAsKey(sysync::KeyH aItemKey, sysync::cItemID aID, sysync::ItemID newID);
    SyncMLStatus deleteItem(sysync::cItemID aID);
};

/**
 * Implements Load/SaveAdminData and MapItem handling in a SyncML
 * server. Uses a single property for the admin data in the "internal"
 * node and a complete node for the map items.
 */
class SyncSourceAdmin : public virtual SyncSourceBase
{
    boost::shared_ptr<ConfigNode> m_configNode;
    std::string m_adminPropertyName;
    boost::shared_ptr<ConfigNode> m_mappingNode;
    bool m_mappingLoaded;

    ConfigProps m_mapping;
    ConfigProps::const_iterator m_mappingIterator;

    sysync::TSyError loadAdminData(const char *aLocDB,
                                   const char *aRemDB,
                                   char **adminData);
    sysync::TSyError saveAdminData(const char *adminData);
    bool readNextMapItem(sysync::MapID mID, bool aFirst);
    sysync::TSyError insertMapItem(sysync::cMapID mID);
    sysync::TSyError updateMapItem(sysync::cMapID mID);
    sysync::TSyError deleteMapItem(sysync::cMapID mID);
    SyncMLStatus flush();

    void resetMap();
    void mapid2entry(sysync::cMapID mID, string &key, string &value);
    void entry2mapid(const string &key, const string &value, sysync::MapID mID);

 public:
    /** flexible initialization */
    void init(SyncSource::Operations &ops,
              const boost::shared_ptr<ConfigNode> &config,
              const std::string adminPropertyName,
              const boost::shared_ptr<ConfigNode> &mapping);

    /**
     * simpler initialization, using the default placement of data
     * inside the SyncSourceConfig base class
     */
    void init(SyncSource::Operations &ops, SyncSource *source);
};

/**
 * Implements Read/Write/DeleteBlob. Blobs are stored inside a
 * configurable directory, which has to be unique for the current
 * peer.
 */
class SyncSourceBlob : public virtual SyncSourceBase
{
    /**
     * Only one blob is active at a time.
     * This utility class provides the actual implementation.
     */
    sysync::TBlob m_blob;

    sysync::TSyError readBlob(sysync::cItemID aID, const char *aBlobID,
                              void **aBlkPtr, size_t *aBlkSize,
                              size_t *aTotSize,
                              bool aFirst, bool *aLast) {
        // Translate between sysync::memSize and size_t, which
        // is different on s390 (or at least the compiler complains...).
        sysync::memSize blksize = aBlkSize ? static_cast<sysync::memSize>(*aBlkSize) : 0,
            totsize = aTotSize ? static_cast<sysync::memSize>(*aTotSize) : 0;
        sysync::TSyError err = m_blob.ReadBlob(aID, aBlobID, aBlkPtr,
                                               &blksize,
                                               &totsize,
                                               aFirst, aLast);
        if (aBlkSize) {
            *aBlkSize = blksize;
        }
        if (aTotSize) {
            *aTotSize = totsize;
        }
        return err;
    }
    sysync::TSyError writeBlob(sysync::cItemID aID, const char *aBlobID,
                               void *aBlkPtr, size_t aBlkSize,
                               size_t aTotSize,
                               bool aFirst, bool aLast) {
        mkdir_p(m_blob.getBlobPath());
        return m_blob.WriteBlob(aID, aBlobID, aBlkPtr, aBlkSize, aTotSize, aFirst, aLast);
    }
    sysync::TSyError deleteBlob(sysync::cItemID aID, const char *aBlobID) {
        return m_blob.DeleteBlob(aID, aBlobID);
    }

    sysync::TSyError loadAdminData(sysync::cItemID aID, const char *aBlobID,
                                   void **aBlkPtr, size_t *aBlkSize, size_t *aTotSize,
                                   bool aFirst, bool *aLast);

 public:
    void init(SyncSource::Operations &ops,
              const std::string &dir);
};


/**
 * This is an interface definition that is expected by the client-test
 * program. Part of the reason for this requirement is that the test
 * program was originally written for the Funambol SyncSource API.
 * The other reason is that the testing is based on importing/exporting
 * items in the internal format of the sync source, which has to be
 * text based or even MIMEDIR based (for tests involving synccompare).
 */
class TestingSyncSource : public SyncSource,
    virtual public SyncSourceSession,
    virtual public SyncSourceChanges,
    virtual public SyncSourceDelete,
    virtual public SyncSourceSerialize {
 public:
    TestingSyncSource(const SyncSourceParams &params) :
       SyncSource(params)
    {
        SyncSourceSession::init(m_operations);
        SyncSourceChanges::init(m_operations);
        SyncSourceDelete::init(m_operations);
        SyncSourceSerialize::init(m_operations);
    }
    ~TestingSyncSource() {}

    virtual InitState<SourceType> getSourceType() const { return SyncSourceConfig::getSourceType(); }

    virtual void removeAllItems();
};


SE_END_CXX
#endif // INCL_SYNCSOURCE
