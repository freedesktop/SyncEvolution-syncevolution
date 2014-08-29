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

#ifndef INCL_EVOLUTIONSYNCCLIENT
#define INCL_EVOLUTIONSYNCCLIENT

#include <syncevo/SmartPtr.h>
#include <syncevo/SyncConfig.h>
#include <syncevo/SyncML.h>
#include <syncevo/SynthesisEngine.h>
#include <syncevo/UserInterface.h>

#include <string>
#include <set>
#include <map>
#include <stdint.h>

#include <boost/smart_ptr.hpp>
#include <boost/signals2.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class TransportAgent;
class SourceList;
class SyncSource;
class SyncSourceEvent;

/**
 * This is the main class inside SyncEvolution which
 * looks at the configuration, activates all enabled
 * sources and executes the synchronization.
 *
 * All interaction with the user (reporting progress, asking for
 * passwords, ...) is done via virtual methods. The default
 * implementation of those uses stdin/out.
 *
 */
class SyncContext : public SyncConfig {
    /**
     * the string used to request a config,
     * *not* the normalized config name itself;
     * for that use SyncConfig::getConfigName()
     */
    const string m_server;

    bool m_doLogging;
    bool m_quiet;
    bool m_dryrun;

    enum SyncFreeze {
        SYNC_FREEZE_NONE,
        SYNC_FREEZE_RUNNING,
        SYNC_FREEZE_FROZEN
    } m_syncFreeze;
    static const char *SyncFreezeName(SyncFreeze syncFreeze);
    bool m_localSync;
    string m_localPeerContext; /**< context name (including @) if doing local sync */
    string m_localClientRootPath;
    bool m_serverMode;
    bool m_serverAlerted;      /**< sync was initiated by server (applies to client and server mode) */
    bool m_configNeeded;
    std::string m_sessionID;
    SharedBuffer m_initialMessage;
    string m_initialMessageType;
    string m_syncDeviceID;

    FullProps m_configFilters;
    
    boost::shared_ptr<TransportAgent> m_agent;
    boost::shared_ptr<UserInterface> m_userInterface;

    /**
     * a pointer to the active SourceList instance for this context if one exists
     */
    SourceList *m_sourceListPtr;

    /**
     * a pointer to the active SyncContext instance if one exists;
     * set by sync() and/or SwapContext
     */
    static SyncContext *m_activeContext;
    class SwapContext {
        SyncContext *m_oldContext;
    public:
        SwapContext(SyncContext *newContext) :
            m_oldContext(SyncContext::m_activeContext) {
            SyncContext::m_activeContext = newContext;
        }
        ~SwapContext() {
            SyncContext::m_activeContext = m_oldContext;
        }
    };

    /**
     * Connection to the Synthesis engine. Always valid in a
     * constructed SyncContext. Use getEngine() to reference
     * it.
     */
    SharedEngine m_engine;

    /**
     * Synthesis session handle. Only valid while sync is running.
     */
    SharedSession m_session;

    /**
     * installs session in SyncContext and removes it again
     * when going out of scope
     */
    class SessionSentinel {
        SyncContext &m_client;
    public:
        SessionSentinel(SyncContext &client, SharedSession &session) :
        m_client(client) {
            m_client.m_session = session;
        }
        ~SessionSentinel() {
            m_client.m_session.reset();
        }
    };

    /*
     * The URL this SyncContext is actually using, since we may support multiple
     * urls in the configuration.
     * */
    string m_usedSyncURL;

    /* True iff current sync session was triggered by us
     * (such as in server alerted sync).
     */
    bool m_remoteInitiated;
  public:
    /**
     * Common initialization code which needs to be done once
     * at the start of main() in any application using SyncEvolution.
     * For example, initializes (if applicable) glib and EDS.
     *
     * @param appname     static string, must remain valid, defines name of executable (see g_set_prgname())
     */
    static void initMain(const char *appname);

    /**
     * A signal invoked as part of initMain().
     * Backends can connect to it to extend initMain().
     */
    typedef boost::signals2::signal<void (const char *appname)> InitMainSignal;
    static InitMainSignal &GetInitMainSignal();

    /**
     * A signal invoked each time a source has gone through a sync cycle.
     */
    typedef boost::signals2::signal<void (const std::string &name, const SyncSourceReport &source)> SourceSyncedSignal;
    SourceSyncedSignal m_sourceSyncedSignal;

