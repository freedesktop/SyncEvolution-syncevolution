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

#include "EvolutionSyncClient.h"
#include "EvolutionSyncSource.h"
#include <EvolutionClientConfig.h>

#include <posix/base/posixlog.h>

#include <list>
#include <memory>
#include <vector>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
using namespace std;

#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

SourceList *EvolutionSyncClient::m_sourceListPtr;

EvolutionSyncClient::EvolutionSyncClient(const string &server,
                                         bool doLogging, const set<string> &sources,
                                         const string &configRoot ) :
    m_server(server),
    m_sources(sources),
    m_doLogging(doLogging),
    m_configPath(configRoot + server),
    m_quiet(false),
    m_syncMode(SYNC_NONE)
{
}

EvolutionSyncClient::~EvolutionSyncClient()
{
}

/// remove all files in the given directory and the directory itself
static void rmBackupDir(const string &dirname)
{
    DIR *dir = opendir(dirname.c_str());
    if (!dir) {
        EvolutionSyncClient::throwError(dirname + ": " + strerror(errno));
    }
    vector<string> entries;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        entries.push_back(entry->d_name);
    }
    closedir(dir);

    for (vector<string>::iterator it = entries.begin();
         it != entries.end();
         ++it) {
        string path = dirname + "/" + *it;
        if (unlink(path.c_str())
            && errno != ENOENT
#ifdef EISDIR
            && errno != EISDIR
#endif
            ) {
            EvolutionSyncClient::throwError(path + ": " + strerror(errno));
        }
    }

    if (rmdir(dirname.c_str())) {
        EvolutionSyncClient::throwError(dirname + ": " + strerror(errno));
    }
}

// this class owns the logging directory and is responsible
// for redirecting output at the start and end of sync (even
// in case of exceptions thrown!)
class LogDir {
    string m_logdir;         /**< configured backup root dir, empty if none */
    int m_maxlogdirs;        /**< number of backup dirs to preserve, 0 if unlimited */
    string m_prefix;         /**< common prefix of backup dirs */
    string m_path;           /**< path to current logging and backup dir */
    string m_logfile;        /**< path to log file there, empty if not writing one */
    const string &m_server;  /**< name of the server for this synchronization */
    LogLevel m_oldLogLevel;  /**< logging level to restore */
    bool m_restoreLog;       /**< false if nothing needs to be restored because setLogdir() was never called */

public:
    LogDir(const string &server) : m_server(server),
                                   m_restoreLog(false)
    {
        // SyncEvolution-<server>-<yyyy>-<mm>-<dd>-<hh>-<mm>
        m_prefix = "SyncEvolution-";
        m_prefix += m_server;

        // default: $TMPDIR/SyncEvolution-<username>-<server>
        stringstream path;
        char *tmp = getenv("TMPDIR");
        if (tmp) {
            path << tmp;
        } else {
            path << "/tmp";
        }
        path << "/SyncEvolution-";
        struct passwd *user = getpwuid(getuid());
        if (user && user->pw_name) {
            path << user->pw_name;
        } else {
            path << getuid();
        }
        path << "-" << m_server;

        m_path = path.str();
    }

    /**
     * Finds previous log directory. Must be called before setLogdir().
     *
     * @param path        path to configured backup directy, NULL if defaulting to /tmp, "none" if not creating log file
     * @return full path of previous log directory, empty string if not found
     */
    string previousLogdir(const char *path) {
        string logdir;

        if (path && !strcasecmp(path, "none")) {
            return "";
        } else if (path && path[0]) {
            vector<string> entries;
            try {
                getLogdirs(path, entries);
            } catch(const std::exception &ex) {
                LOG.error("%s", ex.what());
                return "";
            }

            logdir = entries.size() ? string(path) + "/" + entries[entries.size()-1] : "";
        } else {
            logdir = m_path;
        }

        if (access(logdir.c_str(), R_OK|X_OK) == 0) {
            return logdir;
        } else {
            return "";
        }
    }