    /**
     * true if binary was compiled as stable release
     * (see gen-autotools.sh)
     */
    static bool isStableRelease();

    /**
     * override stable release mode (for testing purposes)
     */
    static void setStableRelease(bool isStableRelease);

    /**
     * SyncContext using a volatile config
     * and no logging.
     */
    SyncContext();

    /**
     * Constructor for syncing with a SyncML peer.
     *
     * @param peer       identifies the client or server config to be used
     * @param doLogging  write additional log and datatbase files about the sync;
     *                   true for regular syncs, false for debugging
     */
    SyncContext(const string &server,
                bool doLogging = false);

    /**
     * Constructor for client in a local sync.
     *
     * @param client     identifies the client context to be used (@foobar)
     * @param server     identifies the server peer (foo@bar)
     * @param rootPath   use this directory as config directory for the
     *                   peer-specific files (located inside peer directory
     *                   of server config)
     * @param agent      transport agent, ready for communication with server
     * @param doLogging  write additional log and datatbase files about the sync
     */
    SyncContext(const string &client,
                const string &server,
                const string &rootPath,
                const boost::shared_ptr<TransportAgent> &agent,
                bool doLogging = false);

    virtual ~SyncContext();

    bool getQuiet() { return m_quiet; }
    void setQuiet(bool quiet) { m_quiet = quiet; }

    bool getDryRun() { return m_dryrun; }
    void setDryRun(bool dryrun) { m_dryrun = dryrun; }

    bool isLocalSync() const { return m_localSync; }

    bool isServerAlerted() const { return m_serverAlerted; }
    void setServerAlerted(bool serverAlerted) { m_serverAlerted = serverAlerted; }

    boost::shared_ptr<UserInterface> getUserInterface() { return m_userInterface; }
    void setUserInterface(const boost::shared_ptr<UserInterface> &userInterface) { m_userInterface = userInterface; }

    /** use config UI owned by caller, without reference counting */
    void setUserInterface(UserInterface *userInterface) { m_userInterface = boost::shared_ptr<UserInterface>(userInterface, NopDestructor()); }

    /**
     * In contrast to getUserInterface(), this call here never returns NULL.
     * If no UserInterface is currently set, then it returns
     * a reference to a dummy instance which doesn't do anything.
     */
    UserInterface &getUserInterfaceNonNull();

    /**
     * Running operations typically checks that a config really exists
     * on disk. Setting false disables the check.
     */
    bool isConfigNeeded() const { return m_configNeeded; }
    void setConfigNeeded(bool configNeeded) { m_configNeeded = configNeeded; }

    /**
     * throws error if config is needed and not available
     *
     * @param operation   a noun describing what is to be done next ("proceed with %s", operation)
     */
    void checkConfig(const std::string &operation) const;

    /**
     * Sets configuration filters. Currently only used in local sync
     * to configure the sync client.
     */
    void setConfigProps(const FullProps &props) { m_configFilters = props; }
    const FullProps &getConfigProps() const { return m_configFilters; }

    /** only for server: device ID of peer */
    void setSyncDeviceID(const std::string &deviceID) { m_syncDeviceID = deviceID; }
    std::string getSyncDeviceID() const { return m_syncDeviceID; }

    /*
     * Use sendSAN as the first step is sync() if this is a server alerted sync.
     * Prepare the san package and send the SAN request to the peer.
     * Returns false if failed to get a valid client sync request
     * otherwise put the client sync request into m_initialMessage which will
     * be used to initalze the server via initServer(), then continue sync() to
     * start the real sync serssion.
     * @version indicates the SAN protocal version used (1.2 or 1.1/1.0)
     */
    bool sendSAN(uint16_t version);

    /**
     * Initializes the session so that it runs as SyncML server once
     * sync() is called. For this to work the first client message
     * must be available already.
     *
     * @param sessionID    session ID to be used by server
     * @param data         content of initial message sent by the client
     * @param messageType  content type set by the client
     */
    void initServer(const std::string &sessionID,
                    SharedBuffer data,
                    const std::string &messageType);

    /**
     * Executes the sync, throws an exception in case of failure.
     * Handles automatic backups and report generation.
     *
     * @retval complete sync report, skipped if NULL
     * @return overall sync status, for individual sources see report
     */
    SyncMLStatus sync(SyncReport *report = NULL);