    // setup log directory and redirect logging into it
    // @param path        path to configured backup directy, NULL if defaulting to /tmp, "none" if not creating log file
    // @param maxlogdirs  number of backup dirs to preserve in path, 0 if unlimited
    // @param logLevel    0 = default, 1 = ERROR, 2 = INFO, 3 = DEBUG
    void setLogdir(const char *path, int maxlogdirs, int logLevel = 0) {
        m_maxlogdirs = maxlogdirs;
        if (path && !strcasecmp(path, "none")) {
            m_logfile = "";
        } else if (path && path[0]) {
            m_logdir = path;

            // create unique directory name in the given directory
            time_t ts = time(NULL);
            struct tm *tm = localtime(&ts);
            stringstream base;
            base << path << "/"
                 << m_prefix
                 << "-"
                 << setfill('0')
                 << setw(4) << tm->tm_year + 1900 << "-"
                 << setw(2) << tm->tm_mon + 1 << "-"
                 << setw(2) << tm->tm_mday << "-"
                 << setw(2) << tm->tm_hour << "-"
                 << setw(2) << tm->tm_min;
            int seq = 0;
            while (true) {
                stringstream path;
                path << base.str();
                if (seq) {
                    path << "-" << seq;
                }
                m_path = path.str();
                if (!mkdir(m_path.c_str(), S_IRWXU)) {
                    break;
                }
                if (errno != EEXIST) {
                    LOG.debug("%s: %s", m_path.c_str(), strerror(errno));
                    EvolutionSyncClient::throwError(m_path + ": " + strerror(errno));
                }
                seq++;
            }
            m_logfile = m_path + "/client.log";
        } else {
            // use the default temp directory
            if (mkdir(m_path.c_str(), S_IRWXU)) {
                if (errno != EEXIST) {
                    EvolutionSyncClient::throwError(m_path + ": " + strerror(errno));
                }
            }
            m_logfile = m_path + "/client.log";
        }

        if (m_logfile.size()) {
            // redirect logging into that directory, including stderr,
            // after truncating it
            FILE *file = fopen(m_logfile.c_str(), "w");
            if (file) {
                fclose(file);
                setLogFile(m_logfile.c_str(), true);
            } else {
                LOG.error("creating log file %s failed", m_logfile.c_str());
            }
        }
        m_oldLogLevel = LOG.getLevel();
        LOG.setLevel(logLevel > 0 ? (LogLevel)(logLevel - 1) /* fixed level */ :
                     m_logfile.size() ? LOG_LEVEL_DEBUG /* default for log file */ :
                     LOG_LEVEL_INFO /* default for console output */ );
        m_restoreLog = true;
    }

    /** sets a fixed directory for database files without redirecting logging */
    void setPath(const string &path) { m_path = path; }

    // return log directory, empty if not enabled
    const string &getLogdir() {
        return m_path;
    }

    // return log file, empty if not enabled
    const string &getLogfile() {
        return m_logfile;
    }

    /** find all entries in a given directory, return as sorted array */
    void getLogdirs(const string &logdir, vector<string> &entries) {
        DIR *dir = opendir(logdir.c_str());
        if (!dir) {
            EvolutionSyncClient::throwError(m_logdir + ": " + strerror(errno));
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strlen(entry->d_name) >= m_prefix.size() &&
                !m_prefix.compare(0, m_prefix.size(), entry->d_name, m_prefix.size())) {
                entries.push_back(entry->d_name);
            }
        }
        closedir(dir);
                
        sort(entries.begin(), entries.end());
    }


    // remove oldest backup dirs if exceeding limit
    void expire() {
        if (m_logdir.size() && m_maxlogdirs > 0 ) {
            vector<string> entries;
            getLogdirs(m_logdir, entries);

            unsigned int deleted = 0;
            for (vector<string>::iterator it = entries.begin();
                 it != entries.end() && entries.size() - deleted > m_maxlogdirs;
                 ++it, ++deleted) {
                string path = m_logdir + "/" + *it;
                string msg = "removing " + path;
                LOG.info(msg.c_str());
                rmBackupDir(path);
            }
        }
    }

    // remove redirection of stderr and (optionally) also of logging
    void restore(bool all) {
        if (!m_restoreLog) {
            return;
        }
          
        if (all) {
            if (m_logfile.size()) {
                setLogFile("-", false);
            }
            LOG.setLevel(m_oldLogLevel);
        } else {
            if (m_logfile.size()) {
                setLogFile(m_logfile.c_str(), false);
            }
        }
    }

    ~LogDir() {
        restore(true);
    }
};

// this class owns the sync sources and (together with
// a logdir) handles writing of per-sync files as well
// as the final report (
class SourceList : public list<EvolutionSyncSource *> {
    LogDir m_logdir;     /**< our logging directory */
    bool m_prepared;     /**< remember whether syncPrepare() dumped databases successfully */
    bool m_doLogging;    /**< true iff additional files are to be written during sync */
    SyncClient &m_client; /**< client which holds the sync report after a sync */
    bool m_reportTodo;   /**< true if syncDone() shall print a final report */
    arrayptr<SyncSource *> m_sourceArray;  /** owns the array that is expected by SyncClient::sync() */
    const bool m_quiet;  /**< avoid redundant printing to screen */
    string m_previousLogdir; /**< remember previous log dir before creating the new one */

    /** create name in current (if set) or previous logdir */
    string databaseName(EvolutionSyncSource &source, const string suffix, string logdir = "") {
        if (!logdir.size()) {
            logdir = m_logdir.getLogdir();
        }
        return logdir + "/" +
            source.getName() + "." + suffix + "." +
            source.fileSuffix();
    }

public:
    /**
     * dump into files with a certain suffix
     */
    void dumpDatabases(const string &suffix) {
        ofstream out;
#ifndef IPHONE
        // output stream on iPhone raises exception even though it is in a good state;
        // perhaps the missing C++ exception support is the reason:
        // http://code.google.com/p/iphone-dev/issues/detail?id=48
        out.exceptions(ios_base::badbit|ios_base::failbit|ios_base::eofbit);
#endif

        for( iterator it = begin();
             it != end();
             ++it ) {
            string file = databaseName(**it, suffix);
            LOG.debug("creating %s", file.c_str());
            out.open(file.c_str());
            (*it)->exportData(out);
            out.close();
            LOG.debug("%s created", file.c_str());
        }
    }

    /** remove database dumps with a specific suffix */
    void removeDatabases(const string &removeSuffix) {
        for( iterator it = begin();
             it != end();
             ++it ) {
            string file;

            file = databaseName(**it, removeSuffix);
            unlink(file.c_str());
        }
    }
        
    SourceList(const string &server, bool doLogging, SyncClient &client, bool quiet) :
        m_logdir(server),
        m_prepared(false),
        m_doLogging(doLogging),
        m_client(client),
        m_reportTodo(true),
        m_quiet(quiet)
    {
    }
    
    // call as soon as logdir settings are known
    void setLogdir(const char *logDirPath, int maxlogdirs, int logLevel) {
        m_previousLogdir = m_logdir.previousLogdir(logDirPath);
        if (m_doLogging) {
            m_logdir.setLogdir(logDirPath, maxlogdirs, logLevel);
        } else {
            // at least increase log level
            LOG.setLevel(LOG_LEVEL_DEBUG);
        }
    }

    /** return previous log dir found in setLogdir() */
    const string &getPrevLogdir() const { return m_previousLogdir; }

    /** set directory for database files without actually redirecting the logging */
    void setPath(const string &path) { m_logdir.setPath(path); }