    /** result of analyzeSyncMLMessage() */
    struct SyncMLMessageInfo {
        std::string m_deviceID;

        /** a string representation of the whole structure for debugging */
        std::string toString() { return std::string("deviceID ") + m_deviceID; }
    };

    /**
     * Instead or executing a sync, analyze the initial message
     * without changing any local data. Returns once the LocURI =
     * device ID of the client is known.
     *
     * @return device ID, empty if not in data
     */
    static SyncMLMessageInfo
        analyzeSyncMLMessage(const char *data, size_t len,
                             const std::string &messageType);

    /**
     * Convenience function, to be called inside a catch() block of
     * (or for) the sync.
     *
     * Rethrows the exception to determine what it is, then logs it
     * as an error and returns a suitable error code (usually a general
     * STATUS_DATASTORE_FAILURE).
     */
    SyncMLStatus handleException();

    /**
     * Determines the log directory of the previous sync (either in
     * temp or logdir) and shows changes since then.
     */
    void status();

    enum RestoreDatabase {
        DATABASE_BEFORE_SYNC,
        DATABASE_AFTER_SYNC
    };

    /**
     * Restore data of selected sources from before or after the given
     * sync session, identified by absolute path to the log dir.
     */
    void restore(const string &dirname, RestoreDatabase database);

    /**
     * fills vector with absolute path to information about previous
     * sync sessions, oldest one first
     */
    void getSessions(vector<string> &dirs);

    /**
     * fills report with information about previous session
     * @return the peer name from the dir.
     */
    string readSessionInfo(const string &dir, SyncReport &report);

    /**
     * fills report with information about local changes
     *
     * Only sync sources selected in the SyncContext
     * constructor are checked. The local item changes will be set in
     * the SyncReport's ITEM_LOCAL ITEM_ADDED/UPDATED/REMOVED.
     *
     * Some sync sources might not be able to report this
     * information outside of a regular sync, in which case
     * these fields are set to -1. 
     *
     * Start and end times of the check are also reported.
     */
    void checkStatus(SyncReport &report);

    /**
     * When using Evolution this function starts a background thread
     * which drives the default event loop. Without that loop
     * "backend-died" signals are not delivered. The problem with
     * the thread is that it seems to interfere with gconf startup
     * when added to the main() function of syncevolution. Therefore
     * it is started by SyncSource::beginSync() (for unit
     * testing of sync sources) and SyncContext::sync() (for
     * normal operation).
     */
    static void startLoopThread();

    /**
     * Finds activated sync source by name. May return  NULL
     * if no such sync source was defined or is not currently
     * instantiated. Pointer remains valid throughout the sync
     * session. Called by Synthesis DB plugin to find active
     * sources.
     *
     * @param name     can be both <SyncSource::getName()> as well as <prefix>_<SyncSource::getName()>
     *                 (necessary when renaming sources in the Synthesis XML config)
     *
     * @TODO: roll SourceList into SyncContext and
     * make this non-static
     */
    static SyncSource *findSource(const std::string &name);
    static const char m_findSourceSeparator = '@';

    /**
     * Find the active sync context for the given session.
     *
     * @param sessionName      chosen by SyncEvolution and passed to
     *                         Synthesis engine, which calls us back
     *                         with it in SyncEvolution_Session_CreateContext()
     * @return context or NULL if not found
     */
    static SyncContext *findContext(const char *sessionName);

    SharedEngine getEngine() { return m_engine; }
    const SharedEngine getEngine() const { return m_engine; }

    bool getDoLogging() { return m_doLogging; }

    /**
     * Returns the string used to select the peer config
     * used by this instance.
     *
     * Note that this is not the same as a valid configuration
     * name. For example "foo" might be matched against a
     * "foo@bar" config by SyncConfig. Use SyncConfig::getConfigName()
     * to get the underlying config.
     */
    std::string getPeer() { return m_server; }

    /**
     * Handle for active session, may be NULL.
     */
    SharedSession getSession() { return m_session; }

    bool getRemoteInitiated() {return m_remoteInitiated;}
    void setRemoteInitiated(bool remote) {m_remoteInitiated = remote;}