    /**
     * If possible (m_previousLogdir found) and enabled (!m_quiet),
     * then dump changes applied locally.
     *
     * @param oldSuffix      suffix of old database dump: usually "after"
     * @param currentSuffix  the current database dump suffix: "current"
     *                       when not doing a sync, otherwise "before"
     */
    bool dumpLocalChanges(const string &oldSuffix, const string &newSuffix) {
        if (m_quiet || !m_previousLogdir.size()) {
            return false;
        }

        cout << "Local changes to be applied to server during synchronization:\n";
        for( iterator it = begin();
             it != end();
             ++it ) {
            string oldFile = databaseName(**it, oldSuffix, m_previousLogdir);
            string newFile = databaseName(**it, newSuffix);
            cout << "*** " << (*it)->getName() << " ***\n" << flush;
            string cmd = string("env CLIENT_TEST_COMPARISON_FAILED=10 CLIENT_TEST_LEFT_NAME='after last sync' CLIENT_TEST_RIGHT_NAME='current data' CLIENT_TEST_REMOVED='removed since last sync' CLIENT_TEST_ADDED='added since last sync' synccompare 2>/dev/null '" ) +
                oldFile + "' '" + newFile + "'";
            int ret = system(cmd.c_str());
            switch (ret == -1 ? ret : WEXITSTATUS(ret)) {
            case 0:
                cout << "no changes\n";
                break;
            case 10:
                break;
            default:
                cout << "Comparison was impossible.\n";
                break;
            }
        }
        cout << "\n";
        return true;
    }

    // call when all sync sources are ready to dump
    // pre-sync databases
    void syncPrepare() {
        if (m_logdir.getLogfile().size() &&
            m_doLogging) {
            // dump initial databases
            dumpDatabases("before");
            // compare against the old "after" database dump
            dumpLocalChanges("after", "before");
            // now remove the old database dump
            removeDatabases("after");
            m_prepared = true;
        }
    }

    // call at the end of a sync with success == true
    // if all went well to print report
    void syncDone(bool success) {
        if (m_doLogging) {
            // ensure that stderr is seen again
            m_logdir.restore(false);

            if (m_reportTodo) {
                // haven't looked at result of sync yet;
                // don't do it again
                m_reportTodo = false;

                // dump datatbase after sync, but not if already dumping it at the beginning didn't complete
                if (m_prepared) {
                    try {
                        dumpDatabases("after");
                    } catch (const std::exception &ex) {
                        LOG.error( "%s", ex.what() );
                        m_prepared = false;
                    }
                }

                // scan for error messages
                string logfile = m_logdir.getLogfile();
                if (!m_quiet && logfile.size()) {
                    ifstream in;
                    in.open(m_logdir.getLogfile().c_str());
                    while (in.good()) {
                        string line;
                        getline(in, line);
                        if (line.find("[ERROR]") != line.npos) {
                            success = false;
                            cout << line << "\n";
                        } else if (line.find("[INFO]") != line.npos) {
                            cout << line << "\n";
                        }
                    }
                    in.close();
                }

                cout << flush;
                cerr << flush;
                cout << "\n";
                if (success) {
                    cout << "Synchronization successful.\n";
                } else if (logfile.size()) {
                    cout << "Synchronization failed, see "
                         << logfile
                         << " for details.\n";
                } else {
                    cout << "Synchronization failed.\n";
                }

                // pretty-print report
                if (!m_quiet) {
                    cout << "\nChanges applied during synchronization:\n";
                }
                SyncReport *report = m_client.getSyncReport();
                if (!m_quiet && report) {

                    cout << "+-------------------|-------ON CLIENT-------|-------ON SERVER-------|\n";
                    cout << "|                   |   successful / total  |   successful / total  |\n";
                    cout << "|            Source |  NEW  |  MOD  |  DEL  |  NEW  |  MOD  |  DEL  |\n";
                    const char *sep = 
                        "+-------------------+-------+-------+-------+-------+-------+-------+\n";
                    cout << sep;

                    for (unsigned int i = 0; report->getSyncSourceReport(i); i++) {
                        SyncSourceReport* ssr = report->getSyncSourceReport(i);

                        if (ssr->getState() == SOURCE_INACTIVE) {
                            continue;
                        }
                        
                        cout << "|" << right << setw(18) << ssr->getSourceName() << " |";
                        static const char * const targets[] =
                            { CLIENT, SERVER, NULL };
                        for (int target = 0;
                             targets[target];
                             target++) {
                            static const char * const commands[] =
                                { COMMAND_ADD, COMMAND_REPLACE, COMMAND_DELETE, NULL };
                            for (int command = 0;
                                 commands[command];
                                 command++) {
                                cout << right << setw(3) <<
                                    ssr->getItemReportSuccessfulCount(targets[target], commands[command]);
                                cout << "/";
                                cout << left << setw(3) <<
                                    ssr->getItemReportCount(targets[target], commands[command]);
                                cout << "|";
                            }
                        }
                        cout << "\n";
                    }
                    cout << sep;
                }

                // compare databases?
                if (!m_quiet && m_prepared) {
                    cout << "\nChanges applied to client during synchronization:\n";
                    for( iterator it = begin();
                         it != end();
                         ++it ) {
                        cout << "*** " << (*it)->getName() << " ***\n" << flush;

                        string before = databaseName(**it, "before");
                        string after = databaseName(**it, "after");
                        string cmd = string("synccompare '" ) +
                            before + "' '" + after +
                            "' && echo 'no changes'";
                        system(cmd.c_str());
                    }
                    cout << "\n";
                }

                if (success) {
                    m_logdir.expire();
                }
            }
        }
    }

    /** returns current sources as array as expected by SyncClient::sync(), memory owned by this class */
    SyncSource **getSourceArray() {
        m_sourceArray = new SyncSource *[size() + 1];

        int index = 0;
        for (iterator it = begin();
             it != end();
             ++it) {
            ((SyncSource **)m_sourceArray)[index] = *it;
            index++;
        }
        ((SyncSource **)m_sourceArray)[index] = 0;
        return m_sourceArray;
    }
   
    ~SourceList() {
        // free sync sources
        for( iterator it = begin();
             it != end();
             ++it ) {
            delete *it;
        }
    }
};

void unref(SourceList *sourceList)
{
    delete sourceList;
}

void EvolutionSyncClient::throwError(const string &error)
{
#ifdef IPHONE
    /*
     * Catching the runtime_exception fails due to a toolchain problem,
     * so do the error handling now and abort: because there is just
     * one sync source this is probably the only thing that can be done.
     * Still, it's not nice on the server...
     */
    fatalError(NULL, error.c_str());
#else
    throw runtime_error(error);
#endif
}

void EvolutionSyncClient::fatalError(void *object, const char *error)
{
    LOG.error("%s", error);
    if (m_sourceListPtr) {
        m_sourceListPtr->syncDone(false);
    }
    exit(1);
}

#if defined(HAVE_GLIB) && defined(HAVE_EDS)
# define RUN_GLIB_LOOP
#endif

#ifdef RUN_GLIB_LOOP
#include <pthread.h>
#include <signal.h>
static void *mainLoopThread(void *)
{
    // The test framework uses SIGALRM for timeouts.
    // Block the signal here because a) the signal handler
    // prints a stack back trace when called and we are not
    // interessted in the background thread's stack and b)
    // it seems to have confused glib/libebook enough to
    // access invalid memory and segfault when it gets the SIGALRM.
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);

    GMainLoop *mainloop = g_main_loop_new(NULL, TRUE);
    if (mainloop) {
        g_main_loop_run(mainloop);
        g_main_loop_unref(mainloop);
    }
    return NULL;
}
#endif

void EvolutionSyncClient::startLoopThread()
{
#ifdef RUN_GLIB_LOOP
    // when using Evolution we must have a running main loop,
    // otherwise loss of connection won't be reported to us
    static pthread_t loopthread;
    static bool loopthreadrunning;
    if (!loopthreadrunning) {
        loopthreadrunning = !pthread_create(&loopthread, NULL, mainLoopThread, NULL);
    }
#endif
}