    /**
     * If called while a sync session runs,
     * the engine will finish the session and then
     * immediately try to run another one with
     * the same sources.
     *
     * Does nothing when called at the wrong time.
     * There's no guarantee either that restarting is
     * possible.
     */
    static void requestAnotherSync();

    /**
     * If called while a sync runs, it will change the state of that
     * sync. A frozen sync can only be unfrozen (via setFreeze(false))
     * or suspended/aborted (via signals).
     *
     * @return true if there was a running sync, false otherwise
     */
    bool setFreeze(bool freeze);

    /**
     * access to current set of sync sources, NULL if not instantiated yet
     */
    const std::vector<SyncSource *> *getSources() const;

  protected:
    /** exchange active Synthesis engine */
    SharedEngine swapEngine(SharedEngine newengine) {
        SharedEngine oldengine = m_engine;
        m_engine = newengine;
        return oldengine;
    }

    /** sentinel class which creates, installs and removes a new
        Synthesis engine for the duration of its own life time */
    class SwapEngine {
        SyncContext &m_client;
        SharedEngine m_oldengine;

    public:
        SwapEngine(SyncContext &client) :
        m_client(client) {
            SharedEngine syncengine(m_client.createEngine());
            m_oldengine = m_client.swapEngine(syncengine);
        }

        ~SwapEngine() {
            m_client.swapEngine(m_oldengine);
        }
    };

    /**
     * Create a Synthesis engine for the currently active
     * sources (might be empty!) and settings.
     */
    SharedEngine createEngine();

    /**
     * Return skeleton Synthesis client XML configuration.
     *
     * The <scripting/>, <datatypes/>, <clientorserver/> elements (if
     * present) are replaced by the caller with fragments found in the
     * file system. When <datatypes> already has content, that content
     * may contain <fieldlists/>, <profiles/>, <datatypedefs/>, which
     * will be replaced by definitions gathered from backends.
     *
     * The default implementation of this function takes the configuration from
     * (in this order):
     * - $(XDG_CONFIG_HOME)/syncevolution-xml
     * - $(datadir)/syncevolution/xml
     * Files with identical names are read from the first location where they
     * are found. If $(SYNCEVOLUTION_XML_CONFIG_DIR) is set, then it overrides
     * the previous two locations.
     *
     * The syncevolution.xml file is read from the first place where it is found.
     * In addition, further .xml files in sub-directories are gathered and get
     * inserted into the syncevolution.xml template.
     *
     * If none of these locations has XML configs, then builtin strings are
     * used as fallback. This only works for mode == "client". Otherwise an
     * error is thrown.
     *
     * @param mode         "client" or "server"
     * @retval xml         is filled with Synthesis client config which may hav <datastore/>
     * @retval rules       remote rules which the caller needs for <clientorserver/>
     * @retval configname  a string describing where the config came from
     */
    virtual void getConfigTemplateXML(const string &mode,
                                      string &xml,
                                      string &rules,
                                      string &configname);
                                      

    /**
     * Return complete Synthesis XML configuration.
     *
     * Calls getConfigTemplateXML(), then fills in
     * sync source XML fragments if necessary.
     *
     * @param isSync       the XML config will be used for the final engine used for syncing, not just logging
     * @retval xml         is filled with complete Synthesis client config
     * @retval configname  a string describing where the config came from
     */
    virtual void getConfigXML(bool isSync, string &xml, string &configname);

    /**
     * Callback for derived classes: called after initializing the
     * client, but before doing anything with its configuration.
     * Can be used to override the client configuration.
     */
    virtual void prepare() {}

    /**
     * instantiate transport agent
     *
     * Called by engine when it needs to exchange messages.  The
     * transport agent will be used throughout the sync session and
     * unref'ed when no longer needed. At most one agent will be
     * requested at a time. The transport agent is intentionally
     * returned as a Boost shared pointer so that a pointer to a
     * class with a different life cycle is possible, either by
     * keeping a reference or by returning a shared_ptr where the
     * destructor doesn't do anything.
     *
     * The agent must be ready for use:
     * - HTTP specific settings must have been applied
     * - the current SyncContext's timeout must have been
     *   installed via TransportAgent::setTimeout()
     *
     * The default implementation instantiates one of the builtin
     * transport agents, depending on how it was compiled.
     *
     * @param gmainloop    the GMainLoop to be used by transports, if not NULL;
     *                     transports not supporting that should not be created;
     *                     transports will increase the reference count for the loop
     * @return transport agent
     */
    virtual boost::shared_ptr<TransportAgent> createTransportAgent(void *gmainloop);
    virtual boost::shared_ptr<TransportAgent> createTransportAgent() { return createTransportAgent(NULL); }