void EvolutionSyncClient::initSources(SourceList &sourceList, EvolutionClientConfig &config, const string &url)
{
    SyncSourceConfig *sourceconfigs = config.getSyncSourceConfigs();
    set<string> unmatchedSources = m_sources;
    for (int index = 0; index < config.getNumSources(); index++) {
        ManagementNode &node(*config.getSyncSourceNode(index));
        SyncSourceConfig &sc(sourceconfigs[index]);
        
        // is the source enabled?
        string sync = sc.getSync() ? sc.getSync() : "";
        bool enabled = sync != "none";
        SyncMode overrideMode = SYNC_NONE;

        // override state?
        if (m_sources.size()) {
            if (m_sources.find(sc.getName()) != m_sources.end()) {
                if (!enabled) {
                    overrideMode = SYNC_TWO_WAY;
                    enabled = true;
                }
                unmatchedSources.erase(sc.getName());
            } else {
                enabled = false;
            }
        }
        
        if (enabled) {
            // create it
            string type = sc.getType() ? sc.getType() : "";
            if (!type.size()) {
                throwError(string(sc.getName()) + ": type not configured");
            }
            EvolutionSyncSource *syncSource =
                EvolutionSyncSource::createSource(
                                                  sc.getName(),
                                                  &node,
                                                  &sc,
                                                  string("sync4jevolution:") + url + "/" + sc.getName(),
                                                  EvolutionSyncSource::getPropertyValue(node, "evolutionsource"),
                                                  type
                                                  );
            if (!syncSource) {
                throwError(string(sc.getName()) + ": type '" + type + "' unknown" );
            }
            sourceList.push_back(syncSource);

            // Update the backend configuration. The EvolutionClientConfig
            // above prevents that these modifications overwrite the user settings.
            sc.setType(syncSource->getMimeType());
            sc.setVersion(syncSource->getMimeVersion());
            sc.setSupportedTypes(syncSource->getSupportedTypes());
            
            if (overrideMode != SYNC_NONE) {
                // disabled source selected via source name
                syncSource->setPreferredSyncMode(overrideMode);
            }
            const string user(EvolutionSyncSource::getPropertyValue(node, "evolutionuser")),
                passwd(EvolutionSyncSource::getPropertyValue(node, "evolutionpassword"));
            syncSource->setAuthentication(user, passwd);
            
            // also open it; failing now is still safe
            syncSource->open();    
        } else {
            // set garbage in type info: the Funambol server checks that
            // and setting an empty string does not yet eclude the sync source
            sc.setType("foo");
            sc.setVersion("bar");
            sc.setSupportedTypes("foo");
        }
    }

    // check whether there were any sources specified which do not exist
    if (unmatchedSources.size()) {
        string sources;

        for (set<string>::const_iterator it = unmatchedSources.begin();
             it != unmatchedSources.end();
             it++) {
            if (sources.size()) {
                sources += " ";
            }
            sources += *it;
        }
        throwError(string("no such source(s): ") + sources);
    }
}

int EvolutionSyncClient::sync()
{
    int res = 1;
    EvolutionClientConfig config(m_configPath.c_str());
    
    if (!config.read() || !config.open()) {
        throwError("reading configuration failed");
    }

    // remember for use by sync sources
    string url = config.getAccessConfig().getSyncURL() ? config.getAccessConfig().getSyncURL() : "";
    if (!url.size()) {
        LOG.error("no syncURL configured - perhaps the server name \"%s\" is wrong?",
                  m_server.c_str());
        throwError("cannot proceed without configuration");
    }

    // redirect logging as soon as possible
    SourceList sourceList(m_server, m_doLogging, *this, m_quiet);
    m_sourceListPtr = &sourceList;

    try {
        arrayptr<char> logdir(config.getSyncMLNode()->readPropertyValue("logdir"));
        arrayptr<char> maxlogdirs(config.getSyncMLNode()->readPropertyValue("maxlogdirs"));
        arrayptr<char> loglevel(config.getSyncMLNode()->readPropertyValue("logLevel"));
        sourceList.setLogdir(logdir, atoi(maxlogdirs), atoi(loglevel));

        // dump some summary information at the beginning of the log
        LOG.debug("SyncML server account: %s", config.getAccessConfig().getUsername());
        LOG.debug("client: SyncEvolution %s", VERSION);
        time_t now = time(NULL);
        LOG.debug("current UTC date and time: %s", asctime(gmtime(&now)));

        initSources(sourceList, config, url);

        // reconfigure with our fixed properties
        DeviceConfig &dc(config.getDeviceConfig());
        dc.setVerDTD("1.1");
        dc.setMod("SyncEvolution");
        dc.setSwv(VERSION);
        dc.setMan("Patrick Ohly");
        dc.setDevType("workstation");
        dc.setUtc(1);
        dc.setOem("Open Source");

        // give derived class also a chance to update the configs
        prepare(config, sourceList.getSourceArray());

        // start background thread if not running yet:
        // necessary to catch problems with Evolution backend
        startLoopThread();

        // ready to go: dump initial databases and prepare for final report
        sourceList.syncPrepare();

        // do it
        res = SyncClient::sync(config, sourceList.getSourceArray());

        // store modified properties: must be done even after failed
        // sync because the source's anchor might have been reset
        config.save();

        if (res) {
            if (lastErrorCode && lastErrorMsg[0]) {
                throwError(lastErrorMsg);
            }
            // no error code/description?!
            throwError("sync failed without an error description, check log");
        }

        // all went well: print final report before cleaning up
        sourceList.syncDone(true);

        res = 0;
    } catch (const std::exception &ex) {
        LOG.error( "%s", ex.what() );

        // something went wrong, but try to write .after state anyway
        m_sourceListPtr = NULL;
        sourceList.syncDone(false);
    } catch (...) {
        LOG.error( "unknown error" );
        m_sourceListPtr = NULL;
        sourceList.syncDone(false);
    }

    m_sourceListPtr = NULL;
    return res;
}

void EvolutionSyncClient::prepare(SyncManagerConfig &config,
                                  SyncSource **sources) {
    if (m_syncMode != SYNC_NONE) {
        for (SyncSource **source = sources;
             *source;
             source++) {
            (*source)->setPreferredSyncMode(m_syncMode);
        }
    }
}

void EvolutionSyncClient::status()
{
    EvolutionClientConfig config(m_configPath.c_str());
    if (!config.read() || !config.open()) {
        throwError("reading configuration failed");
    }

    // remember for use by sync sources
    string url = config.getAccessConfig().getSyncURL() ? config.getAccessConfig().getSyncURL() : "";
    if (!url.size()) {
        LOG.error("no syncURL configured - perhaps the server name \"%s\" is wrong?",
                  m_server.c_str());
        throwError("cannot proceed without configuration");
    }

    SourceList sourceList(m_server, false, *this, false);
    initSources(sourceList, config, url);

    arrayptr<char> logdir(config.getSyncMLNode()->readPropertyValue("logdir"));
    sourceList.setLogdir(logdir, 0, LOG_LEVEL_NONE);
    LOG.setLevel(LOG_LEVEL_INFO);
    string prevLogdir = sourceList.getPrevLogdir();
    bool found = access(prevLogdir.c_str(), R_OK|X_OK) == 0;

    if (found) {
        try {
            sourceList.setPath(prevLogdir);
            sourceList.dumpDatabases("current");
            sourceList.dumpLocalChanges("after", "current");
        } catch(const std::exception &ex) {
            LOG.error("%s", ex.what());
        }
    } else {
        cerr << "Previous log directory not found.\n";
        if (!logdir || !logdir[0]) {
            cerr << "Enable the 'logdir' option and synchronize to use this feature.\n";
        }
    }
}