    /**
     * display a text message from the server
     *
     * Not really used by SyncML servers. Could be displayed in a
     * modal dialog.
     *
     * @param message     string with local encoding, possibly with line breaks
     */
    virtual void displayServerMessage(const string &message);

    /**
     * display general sync session progress
     *
     * @param type    PEV_*, see <synthesis/engine_defs.h>
     * @param extra1  extra information depending on type
     * @param extra2  extra information depending on type
     * @param extra3  extra information depending on type
     */
    virtual void displaySyncProgress(sysync::TProgressEventEnum type,
                                     int32_t extra1, int32_t extra2, int32_t extra3);

    /**
     * An event plus its parameters, see Synthesis engine.
     */
    class SyncSourceEvent
    {
      public:
        sysync::TProgressEventEnum m_type;
        int32_t m_extra1, m_extra2, m_extra3;

        SyncSourceEvent() :
            m_type(sysync::PEV_NOP)
        {}

        SyncSourceEvent(sysync::TProgressEventEnum type,
                        int32_t extra1,
                        int32_t extra2,
                        int32_t extra3)
        {
            m_type = type;
            m_extra1 = extra1;
            m_extra2 = extra2;
            m_extra3 = extra3;
        }
    };

    /**
     * display sync source specific progress
     *
     * @param source  source which is the target of the event
     * @param event   contains PEV_* and extra parameters, see <synthesis/engine_defs.h>
     * @param flush   if true, then bypass caching events and print directly
     * @return true if the event was cached
     */
    virtual bool displaySourceProgress(SyncSource &source,
                                       const SyncSourceEvent &event,
                                       bool flush);

    /**
     * report step command info
     *
     * Will be called after each step in step loop in SyncContext::doSync().
     * This reports step command info. 
     * @param stepCmd step command enum value 
     */
    virtual void reportStepCmd(sysync::uInt16 stepCmd) {}

 private:
    /** initialize members as part of constructors */
    void init();

    /**
     * generate XML configuration and (re)initialize engine with it
     *
     * @param isSync       the XML config will be used for the final engine used for syncing, not just logging
     */
    void initEngine(bool isSync);

    /**
     * the code common to init() and status():
     * populate source list with active sources and open
     */
    void initSources(SourceList &sourceList);

    /**
     * set m_localSync and m_localPeerContext
     * @param config    config name of peer
     */
    void initLocalSync(const string &config);

    /**
     * called via pre-signal of m_startDataRead
     */
    void startSourceAccess(SyncSource *source);

    /**
     * utility function for status() and getChanges():
     * iterate over sources, check for changes and copy result
     */
    void checkSourceChanges(SourceList &sourceList, SyncReport &changes);

    /**
     * A method to report sync is really successfully started.
     * It happens at the same time SynthesDBPlugin starts to access source.
     * For each sync, it is only called at most one time.
     * The default action is nothing.
     */
    virtual void syncSuccessStart() { }

    /**
     * sets up Synthesis session and executes it
     */
    SyncMLStatus doSync();

    /**
     * directory for Synthesis client binfiles or
     * Synthesis server textdb files, unique for each
     * peer
     */
    string getSynthesisDatadir();

    /**
     * return true if "delayedabort" session variable is true
     */
    bool checkForScriptAbort(SharedSession session);

    // total retry duration
    int m_retryDuration;
    // message resend interval
    int m_retryInterval;
    // Current retry count
    int m_retries;

    //a flag indicating whether it is the first time to start source access.
    //It can be used to report infomation about a sync is successfully started.
    bool m_firstSourceAccess;

    // Cache for use in displaySourceProgress().
    SyncSource *m_sourceProgress;
    SyncSourceEvent m_sourceEvent;
    std::set<std::string> m_sourceStarted;

public:
    /**
     * Returns the URL in the getSyncURL() list which is to be used
     * for sync.  The long term goal is to pick the first URL which
     * uses a currently available transport; right now it simply picks
     * the first supported one.
     */
    string getUsedSyncURL();
};

SE_END_CXX
#endif // INCL_EVOLUTIONSYNCCLIENT
