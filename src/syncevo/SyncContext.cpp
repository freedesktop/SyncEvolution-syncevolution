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

#include <syncevo/SyncContext.h>
#include <syncevo/SyncSource.h>
#include <syncevo/util.h>
#include <syncevo/SuspendFlags.h>
#include <syncevo/ThreadSupport.h>
#include <syncevo/IdentityProvider.h>

#include <syncevo/SafeConfigNode.h>
#include <syncevo/IniConfigNode.h>

#include <syncevo/LogStdout.h>
#include <syncevo/TransportAgent.h>
#include <syncevo/CurlTransportAgent.h>
#include <syncevo/SoupTransportAgent.h>
#include <syncevo/ObexTransportAgent.h>
#include <syncevo/LocalTransportAgent.h>

#include <list>
#include <memory>
#include <vector>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <ctime>
using namespace std;

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/bind.hpp>
#include <boost/utility.hpp>
#include <boost/lambda/bind.hpp>

#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include <synthesis/enginemodulebridge.h>
#include <synthesis/SDK_util.h>
#include <synthesis/san.h>

#ifdef USE_DLT
# include <dlt.h>
#endif

#include "test.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

SyncContext *SyncContext::m_activeContext;

static const char *LogfileBasename = "syncevolution-log";

static std::string RealPath(const std::string &path)
{
    std::string buffer;
    char *newPath = realpath(path.c_str(), NULL);
    if (newPath) {
        buffer = newPath;
        free(newPath);
        return buffer;
    } else {
        return path;
    }
}


SyncContext::SyncContext()
{
    init();
}

SyncContext::SyncContext(const string &server,
                         bool doLogging) :
    SyncConfig(server),
    m_server(server)
{
    init();
    m_doLogging = doLogging;
}

SyncContext::SyncContext(const string &client,
                         const string &server,
                         const string &rootPath,
                         const boost::shared_ptr<TransportAgent> &agent,
                         bool doLogging) :
    SyncConfig(client,
               boost::shared_ptr<ConfigTree>(),
               rootPath),
    m_server(client),
    m_localClientRootPath(rootPath),
    m_agent(agent)
{
    init();
    initLocalSync(server);
    m_doLogging = doLogging;
}

void SyncContext::initLocalSync(const string &config)
{
    m_localSync = true;
    string tmp;
    splitConfigString(config, tmp, m_localPeerContext);
    m_localPeerContext.insert(0, "@");
}

void SyncContext::init()
{
    m_doLogging = false;
    m_quiet = false;
    m_dryrun = false;
    m_localSync = false;
    m_serverMode = false;
    m_serverAlerted = false;
    m_configNeeded = true;
    m_firstSourceAccess = true;
    m_quitSync = false;
    m_remoteInitiated = false;
    m_sourceListPtr = NULL;
    m_syncFreeze = SYNC_FREEZE_NONE;
}

SyncContext::~SyncContext()
{
}

/**
 * Utility code for parsing and comparing
 * log dir names. Also a binary predicate for
 * sorting them.
 */
class LogDirNames {
public:
    // internal prefix for backup directory name: "SyncEvolution-"
    static const char* const DIR_PREFIX;

    /**
     * Compare two directory by its creation time encoded
     * in the directory name sort them in ascending order
     */
    bool operator() (const string &str1, const string &str2) {
        string iDirPath1, iStr1;
        string iDirPath2, iStr2;
        parseLogDir(str1, iDirPath1, iStr1);
        parseLogDir(str2, iDirPath2, iStr2);
        string dirPrefix1, peerName1, dateTime1;
        parseDirName(iStr1, dirPrefix1, peerName1, dateTime1);
        string dirPrefix2, peerName2, dateTime2;
        parseDirName(iStr2, dirPrefix2, peerName2, dateTime2);
        return dateTime1 < dateTime2;
    }

    /**
     * extract backup directory name from a full backup path
     * for example, a full path "/home/xxx/.cache/syncevolution/default/funambol-2009-12-08-14-05"
     * is parsed as "/home/xxx/.cache/syncevolution/default" and "funambol-2009-12-08-14-05"
     */
    static void parseLogDir(const string &fullpath, string &dirPath, string &dirName) {
        string iFullpath = boost::trim_right_copy_if(fullpath, boost::is_any_of("/"));
        size_t off = iFullpath.find_last_of('/');
        if(off != iFullpath.npos) {
            dirPath = iFullpath.substr(0, off);
            dirName = iFullpath.substr(off+1);
        } else {
            dirPath = "";
            dirName = iFullpath;
        }
    }

    // escape '-' and '_' for peer name 
    static string escapePeer(const string &prefix) {
        string escaped = prefix;
        boost::replace_all(escaped, "_", "__");
        boost::replace_all(escaped, "-", "_+");
        return escaped;
    }

    // un-escape '_+' and '__' for peer name 
    static string unescapePeer(const string &escaped) {
        string prefix = escaped;
        boost::replace_all(prefix, "_+", "-");
        boost::replace_all(prefix, "__", "_");
        return prefix;
    }

    /**
     * parse a directory name into dirPrefix(empty or DIR_PREFIX), peerName, dateTime.
     * peerName must be unescaped by the caller to get the real string.
     * If directory name is in the format of '[DIR_PREFIX]-peer[@context]-year-month-day-hour-min'
     * then parsing is sucessful and these 3 strings are correctly set and true is returned. 
     * Otherwise, false is returned. 
     * Here we don't check whether the dir name is matching peer name
     */
    static bool parseDirName(const string &dir, string &dirPrefix, string &config, string &dateTime) {
        string iDir = dir;
        if (!boost::starts_with(iDir, DIR_PREFIX)) {
            dirPrefix = "";
        } else {
            dirPrefix = DIR_PREFIX;
            boost::erase_first(iDir, DIR_PREFIX);
        }
        size_t off = iDir.find('-');
        if (off != iDir.npos) {
            config = iDir.substr(0, off);
            dateTime = iDir.substr(off);
            // m_prefix doesn't contain peer name or it equals with dirPrefix plus peerName
            return checkDirName(dateTime);
        }
        return false;
    }

    // check the dir name is conforming to what format we write
    static bool checkDirName(const string& value) {
        const char* str = value.c_str();
        /** need check whether string after prefix is a valid date-time we wrote, format
         * should be -YYYY-MM-DD-HH-MM and optional sequence number */
        static char table[] = {'-','9','9','9','9', //year
                               '-','1','9', //month
                               '-','3','9', //date
                               '-','2','9', //hour
                               '-','5','9'  //minute
        };
        for(size_t i = 0; i < sizeof(table)/sizeof(table[0]) && *str; i++,str++) {
            switch(table[i]) {
                case '-':
                    if(*str != '-')
                        return false;
                    break;
                case '1':
                    if(*str < '0' || *str > '1')
                        return false;
                    break;
                case '2':
                    if(*str < '0' || *str > '2')
                        return false;
                    break;
                case '3':
                    if(*str < '0' || *str > '3')
                        return false;
                    break;
                case '5':
                    if(*str < '0' || *str > '5')
                        return false;
                    break;
                case '9':
                    if(*str < '0' || *str > '9')
                        return false;
                    break;
                default:
                    return false;
            };
        }
        return true;
    }
};

class LogDir;

/**
 * Helper class for LogDir: acts as proxy for logging into
 * the LogDir's reports and log file.
 */
class LogDirLogger : public Logger
{
    Logger::Handle m_parentLogger;     /**< the logger which was active before we started to intercept messages */
    boost::weak_ptr<LogDir> m_logdir;  /**< grants access to report and Synthesis engine */
#ifdef USE_DLT
    bool m_useDLT;                     /**< SyncEvolution and libsynthesis are logging to DLT */
#endif

public:
    LogDirLogger(const boost::weak_ptr<LogDir> &logdir);
    virtual void remove() throw ();
    virtual void messagev(const MessageOptions &options,
                          const char *format,
                          va_list args);
};

// This class owns the logging directory. It is responsible
// for redirecting output at the start and end of sync (even
// in case of exceptions thrown!).
class LogDir : private boost::noncopyable, private LogDirNames {
    SyncContext &m_client;
    string m_logdir;         /**< configured backup root dir */
    int m_maxlogdirs;        /**< number of backup dirs to preserve, 0 if unlimited */
    string m_prefix;         /**< common prefix of backup dirs */
    string m_path;           /**< path to current logging and backup dir */
    string m_logfile;        /**< Path to log file there, empty if not writing one.
                                  The file is no longer written by this class, nor
                                  does it control the basename of it. Writing the
                                  log file is enabled by the XML configuration that
                                  we prepare for the Synthesis engine; the base name
                                  of the file is hard-coded in the engine. Despite
                                  that this class still is the central point to ask
                                  for the name of the log file. */
    boost::scoped_ptr<SafeConfigNode> m_info;  /**< key/value representation of sync information */

    // Access to m_report and m_client must be thread-safe as soon as
    // LogDirLogger is active, because they are shared between main
    // thread and any thread which might log errors.
    friend class LogDirLogger;
    bool m_readonly;         /**< m_info is not to be written to */
    SyncReport *m_report;    /**< record start/end times here */

    boost::weak_ptr<LogDir> m_self;
    PushLogger<LogDirLogger> m_logger; /**< active logger */

    LogDir(SyncContext &client) : m_client(client), m_info(NULL), m_readonly(false), m_report(NULL)
    {
        // Set default log directory. This will be overwritten with a user-specified
        // location later on, if one was selected by the user. SyncEvolution >= 0.9 alpha
        // and < 0.9 beta 2 used XDG_DATA_HOME because the logs and data base dumps
        // were not considered "non-essential data files". Because XDG_DATA_HOME is
        // searched for .desktop files and creating large amounts of other files there
        // slows down that search, the default was changed to XDG_CACHE_DIR.
        //
        // To migrate old installations seamlessly, this code here renames the old
        // default directory to the new one. Errors (like not found) are silently ignored.
        mkdir_p(SubstEnvironment("${XDG_CACHE_HOME}").c_str());
        rename(SubstEnvironment("${XDG_DATA_HOME}/applications/syncevolution").c_str(),
               SubstEnvironment("${XDG_CACHE_HOME}/syncevolution").c_str());

        string path = m_client.getLogDir();
        if (path.empty()) {
            path = "${XDG_CACHE_HOME}/syncevolution";
        }
        setLogdir(path);
    }

public:
    static boost::shared_ptr<LogDir> create(SyncContext &client)
    {
        boost::shared_ptr<LogDir> logdir(new LogDir(client));
        logdir->m_self = logdir;
        return logdir;
    }

    /**
     * Finds previous log directories for context. Reports errors via exceptions.
     *
     * @retval dirs       vector of full path names, oldest first
     */
    void previousLogdirs(vector<string> &dirs) {
        dirs.clear();
        getLogdirs(dirs);
    }

    /**
     * Finds previous log directory. Returns empty string if anything went wrong.
     *
     * @param path        path to configured backup directy, NULL if defaulting to /tmp, "none" if not creating log file
     * @return full path of previous log directory, empty string if not found
     */
    string previousLogdir() throw() {
        try {
            vector<string> dirs;
            previousLogdirs(dirs);
            return dirs.empty() ? "" : dirs.back();
        } catch (...) {
            Exception::handle();
            return "";
        }
    }

    /**
     * Set log dir and base name used for searching and creating sessions.
     * Default if not called is the getLogDir() value of the context.
     *
     * @param logdir     "none" to disable sessions, "" for default, may contain ${}
     *                   for environment variables
     */
    void setLogdir(const string &logdir) {
        if (logdir.empty()) {
            return;
        }
        m_logdir = SubstEnvironment(logdir);
        m_logdir = boost::trim_right_copy_if(m_logdir, boost::is_any_of("/"));
        if (m_logdir == "none") {
            return;
        }

        // Resolve symbolic links in path now, in case that they change later
        // while the session runs. Relies on being allowed to pass NULL. If that's
        // not allowed, we ignore the error and continue to use the known path.
        errno = 0;
        m_logdir = RealPath(m_logdir);
        SE_LOG_DEBUG(NULL, "log path -> %s, %s",
                     m_logdir.c_str(),
                     errno ? strerror(errno) : "<okay>");

        // the config name has been normalized
        string peer = m_client.getConfigName();

        // escape "_" and "-" the peer name
        peer = escapePeer(peer);

        if (boost::iends_with(m_logdir, "syncevolution")) {
            // use just the server name as prefix
            m_prefix = peer;
        } else {
            // SyncEvolution-<server>-<yyyy>-<mm>-<dd>-<hh>-<mm>
            m_prefix = DIR_PREFIX;
            m_prefix += peer;
        }
    }

    /**
     * access existing log directory to extract status information
     */
    void openLogdir(const string &dir) {
        boost::shared_ptr<ConfigNode> filenode(new IniFileConfigNode(dir, "status.ini", true));
        m_info.reset(new SafeConfigNode(filenode));
        m_info->setMode(false);
        m_readonly = true;
    }
    /*
     * get the corresponding peer name encoded in the logging dir name.
     * The dir name must match the format(see startSession). Otherwise,
     * empty string is returned.
     */
    string getPeerNameFromLogdir(const string &dir) {
        // extract the dir name from the fullpath
        string iDirPath, iDirName;
        parseLogDir(dir, iDirPath, iDirName);
        // extract the peer name from the dir name
        string dirPrefix, peerName, dateTime;
        if(parseDirName(iDirName, dirPrefix, peerName, dateTime)) {
            return unescapePeer(peerName);
        }
        return "";
    }

    /**
     * read sync report for session selected with openLogdir()
     */
    void readReport(SyncReport &report) {
        report.clear();
        if (!m_info) {
            return;
        }
        *m_info >> report;
    }

    /**
     * write sync report for current session
     */
    void writeReport(SyncReport &report) {
        if (m_info) {
            *m_info << report;

            /* write in slightly different format and flush at the end */
            writeTimestamp("start", report.getStart(), false);
            writeTimestamp("end", report.getEnd(), true);
        }
    }

    enum SessionMode {
        SESSION_USE_PATH,      /**< write directly into path, don't create and manage subdirectories */
        SESSION_READ_ONLY,     /**< access an existing session directory identified with path */
        SESSION_CREATE         /**< create a new session directory inside the given path */
    };

    // setup log directory and redirect logging into it
    // @param path        path to configured backup directy, empty for using default, "none" if not creating log file
    // @param mode        determines how path is interpreted and which session is accessed
    // @param maxlogdirs  number of backup dirs to preserve in path, 0 if unlimited
    // @param logLevel    0 = default, 1 = ERROR, 2 = INFO, 3 = DEBUG
    // @param report      record information about session here (may be NULL)
    void startSession(const string &path, SessionMode mode, int maxlogdirs, int logLevel, SyncReport *report) {
        m_maxlogdirs = maxlogdirs;
        m_report = report;
        m_logfile = "";
        if (boost::iequals(path, "none")) {
            m_path = "";
        } else {
            setLogdir(path);
            SE_LOG_DEBUG(NULL, "checking log dir %s", m_logdir.c_str());
            if (mode == SESSION_CREATE) {
                // create unique directory name in the given directory
                time_t ts = time(NULL);
                struct tm tmbuffer;
                struct tm *tm = localtime_r(&ts, &tmbuffer);
                if (!tm) {
                    SE_THROW("localtime_r() failed");
                }
                stringstream base;
                base << "-"
                     << setfill('0')
                     << setw(4) << tm->tm_year + 1900 << "-"
                     << setw(2) << tm->tm_mon + 1 << "-"
                     << setw(2) << tm->tm_mday << "-"
                     << setw(2) << tm->tm_hour << "-"
                     << setw(2) << tm->tm_min;
                // If other sessions, regardless of which peer, have
                // the same date and time, then append a sequence
                // number to ensure correct sorting. Solve this by
                // finding the maximum sequence number for any kind of
                // date time. Backwards running clocks or changing the
                // local time will still screw our ordering, though.
                typedef std::map<string, int> SeqMap_t;
                SeqMap_t dateTimes2Seq;
                if (isDir(m_logdir)) {
                    ReadDir dir(m_logdir);
                    BOOST_FOREACH(const string &entry, dir) {
                        string dirPrefix, peerName, dateTime;
                        if (parseDirName(entry, dirPrefix, peerName, dateTime)) {
                            // dateTime = -2010-01-31-12-00[-rev]
                            size_t off = 0;
                            for (int i = 0; off != dateTime.npos && i < 5; i++) {
                                off = dateTime.find('-', off + 1);
                            }
                            int sequence;
                            if (off != dateTime.npos) {
                                sequence = dateTime[off + 1] - 'a';
                                dateTime.resize(off);
                            } else {
                                sequence = -1;
                            }
                            pair <SeqMap_t::iterator, bool> entry = dateTimes2Seq.insert(make_pair(dateTime, sequence));
                            if (sequence > entry.first->second) {
                                entry.first->second = sequence;
                            }
                        }
                    }
                }
                stringstream path;
                path << base.str();
                SeqMap_t::iterator it = dateTimes2Seq.find(path.str());
                if (it != dateTimes2Seq.end()) {
                    path << "-" << (char)('a' + it->second + 1);
                }
                m_path = m_logdir + "/";
                m_path += m_prefix;
                m_path += path.str();
                mkdir_p(m_path);
            } else {
                m_path = m_logdir;
                if (mkdir(m_path.c_str(), S_IRWXU) &&
                    errno != EEXIST) {
                    SE_LOG_DEBUG(NULL, "%s: %s", m_path.c_str(), strerror(errno));
                    Exception::throwError(SE_HERE, m_path, errno);
                }
            }
            m_logfile = m_path + "/" + LogfileBasename + ".html";
            SE_LOG_DEBUG(NULL, "logfile: %s", m_logfile.c_str());
        }

        // update log level of default logger and our own replacement
        Logger::Level level;
        switch (logLevel) {
        case 0:
            // default for console output
            level = Logger::INFO;
            break;
        case 1:
            level = Logger::ERROR;
            break;
        case 2:
            level = Logger::INFO;
            break;
        default:
            if (m_logfile.empty() || getenv("SYNCEVOLUTION_DEBUG")) {
                // no log file or user wants to see everything:
                // print all information to the console
                level = Logger::DEBUG;
            } else {
                // have log file: avoid excessive output to the console,
                // full information is in the log file
                level = Logger::INFO;
            }
            break;
        }
        if (mode != SESSION_USE_PATH) {
            Logger::instance().setLevel(level);
        }
        boost::shared_ptr<Logger> logger(new LogDirLogger(m_self));
        logger->setLevel(level);
        m_logger.reset(logger);

        time_t start = time(NULL);
        if (m_report) {
            m_report->setStart(start);
        }
        m_readonly = mode == SESSION_READ_ONLY;
        if (!m_path.empty()) {
            boost::shared_ptr<ConfigNode> filenode(new IniFileConfigNode(m_path, "status.ini", m_readonly));
            m_info.reset(new SafeConfigNode(filenode));
            m_info->setMode(false);
            if (mode != SESSION_READ_ONLY) {
                // Create a status.ini which contains an error.
                // Will be overwritten later on, unless we crash.
                m_info->setProperty("status", STATUS_DIED_PREMATURELY);
                m_info->setProperty("error", InitStateString("synchronization process died prematurely", true));
                writeTimestamp("start", start);
            }
        }
    }

    /** sets a fixed directory for database files without redirecting logging */
    void setPath(const string &path) { m_path = RealPath(path); SE_LOG_DEBUG(NULL, "setPath(%s) -> %s", path.c_str(), m_path.c_str()); }

    // return log directory, empty if not enabled
    const string &getLogdir() {
        return m_path;
    }

    // return log file, empty if not enabled
    const string &getLogfile() {
        return m_logfile;
    }

    /**
     * remove backup dir(s) if exceeding limit
     *
     * Assign a priority to each session dir, with lower
     * meaning "less important". Then sort by priority and (if
     * equal) creation time (aka index) in ascending
     * order. The sessions at the beginning of the sorted
     * vector are then removed first.
     *
     * DUMPS = any kind of database dump was made
     * ERROR = session failed
     * CHANGES = local data modified since previous dump (based on dumps
     *           of the current peer, for simplicity reasons),
     *           dump created for the first time,
     *           changes made during sync (detected with dumps and statistics)
     *
     * The goal is to preserve as many database dumps as possible
     * and ideally those where something happened.
     *
     * Some criteria veto the removal of a session:
     * - it is the only one holding a dump of a specific source
     * - it is the last session
     */
    void expire() {
        if (m_logdir.size() && m_maxlogdirs > 0 ) {
            vector<string> dirs;
            getLogdirs(dirs);

            /** stores priority and index in "dirs"; after sorting, delete from the start */
            vector< pair<Priority, size_t> > victims;
            /** maps from source name to list of information about dump, oldest first */
            typedef map< string, list<DumpInfo> > Dumps_t;
            Dumps_t dumps;
            for (size_t i = 0;
                 i < dirs.size();
                 i++) {
                bool changes = false;
                bool havedumps = false;
                bool errors = false;

                LogDir logdir(m_client);
                logdir.openLogdir(dirs[i]);
                SyncReport report;
                logdir.readReport(report);
                SyncMLStatus status = report.getStatus();
                if (status != STATUS_OK && status != STATUS_HTTP_OK) {
                    errors = true;
                }
                BOOST_FOREACH(SyncReport::SourceReport_t source, report) {
                    string &sourcename = source.first;
                    SyncSourceReport &sourcereport = source.second;
                    list<DumpInfo> &dumplist = dumps[sourcename];
                    if (sourcereport.m_backupBefore.isAvailable() ||
                        sourcereport.m_backupAfter.isAvailable()) {
                        // yes, we have backup dumps
                        havedumps = true;

                        DumpInfo info(i,
                                      sourcereport.m_backupBefore.getNumItems(),
                                      sourcereport.m_backupAfter.getNumItems());

                        // now check for changes, if none found yet
                        if (!changes) {
                            if (dumplist.empty()) {
                                // new backup dump
                                changes = true;
                            } else {
                                DumpInfo &previous = dumplist.back();
                                changes =
                                    // item count changed -> items changed
                                    previous.m_itemsDumpedAfter != info.m_itemsDumpedBefore ||
                                    sourcereport.wasChanged(SyncSourceReport::ITEM_LOCAL) ||
                                    sourcereport.wasChanged(SyncSourceReport::ITEM_REMOTE) ||
                                    haveDifferentContent(sourcename,
                                                         dirs[previous.m_dirIndex], "after",
                                                         dirs[i], "before");
                            }
                        }

                        dumplist.push_back(info);
                    }
                }
                Priority pri =
                    havedumps ?
                    (changes ?
                     HAS_DUMPS_WITH_CHANGES :
                     errors ?
                     HAS_DUMPS_NO_CHANGES_WITH_ERRORS :
                     HAS_DUMPS_NO_CHANGES) :
                    (changes ?
                     NO_DUMPS_WITH_CHANGES :
                     errors ?
                     NO_DUMPS_WITH_ERRORS :
                     NO_DUMPS_NO_ERRORS);
                victims.push_back(make_pair(pri, i));
            }
            sort(victims.begin(), victims.end());

            int deleted = 0;
            for (size_t e = 0;
                 e < victims.size() && (int)dirs.size() - deleted > m_maxlogdirs;
                 ++e) {
                size_t index = victims[e].second;
                string &path = dirs[index];
                // preserve latest session
                if (index != dirs.size() - 1) {
                    bool mustkeep = false;
                    // also check whether it holds the only backup of a source
                    BOOST_FOREACH(Dumps_t::value_type dump, dumps) {
                        if (dump.second.size() == 1 &&
                            dump.second.front().m_dirIndex == index) {
                            mustkeep = true;
                            break;
                        }
                    }
                    if (!mustkeep) {
                        SE_LOG_DEBUG(NULL, "removing %s", path.c_str());
                        rm_r(path);
                        ++deleted;
                    }
                }
            }
        }
    }

    // finalize session
    void endSession()
    {
        time_t end = time(NULL);
        if (m_report) {
            m_report->setEnd(end);
        }
        if (m_info) {
            if (!m_readonly) {
                writeTimestamp("end", end);
                if (m_report) {
                    RecMutex::Guard guard = Logger::lock();
                    writeReport(*m_report);
                }
                m_info->flush();
            }
            m_info.reset();
        }
    }

    // Remove redirection of logging.
    void restore() {
        m_logger.reset();
    }

    ~LogDir() {
        restore();
    }


#if 0
    /**
     * A quick check for level = SHOW text dumps whether the text dump
     * starts with the [ERROR] prefix; used to detect error messages
     * from forked process which go through this instance but are not
     * already tagged as error messages and thus would not show up as
     * "first error" in sync reports.
     *
     * Example for the problem:
     * [ERROR] onConnect not implemented                [from child process]
     * [ERROR] child process quit with return code 1    [from parent]
     * ...
     * Changes applied during synchronization:
     * ...
     * First ERROR encountered: child process quit with return code 1
     */
    static bool isErrorString(const char *format,
                              va_list args)
    {
        const char *text;
        if (!strcmp(format, "%s")) {
            va_list argscopy;
            va_copy(argscopy, args);
            text = va_arg(argscopy, const char *);
            va_end(argscopy);
        } else {
            text = format;
        }
        return boost::starts_with(text, "[ERROR");
    }
#endif

    /**
     * Compare two database dumps just based on their inodes.
     * @return true    if inodes differ
     */
    static bool haveDifferentContent(const string &sourceName,
                                     const string &firstDir,
                                     const string &firstSuffix,
                                     const string &secondDir,
                                     const string &secondSuffix)
    {
        string first = firstDir + "/" + sourceName + "." + firstSuffix;
        string second = secondDir + "/" + sourceName + "." + secondSuffix;
        ReadDir firstContent(first);
        ReadDir secondContent(second);
        set<ino_t> firstInodes;
        BOOST_FOREACH(const string &name, firstContent) {
            struct stat buf;
            string fullpath = first + "/" + name;
            if (stat(fullpath.c_str(), &buf)) {
                Exception::throwError(SE_HERE, fullpath, errno);
            }
            firstInodes.insert(buf.st_ino);
        }
        BOOST_FOREACH(const string &name, secondContent) {
            struct stat buf;
            string fullpath = second + "/" + name;
            if (stat(fullpath.c_str(), &buf)) {
                Exception::throwError(SE_HERE, fullpath, errno);
            }
            set<ino_t>::iterator it = firstInodes.find(buf.st_ino);
            if (it == firstInodes.end()) {
                // second dir has different file
                return true;
            } else {
                firstInodes.erase(it);
            }
        }
        if (!firstInodes.empty()) {
            // first dir has different file
            return true;
        }
        // exact match of inodes
        return false;
    }

private:
    enum Priority {
        NO_DUMPS_NO_ERRORS,
        NO_DUMPS_WITH_ERRORS,
        NO_DUMPS_WITH_CHANGES,
        HAS_DUMPS_NO_CHANGES,
        HAS_DUMPS_NO_CHANGES_WITH_ERRORS,
        HAS_DUMPS_WITH_CHANGES
    };

    struct DumpInfo {
        size_t m_dirIndex;
        int m_itemsDumpedBefore;
        int m_itemsDumpedAfter;
        DumpInfo(size_t dirIndex,
                 int itemsDumpedBefore,
                 int itemsDumpedAfter) :
            m_dirIndex(dirIndex),
            m_itemsDumpedBefore(itemsDumpedBefore),
            m_itemsDumpedAfter(itemsDumpedAfter)
        {}
    };

    /** 
     * Find all entries in a given directory, return as sorted array of full paths in ascending order.
     * If m_prefix doesn't contain peer name information, then all log dirs for different peers in the
     * logdir are returned.
     */
    void getLogdirs(vector<string> &dirs) {
        if (m_logdir != "none" && !isDir(m_logdir)) {
            return;
        }
        string peer = m_client.getConfigName();
        string peerName, context;
        SyncConfig::splitConfigString(peer, peerName, context);

        ReadDir dir(m_logdir);
        BOOST_FOREACH(const string &entry, dir) {
            string tmpDirPrefix, tmpPeer, tmpDateTime;
            // if directory name could not be parsed, ignore it
            if(parseDirName(entry, tmpDirPrefix, tmpPeer, tmpDateTime)) {
                if(!peerName.empty() && (m_prefix == (tmpDirPrefix + tmpPeer))) {
                    // if peer name exists, match the logs for the given peer
                    dirs.push_back(m_logdir + "/" + entry);
                } else if(peerName.empty()) {
                    // if no peer name and only context, match for all logs under the given context
                    string tmpName, tmpContext;
                    SyncConfig::splitConfigString(unescapePeer(tmpPeer), tmpName, tmpContext);
                    if( context == tmpContext && boost::starts_with(m_prefix, tmpDirPrefix)) {
                        dirs.push_back(m_logdir + "/" + entry);
                    }
                }
            }
        }
        // sort vector in ascending order
        // if no peer name
        if(peerName.empty()){
            sort(dirs.begin(), dirs.end(), LogDirNames());
        } else {
            sort(dirs.begin(), dirs.end());
        }
    }

    // store time stamp in session info
    void writeTimestamp(const string &key, time_t val, bool flush = true) {
        if (m_info) {
            char buffer[160];
            struct tm tmbuffer, *tm;
            // be nice and store a human-readable date in addition the seconds since the epoch
            tm = localtime_r(&val, &tmbuffer);
            if (tm) {
                strftime(buffer, sizeof(buffer), "%s, %Y-%m-%d %H:%M:%S %z", tm);
            } else {
                // Less suitable fallback. Won't work correctly for 32
                // bit long beyond 2038.
                sprintf(buffer, "%lu", (long unsigned)val);
            }
            m_info->setProperty(key, buffer);
            if (flush) {
                m_info->flush();
            }
        }
    }
};

LogDirLogger::LogDirLogger(const boost::weak_ptr<LogDir> &logdir) :
    m_parentLogger(Logger::instance()),
    m_logdir(logdir)
#ifdef USE_DLT
    , m_useDLT(getenv("SYNCEVOLUTION_USE_DLT") != NULL)
#endif
{
}

void LogDirLogger::remove() throw ()
{
    // Forget reference to LogDir. This prevents accessing it in
    // future messagev() calls.
    RecMutex::Guard guard = Logger::lock();
    m_logdir.reset();
}

void LogDirLogger::messagev(const MessageOptions &options,
                            const char *format,
                            va_list args)
{
    // Protects ordering of log messages and access to shared
    // variables like m_report and m_engine.
    RecMutex::Guard guard = Logger::lock();

    // always to parent first (usually stdout):
    // if the parent is a LogRedirect instance, then
    // it'll flush its own output first, which ensures
    // that the new output comes later (as desired)
    va_list argscopy;
    va_copy(argscopy, args);
    m_parentLogger.messagev(options, format, argscopy);
    va_end(argscopy);

    // Special handling of our own messages: include in sync report
    // (always, because that is how we did it traditionally) and write
    // to our own syncevolution-log.html (if not already logged).
    //
    // The TestLocalSync.testServerFailure and some others check that
    // we record the child's error message in our sync report. If we
    // don't then it shows up later marked as an "error on the target
    // side", which is probably not what we want.
    boost::shared_ptr<LogDir> logdir;
    if ((bool)(logdir = m_logdir.lock())) {
        if (logdir->m_report &&
            options.m_level <= ERROR &&
            logdir->m_report->getError().empty()) {
            va_list argscopy;
            va_copy(argscopy, args);
            string error = StringPrintfV(format, argscopy);
            va_end(argscopy);

            logdir->m_report->setError(error);
        }

        if (!(options.m_flags & MessageOptions::ALREADY_LOGGED) &&
#ifdef USE_DLT
            // Don't send to  libsynthesis if using DLT,
            // because then it would end up getting logged
            // in DLT twice.
            !m_useDLT &&
#endif
            logdir->m_client.getEngine().get()) {
            va_list argscopy;
            va_copy(argscopy, args);
            // Once to Synthesis log, with full debugging.
            // The API does not support a process name, so
            // put it into the prefix as "<procname> <prefix>".
            std::string prefix;
            if (options.m_processName) {
                prefix += *options.m_processName;
            }
            if (options.m_prefix) {
                if (!prefix.empty()) {
                    prefix += " ";
                }
                prefix += *options.m_prefix;
            }
            logdir->m_client.getEngine().doDebug(options.m_level,
                                                 prefix.empty() ? NULL : prefix.c_str(),
                                                 options.m_file,
                                                 options.m_line,
                                                 options.m_function,
                                                 format,
                                                 argscopy);
            va_end(argscopy);
        }
    }
}


const char* const LogDirNames::DIR_PREFIX = "SyncEvolution-";

/**
 * This class owns the sync sources. For historic reasons (required
 * by Funambol) SyncSource instances are stored as plain pointers
 * deleted by this class. Virtual sync sources were added later
 * and are stored as shared pointers which are freed automatically.
 * It is possible to iterate over the two classes of sources
 * separately.
 *
 * The SourceList ensures that all sources (normal and virtual) have
 * a valid and unique integer ID as needed for Synthesis. Traditionally
 * this used to be a simple hash of the source name (which is unique
 * by design), without checking for hash collisions. Now the ID is assigned
 * the first time a source is added here and doesn't have one yet.
 * For backward compatibility (the ID is stored in the .synthesis dir),
 * the same Hash() value is tested first. Assuming that there were no
 * hash conflicts, the same IDs will be generated as before.
 *
 * Together with a logdir, the SourceList
 * handles writing of per-sync files as well as the final report.
 * It is not stateless. The expectation is that it is instantiated
 * together with a SyncContext for one particular operation (sync
 * session, status check, restore). In contrast to a SyncContext,
 * this class has to be recreated for another operation.
 *
 * When running as client, only the active sources get added. They can
 * be dumped one after the other before running a sync.
 *
 * As a server, all sources get added, regardless whether they are
 * active. This implies that at least their "type" must be valid. Then
 * later when a client really starts using them, they are opened() and
 * database dumps are made.
 *
 * Virtual datastores are stored here when they get initialized
 * together with the normal sources by the user of SourceList.
 *
 * 
 */
class SourceList : private vector<SyncSource *> {
    typedef vector<SyncSource *> inherited;

public:
    enum LogLevel {
        LOGGING_QUIET,    /**< avoid all extra output */
        LOGGING_SUMMARY,  /**< sync report, but no database comparison */
        LOGGING_FULL      /**< everything */
    };

    typedef std::vector< boost::shared_ptr<VirtualSyncSource> > VirtualSyncSources_t;

    /** reading our set of virtual sources is okay, modifying it is not */
    const VirtualSyncSources_t &getVirtualSources() { return m_virtualSources; }
    void addSource(const boost::shared_ptr<VirtualSyncSource> &source) { checkSource(source.get()); m_virtualSources.push_back(source); }

    using inherited::iterator;
    using inherited::const_iterator;
    using inherited::empty;
    using inherited::begin;
    using inherited::end;
    using inherited::rbegin;
    using inherited::rend;
    using inherited::size;

    /** transfers ownership (historic reasons for storing plain pointer...) */
    void addSource(cxxptr<SyncSource> &source) { checkSource(source); push_back(source.release()); }

private:
    VirtualSyncSources_t m_virtualSources; /**< all configured virtual data sources (aka Synthesis <superdatastore>) */
    boost::shared_ptr<LogDir> m_logdir;     /**< our logging directory */
    SyncContext &m_client; /**< the context in which we were instantiated */
    set<string> m_prepared;   /**< remember for which source we dumped databases successfully */
    string m_intro;      /**< remembers the dumpLocalChanges() intro and only prints it again
                            when different from last dumpLocalChanges() call */
    bool m_doLogging;    /**< true iff the normal logdir handling is enabled
                            (creating and expiring directoties, before/after comparison) */
    bool m_reportTodo;   /**< true if syncDone() shall print a final report */
    LogLevel m_logLevel; /**< chooses how much information is printed */
    string m_previousLogdir; /**< remember previous log dir before creating the new one */

    /** create name in current (if set) or previous logdir */
    string databaseName(SyncSource &source, const string &suffix, string logdir = "") {
        if (!logdir.size()) {
            logdir = m_logdir->getLogdir();
        }
        return logdir + "/" +
            source.getName() + "." + suffix;
    }

    /** ensure that Synthesis ID is set and unique */
    void checkSource(SyncSource *source) {
        if (source->getSynthesisID()) {
            return;
        }
        int id = Hash(source->getName()) % INT_MAX;
        while (true) {
            // avoid negative values
            if (id < 0) {
                id = -id;
            }
            // avoid zero, it means unset
            if (!id) {
                id = 1;
            }
            // check for collisions
            bool collision = false;
            BOOST_FOREACH(const string &other, m_client.getSyncSources()) {
                boost::shared_ptr<PersistentSyncSourceConfig> sc(m_client.getSyncSourceConfig(other));
                int other_id = sc->getSynthesisID();
                if (other_id == id) {
                    ++id;
                    collision = true;
                    break;
                }
            }
            if (!collision) {
                source->setSynthesisID(id);
                return;
            }
        }
    }

public:
    /** allow iterating over sources */
    const inherited *getSourceSet() const { return this; }

    LogLevel getLogLevel() const { return m_logLevel; }
    void setLogLevel(LogLevel logLevel) { m_logLevel = logLevel; }

    /**
     * Dump into files with a certain suffix, optionally store report
     * in member of SyncSourceReport. Remembers which sources were
     * dumped before a sync and only dumps those again afterward.
     *
     * @param suffix        "before/after/current" - before sync, after sync, during status check
     * @param excludeSource when not empty, only dump that source
     */
    void dumpDatabases(const string &suffix,
                       BackupReport SyncSourceReport::*report,
                       const string &excludeSource = "") {
        // Identify all logdirs of current context, of any peer.  Used
        // to search for previous backups of each source, if
        // necessary.
        SyncContext context(m_client.getContextName());
        boost::shared_ptr<LogDir> logdir(LogDir::create(context));
        vector<string> dirs;
        logdir->previousLogdirs(dirs);

        BOOST_FOREACH(SyncSource *source, *this) {
            if ((!excludeSource.empty() && excludeSource != source->getName()) ||
                (suffix == "after" && m_prepared.find(source->getName()) == m_prepared.end())) {
                continue;
            }

            string dir = databaseName(*source, suffix);
            boost::shared_ptr<ConfigNode> node = ConfigNode::createFileNode(dir + ".ini");
            SE_LOG_DEBUG(NULL, "creating %s", dir.c_str());
            rm_r(dir);
            BackupReport dummy;
            if (source->getOperations().m_backupData) {
                SyncSource::Operations::ConstBackupInfo oldBackup;
                // Now look for a backup of the current source,
                // starting with the most recent one.
                for (vector<string>::const_reverse_iterator it = dirs.rbegin();
                     it != dirs.rend();
                     ++it) {
                    const string &sessiondir = *it;
                    string oldBackupDir;
                    SyncSource::Operations::BackupInfo::Mode mode =
                        SyncSource::Operations::BackupInfo::BACKUP_AFTER;
                    oldBackupDir = databaseName(*source, "after", sessiondir);
                    if (!isDir(oldBackupDir)) {
                        mode = SyncSource::Operations::BackupInfo::BACKUP_BEFORE;
                        oldBackupDir = databaseName(*source, "before", sessiondir);
                        if (!isDir(oldBackupDir)) {
                            // try next session
                            continue;
                        }
                    }

                    oldBackup.m_mode = mode;
                    oldBackup.m_dirname = oldBackupDir;
                    oldBackup.m_node = ConfigNode::createFileNode(oldBackupDir + ".ini");
                    break;
                }
                mkdir_p(dir);
                SyncSource::Operations::BackupInfo newBackup(suffix == "before" ?
                                                             SyncSource::Operations::BackupInfo::BACKUP_BEFORE :
                                                             suffix == "after" ?
                                                             SyncSource::Operations::BackupInfo::BACKUP_AFTER :
                                                             SyncSource::Operations::BackupInfo::BACKUP_OTHER,
                                                             dir, node);
                source->getOperations().m_backupData(oldBackup, newBackup,
                                                     report ? source->*report : dummy);
                SE_LOG_DEBUG(NULL, "%s created", dir.c_str());

                // remember that we have dumped at the beginning of a sync
                if (suffix == "before") {
                    m_prepared.insert(source->getName());
                }
            }
        }
    }

    void restoreDatabase(SyncSource &source, const string &suffix, bool dryrun, SyncSourceReport &report)
    {
        string dir = databaseName(source, suffix);
        boost::shared_ptr<ConfigNode> node = ConfigNode::createFileNode(dir + ".ini");
        if (!node->exists()) {
            Exception::throwError(SE_HERE, dir + ": no such database backup found");
        }
        if (source.getOperations().m_restoreData) {
            source.getOperations().m_restoreData(SyncSource::Operations::ConstBackupInfo(SyncSource::Operations::BackupInfo::BACKUP_OTHER, dir, node),
                                                 dryrun, report);
        }
    }

    SourceList(SyncContext &client, bool doLogging) :
        m_logdir(LogDir::create(client)),
        m_client(client),
        m_doLogging(doLogging),
        m_reportTodo(true),
        m_logLevel(LOGGING_FULL)
    {
    }
    
    // call as soon as logdir settings are known
    void startSession(const string &logDirPath, int maxlogdirs, int logLevel, SyncReport *report) {
        m_logdir->setLogdir(logDirPath);
        m_previousLogdir = m_logdir->previousLogdir();
        if (m_doLogging) {
            m_logdir->startSession(logDirPath, LogDir::SESSION_CREATE, maxlogdirs, logLevel, report);
        } else {
            // Run debug session without paying attention to
            // the normal logdir handling. The log level here
            // refers to stdout. The log file will be as complete
            // as possible.
            m_logdir->startSession(logDirPath, LogDir::SESSION_USE_PATH, 0, 1, report);
        }
    }

    /** read-only access to existing session, identified in logDirPath */
    void accessSession(const string &logDirPath) {
        m_logdir->setLogdir(logDirPath);
        m_previousLogdir = m_logdir->previousLogdir();
        m_logdir->startSession(logDirPath, LogDir::SESSION_READ_ONLY, 0, 0, NULL);
    }


    /** return log directory, empty if not enabled */
    const string &getLogdir() {
        return m_logdir->getLogdir();
    }

    /** return previous log dir found in startSession() */
    const string &getPrevLogdir() const { return m_previousLogdir; }

    /** set directory for database files without actually redirecting the logging */
    void setPath(const string &path) { m_logdir->setPath(path); }

    /**
     * If possible (directory to compare against available) and enabled,
     * then dump changes applied locally.
     *
     * @param oldSession     directory to compare against; "" searches in sessions of current peer
     *                       as selected by context for the lastest one involving each source
     * @param oldSuffix      suffix of old database dump: usually "after"
     * @param currentSuffix  the current database dump suffix: "current"
     *                       when not doing a sync, otherwise "before"
     * @param excludeSource  when not empty, only dump that source
     */
    bool dumpLocalChanges(const string &oldSession,
                          const string &oldSuffix, const string &newSuffix,
                          const string &excludeSource,
                          const string &intro = "Local data changes to be applied remotely during synchronization:\n",
                          const string &config = "CLIENT_TEST_LEFT_NAME='after last sync' CLIENT_TEST_RIGHT_NAME='current data' CLIENT_TEST_REMOVED='removed since last sync' CLIENT_TEST_ADDED='added since last sync'") {
        if (m_logLevel <= LOGGING_SUMMARY) {
            return false;
        }

        vector<string> dirs;
        if (oldSession.empty()) {
            m_logdir->previousLogdirs(dirs);
        }

        BOOST_FOREACH(SyncSource *source, *this) {
            if ((!excludeSource.empty() && excludeSource != source->getName()) ||
                (newSuffix == "after" && m_prepared.find(source->getName()) == m_prepared.end())) {
                continue;
            }

            // dump only if not done before or changed
            if (m_intro != intro) {
                SE_LOG_SHOW(NULL, "%s", intro.c_str());
                m_intro = intro;
            }

            string oldDir;
            if (oldSession.empty()) {
                // Now look for the latest session involving the current source,
                // starting with the most recent one.
                for (vector<string>::const_reverse_iterator it = dirs.rbegin();
                     it != dirs.rend();
                     ++it) {
                    const string &sessiondir = *it;
                    boost::shared_ptr<LogDir> oldsession(LogDir::create(m_client));
                    oldsession->openLogdir(sessiondir);
                    SyncReport report;
                    oldsession->readReport(report);
                    if (report.find(source->getName()) != report.end())  {
                        // source was active in that session, use dump
                        // made there
                        oldDir = databaseName(*source, oldSuffix, sessiondir);
                        break;
                    }
                }
            } else {
                oldDir = databaseName(*source, oldSuffix, oldSession);
            }
            string newDir = databaseName(*source, newSuffix);
            SE_LOG_SHOW(NULL, "*** %s ***", source->getDisplayName().c_str());
            string cmd = string("env CLIENT_TEST_COMPARISON_FAILED=10 " + config + " synccompare '" ) +
                oldDir + "' '" + newDir + "'";
            int ret = Execute(cmd, EXECUTE_NO_STDERR);
            switch (ret == -1 ? ret :
                    WIFEXITED(ret) ? WEXITSTATUS(ret) :
                    -1) {
            case 0:
                SE_LOG_SHOW(NULL, "no changes");
                break;
            case 10:
                break;
            default:
                SE_LOG_SHOW(NULL, "Comparison was impossible.");
                break;
            }
        }
        SE_LOG_SHOW(NULL, "\n");
        return true;
    }

    // call when all sync sources are ready to dump
    // pre-sync databases
    // @param sourceName   limit preparation to that source
    void syncPrepare(const string &sourceName) {
        if (m_prepared.find(sourceName) != m_prepared.end()) {
            // data dump was already done (can happen when running multiple
            // SyncML sessions)
            return;
        }

        if (m_logdir->getLogfile().size() &&
            m_doLogging &&
            (m_client.getDumpData() || m_client.getPrintChanges())) {
            // dump initial databases
            SE_LOG_INFO(NULL, "creating complete data backup of datastore %s before sync (%s)",
                        sourceName.c_str(),
                        (m_client.getDumpData() && m_client.getPrintChanges()) ? "enabled with dumpData and needed for printChanges" :
                        m_client.getDumpData() ? "because it was enabled with dumpData" :
                        m_client.getPrintChanges() ? "needed for printChanges" :
                        "???");
            dumpDatabases("before", &SyncSourceReport::m_backupBefore, sourceName);
            if (m_client.getPrintChanges()) {
                // compare against the old "after" database dump
                dumpLocalChanges("", "after", "before", sourceName,
                                 StringPrintf("%s data changes to be applied during synchronization:\n",
                                              m_client.isLocalSync() ? m_client.getContextName().c_str() : "Local"));
            }
        }
    }

    // call at the end of a sync with success == true
    // if all went well to print report
    void syncDone(SyncMLStatus status, SyncReport *report) {
        // record status - failures from now only affect post-processing
        // and thus do no longer change that result
        if (report) {
            report->setStatus(status == 0 ? STATUS_HTTP_OK : status);
        }

        // dump database after sync if explicitly enabled or
        // needed for comparison;
        // in the latter case only if dumping it at the beginning completed
        if (m_doLogging &&
            (m_client.getDumpData() ||
             (m_client.getPrintChanges() && m_reportTodo && !m_prepared.empty()))) {
            try {
                SE_LOG_INFO(NULL, "creating complete data backup after sync (%s)",
                            (m_client.getDumpData() && m_client.getPrintChanges()) ? "enabled with dumpData and needed for printChanges" :
                            m_client.getDumpData() ? "because it was enabled with dumpData" :
                            m_client.getPrintChanges() ? "needed for printChanges" :
                            "???");
                dumpDatabases("after", &SyncSourceReport::m_backupAfter);
            } catch (...) {
                Exception::handle();
                // not exactly sure what the problem was, but don't
                // try it again
                m_prepared.clear();
            }
        }

        if (m_doLogging) {
            if (m_reportTodo && !m_prepared.empty() && report) {
                // update report with more recent information about m_backupAfter
                updateSyncReport(*report);
            }

            // ensure that stderr is seen again
            m_logdir->restore();

            // write out session status
            m_logdir->endSession();

            if (m_reportTodo) {
                // haven't looked at result of sync yet;
                // don't do it again
                m_reportTodo = false;

                string logfile = m_logdir->getLogfile();
                if (status == STATUS_OK) {
                    SE_LOG_SHOW(NULL, "\nSynchronization successful.");
                } else if (logfile.size()) {
                    SE_LOG_SHOW(NULL, "\nSynchronization failed, see %s for details.",
                                logfile.c_str());
                } else {
                    SE_LOG_SHOW(NULL, "\nSynchronization failed.");
                }

                // pretty-print report
                if (m_logLevel > LOGGING_QUIET) {
                    std::string procname = Logger::getProcessName();
                    SE_LOG_SHOW(NULL, "\nChanges applied during synchronization%s%s%s:",
                                procname.empty() ? "" : " (",
                                procname.c_str(),
                                procname.empty() ? "" : ")");
                }
                if (m_logLevel > LOGGING_QUIET && report) {
                    ostringstream out;
                    out << *report;
                    std::string slowSync = report->slowSyncExplanation(m_client.getPeer());
                    if (!slowSync.empty()) {
                        out << endl << slowSync;
                    }
                    SE_LOG_SHOW(NULL, "%s", out.str().c_str());
                }

                // compare databases?
                if (m_client.getPrintChanges()) {
                    dumpLocalChanges(m_logdir->getLogdir(),
                                     "before", "after", "",
                                     StringPrintf("\nData modified %s during synchronization:\n",
                                                  m_client.isLocalSync() ? m_client.getContextName().c_str() : "locally"),
                                     "CLIENT_TEST_LEFT_NAME='before sync' CLIENT_TEST_RIGHT_NAME='after sync' CLIENT_TEST_REMOVED='removed during sync' CLIENT_TEST_ADDED='added during sync'");
                }

                // now remove some old logdirs
                m_logdir->expire();
            }
        } else {
            // finish debug session
            m_logdir->restore();
            m_logdir->endSession();
        }
    }

    /** copies information about sources into sync report */
    void updateSyncReport(SyncReport &report) {
        BOOST_FOREACH(SyncSource *source, *this) {
            report.addSyncSourceReport(source->getName(), *source);
        }
    }

    /** returns names of active sources */
    set<string> getSources() {
        set<string> res;

        BOOST_FOREACH(SyncSource *source, *this) {
            res.insert(source->getName());
        }
        return res;
    }
   
    ~SourceList() {
        // free sync sources
        BOOST_FOREACH(SyncSource *source, *this) {
            delete source;
        }
    }

    /** find sync source by name (both normal and virtual sources) */
    SyncSource *operator [] (const string &name) {
        BOOST_FOREACH(SyncSource *source, *this) {
            if (name == source->getName()) {
                return source;
            }
        }
        BOOST_FOREACH(boost::shared_ptr<VirtualSyncSource> &source, m_virtualSources) {
            if (name == source->getName()) {
                return source.get();
            }
        }
        return NULL;
    }

    /** find by XML <dbtypeid> (the ID used by Synthesis to identify sources in progress events) */
    SyncSource *lookupBySynthesisID(int synthesisid) {
        BOOST_FOREACH(SyncSource *source, *this) {
            if (source->getSynthesisID() == synthesisid) {
                return source;
            }
        }
        BOOST_FOREACH(boost::shared_ptr<VirtualSyncSource> &source, m_virtualSources) {
            if (source->getSynthesisID() == synthesisid) {
                return source.get();
            }
        }
        return NULL;
    }

    std::list<std::string> getSourceNames() const;
};

std::list<std::string> SourceList::getSourceNames() const
{
    std::list<std::string> sourceNames;
    BOOST_FOREACH (SyncSource *source, *this) {
        sourceNames.push_back(source->getName());
    }
    return sourceNames;
}

void unref(SourceList *sourceList)
{
    delete sourceList;
}

UserInterface &SyncContext::getUserInterfaceNonNull()
{
    if (m_userInterface) {
        return *m_userInterface;
    } else {
        // Doesn't use keyring.
        static SimpleUserInterface dummy("0");
        return dummy;
    }
}

void SyncContext::requestAnotherSync()
{
    if (m_activeContext &&
        m_activeContext->m_engine.get() &&
        m_activeContext->m_session) {
        SharedKey sessionKey =
            m_activeContext->m_engine.OpenSessionKey(m_activeContext->m_session);
        m_activeContext->m_engine.SetInt32Value(sessionKey,
                                                "restartsync",
                                                true);
    }
}

const std::vector<SyncSource *> *SyncContext::getSources() const
{
    return m_sourceListPtr ?
        m_sourceListPtr->getSourceSet() :
        NULL;
}

string SyncContext::getUsedSyncURL() {
    vector<string> urls = getSyncURL();
    BOOST_FOREACH (string url, urls) {
        if (boost::starts_with(url, "http://") ||
                boost::starts_with(url, "https://")) {
#ifdef ENABLE_LIBSOUP
            return url;
#elif defined(ENABLE_LIBCURL)
            return url;
#endif
        } else if (url.find("obex-bt://") ==0) {
#ifdef ENABLE_BLUETOOTH
            return url;
#endif
        } else if (boost::starts_with(url, "local://")) {
            return url;
        }
    }
    return "";
}

static void CancelTransport(TransportAgent *agent, SuspendFlags &flags)
{
    if (flags.getState() == SuspendFlags::ABORT) {
        SE_LOG_DEBUG(NULL, "CancelTransport: cancelling because of SuspendFlags::ABORT");
        agent->cancel();
    }
}

/**
 * common initialization for all kinds of transports, to be called
 * before using them
 */
static void InitializeTransport(const boost::shared_ptr<TransportAgent> &agent,
                                int timeout)
{
    agent->setTimeout(timeout);

    // Automatically call cancel() when we an abort request
    // is detected. Relies of automatic connection management
    // to disconnect when agent is deconstructed.
    SuspendFlags &flags(SuspendFlags::getSuspendFlags());
    flags.m_stateChanged.connect(SuspendFlags::StateChanged_t::slot_type(CancelTransport, agent.get(), _1).track(agent));
}

boost::shared_ptr<TransportAgent> SyncContext::createTransportAgent(void *gmainloop)
{
    string url = getUsedSyncURL();
    m_retryInterval = getRetryInterval();
    m_retryDuration = getRetryDuration();
    int timeout = m_serverMode ? m_retryDuration : min(m_retryInterval, m_retryDuration);

    if (m_localSync) {
        string peer = url.substr(strlen("local://"));
        boost::shared_ptr<LocalTransportAgent> agent(LocalTransportAgent::create(this, peer, gmainloop));
        InitializeTransport(agent, timeout);
        agent->start();
        return agent;
    } else if (boost::starts_with(url, "http://") ||
        boost::starts_with(url, "https://")) {
#ifdef ENABLE_LIBSOUP
        boost::shared_ptr<SoupTransportAgent> agent(new SoupTransportAgent(static_cast<GMainLoop *>(gmainloop)));
        agent->setConfig(*this);
        InitializeTransport(agent, timeout);
        return agent;
#elif defined(ENABLE_LIBCURL)
        boost::shared_ptr<CurlTransportAgent> agent(new CurlTransportAgent());
        agent->setConfig(*this);
        InitializeTransport(agent, timeout);
        return agent;
#endif
    } else if (url.find("obex-bt://") ==0) {
#ifdef ENABLE_BLUETOOTH
        std::string btUrl = url.substr (strlen ("obex-bt://"), std::string::npos);
        boost::shared_ptr<ObexTransportAgent> agent(new ObexTransportAgent(ObexTransportAgent::OBEX_BLUETOOTH,
                                                                           static_cast<GMainLoop *>(gmainloop)));
        agent->setURL (btUrl);
        InitializeTransport(agent, timeout);
        // this will block already
        agent->connect();
        return agent;
#endif
    }

    SE_THROW("unsupported transport type is specified in the configuration");
}

void SyncContext::displayServerMessage(const string &message)
{
    SE_LOG_INFO(NULL, "message from server: %s", message.c_str());
}

void SyncContext::displaySyncProgress(sysync::TProgressEventEnum type,
                                              int32_t extra1, int32_t extra2, int32_t extra3)
{
    
}

bool SyncContext::displaySourceProgress(SyncSource &source,
                                        const SyncSourceEvent &event,
                                        bool flush)
{
    if (!flush) {
        // Certain events do not need to be printed immediately.
        // For example, instead of multiple PEV_ITEMRECEIVED events
        //   foo: received 1/100
        //   foo: received 2/100
        //   foo: ...
        //   foo: received 100/100
        // it is better to just print one:
        //   foo: received 100/100
        switch (event.m_type) {
        case sysync::PEV_ITEMPROCESSED:
            // Ignore this one completely. There is one such event
            // after each PEV_ITEMRECEIVED, so processing
            // PEV_ITEMPROCESSED would break the merging of
            // PEV_ITEMRECEIVED, at least the way it is implemented
            // now. PEV_ITEMPROCESSED also doesn't add much
            // information.
            return true;
        case sysync::PEV_DELETING:
        case sysync::PEV_ITEMRECEIVED:
        case sysync::PEV_ITEMSENT:
            // Flush when switching to a different event type or source.
            if (m_sourceEvent.m_type != sysync::PEV_NOP &&
                (m_sourceEvent.m_type != event.m_type ||
                 m_sourceProgress != &source)) {
                displaySourceProgress(*m_sourceProgress, m_sourceEvent, true);
            }
            m_sourceEvent.m_type = event.m_type;
            m_sourceEvent.m_extra1 = event.m_extra1;
            m_sourceEvent.m_extra2 = event.m_extra2;
            m_sourceEvent.m_extra3 = event.m_extra3;
            m_sourceProgress = &source;
            return true;
            break;
        default:
            if (m_sourceEvent.m_type != sysync::PEV_NOP) {
                displaySourceProgress(*m_sourceProgress, m_sourceEvent, true);
                m_sourceEvent.m_type = sysync::PEV_NOP;
            }
            break;
        }
    }

    switch(event.m_type) {
    case sysync::PEV_PREPARING:
        /* preparing (e.g. preflight in some clients), extra1=progress, extra2=total */
        /* extra2 might be zero */
        /*
         * At the moment, preparing items doesn't do any real work.
         * Printing this progress just increases the output and slows
         * us down. Disabled.
         */
        if (true || source.getFinalSyncMode() == SYNC_NONE) {
            // not active, suppress output
        } else if (event.m_extra2) {
            SE_LOG_INFO(NULL, "%s: preparing %d/%d",
                        source.getDisplayName().c_str(), event.m_extra1, event.m_extra2);
        } else {
            SE_LOG_INFO(NULL, "%s: preparing %d",
                        source.getDisplayName().c_str(), event.m_extra1);
        }
        break;
    case sysync::PEV_DELETING:
        /* deleting (zapping datastore), extra1=progress, extra2=total */
        if (event.m_extra2) {
            SE_LOG_INFO(NULL, "%s: deleting %d/%d",
                        source.getDisplayName().c_str(), event.m_extra1, event.m_extra2);
        } else {
            SE_LOG_INFO(NULL, "%s: deleting %d",
                        source.getDisplayName().c_str(), event.m_extra1);
        }
        break;
    case sysync::PEV_ALERTED: {
        /* datastore alerted (extra1=0 for normal, 1 for slow, 2 for first time slow, 
           extra2=1 for resumed session,
           extra3 0=twoway, 1=fromserver, 2=fromclient */
        // -1 is used for alerting a restore from backup. Synthesis won't use this
        bool peerIsClient = getPeerIsClient();
        if (event.m_extra1 != -1) {
            SE_LOG_INFO(NULL, "%s: %s %s sync%s (%s)",
                        source.getDisplayName().c_str(),
                        event.m_extra2 ? "resuming" : "starting",
                        event.m_extra1 == 0 ? "normal" :
                        event.m_extra1 == 1 ? "slow" :
                        event.m_extra1 == 2 ? "first time" :
                        "unknown",
                        event.m_extra3 == 0 ? ", two-way" :
                        event.m_extra3 == 1 ? " from server" :
                        event.m_extra3 == 2 ? " from client" :
                        ", unknown direction",
                        peerIsClient ? "peer is client" : "peer is server");
         
            SimpleSyncMode mode = SIMPLE_SYNC_NONE;
            SyncMode sync = StringToSyncMode(source.getSync());
            switch (event.m_extra1) {
            case 0:
                switch (event.m_extra3) {
                case 0:
                    mode = SIMPLE_SYNC_TWO_WAY;
                    if (m_serverMode &&
                        m_serverAlerted) {
                        if (sync == SYNC_ONE_WAY_FROM_SERVER ||
                            sync == SYNC_ONE_WAY_FROM_LOCAL) {
                            // As in the slow/refresh-from-server case below,
                            // pretending to do a two-way incremental sync
                            // is a correct way of executing the requested
                            // one-way sync, as long as the client doesn't
                            // send any of its own changes. The Synthesis
                            // engine does that.
                            mode = SIMPLE_SYNC_ONE_WAY_FROM_LOCAL;
                        } else if (sync == SYNC_LOCAL_CACHE_SLOW ||
                                   sync == SYNC_LOCAL_CACHE_INCREMENTAL) {
                            mode = SIMPLE_SYNC_LOCAL_CACHE_INCREMENTAL;
                        }
                    }
                    break;
                case 1:
                    mode = peerIsClient ? SIMPLE_SYNC_ONE_WAY_FROM_LOCAL : SIMPLE_SYNC_ONE_WAY_FROM_REMOTE;
                    break;
                case 2:
                    mode = peerIsClient ? SIMPLE_SYNC_ONE_WAY_FROM_REMOTE : SIMPLE_SYNC_ONE_WAY_FROM_LOCAL;
                    break;
                }
                break;
            case 1:
            case 2:
                switch (event.m_extra3) {
                case 0:
                    mode = SIMPLE_SYNC_SLOW;
                    if (m_serverMode &&
                        m_serverAlerted) {
                        if (sync == SYNC_REFRESH_FROM_SERVER ||
                            sync == SYNC_REFRESH_FROM_LOCAL) {
                            // We run as server and told the client to refresh
                            // its data. A slow sync is how some clients (the
                            // Synthesis engine included) execute that sync mode;
                            // let's be optimistic and assume that the client
                            // did as it was told and deleted its data.
                            mode = SIMPLE_SYNC_REFRESH_FROM_LOCAL;
                        } else if (sync == SYNC_LOCAL_CACHE_SLOW ||
                                   sync == SYNC_LOCAL_CACHE_INCREMENTAL) {
                            mode = SIMPLE_SYNC_LOCAL_CACHE_SLOW;
                        }
                    }
                    break;
                case 1:
                    mode = peerIsClient ? SIMPLE_SYNC_REFRESH_FROM_LOCAL : SIMPLE_SYNC_REFRESH_FROM_REMOTE;
                    break;
                case 2:
                    mode = peerIsClient ? SIMPLE_SYNC_REFRESH_FROM_REMOTE : SIMPLE_SYNC_REFRESH_FROM_LOCAL;
                    break;
                }
                break;
            }

            if (SyncMode(mode) != SYNC_NONE) {
                SE_LOG_DEBUG(NULL, "reading: set read-ahead based on sync mode %s",
                             PrettyPrintSyncMode(SyncMode(mode)).c_str());
                switch (mode) {
                case SIMPLE_SYNC_NONE:
                case SIMPLE_SYNC_INVALID:
                case SIMPLE_SYNC_RESTORE_FROM_BACKUP:
                case SIMPLE_SYNC_ONE_WAY_FROM_REMOTE:
                case SIMPLE_SYNC_REFRESH_FROM_REMOTE:
                case SIMPLE_SYNC_LOCAL_CACHE_INCREMENTAL:
                    source.setReadAheadOrder(SyncSourceBase::READ_NONE);
                    break;
                case SIMPLE_SYNC_TWO_WAY:
                case SIMPLE_SYNC_ONE_WAY_FROM_LOCAL:
                    source.setReadAheadOrder(SyncSourceBase::READ_CHANGED_ITEMS);
                    break;
                case SIMPLE_SYNC_SLOW:
                case SIMPLE_SYNC_REFRESH_FROM_LOCAL:
                case SIMPLE_SYNC_LOCAL_CACHE_SLOW:
                    source.setReadAheadOrder(SyncSourceBase::READ_ALL_ITEMS);
                    break;
                }
            }
            if (source.getFinalSyncMode() == SYNC_NONE) {
                source.recordFinalSyncMode(SyncMode(mode));
                source.recordFirstSync(event.m_extra1 == 2);
                source.recordResumeSync(event.m_extra2 == 1);
            } else if (SyncMode(mode) != SYNC_NONE) {
                // Broadcast statistics before moving into next cycle.
                m_sourceSyncedSignal(source.getName(), source);
                // may happen when the source is used in multiple
                // SyncML sessions; only remember the initial sync
                // mode in that case and count all following syncs
                // (they should only finish the work of the initial
                // one)
                source.recordRestart();
                if (m_serverMode) {
                    // Done with first cycle, revert to normal photo
                    // handling if it was disabled.
                    SharedKey sessionKey = m_engine.OpenSessionKey(m_session);
                    SharedKey contextKey = m_engine.OpenKeyByPath(sessionKey, "/sessionvars");
                    m_engine.SetInt32Value(contextKey, "keepPhotoData", false);
                }
                // Reset "started" flags for PEV_SYNCSTART.
                m_sourceStarted.clear();
            }
        } else {
            SE_LOG_INFO(NULL, "%s: restore from backup", source.getDisplayName().c_str());
            source.recordFinalSyncMode(SYNC_RESTORE_FROM_BACKUP);
        }
        break;
    }
    case sysync::PEV_SYNCSTART:
        /* sync started */
        /* Get's triggered by libsynthesis frequently. Limit it to once per sync cycle. */
        if (m_sourceStarted.find(source.getName()) == m_sourceStarted.end()) {
            SE_LOG_INFO(NULL, "%s: started",
                        source.getDisplayName().c_str());
            m_sourceStarted.insert(source.getName());
        }
        break;
    case sysync::PEV_ITEMRECEIVED:
        /* item received, extra1=current item count,
           extra2=number of expected changes (if >= 0) */
        if (source.getFinalSyncMode() == SYNC_NONE) {
        } else if (event.m_extra2 > 0) {
            SE_LOG_INFO(NULL, "%s: received %d/%d",
                        source.getDisplayName().c_str(), event.m_extra1, event.m_extra2);
        } else {
            SE_LOG_INFO(NULL, "%s: received %d",
                        source.getDisplayName().c_str(), event.m_extra1);
        }
        source.recordTotalNumItemsReceived(event.m_extra1);
        break;
    case sysync::PEV_ITEMSENT:
        /* item sent,     extra1=current item count,
           extra2=number of expected items to be sent (if >=0) */
        if (source.getFinalSyncMode() == SYNC_NONE) {
        } else if (event.m_extra2 > 0) {
            SE_LOG_INFO(NULL, "%s: sent %d/%d",
                        source.getDisplayName().c_str(), event.m_extra1, event.m_extra2);
        } else {
            SE_LOG_INFO(NULL, "%s: sent %d",
                        source.getDisplayName().c_str(), event.m_extra1);
        }
        source.recordTotalNumItemsSent(event.m_extra1);
        break;
    // Not reached, see above.
    case sysync::PEV_ITEMPROCESSED:
        /* item locally processed,               extra1=# added, 
           extra2=# updated,
           extra3=# deleted */
        if (source.getFinalSyncMode() == SYNC_NONE) {
        } else if (source.getFinalSyncMode() != SYNC_NONE) {
            SE_LOG_INFO(NULL, "%s: added %d, updated %d, removed %d",
                        source.getDisplayName().c_str(), event.m_extra1, event.m_extra2, event.m_extra3);
        }
        break;
    case sysync::PEV_SYNCEND: {
        /* sync finished, probably with error in extra1 (0=ok),
           syncmode in extra2 (0=normal, 1=slow, 2=first time), 
           extra3=1 for resumed session) */
        if (source.getFinalSyncMode() == SYNC_NONE) {
            SE_LOG_INFO(NULL, "%s: inactive", source.getDisplayName().c_str());
        } else if(source.getFinalSyncMode() == SYNC_RESTORE_FROM_BACKUP) {
            SE_LOG_INFO(NULL, "%s: restore done %s", 
                        source.getDisplayName().c_str(),
                        event.m_extra1 ? "unsuccessfully" : "successfully" );
        } else {
            SE_LOG_INFO(NULL, "%s: %s%s sync done %s",
                        source.getDisplayName().c_str(),
                        event.m_extra3 ? "resumed " : "",
                        event.m_extra2 == 0 ? "normal" :
                        event.m_extra2 == 1 ? "slow" :
                        event.m_extra2 == 2 ? "first time" :
                        "unknown",
                        event.m_extra1 ? "unsuccessfully" : "successfully");
        }
        int32_t extra1 = event.m_extra1;
        switch (extra1) {
        case 401:
            // TODO: reset cached password
            SE_LOG_INFO(NULL, "authorization failed, check username '%s' and password", getSyncUser().toString().c_str());
            break;
        case 403:
            SE_LOG_INFO(source.getDisplayName(), "log in succeeded, but server refuses access - contact server operator");
            break;
        case 407:
            SE_LOG_INFO(NULL, "proxy authorization failed, check proxy username and password");
            break;
        case 404:
            SE_LOG_INFO(source.getDisplayName(), "server database not found, check URI '%s'", source.getURINonEmpty().c_str());
            break;
        case 0:
            break;
        case sysync::LOCERR_DATASTORE_ABORT:
            // this can mean only one thing in SyncEvolution: unexpected slow sync
            extra1 = STATUS_UNEXPECTED_SLOW_SYNC;
            // no break!
        default:
            // Printing unknown status codes here is of somewhat questionable value,
            // because even "good" sources will get a bad status when the overall
            // session turns bad. We also don't have good explanations for the
            // status here.
            SE_LOG_ERROR(source.getDisplayName(), "%s", Status2String(SyncMLStatus(event.m_extra1)).c_str());
            break;
        }
        source.recordStatus(SyncMLStatus(extra1));
        break;
    }
    case sysync::PEV_DSSTATS_L:
        /* datastore statistics for local       (extra1=# added, 
           extra2=# updated,
           extra3=# deleted) */
        source.setItemStat(SyncSource::ITEM_LOCAL,
                           SyncSource::ITEM_ADDED,
                           SyncSource::ITEM_TOTAL,
                           event.m_extra1);
        source.setItemStat(SyncSource::ITEM_LOCAL,
                           SyncSource::ITEM_UPDATED,
                           SyncSource::ITEM_TOTAL,
                           event.m_extra2);
        source.setItemStat(SyncSource::ITEM_LOCAL,
                           SyncSource::ITEM_REMOVED,
                           SyncSource::ITEM_TOTAL,
                           // Synthesis engine doesn't count locally
                           // deleted items during
                           // refresh-from-server/client. That's a matter of
                           // taste. In SyncEvolution we'd like these
                           // items to show up, so add it here.
                           (source.getFinalSyncMode() == (m_serverMode ? SYNC_REFRESH_FROM_CLIENT : SYNC_REFRESH_FROM_SERVER) ||
                            source.getFinalSyncMode() == SYNC_REFRESH_FROM_REMOTE) ?
                           source.getNumDeleted() :
                           event.m_extra3);
        break;
    case sysync::PEV_DSSTATS_R:
        /* datastore statistics for remote      (extra1=# added, 
           extra2=# updated,
           extra3=# deleted) */
        source.setItemStat(SyncSource::ITEM_REMOTE,
                           SyncSource::ITEM_ADDED,
                           SyncSource::ITEM_TOTAL,
                           event.m_extra1);
        source.setItemStat(SyncSource::ITEM_REMOTE,
                           SyncSource::ITEM_UPDATED,
                           SyncSource::ITEM_TOTAL,
                           event.m_extra2);
        source.setItemStat(SyncSource::ITEM_REMOTE,
                           SyncSource::ITEM_REMOVED,
                           SyncSource::ITEM_TOTAL,
                           event.m_extra3);
        break;
    case sysync::PEV_DSSTATS_E:
        /* datastore statistics for local/remote rejects (extra1=# locally rejected, 
           extra2=# remotely rejected) */
        source.setItemStat(SyncSource::ITEM_LOCAL,
                           SyncSource::ITEM_ANY,
                           SyncSource::ITEM_REJECT,
                           event.m_extra1);
        source.setItemStat(SyncSource::ITEM_REMOTE,
                           SyncSource::ITEM_ANY,
                           SyncSource::ITEM_REJECT,
                           event.m_extra2);
        break;
    case sysync::PEV_DSSTATS_S:
        /* datastore statistics for server slowsync  (extra1=# slowsync matches) */
        source.setItemStat(SyncSource::ITEM_REMOTE,
                           SyncSource::ITEM_ANY,
                           SyncSource::ITEM_MATCH,
                           event.m_extra1);
        break;
    case sysync::PEV_DSSTATS_C:
        /* datastore statistics for server conflicts (extra1=# server won,
           extra2=# client won,
           extra3=# duplicated) */
        source.setItemStat(SyncSource::ITEM_REMOTE,
                           SyncSource::ITEM_ANY,
                           SyncSource::ITEM_CONFLICT_SERVER_WON,
                           event.m_extra1);
        source.setItemStat(SyncSource::ITEM_REMOTE,
                           SyncSource::ITEM_ANY,
                           SyncSource::ITEM_CONFLICT_CLIENT_WON,
                           event.m_extra2);
        source.setItemStat(SyncSource::ITEM_REMOTE,
                           SyncSource::ITEM_ANY,
                           SyncSource::ITEM_CONFLICT_DUPLICATED,
                           event.m_extra3);
        break;
    case sysync::PEV_DSSTATS_D:
        /* datastore statistics for data   volume    (extra1=outgoing bytes,
           extra2=incoming bytes) */
        source.setItemStat(SyncSource::ITEM_LOCAL,
                           SyncSource::ITEM_ANY,
                           SyncSource::ITEM_SENT_BYTES,
                           event.m_extra1);
        source.setItemStat(SyncSource::ITEM_LOCAL,
                           SyncSource::ITEM_ANY,
                           SyncSource::ITEM_RECEIVED_BYTES,
                           event.m_extra2);
        break;
    case sysync::PEV_NOP:
        // Handled, do not process further.
        return true;
        break;
    default:
        SE_LOG_DEBUG(NULL, "%s: progress event %d, extra %d/%d/%d",
                     source.getDisplayName().c_str(),
                     event.m_type, event.m_extra1, event.m_extra2, event.m_extra3);
    }
    return false;
}

/*
 * There have been segfaults inside glib in the background
 * thread which ran the second event loop. Disabled it again,
 * even though the synchronous EDS API calls will block then
 * when EDS dies.
 */
#if 0 && defined(HAVE_GLIB) && defined(HAVE_EDS)
# define RUN_GLIB_LOOP
#endif

#ifdef RUN_GLIB_LOOP
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

void SyncContext::startLoopThread()
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

SyncSource *SyncContext::findSource(const std::string &name)
{
    if (!m_activeContext || !m_activeContext->m_sourceListPtr) {
        return NULL;
    }
    const char *realname = strrchr(name.c_str(), m_findSourceSeparator);
    if (realname) {
        realname++;
    } else {
        realname = name.c_str();
    }
    return (*m_activeContext->m_sourceListPtr)[realname];
}

SyncContext *SyncContext::findContext(const char *sessionName)
{
    return m_activeContext;
}

void SyncContext::initSources(SourceList &sourceList)
{
    list<string> configuredSources = getSyncSources();
    map<string, string> subSources;

    // Disambiguate source names because we have multiple with the same
    // name active?
    string contextName;
    if (m_localSync) {
        contextName = getContextName();
    }

    // Phase 1, check all virtual sync soruces
    BOOST_FOREACH(const string &name, configuredSources) {
        boost::shared_ptr<PersistentSyncSourceConfig> sc(getSyncSourceConfig(name));
        SyncSourceNodes source = getSyncSourceNodes (name);
        std::string sync = sc->getSync();
        SyncMode mode = StringToSyncMode(sync);
        if (mode != SYNC_NONE) {
            SourceType sourceType = SyncSource::getSourceType(source);
            if (sourceType.m_backend == "virtual") {
                //This is a virtual sync source, check and enable the referenced
                //sub syncsources here
                SyncSourceParams params(name, source, boost::shared_ptr<SyncConfig>(this, SyncConfigNOP()), contextName);
                boost::shared_ptr<VirtualSyncSource> vSource = boost::shared_ptr<VirtualSyncSource> (new VirtualSyncSource (params));
                std::vector<std::string> mappedSources = vSource->getMappedSources();
                BOOST_FOREACH (std::string source, mappedSources) {
                    //check whether the mapped source is really available
                    boost::shared_ptr<PersistentSyncSourceConfig> source_config 
                        = getSyncSourceConfig(source);
                    if (!source_config || !source_config->exists()) {
                        Exception::throwError(SE_HERE,
                                              StringPrintf("Virtual datastore \"%s\" references a nonexistent datasource \"%s\".", name.c_str(), source.c_str()));
                    }
                    pair< map<string, string>::iterator, bool > res = subSources.insert(make_pair(source, name));
                    if (!res.second) {
                        Exception::throwError(SE_HERE,
                                              StringPrintf("Datastore \"%s\" included in the virtual datastores \"%s\" and \"%s\". It can only be included in one virtual datastore at a time.",
                                                           source.c_str(), res.first->second.c_str(), name.c_str()));
                    }

                }
                FilterConfigNode::ConfigFilter vFilter;
                vFilter["sync"] = sync;
                if (!m_serverMode) {
                    // must set special URI for clients so that
                    // engine knows about superdatastore and its
                    // URI
                    vFilter["uri"] = string("<") + vSource->getName() + ">" + vSource->getURINonEmpty();
                }
                BOOST_FOREACH (std::string source, mappedSources) {
                    setConfigFilter (false, source, vFilter);
                }
                sourceList.addSource(vSource);
            }
        }
    }

    BOOST_FOREACH(const string &name, configuredSources) {
        boost::shared_ptr<PersistentSyncSourceConfig> sc(getSyncSourceConfig(name));

        SyncSourceNodes source = getSyncSourceNodes (name);
        if (!sc->isDisabled()) {
            SourceType sourceType = SyncSource::getSourceType(source);
            if (sourceType.m_backend != "virtual") {
                SyncSourceParams params(name,
                                        source,
                                        boost::shared_ptr<SyncConfig>(this, SyncConfigNOP()),
                                        contextName);
                cxxptr<SyncSource> syncSource(SyncSource::createSource(params));
                if (!syncSource) {
                    Exception::throwError(SE_HERE, name + ": type unknown" );
                }
                if (subSources.find(name) != subSources.end()) {
                    syncSource->recordVirtualSource(subSources[name]);
                }
                sourceList.addSource(syncSource);
            }
        } else {
            // the Synthesis engine is never going to see this source,
            // therefore we have to mark it as 100% complete and
            // "done"
            class DummySyncSource source(name, contextName);
            source.recordFinalSyncMode(SYNC_NONE);
            displaySourceProgress(source,
                                  SyncSourceEvent(sysync::PEV_PREPARING, 0, 0, 0),
                                  true);
            displaySourceProgress(source,
                                  SyncSourceEvent(sysync::PEV_ITEMPROCESSED, 0, 0, 0),
                                  true);
            displaySourceProgress(source,
                                  SyncSourceEvent(sysync::PEV_ITEMRECEIVED, 0, 0, 0),
                                  true);
            displaySourceProgress(source,
                                  SyncSourceEvent(sysync::PEV_ITEMSENT, 0, 0, 0),
                                  true);
            displaySourceProgress(source,
                                  SyncSourceEvent(sysync::PEV_SYNCEND, 0, 0, 0),
                                  true);
        }
    }
}

SyncMLStatus SyncContext::startSourceAccess(SyncSource *source)
{
    if(m_firstSourceAccess) {
        syncSuccessStart();
        m_firstSourceAccess = false;
    }
    if (m_serverMode) {
        // When using the source as cache, change tracking
        // is not required. Disabling it can make item
        // changes faster.
        SyncMode mode = StringToSyncMode(source->getSync());
        if (mode == SYNC_LOCAL_CACHE_SLOW ||
            mode == SYNC_LOCAL_CACHE_INCREMENTAL) {
            source->setNeedChanges(false);
        }
        // source is active in sync, now open it
        source->open();
    }
    // database dumping is delayed in both client and server
    m_sourceListPtr->syncPrepare(source->getName());

    return STATUS_OK;
}

// XML configuration converted to C string constants
extern "C" {
    // including all known fragments for a client
    extern const char *SyncEvolutionXMLClient;
    // the remote rules for a client
    extern const char *SyncEvolutionXMLClientRules;
}

/**
 * helper class which scans directories for
 * XML config files
 */
class XMLFiles
{
public:
    enum Category {
        MAIN,           /**< files directly under searched directories */
        DATATYPES,      /**< inside datatypes and datatypes/<mode> */
        SCRIPTING,      /**< inside scripting and scripting/<mode> */
        REMOTERULES,    /**< inside remoterules and remoterules/<mode> */
        MAX_CATEGORY
    };

    /** search file system for XML config fragments */
    void scan(const string &mode);
    /** datatypes, scripts and rules concatenated, empty if none found */
    string get(Category category);
    /** main file, typically "syncevolution.xml", empty if not found */
    string get(const string &file);

    static const string m_syncevolutionXML;

private:
    /* base name as sort key + full file path, iterating is done in lexical order */
    StringMap m_files[MAX_CATEGORY];

    /**
     * scan a specific directory for main files directly inside it
     * and inside datatypes, scripting, remoterules;
     * it is not an error when it does not exist or is not a directory
     */
    void scanRoot(const string &mode, const string &dir);

    /**
     * scan a datatypes/scripting/remoterules sub directory,
     * including the <mode> sub-directory
     */
    void scanFragments(const string &mode, const string &dir, Category category);

    /**
     * add all .xml files to the right hash, overwriting old entries
     */
    void addFragments(const string &dir, Category category);
};

const string XMLFiles::m_syncevolutionXML("syncevolution.xml");

void XMLFiles::scan(const string &mode)
{
    const char *dir = getenv("SYNCEVOLUTION_XML_CONFIG_DIR");
    /*
     * read either one or the other, so that testing can run without
     * accidentally reading installed files
     */
    if (dir) {
        scanRoot(mode, dir);
    } else {
        scanRoot(mode, XML_CONFIG_DIR);
        scanRoot(mode, SubstEnvironment("${XDG_CONFIG_HOME}/syncevolution-xml"));
    }
}

void XMLFiles::scanRoot(const string &mode, const string &dir)
{
    addFragments(dir, MAIN);
    scanFragments(mode, dir + "/scripting", SCRIPTING);
    scanFragments(mode, dir + "/datatypes", DATATYPES);
    scanFragments(mode, dir + "/remoterules", REMOTERULES);
}

void XMLFiles::scanFragments(const string &mode, const string &dir, Category category)
{
    addFragments(dir, category);
    addFragments(dir + "/" + mode, category);
}

void XMLFiles::addFragments(const string &dir, Category category)
{
    if (!isDir(dir)) {
        return;
    }
    ReadDir content(dir);
    BOOST_FOREACH(const string &file, content) {
        if (boost::ends_with(file, ".xml")) {
            m_files[category][file] = dir + "/" + file;
        }
    }
}

string XMLFiles::get(Category category)
{
    string res;

    BOOST_FOREACH(const StringPair &entry, m_files[category]) {
        string content;
        ReadFile(entry.second, content);
        res += content;
    }
    return res;
}

string XMLFiles::get(const string &file)
{
    string res;
    StringMap::const_iterator entry = m_files[MAIN].find(file);
    if (entry != m_files[MAIN].end()) {
        ReadFile(entry->second, res);
    }
    return res;
}

static void substTag(string &xml, const string &tagname, const string &replacement, bool replaceElement = false)
{
    string tag;
    size_t index;

    tag.reserve(tagname.size() + 3);
    tag += "<";
    tag += tagname;
    tag += "/>";

    index = xml.find(tag);
    if (index != xml.npos) {
        string tmp;
        tmp.reserve(tagname.size() * 2 + 2 + 3 + replacement.size());
        if (!replaceElement) {
            tmp += "<";
            tmp += tagname;
            tmp += ">";
        }
        tmp += replacement;
        if (!replaceElement) {
            tmp += "</";
            tmp += tagname;
            tmp += ">";
        }
        xml.replace(index, tag.size(), tmp);
    }
}

static void substTag(string &xml, const string &tagname, const char *replacement, bool replaceElement = false)
{
    substTag(xml, tagname, std::string(replacement), replaceElement);
}

template <class T> void substTag(string &xml, const string &tagname, const T replacement, bool replaceElement = false)
{
    stringstream str;
    str << replacement;
    substTag(xml, tagname, str.str(), replaceElement);
}

void SyncContext::getConfigTemplateXML(const string &mode,
                                       string &xml,
                                       string &rules,
                                       string &configname)
{
    XMLFiles files;

    files.scan(mode);
    xml = files.get(files.m_syncevolutionXML);
    if (xml.empty()) {
        if (mode != "client") {
            SE_THROW(files.m_syncevolutionXML + " not found");
        }
        configname = "builtin XML configuration";
        xml = SyncEvolutionXMLClient;
        rules = SyncEvolutionXMLClientRules;
    } else {
        configname = "XML configuration files";
        rules = files.get(XMLFiles::REMOTERULES);
        substTag(xml, "datatypes",
                 files.get(XMLFiles::DATATYPES) +
                 "    <fieldlists/>\n    <profiles/>\n    <datatypedefs/>\n");
        substTag(xml, "scripting", files.get(XMLFiles::SCRIPTING));
    }
}

void SyncContext::getConfigXML(bool isSync, string &xml, string &configname)
{
    string rules;
    getConfigTemplateXML(m_serverMode ? "server" : "client",
                         xml,
                         rules,
                         configname);

    string tag;
    size_t index;
    unsigned long hash = 0;


    std::set<std::string> flags = getSyncMLFlags();
    bool noctcap = flags.find("noctcap") != flags.end();
    bool norestart = flags.find("norestart") != flags.end();
    const char *PBAPSyncMode = getenv("SYNCEVOLUTION_PBAP_SYNC");
    bool keepPhotoData = PBAPSyncMode &&
        (boost::iequals(PBAPSyncMode, "incremental") ||
         boost::iequals(PBAPSyncMode, "text"));
    std::string sessioninitscript =
        "    <sessioninitscript><![CDATA[\n"
        "      // these variables are possibly modified by rule scripts\n"
        "      TIMESTAMP mindate; // earliest date remote party can handle\n"
        "      INTEGER retransfer_body; // if set to true, body is re-sent to client when message is moved from outbox to sent\n"
        "      mindate=EMPTY; // no limit by default\n"
        "      retransfer_body=FALSE; // normally, do not retransfer email body (and attachments) when moving items to sent box\n"
        "      INTEGER delayedabort;\n"
        "      delayedabort = FALSE;\n"
        "      INTEGER alarmTimeToUTC;\n"
        "      alarmTimeToUTC = FALSE;\n"
        "      INTEGER addInternetEmail;\n"
        "      addInternetEmail = FALSE;\n"
        "      INTEGER stripUID;\n"
        "      stripUID = FALSE;\n"
        "      INTEGER keepPhotoData;\n"
        "      keepPhotoData = "
        // Keep local photos in first cycle when using special sync
        // mode for PBAP. PBAP source will request second cycle if it
        // has contacts whose photo data was not donwloaded. Then we
        // will disable this special handling for that cycle and photo
        // can be updated and removed normally.
        + std::string(keepPhotoData ? "TRUE" : "FALSE") + ";\n"
        "    ]]></sessioninitscript>\n";

    ostringstream clientorserver;
    if (m_serverMode) {
        clientorserver <<
            "  <server type='plugin'>\n"
            "    <plugin_module>SyncEvolution</plugin_module>\n"
            "    <plugin_sessionauth>yes</plugin_sessionauth>\n"
            "    <plugin_deviceadmin>yes</plugin_deviceadmin>\n";

        InitState<unsigned int> configrequestmaxtime = getRequestMaxTime();
        unsigned int requestmaxtime;
        if (configrequestmaxtime.wasSet()) {
            // Explicitly set, use it regardless of the kind of sync.
            // We allow this even if thread support was not available,
            // because if a user enables it explicitly, it's probably
            // for a good reason (= failing client), in which case
            // risking multithreading issues is preferable.
            requestmaxtime = configrequestmaxtime.get();
        } else if (m_remoteInitiated || m_localSync) {
            // We initiated the sync (local sync, Bluetooth). The client
            // should not time out, so there is no need for intermediate
            // message sending.
            //
            // To avoid potential problems and get a single log file,
            // avoid it and multithreading by default.
            requestmaxtime = 0;
        } else {
            // We were contacted by an HTTP client. Reply to client
            // not later than 120 seconds while storage initializes
            // in a background thread.
#ifdef HAVE_THREAD_SUPPORT
            requestmaxtime = 120; // default in seconds
#else
            requestmaxtime = 0;
#endif
        }
        if (requestmaxtime) {
            clientorserver <<
                "    <multithread>yes</multithread>\n"
                "    <requestmaxtime>" << requestmaxtime << "</requestmaxtime>\n";
        } else {
            clientorserver <<
                "    <multithread>no</multithread>\n";
        }

        clientorserver <<
            "\n" <<
            sessioninitscript <<
            "    <sessiontimeout>300</sessiontimeout>\n"
            "\n";
        //do not send respuri if over bluetooth
        if (boost::starts_with (getUsedSyncURL(), "obex-bt://")) {
            clientorserver << "    <sendrespuri>no</sendrespuri>\n"
            "\n";
        }
        clientorserver << "    <syncmodeextensions>" << (norestart ? "no" : "yes" ) << "</syncmodeextensions>\n";
        if (noctcap) {
            clientorserver << "    <showctcapproperties>no</showctcapproperties>\n"
            "\n";
        }
        clientorserver<<"    <defaultauth/>\n"
            "\n"
            "    <datastore/>\n"
            "\n"
            "    <remoterules/>\n"
            "  </server>\n";
    } else {
        clientorserver <<
            "  <client type='plugin'>\n"
            "    <binfilespath>$(binfilepath)</binfilespath>\n"
            "    <multithread>no</multithread>\n"
            "    <defaultauth/>\n";
        if (getRefreshSync()) {
            clientorserver <<
                "    <preferslowsync>no</preferslowsync>\n";
        }
        clientorserver <<
            "\n" ;

         string syncMLVersion (getSyncMLVersion());
         if (!syncMLVersion.empty()) {
             clientorserver << "<defaultsyncmlversion>"
                 <<syncMLVersion.c_str()<<"</defaultsyncmlversion>\n";
         }

         clientorserver << "    <syncmodeextensions>" << (norestart ? "no" : "yes" ) << "</syncmodeextensions>\n";
         if (noctcap) {
             clientorserver << "    <showctcapproperties>no</showctcapproperties>\n"
                 "\n";
         }

         clientorserver << sessioninitscript <<
            // SyncEvolution has traditionally not folded long lines in
            // vCard.  Testing showed that servers still have problems with
            // it, so avoid it by default
            "    <donotfoldcontent>yes</donotfoldcontent>\n"
            "\n"
            "    <fakedeviceid/>\n"
            "\n"
            "    <datastore/>\n"
            "\n"
            "    <remoterules/>\n"
            "  </client>\n";
    }
    substTag(xml,
             "clientorserver",
             clientorserver.str(),
             true);

    tag = "<debug/>";
    index = xml.find(tag);
    if (index != xml.npos) {
        stringstream debug;
        bool logging = !m_sourceListPtr->getLogdir().empty();
        int loglevel = getLogLevel();
#ifdef USE_DLT
        const char *useDLT = getenv("SYNCEVOLUTION_USE_DLT");
#else
        static const char *useDLT = NULL;
#endif

        debug <<
            "  <debug>\n"
            // logpath is a config variable set by SyncContext::doSync()
            "    <logpath>$(logpath)</logpath>\n"
            "    <filename>" << (useDLT ? "" : LogfileBasename) << "</filename>" <<
            "    <logflushmode>flush</logflushmode>\n"
            "    <logformat>" << (useDLT ? "dlt" : "html") << "</logformat>\n"
            "    <folding>auto</folding>\n" <<
            (useDLT ?
             "    <timestamp>no</timestamp>\n"
             "    <timestampall>no</timestampall>\n" :
             "    <timestamp>yes</timestamp>\n"
             "    <timestampall>yes</timestampall>\n") <<
            "    <timedsessionlognames>no</timedsessionlognames>\n"
            "    <subthreadmode>separate</subthreadmode>\n"
            "    <logsessionstoglobal>yes</logsessionstoglobal>\n"
            "    <singlegloballog>yes</singlegloballog>\n";
#ifdef USE_DLT
        if (useDLT) {
            const char *contexts[] = {
                "PROT",
                "SESS",
                "ADMN",
                "DATA",
                "REMI",
                "PARS",
                "GEN",
                "TRNS",
                "SMLT",
                "SYS"
            };
            BOOST_FOREACH (const char *context, contexts) {
                // Help libsynthesis debuglogger.cpp set default log levels,
                // based on our own one.
                SE_LOG_DEBUG(NULL, "default libsynthesis DLT logging of %s = %s",
                             context, useDLT);
                setenv((std::string("LIBSYNTHESIS_") + context).c_str(),
                       useDLT,
                       false);
            }

            debug <<
                // We have to enable all logging inside libsynthesis.
                // The actual filtering then takes place inside DLT.
                // Message logging is not supported.
                "    <enable option=\"all\"/>\n"
                // Allow logging outside of sessions.
                "    <globallogs>yes</globallogs>\n"
                // Don't try per-session logging, it all goes to DLT anyway.
                "    <sessionlogs>yes</sessionlogs>\n"
                ;

            // Be extra verbose if currently enabled. Cannot be changed later on.
            if (atoi(useDLT) > DLT_LOG_DEBUG) {
                debug <<
                    "    <enable option=\"userdata\"/>\n"
                    "    <enable option=\"scripts\"/>\n";
            }
            if (atoi(useDLT) > DLT_LOG_DEBUG) {
                debug <<
                    "    <enable option=\"exotic\"/>\n";
            }
        } else
#endif // USE_DLT
            if (logging) {
            debug <<
                "    <sessionlogs>yes</sessionlogs>\n"
                "    <globallogs>yes</globallogs>\n";
            debug << "<msgdump>" << (loglevel >= 5 ? "yes" : "no") << "</msgdump>\n";
            debug << "<xmltranslate>" << (loglevel >= 4 ? "yes" : "no") << "</xmltranslate>\n";
            if (loglevel >= 3) {
                debug <<
                    "    <sourcelink>doxygen</sourcelink>\n"
                    "    <enable option=\"all\"/>\n"
                    "    <enable option=\"userdata\"/>\n"
                    "    <enable option=\"scripts\"/>\n"
                    "    <enable option=\"exotic\"/>\n";
            }
        } else {
            debug <<
                "    <sessionlogs>no</sessionlogs>\n"
                "    <globallogs>no</globallogs>\n"
                "    <msgdump>no</msgdump>\n"
                "    <xmltranslate>no</xmltranslate>\n"
                "    <disable option=\"all\"/>";
        }
        debug <<
            "  </debug>\n";

        xml.replace(index, tag.size(), debug.str());
    }

    XMLConfigFragments fragments;
    tag = "<datastore/>";
    index = xml.find(tag);
    if (index != xml.npos) {
        stringstream datastores;

        BOOST_FOREACH(SyncSource *source, *m_sourceListPtr) {
            string fragment;
            source->getDatastoreXML(fragment, fragments);
            string name;

            // Make sure that sub-datastores do not interfere with the global URI namespace
            // by adding a <superdatastore>/ prefix. That way we can have a "calendar"
            // alias for "calendar+todo" without conflicting with the underlying
            // "calendar", which will be called "calendar+todo/calendar" in the XML config.
            name = source->getVirtualSource();
            if (!name.empty()) {
                name += m_findSourceSeparator;
            }
            name += source->getName();

            datastores << "    <datastore name='" << name << "' type='plugin'>\n" <<
                "      <dbtypeid>" << source->getSynthesisID() << "</dbtypeid>\n" <<
                fragment;

            datastores << "      <resumesupport>on</resumesupport>\n";
            if (source->getOperations().m_writeBlob) {
                // BLOB support is essential for caching partially received items.
                datastores << "      <resumeitemsupport>on</resumeitemsupport>\n";
            }

            SyncMode mode = StringToSyncMode(source->getSync());
            if (source->getForceSlowSync()) {
                // we *want* a slow sync, but couldn't tell the client -> force it server-side
                datastores << "      <alertscript> FORCESLOWSYNC(); </alertscript>\n";
            } else if (mode == SYNC_LOCAL_CACHE_SLOW ||
                       mode == SYNC_LOCAL_CACHE_INCREMENTAL) {
                if (!m_serverMode) {
                    SE_THROW("sync modes 'local-cache-*' are only supported on the server side");
                }
                datastores << "      <alertscript>SETREFRESHONLY(1); SETCACHEDATA(1);</alertscript>\n";
                // datastores << "      <datastoreinitscript>REFRESHONLY(); CACHEDATA(); SLOWSYNC(); ALERTCODE();</datastoreinitscript>\n";
            } else if (mode != SYNC_SLOW &&
                       // slow-sync detection not implemented when running as server,
                       // not even when initiating the sync (direct sync with phone)
                       !m_serverMode &&
                       // is implemented as "delete local data" + "slow sync",
                       // so a slow sync is acceptable in this case
                       mode != SYNC_REFRESH_FROM_SERVER &&
                       mode != SYNC_REFRESH_FROM_REMOTE &&
                       // The forceSlow should be disabled if the sync session is
                       // initiated by a remote peer (eg. Server Alerted Sync)
                       !m_remoteInitiated &&
                       getPreventSlowSync() &&
                       (!source->getOperations().m_isEmpty ||    // check is only relevant if we have local data;
                        !source->getOperations().m_isEmpty())) { // if we cannot check, assume we have data
                // We are not expecting a slow sync => refuse to execute one.
                // This is the client check for this, server must be handled
                // differently (TODO, MB #2416).
                datastores <<
                    "      <datastoreinitscript><![CDATA[\n"
                    "           if (SLOWSYNC() && ALERTCODE() != 203) {\n" // SLOWSYNC() is true for acceptable refresh-from-client, check for that
                    "              DEBUGMESSAGE(\"slow sync not expected by SyncEvolution, disabling datastore\");\n"
                    "              ABORTDATASTORE(" << sysync::LOCERR_DATASTORE_ABORT << ");\n"
                    "              // tell UI to abort instead of sending the next message\n"
                    "              SETSESSIONVAR(\"delayedabort\", 1);\n"
                    "           }\n"
                    "      ]]></datastoreinitscript>\n";
            }

            if (m_serverMode && !m_localSync) {
                string uri = source->getURI();
                if (!uri.empty()) {
                    datastores << " <alias name='" << uri << "'/>";
                }
            }

            datastores << "    </datastore>\n\n";
        }

        /*If there is super datastore, add it here*/
        //TODO generate specific superdatastore contents (MB #8753)
        //Now only works for synthesis built-in events+tasks
        BOOST_FOREACH (boost::shared_ptr<VirtualSyncSource> vSource, m_sourceListPtr->getVirtualSources()) {
            std::string superType = vSource->getSourceType().m_format;
            std::string evoSyncSource = vSource->getDatabaseID();
            std::vector<std::string> mappedSources = unescapeJoinedString (evoSyncSource, ',');

            // always check for a consistent config
            SourceType sourceType = vSource->getSourceType();
            BOOST_FOREACH (std::string source, mappedSources) {
                //check the data type
                SyncSource *subSource = (*m_sourceListPtr)[source];
                SourceType subType = subSource->getSourceType();

                //If there is no format explictly selected in sub SyncSource, we
                //have no way to determine whether it works with the format
                //specific in the virtual SyncSource, thus no warning in this
                //case.
                if (!subType.m_format.empty() && (
                    sourceType.m_format != subType.m_format ||
                    sourceType.m_forceFormat != subType.m_forceFormat)) {
                    SE_LOG_WARNING(NULL, 
                                   "Virtual datastore \"%s\" and sub datastore \"%s\" have different data format. Will use the format in virtual datastore.",
                                   vSource->getDisplayName().c_str(), source.c_str());
                }
            }

            if (mappedSources.size() !=2) {
                vSource->throwError(SE_HERE, "virtual datastore currently only supports events+tasks combinations");
            } 

            string name = vSource->getName();
            datastores << "    <superdatastore name= '" << name << "'> \n";
            datastores << "      <contains datastore = '" << name << m_findSourceSeparator << mappedSources[0] <<"'>\n"
                << "        <dispatchfilter>F.ISEVENT:=1</dispatchfilter>\n"
                << "        <guidprefix>e</guidprefix>\n"
                << "      </contains>\n"
                << "\n      <contains datastore = '" << name << m_findSourceSeparator << mappedSources[1] <<"'>\n"
                << "        <dispatchfilter>F.ISEVENT:=0</dispatchfilter>\n"
                << "        <guidprefix>t</guidprefix>\n"
                <<"      </contains>\n" ;

            if (m_serverMode && !m_localSync) {
                string uri = vSource->getURI();
                if (!uri.empty()) {
                    datastores << " <alias name='" << uri << "'/>";
                }
            }

            if (vSource->getForceSlowSync()) {
                // we *want* a slow sync, but couldn't tell the client -> force it server-side
                datastores << "      <alertscript> FORCESLOWSYNC(); </alertscript>\n";
            }

            std::string typesupport;
            typesupport = vSource->getDataTypeSupport();
            datastores << "      <typesupport>\n"
                << typesupport 
                << "      </typesupport>\n";
            datastores <<"\n    </superdatastore>";
        }

        if (datastores.str().empty()) {
            // Add dummy datastore, the engine needs it. sync()
            // checks that we have a valid configuration if it is
            // really needed.
#if 0
            datastores << "<datastore name=\"____dummy____\" type=\"plugin\">"
                "<plugin_module>SyncEvolution</plugin_module>"
                "<fieldmap fieldlist=\"contacts\"/>"
                "<typesupport>"
                "<use datatype=\"vCard30\"/>"
                "</typesupport>"
                "</datastore>";
#endif
        }
        xml.replace(index, tag.size(), datastores.str());
    }

    substTag(xml, "fieldlists", fragments.m_fieldlists.join(), true);
    substTag(xml, "profiles", fragments.m_profiles.join(), true);
    substTag(xml, "datatypedefs", fragments.m_datatypes.join(), true);
    substTag(xml, "remoterules",
             rules +
             fragments.m_remoterules.join(),
             true);

    if (m_serverMode) {
        // TODO: set the device ID for an OBEX server
    } else {
        substTag(xml, "fakedeviceid", getDevID());
    }
    substTag(xml, "model", getMod());
    substTag(xml, "manufacturer", getMan());
    substTag(xml, "hardwareversion", getHwv());
    // abuse (?) the firmware version to store the SyncEvolution version number
    substTag(xml, "firmwareversion", getSwv());
    substTag(xml, "devicetype", getDevType());
    substTag(xml, "maxmsgsize", std::max(getMaxMsgSize().get(), 10000ul));
    substTag(xml, "maxobjsize", std::max(getMaxObjSize().get(), 1024u));
    if (m_serverMode) {
        UserIdentity id = getSyncUser();
        Credentials cred = IdentityProviderCredentials(id, getSyncPassword());
        const string &user = cred.m_username;
        const string &password = cred.m_password;

        /*
         * Do not check username/pwd if this local sync or over
         * bluetooth transport. Need credentials for checking.
         */
        if (!m_localSync &&
            !boost::starts_with(getUsedSyncURL(), "obex-bt") &&
            (!user.empty() || !password.empty())) {
            // require authentication with the configured password
            substTag(xml, "defaultauth",
                     "<requestedauth>md5</requestedauth>\n"
                     "<requiredauth>basic</requiredauth>\n"
                     "<autononce>yes</autononce>\n",
                     true);
        } else {
            // no authentication required
            substTag(xml, "defaultauth",
                     "<logininitscript>return TRUE</logininitscript>\n"
                     "<requestedauth>none</requestedauth>\n"
                     "<requiredauth>none</requiredauth>\n"
                     "<autononce>yes</autononce>\n",
                     true);
        }
    } else {
        substTag(xml, "defaultauth", getClientAuthType());
    }

    if (isSync ||
        !getConfigDate().wasSet()) {
        // If the hash code of the main sync XML config is changed, that
        // means the content of the config has changed. Save the new hash
        // and regen the configdate. Also necessary when no config data
        // has ever been set.
        hash = Hash(xml.c_str());
        if (getHashCode() != hash) {
            setConfigDate();
            setHashCode(hash);
            flush();
        }
    }
    substTag(xml, "configdate", getConfigDate().c_str());
}

SharedEngine SyncContext::createEngine()
{
    SharedEngine engine(new sysync::TEngineModuleBridge);

    // This instance of the engine is used outside of the sync session
    // itself for logging. doSync() then reinitializes it with a full
    // datastore configuration.
    engine.Connect(m_serverMode ?
#ifdef ENABLE_SYNCML_LINKED
                   // use Synthesis client or server engine that we were linked against
                   "[server:]" : "[]",
#else
                   // load engine dynamically
                   "server:libsynthesis.so.0" : "libsynthesis.so.0",
#endif
                   0,
                   sysync::DBG_PLUGIN_NONE|
                   sysync::DBG_PLUGIN_INT|
                   sysync::DBG_PLUGIN_DB|
                   sysync::DBG_PLUGIN_EXOT);

    SharedKey configvars = engine.OpenKeyByPath(SharedKey(), "/configvars");
    string logdir;
    if (m_sourceListPtr) {
        logdir = m_sourceListPtr->getLogdir();
    }
    engine.SetStrValue(configvars, "defout_path",
                       logdir.size() ? logdir : "/dev/null");
    engine.SetStrValue(configvars, "conferrpath", "console");
    engine.SetStrValue(configvars, "binfilepath", getSynthesisDatadir().c_str());
    configvars.reset();

    return engine;
}

namespace {
    void GnutlsLogFunction(int level, const char *str)
    {
        SE_LOG_DEBUG("GNUTLS", "level %d: %s", level, str);
    }
}

void SyncContext::initServer(const std::string &sessionID,
                             SharedBuffer data,
                             const std::string &messageType)
{
    m_serverMode = true;
    m_sessionID = sessionID;
    m_initialMessage = data;
    m_initialMessageType = messageType;
    
}

struct SyncContext::SyncMLMessageInfo
SyncContext::analyzeSyncMLMessage(const char *data, size_t len,
                                  const std::string &messageType)
{
    SyncContext sync;
    SourceList sourceList(sync, false);
    sourceList.setLogLevel(SourceList::LOGGING_SUMMARY);
    sync.m_sourceListPtr = &sourceList;
    SwapContext syncSentinel(&sync);
    sync.initServer("", SharedBuffer(), "");
    SwapEngine swapengine(sync);
    sync.initEngine(false);

    sysync::TEngineProgressInfo progressInfo;
    sysync::uInt16 stepCmd = sysync::STEPCMD_GOTDATA;
    SharedSession session = sync.m_engine.OpenSession(sync.m_sessionID);
    SessionSentinel sessionSentinel(sync, session);

    sync.m_engine.WriteSyncMLBuffer(session, data, len);
    SharedKey sessionKey = sync.m_engine.OpenSessionKey(session);
    sync.m_engine.SetStrValue(sessionKey,
                              "contenttype",
                              messageType);

    // analyze main loop: runs until SessionStep() signals reply or error.
    // Will call our SynthesisDBPlugin callbacks, most importantly
    // SyncEvolution_Session_CheckDevice(), which records the device ID
    // for us.
    do {
        sync.m_engine.SessionStep(session, stepCmd, &progressInfo);
        switch (stepCmd) {
        case sysync::STEPCMD_OK:
        case sysync::STEPCMD_PROGRESS:
            stepCmd = sysync::STEPCMD_STEP;
            break;
        default:
            // whatever it is, cannot proceed
            break;
        }
    } while (stepCmd == sysync::STEPCMD_STEP);

    SyncMLMessageInfo info;
    info.m_deviceID = sync.getSyncDeviceID();
    return info;
}

void SyncContext::initEngine(bool isSync)
{
    string xml, configname;
    getConfigXML(isSync, xml, configname);
    try {
        m_engine.InitEngineXML(xml.c_str());
    } catch (const BadSynthesisResult &ex) {
        SE_LOG_ERROR(NULL,
                     "internal error, invalid XML configuration (%s):\n%s",
                     m_sourceListPtr && !m_sourceListPtr->empty() ?
                     "with datastores" :
                     "without datastores",
                     xml.c_str());
        throw;
    }
    if (isSync &&
        getLogLevel() >= 5) {
        SE_LOG_DEV(NULL, "Full XML configuration:\n%s", xml.c_str());
    }
}

// This is just the declaration. The actual function pointer instance
// is inside libsynthesis, which, for historic purposes, doesn't define
// it in its header files (yet).
extern "C" int (*SySync_ConsolePrintf)(FILE *stream, const char *format, ...);

static int nopPrintf(FILE *stream, const char *format, ...) { return 0; }

extern "C"
{
    extern int (*SySync_CondTimedWait)(pthread_cond_t *cond, pthread_mutex_t *mutex, bool &aTerminated, long aMilliSecondsToWait);
}

#ifdef HAVE_GLIB
static gboolean timeout(gpointer data)
{
    // Call me again...
    return true;
}

static bool CondTimedWaitContinue(pthread_mutex_t *mutex,
                                  bool &terminated,
                                  long milliSecondsToWait,
                                  Timespec &deadline,
                                  SuspendFlags &flags,
                                  int &result)
{
    // Thread has terminated?
    pthread_mutex_lock(mutex);
    if (terminated) {
        pthread_mutex_unlock(mutex);
        SE_LOG_DEBUG(NULL, "background thread completed");
        return false;
    }
    pthread_mutex_unlock(mutex);

    // Abort? Ignore when waiting for final thread shutdown, because
    // in that case we just get called again.
    if (milliSecondsToWait > 0 && flags.isAborted()) {
        SE_LOG_DEBUG(NULL, "give up waiting for background thread, aborted");
        // Signal error. libsynthesis then assumes that the thread still
        // runs and enters its parallel message sending, which eventually
        // returns control to us.
        result = 1;
        return false;
    }

    // Timeout?
    if (!milliSecondsToWait ||
        (milliSecondsToWait > 0 && deadline <= Timespec::system())) {
        SE_LOG_DEBUG(NULL, "give up waiting for background thread, timeout");
        result = 1;
        return false;
    }

    return true;
}

static int CondTimedWaitGLib(pthread_cond_t * /* cond */, pthread_mutex_t *mutex,
                             bool &terminated, long milliSecondsToWait)
{
    int result = 0;

    // return abstime ? pthread_cond_timedwait(cond, mutex, abstime) : pthread_cond_wait(cond, mutex);
    try {
        pthread_mutex_unlock(mutex);

        SE_LOG_DEBUG(NULL, "wait for background thread: %lds", milliSecondsToWait);
        SuspendFlags &flags = SuspendFlags::getSuspendFlags();

        Timespec now = Timespec::system();
        Timespec wait(milliSecondsToWait / 1000, milliSecondsToWait % 1000);
        Timespec deadline = now + wait;

        // We don't need to react to thread shutdown immediately (only
        // called once per sync), so a relatively long check interval of
        // one second is okay.
        GLibEvent id(g_timeout_add_seconds(1, timeout, NULL), "timeout");

        GRunWhile(boost::bind(CondTimedWaitContinue,
                              mutex,
                              boost::ref(terminated),
                              milliSecondsToWait,
                              boost::ref(deadline),
                              boost::ref(flags),
                              boost::ref(result)));
    } catch (...) {
        Exception::handle(HANDLE_EXCEPTION_FATAL);
    }

    pthread_mutex_lock(mutex);
    return result;
}

#endif

void SyncContext::initMain(const char *appname)
{
#if defined(HAVE_GLIB)
    // this is required when using glib directly or indirectly
#if !GLIB_CHECK_VERSION(2,36,0)
    g_type_init();
#endif
#if !GLIB_CHECK_VERSION(2,32,0)
    g_thread_init(NULL);
#endif
    g_set_prgname(appname);

    // redirect glib logging into our own logging
    g_log_set_default_handler(Logger::glogFunc, NULL);

    // Only the main thread may use the default GMainContext.
    // Anything else is unsafe, see https://mail.gnome.org/archives/gtk-list/2013-April/msg00040.html
    // util.cpp:Sleep() checks this and uses the default context
    // when called by the main thread, otherwise falls back to
    // select(). GRunWhile() is always safe to use.
    g_main_context_acquire(NULL);

    SySync_CondTimedWait = CondTimedWaitGLib;
#endif
    if (atoi(getEnv("SYNCEVOLUTION_DEBUG", "0")) > 3) {
        SySync_ConsolePrintf = Logger::sysyncPrintf;
    } else {
        SySync_ConsolePrintf = nopPrintf;
    }

    // invoke optional init parts, for example KDE KApplication init
    // in KDE backend
    GetInitMainSignal()(appname);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    // Initializing a potential use of EDS early is necessary for
    // libsynthesis when compiled with
    // --enable-evolution-compatibility: in that mode libical will
    // only be found by libsynthesis after EDSAbiWrapperInit()
    // pulls it into the process by loading libecal.
    EDSAbiWrapperInit();

    if (const char *gnutlsdbg = getenv("SYNCEVOLUTION_GNUTLS_DEBUG")) {
        // Enable libgnutls debugging without creating a hard dependency on it,
        // because we don't call it directly and might not even be linked against
        // it. Therefore check for the relevant symbols via dlsym().
        void (*set_log_level)(int);
        typedef void (*LogFunc_t)(int level, const char *str);
        void (*set_log_function)(LogFunc_t func);
        
        set_log_level = (typeof(set_log_level))dlsym(RTLD_DEFAULT, "gnutls_global_set_log_level");
        set_log_function = (typeof(set_log_function))dlsym(RTLD_DEFAULT, "gnutls_global_set_log_function");

        if (set_log_level && set_log_function) {
            set_log_level(atoi(gnutlsdbg));
            set_log_function(GnutlsLogFunction);
        } else {
            SE_LOG_ERROR(NULL, "SYNCEVOLUTION_GNUTLS_DEBUG debugging not possible, log functions not found");
        }
    }
}

SyncContext::InitMainSignal &SyncContext::GetInitMainSignal()
{
    static InitMainSignal initMainSignal;
    return initMainSignal;
}

static bool IsStableRelease =
#ifdef SYNCEVOLUTION_STABLE_RELEASE
                   true
#else
                   false
#endif
                   ;
bool SyncContext::isStableRelease()
{
    return IsStableRelease;
}
void SyncContext::setStableRelease(bool isStableRelease)
{
    IsStableRelease = isStableRelease;
}

void SyncContext::checkConfig(const std::string &operation) const
{
    std::string peer, context;
    splitConfigString(m_server, peer, context);
    if (isConfigNeeded() &&
        (!exists() || peer.empty())) {
        if (peer.empty()) {
            SE_LOG_INFO(NULL, "Configuration \"%s\" does not refer to a sync peer.", m_server.c_str());
        } else {
            SE_LOG_INFO(NULL, "Configuration \"%s\" does not exist.", m_server.c_str());
        }
        Exception::throwError(SE_HERE, StringPrintf("Cannot proceed with %s without a configuration.", operation.c_str()));
    }
}

SyncMLStatus SyncContext::sync(SyncReport *report)
{
    SyncMLStatus status = STATUS_OK;

    checkConfig("sync");

    if (getenv("SYNCEVOLUTION_EPHEMERAL")) {
        SE_LOG_INFO(NULL, "turning on ephemeral sync mode as requested by SYNCEVOLUTION_EPHEMERAL variable");
        makeEphemeral();
    }

    // redirect logging as soon as possible
    SourceList sourceList(*this, m_doLogging);
    sourceList.setLogLevel(m_quiet ? SourceList::LOGGING_QUIET :
                           getPrintChanges() ? SourceList::LOGGING_FULL :
                           SourceList::LOGGING_SUMMARY);

    // careful about scope, this is needed for writing the
    // report below
    SyncReport buffer;

    SwapContext syncSentinel(this);
    try {
        m_sourceListPtr = &sourceList;
        string url = getUsedSyncURL();
        if (boost::starts_with(url, "local://")) {
            initLocalSync(url.substr(strlen("local://")));
        }

        if (!report) {
            report = &buffer;
        }
        report->clear();
        if (m_localSync) {
            report->setRemoteName(m_localPeerContext);
            report->setLocalName(getContextName());
        }

        // let derived classes override settings, like the log dir
        prepare();

        // choose log directory
        sourceList.startSession(getLogDir(),
                                getMaxLogDirs(),
                                getLogLevel(),
                                report);

        /* Must detect server or client session before creating the
         * underlying SynthesisEngine 
         * */
        if ( getPeerIsClient()) {
            m_serverMode = true;
        } else if (m_localSync && !m_agent) {
            Exception::throwError(SE_HERE, "configuration error, syncURL = local can only be used in combination with peerIsClient = 1");
        }

        // create a Synthesis engine, used purely for logging purposes
        // at this time
        SwapEngine swapengine(*this);
        initEngine(false);

        try {
            // dump some summary information at the beginning of the log
            SE_LOG_DEV(NULL, "SyncML server account: %s", getSyncUser().toString().c_str());
            SE_LOG_DEV(NULL, "client: SyncEvolution %s for %s", getSwv().c_str(), getDevType().c_str());
            SE_LOG_DEV(NULL, "device ID: %s", getDevID().c_str());
            SE_LOG_DEV(NULL, "%s", EDSAbiWrapperDebug());
            SE_LOG_DEV(NULL, "%s", SyncSource::backendsDebug().c_str());

            // ensure that config can be modified (might have to be migrated first)
            prepareConfigForWrite();

            // instantiate backends, but do not open them yet
            initSources(sourceList);
            if (sourceList.empty()) {
                Exception::throwError(SE_HERE, "no datastores active, check configuration");
            }

            // request all config properties once: throwing exceptions
            // now is okay, whereas later it would lead to leaks in the
            // not exception safe client library
            SyncConfig dummy;
            set<string> activeSources = sourceList.getSources();
            dummy.copy(*this, &activeSources);

            // start background thread if not running yet:
            // necessary to catch problems with Evolution backend
            startLoopThread();

            // ask for passwords now
            PasswordConfigProperty::checkPasswords(getUserInterfaceNonNull(), *this,
                                                   PasswordConfigProperty::CHECK_PASSWORD_ALL,
                                                   sourceList.getSourceNames());

            // open each source - failing now is still safe
            // in clients; in servers we wait until the source
            // is really needed
            BOOST_FOREACH(SyncSource *source, sourceList) {
                if (m_serverMode) {
                    source->enableServerMode();
                } else {
                    source->open();
                }

                // request callback when starting to use source
                source->getOperations().m_startDataRead.getPreSignal().connect(boost::bind(&SyncContext::startSourceAccess, this, source));
            }

            // ready to go
            status = doSync();
        } catch (...) {
            // handle the exception here while the engine (and logging!) is still alive
            Exception::handle(&status);
            goto report;
        }
    } catch (...) {
        Exception::handle(&status);
    }

 report:
    if (status == SyncMLStatus(sysync::LOCERR_DATASTORE_ABORT)) {
        // this can mean only one thing in SyncEvolution: unexpected slow sync
        status = STATUS_UNEXPECTED_SLOW_SYNC;
    }
                            

    try {
        // Print final report before cleaning up.
        // Status was okay only if all sources succeeded.
        // When a source or the overall sync was successful,
        // but some items failed, we report a "partial failure"
        // status.
        BOOST_FOREACH(SyncSource *source, sourceList) {
            m_sourceSyncedSignal(source->getName(), *source);
            if (source->getStatus() == STATUS_OK &&
                (source->getItemStat(SyncSource::ITEM_LOCAL,
                                     SyncSource::ITEM_ANY,
                                     SyncSource::ITEM_REJECT) ||
                 source->getItemStat(SyncSource::ITEM_REMOTE,
                                     SyncSource::ITEM_ANY,
                                     SyncSource::ITEM_REJECT))) {
                source->recordStatus(STATUS_PARTIAL_FAILURE);
            }
            if (source->getStatus() != STATUS_OK &&
                status == STATUS_OK) {
                status = source->getStatus();
                break;
            }
        }

        // Also take into account result of client side in local sync,
        // if any existed. A non-success status code in the client's report
        // was already propagated to the parent via a TransportStatusException
        // in LocalTransportAgent::checkChildReport(). What we can do here
        // is updating the individual's sources status.
        if (m_localSync && m_agent && getPeerIsClient()) {
            boost::shared_ptr<LocalTransportAgent> agent = boost::static_pointer_cast<LocalTransportAgent>(m_agent);
            SyncReport childReport;
            agent->getClientSyncReport(childReport);
            BOOST_FOREACH(SyncSource *source, sourceList) {
                const SyncSourceReport *childSourceReport = childReport.findSyncSourceReport(source->getURINonEmpty());
                if (childSourceReport) {
                    SyncMLStatus parentSourceStatus = source->getStatus();
                    SyncMLStatus childSourceStatus = childSourceReport->getStatus();
                    // child source had an error *and*
                    // parent error is either unspecific (USERABORT) or
                    // is a remote error (HTTP error range)
                    if (childSourceStatus != STATUS_OK && childSourceStatus != STATUS_HTTP_OK &&
                        (parentSourceStatus == SyncMLStatus(sysync::LOCERR_USERABORT) ||
                         parentSourceStatus < SyncMLStatus(sysync::LOCAL_STATUS_CODE))) {
                        source->recordStatus(childSourceStatus);
                    }
                }
            }
        }

        sourceList.updateSyncReport(*report);
        sourceList.syncDone(status, report);
    } catch(...) {
        Exception::handle(&status);
    }

    m_agent.reset();
    m_sourceListPtr = NULL;
    return status;
}

bool SyncContext::sendSAN(uint16_t version) 
{
    sysync::SanPackage san;
    bool legacy = version < 12;
    /* Should be nonce sent by the server in the preceeding sync session */
    string nonce = "SyncEvolution";
    UserIdentity id = getSyncUser();
    Credentials cred = IdentityProviderCredentials(id, getSyncPassword());
    const std::string &user = cred.m_username;
    const std::string &password = cred.m_password;
    string uauthb64 = san.B64_H(user, password);
    /* Client is expected to conduct the sync in the backgroud */
    sysync::UI_Mode mode = sysync::UI_not_specified;

    uint16_t sessionId = 1;
    string serverId = getRemoteIdentifier();
    if(serverId.empty()) {
        serverId = getDevID();
    }
    SE_LOG_DEBUG(NULL, "starting SAN %u auth %s nonce %s session %u server %s",
                 version,
                 uauthb64.c_str(),
                 nonce.c_str(),
                 sessionId,
                 serverId.c_str());
    san.PreparePackage( uauthb64, nonce, version, mode, 
            sysync::Initiator_Server, sessionId, serverId);

    san.CreateEmptyNotificationBody();
    bool hasSource = false;
     
    std::set<std::string> dataSources = m_sourceListPtr->getSources();

    /* For each virtual datasoruce, generate the SAN accoring to it and ignoring
     * sub datasource in the later phase*/
    BOOST_FOREACH (boost::shared_ptr<VirtualSyncSource> vSource, m_sourceListPtr->getVirtualSources()) {
            std::string evoSyncSource = vSource->getDatabaseID();
            std::string sync = vSource->getSync();
            SANSyncMode mode = AlertSyncMode(StringToSyncMode(sync, true), getPeerIsClient());
            std::vector<std::string> mappedSources = unescapeJoinedString (evoSyncSource, ',');
            BOOST_FOREACH (std::string source, mappedSources) {
                dataSources.erase (source);
                if (mode == SA_SLOW) {
                    // We force a source which the client is not expected to use into slow mode.
                    // Shouldn't we rather reject attempts to synchronize it?
                    (*m_sourceListPtr)[source]->setForceSlowSync(true);
                }
            }
            dataSources.insert (vSource->getName());
    }

    SANSyncMode syncMode = SA_INVALID;
    vector<pair <string, string> > alertedSources;

    /* For each source to be notified do the following: */
    BOOST_FOREACH (string name, dataSources) {
        boost::shared_ptr<PersistentSyncSourceConfig> sc(getSyncSourceConfig(name));
        string sync = sc->getSync();
        SANSyncMode mode = AlertSyncMode(StringToSyncMode(sync, true), getPeerIsClient());
        if (mode == SA_SLOW) {
            (*m_sourceListPtr)[name]->setForceSlowSync(true);
            mode = SA_TWO_WAY;
        }
        if (mode < SA_FIRST || mode > SA_LAST) {
            SE_LOG_DEV(NULL, "Ignoring datastore %s with an invalid sync mode", name.c_str());
            continue;
        }
        syncMode = mode;
        hasSource = true;
        string uri = sc->getURINonEmpty();

        SourceType sourceType = sc->getSourceType();
        /*If the type is not set by user explictly, let's use backend default
         * value*/
        if(sourceType.m_format.empty()) {
            sourceType.m_format = (*m_sourceListPtr)[name]->getPeerMimeType();
        }
        if (!legacy) {
            /*If user did not use force type, we will always use the older type as
             * this is what most phones support*/
            int contentTypeB = StringToContentType (sourceType.m_format, sourceType.m_forceFormat);
            if (contentTypeB == WSPCTC_UNKNOWN) {
                contentTypeB = 0;
                SE_LOG_DEBUG(NULL, "Unknown datasource mimetype, use 0 as default");
            }
            SE_LOG_DEBUG(NULL, "SAN datastore %s uri %s type %u mode %d",
                         name.c_str(),
                         uri.c_str(),
                         contentTypeB,
                         mode);
            if ( san.AddSync(mode, (uInt32) contentTypeB, uri.c_str())) {
                SE_LOG_ERROR(NULL, "SAN: adding server alerted sync element failed");
            };
        } else {
            string mimetype = GetLegacyMIMEType(sourceType.m_format, sourceType.m_forceFormat);
            SE_LOG_DEBUG(NULL, "SAN datastore %s uri %s type %s",
                         name.c_str(),
                         uri.c_str(),
                         mimetype.c_str());
            alertedSources.push_back(std::make_pair(mimetype, uri));
        }
    }

    if (!hasSource) {
        SE_THROW ("No datastore enabled for server alerted sync!");
    }

    /* Generate the SAN Package */
    void *buffer;
    size_t sanSize;
    if (!legacy) {
        if (san.GetPackage(buffer, sanSize)){
            SE_LOG_ERROR(NULL, "SAN package generating failed");
            return false;
        }
        //TODO log the binary SAN content
    } else {
        SE_LOG_DEBUG(NULL, "SAN with overall sync mode %d", syncMode);
        if (san.GetPackageLegacy(buffer, sanSize, alertedSources, syncMode, getWBXML())){
            SE_LOG_ERROR(NULL, "SAN package generating failed");
            return false;
        }
        //SE_LOG_DEBUG(NULL, "SAN package content: %s", (char*)buffer);
    }

    m_agent = createTransportAgent();
    SE_LOG_INFO(NULL, "Server sending SAN");
    m_serverAlerted = true;
    m_agent->setContentType(!legacy ? 
                           TransportAgent::m_contentTypeServerAlertedNotificationDS
                           : (getWBXML() ? TransportAgent::m_contentTypeSyncWBXML :
                            TransportAgent::m_contentTypeSyncML));
    m_agent->send(reinterpret_cast <char *> (buffer), sanSize);
    //change content type
    m_agent->setContentType(getWBXML() ? TransportAgent::m_contentTypeSyncWBXML :
                            TransportAgent::m_contentTypeSyncML);

    TransportAgent::Status status;
    do {
        status = m_agent->wait();
    } while(status == TransportAgent::ACTIVE);
    if (status == TransportAgent::GOT_REPLY) {
        const char *reply;
        size_t replyLen;
        string contentType;
        m_agent->getReply (reply, replyLen, contentType);

        //sanity check for the reply 
        if (contentType.empty() || 
            contentType.find(TransportAgent::m_contentTypeSyncML) != contentType.npos ||
            contentType.find(TransportAgent::m_contentTypeSyncWBXML) != contentType.npos) {
            SharedBuffer request (reply, replyLen);
            //TODO should generate more reasonable sessionId here
            string sessionId ="";
            initServer (sessionId, request, contentType);
            return true;
        }
    }
    return false;
}

static string Step2String(sysync::uInt16 stepcmd)
{
    switch (stepcmd) {
    case sysync::STEPCMD_CLIENTSTART: return "STEPCMD_CLIENTSTART";
    case sysync::STEPCMD_CLIENTAUTOSTART: return "STEPCMD_CLIENTAUTOSTART";
    case sysync::STEPCMD_STEP: return "STEPCMD_STEP";
    case sysync::STEPCMD_GOTDATA: return "STEPCMD_GOTDATA";
    case sysync::STEPCMD_SENTDATA: return "STEPCMD_SENTDATA";
    case sysync::STEPCMD_SUSPEND: return "STEPCMD_SUSPEND";
    case sysync::STEPCMD_ABORT: return "STEPCMD_ABORT";
    case sysync::STEPCMD_TRANSPFAIL: return "STEPCMD_TRANSPFAIL";
    case sysync::STEPCMD_TIMEOUT: return "STEPCMD_TIMEOUT";
    case sysync::STEPCMD_SAN_CHECK: return "STEPCMD_SAN_CHECK";
    case sysync::STEPCMD_AUTOSYNC_CHECK: return "STEPCMD_AUTOSYNC_CHECK";
    case sysync::STEPCMD_OK: return "STEPCMD_OK";
    case sysync::STEPCMD_PROGRESS: return "STEPCMD_PROGRESS";
    case sysync::STEPCMD_ERROR: return "STEPCMD_ERROR";
    case sysync::STEPCMD_SENDDATA: return "STEPCMD_SENDDATA";
    case sysync::STEPCMD_NEEDDATA: return "STEPCMD_NEEDDATA";
    case sysync::STEPCMD_RESENDDATA: return "STEPCMD_RESENDDATA";
    case sysync::STEPCMD_DONE: return "STEPCMD_DONE";
    case sysync::STEPCMD_RESTART: return "STEPCMD_RESTART";
    case sysync::STEPCMD_NEEDSYNC: return "STEPCMD_NEEDSYNC";
    default: return StringPrintf("STEPCMD %d", stepcmd);
    }
}

const char *SyncContext::SyncFreezeName(SyncFreeze syncFreeze)
{
    switch (syncFreeze) {
    case SYNC_FREEZE_NONE: return "none";
    case SYNC_FREEZE_RUNNING: return "running";
    case SYNC_FREEZE_FROZEN: return "frozen";
    }
    return "???";
}

bool SyncContext::setFreeze(bool freeze)
{
    SyncFreeze newSyncFreeze = freeze ? SYNC_FREEZE_FROZEN : SYNC_FREEZE_RUNNING;
    if (m_syncFreeze == SYNC_FREEZE_NONE ||
        newSyncFreeze == m_syncFreeze) {
        SE_LOG_DEBUG(NULL, "SyncContext::setFreeze(%s): not changing freeze state: %s",
                     freeze ? "freeze" : "thaw",
                     SyncFreezeName(m_syncFreeze));
        return false;
    } else {
        SE_LOG_DEBUG(NULL, "SyncContext::setFreeze(%s): changing freeze state: %s -> %s",
                     freeze ? "freeze" : "thaw",
                     SyncFreezeName(m_syncFreeze),
                     SyncFreezeName(newSyncFreeze));
        if (m_agent) {
            SE_LOG_DEBUG(NULL, "SyncContext::setFreeze(): transport agent");
            m_agent->setFreeze(freeze);
        }
        if (m_sourceListPtr) {
            BOOST_FOREACH (SyncSource *source, *m_sourceListPtr) {
                SE_LOG_DEBUG(NULL, "SyncContext::setFreeze(): datastore %s", source->getDisplayName().c_str());
                source->setFreeze(freeze);
            }
        }
        m_syncFreeze = newSyncFreeze;
        return true;
    }
}

SyncMLStatus SyncContext::preSaveAdminData(SyncSource &source)
{
    if (!source.getTotalNumItemsReceived() &&
        !source.getTotalNumItemsSent() &&
        source.getFinalSyncMode() == SYNC_TWO_WAY &&
        !source.isFirstSync()) {
        SE_LOG_DEBUG(NULL, "requesting end of two-way sync with one source early because nothing changed");
        m_quitSync = true;
        return STATUS_SYNC_END_SHORTCUT;
    } else {
        return STATUS_OK;
    }
}

SharedSession *keepSession;

SyncMLStatus SyncContext::doSync()
{
    boost::shared_ptr<SuspendFlags::Guard> signalGuard;
    // install signal handlers unless this was explicitly disabled
    bool catchSignals = getenv("SYNCEVOLUTION_NO_SYNC_SIGNALS") == NULL;
    if (catchSignals) {
        SE_LOG_DEBUG(NULL, "sync is starting, catch signals");
        signalGuard = SuspendFlags::getSuspendFlags().activate();
    }

    // From now on it is possible to freeze the sync.
    m_syncFreeze = SYNC_FREEZE_RUNNING;

    // delay the sync for debugging purposes
    SE_LOG_DEBUG(NULL, "ready to sync");
    const char *delay = getenv("SYNCEVOLUTION_SYNC_DELAY");
    if (delay) {
        Sleep(atoi(delay));
    }

    SuspendFlags &flags = SuspendFlags::getSuspendFlags();
    if (!flags.isNormal()) {
        return (SyncMLStatus)sysync::LOCERR_USERABORT;
    }

    SyncMLStatus status = STATUS_OK;
    std::string s;

    if (m_serverMode &&
        !m_initialMessage.size() &&
        !m_localSync) {
        //This is a server alerted sync !
        string sanFormat (getSyncMLVersion());
        uint16_t version = 12;
        if (boost::iequals (sanFormat, "1.2") ||
            sanFormat == "") {
            version = 12;
        } else if (boost::iequals (sanFormat, "1.1")) {
            version = 11;
        } else {
            version = 10;
        }

        bool status = true;
        try {
            status = sendSAN (version);
        } catch (TransportException e) {
            if (!sanFormat.empty()){
                throw;
            }
            status = false;
            //by pass the exception if we will try again with legacy SANFormat
        }

        if (!flags.isNormal()) {
            return (SyncMLStatus)sysync::LOCERR_USERABORT;
        }

        if (! status) {
            if (sanFormat.empty()) {
                SE_LOG_DEBUG(NULL, "Server Alerted Sync init with SANFormat %d failed, trying with legacy format", version);
                version = 11;
                if (!sendSAN (version)) {
                    // return a proper error code 
                    Exception::throwError(SE_HERE, "Server Alerted Sync init failed");
                }
            } else {
                // return a proper error code 
                Exception::throwError(SE_HERE, "Server Alerted Sync init failed");
            }
        }
    }

    if (!flags.isNormal()) {
        return (SyncMLStatus)sysync::LOCERR_USERABORT;
    }

    // re-init engine with all sources configured
    string xml, configname;
    initEngine(true);

    SharedKey targets;
    SharedKey target;
    if (m_serverMode) {
        // Server engine has no profiles. All settings have be done
        // via the XML configuration or function parameters (session ID
        // in OpenSession()).
    } else {
        // check the settings status (MUST BE DONE TO MAKE SETTINGS READY)
        SharedKey profiles = m_engine.OpenKeyByPath(SharedKey(), "/profiles");
        m_engine.GetStrValue(profiles, "settingsstatus");
        // allow creating new settings when existing settings are not up/downgradeable
        m_engine.SetStrValue(profiles, "overwrite",  "1");
        // check status again
        m_engine.GetStrValue(profiles, "settingsstatus");
    
        // open first profile
        SharedKey profile;
        profile = m_engine.OpenSubkey(profiles, sysync::KEYVAL_ID_FIRST, true);
        if (!profile) {
            // no profile exists  yet, create default profile
            profile = m_engine.OpenSubkey(profiles, sysync::KEYVAL_ID_NEW_DEFAULT);
        }
        if (!m_localSync) {
            // Not needed for local sync and might even be
            // impossible/wrong because username could refer to an
            // identity provider which cannot return a plain string.
            SE_LOG_DEBUG(NULL, "copying syncURL, username, password to Synthesis engine");
            m_engine.SetStrValue(profile, "serverURI", getUsedSyncURL());
            UserIdentity syncUser = getSyncUser();
            InitStateString syncPassword = getSyncPassword();
            boost::shared_ptr<AuthProvider> provider = AuthProvider::create(syncUser, syncPassword);
            Credentials cred = provider->getCredentials();
            const std::string &user = cred.m_username;
            const std::string &password = cred.m_password;
            m_engine.SetStrValue(profile, "serverUser", user);
            m_engine.SetStrValue(profile, "serverPassword", password);
        }
        m_engine.SetInt32Value(profile, "encoding",
                               getWBXML() ? 1 /* WBXML */ : 2 /* XML */);

        // Iterate over all data stores in the XML config
        // and match them with sync sources.
        // TODO: let sync sources provide their own
        // XML snippets (inside <client> and inside <datatypes>).
        targets = m_engine.OpenKeyByPath(profile, "targets");

        for(target = m_engine.OpenSubkey(targets, sysync::KEYVAL_ID_FIRST, true);
            target;
            target = m_engine.OpenSubkey(targets, sysync::KEYVAL_ID_NEXT, true)) {
            s = m_engine.GetStrValue(target, "dbname");
            SyncSource *source = findSource(s);
            if (source) {
                m_engine.SetInt32Value(target, "enabled", 1);
                int slow = 0;
                int direction = 0;
                string sync = source->getSync();
                // this code only runs when we are the client,
                // take that into account for the "from-local/remote" modes
                SimpleSyncMode mode = SimplifySyncMode(StringToSyncMode(sync), false);
                if (mode == SIMPLE_SYNC_SLOW) {
                    slow = 1;
                    direction = 0;
                } else if (mode == SIMPLE_SYNC_TWO_WAY) {
                    slow = 0;
                    direction = 0;
                } else if (mode == SIMPLE_SYNC_REFRESH_FROM_REMOTE) {
                    slow = 1;
                    direction = 1;
                } else if (mode == SIMPLE_SYNC_REFRESH_FROM_LOCAL) {
                    slow = 1;
                    direction = 2;
                } else if (mode == SIMPLE_SYNC_ONE_WAY_FROM_REMOTE) {
                    slow = 0;
                    direction = 1;
                } else if (mode == SIMPLE_SYNC_ONE_WAY_FROM_LOCAL) {
                    slow = 0;
                    direction = 2;
                } else {
                    source->throwError(SE_HERE, string("invalid sync mode: ") + sync);
                }
                m_engine.SetInt32Value(target, "forceslow", slow);
                m_engine.SetInt32Value(target, "syncmode", direction);

                string uri = source->getURINonEmpty();
                m_engine.SetStrValue(target, "remotepath", uri);
            } else {
                m_engine.SetInt32Value(target, "enabled", 0);
            }
        }

        // Close all keys so that engine can flush the modified config.
        // Otherwise the session reads the unmodified values from the
        // created files while the updated values are still in memory.
        target.reset();
        targets.reset();
        profile.reset();
        profiles.reset();

        // reopen profile keys
        profiles = m_engine.OpenKeyByPath(SharedKey(), "/profiles");
        m_engine.GetStrValue(profiles, "settingsstatus");
        profile = m_engine.OpenSubkey(profiles, sysync::KEYVAL_ID_FIRST);
        targets = m_engine.OpenKeyByPath(profile, "targets");
    }

    m_retries = 0;

    //Create the transport agent if not already created
    if(!m_agent) {
        m_agent = createTransportAgent();
    }

    // server in local sync initiates sync by passing data to forked process
    if (m_serverMode && m_localSync) {
        m_serverAlerted = true;
    }

    sysync::TEngineProgressInfo progressInfo;
    sysync::uInt16 stepCmd = 
        (m_localSync && m_serverMode) ? sysync::STEPCMD_NEEDDATA :
        m_serverMode ?
        sysync::STEPCMD_GOTDATA :
        sysync::STEPCMD_CLIENTSTART;
    SharedSession session = m_engine.OpenSession(m_sessionID);
    SharedBuffer sendBuffer;
    std::auto_ptr<SessionSentinel> sessionSentinel(new SessionSentinel(*this, session));

    if (m_serverMode && !m_localSync) {
        m_engine.WriteSyncMLBuffer(session,
                                   m_initialMessage.get(),
                                   m_initialMessage.size());
        SharedKey sessionKey = m_engine.OpenSessionKey(session);
        m_engine.SetStrValue(sessionKey,
                             "contenttype",
                             m_initialMessageType);
        m_initialMessage.reset();

        // TODO: set "sendrespuri" session key to control
        // whether the generated messages contain a respURI
        // (not needed for OBEX)
    }

    // Special case local sync when nothing changed: we can be sure
    // that we can do another sync from exactly the same state (nonce,
    // source change tracking meta data, etc.) and be successful
    // again. In such a case we can avoid unnecessary updates of the
    // .ini and .bfi files.
    //
    // To detect this, the server side hooks into the SaveAdminData
    // operation and replaces it with just returning an "aborted by
    // user" error.
    if (m_serverMode &&
        m_localSync &&
        m_sourceListPtr->size() == 1) {
        SyncSource *source = *(*m_sourceListPtr).begin();
        source->getOperations().m_saveAdminData.getPreSignal().connect(boost::bind(&SyncContext::preSaveAdminData, this, _1));
    }

    // Sync main loop: runs until SessionStep() signals end or error.
    // Exceptions are caught and lead to a call of SessionStep() with
    // parameter STEPCMD_ABORT -> abort session as soon as possible.
    bool aborting = false;
    int suspending = 0; 
    Timespec sendStart, resendStart;
    int requestNum = 0;
    sysync::uInt16 previousStepCmd = stepCmd;
    std::vector<int> numItemsReceived; // source->getTotalNumItemsReceived() for each source, see STEPCMD_SENDDATA
    m_quitSync = false;
    do {
        try {
            if (m_quitSync &&
                !m_serverMode) {
                SE_LOG_DEBUG(NULL, "ending sync early as requested");
                // Intentionally prevent destructing the Synthesis
                // engine and session destruction by keeping a
                // reference to it around forever, because destroying
                // the session would cause undesired disk writes.
                keepSession = new SharedSession(session);
                break;
            }
            // check for suspend, if so, modify step command for next step
            // Since the suspend will actually be committed until it is
            // sending out a message, we can safely delay the suspend to
            // GOTDATA state.
            // After exception occurs, stepCmd will be set to abort to force
            // aborting, must avoid to change it back to suspend cmd.
            if (flags.isSuspended() && stepCmd == sysync::STEPCMD_GOTDATA) {
                SE_LOG_DEBUG(NULL, "suspending before SessionStep() in STEPCMD_GOTDATA as requested by user");
                stepCmd = sysync::STEPCMD_SUSPEND;
            }

            // Aborting is useful while waiting for a reply and before
            // sending a message (which will just lead to us waiting
            // for the reply, but possibly after doing some slow network
            // IO for setting up the message send).
            //
            // While processing a message we let the engine run, because
            // that is a) likely to be done soon and b) may reduce the
            // breakage caused by aborting a running sync.
            //
            // This check here covers the "waiting for reply" case.
            if ((stepCmd == sysync::STEPCMD_RESENDDATA ||
                 stepCmd == sysync::STEPCMD_SENTDATA ||
                 stepCmd == sysync::STEPCMD_NEEDDATA) &&
                flags.isAborted()) {
                SE_LOG_DEBUG(NULL, "aborting before SessionStep() in %s as requested by script",
                             Step2String(stepCmd).c_str());
                stepCmd = sysync::STEPCMD_ABORT;
            }

            // take next step, but don't abort twice: instead
            // let engine contine with its shutdown
            if (stepCmd == sysync::STEPCMD_ABORT) {
                if (aborting) {
                    SE_LOG_DEBUG(NULL, "engine already notified of abort request, reverting to %s",
                                 Step2String(previousStepCmd).c_str());
                    stepCmd = previousStepCmd;
                } else {
                    aborting = true;
                }
            }
            // same for suspending
            if (stepCmd == sysync::STEPCMD_SUSPEND) {
                if (suspending) {
                    SE_LOG_DEBUG(NULL, "engine already notified of suspend request, reverting to %s",
                                 Step2String(previousStepCmd).c_str());
                    stepCmd = previousStepCmd;
                    suspending++;
                } else {
                    suspending++; 
                }
            }

            // Need to wait for setFrozen(false) or suspend/abort request.
            // Such a call can come in via a D-Bus interface that we keep
            // active by servicing the event loop inside GRunWhile().
            //
            // We freeze without notifying our peer. It will freeze itself
            // eventually because we stop exchanging SyncML messages.
            if (!aborting && !suspending && m_syncFreeze == SYNC_FREEZE_FROZEN) {
                SE_LOG_DEBUG(NULL, "freezing sync");
                GRunWhile(boost::lambda::var(m_syncFreeze) == SYNC_FREEZE_FROZEN &&
                          boost::lambda::bind(&SuspendFlags::isNormal, &flags));
            }

            if (stepCmd == sysync::STEPCMD_NEEDDATA) {
                // Engine already notified. Don't call it twice
                // with this state, because it doesn't know how
                // to handle this. Skip the SessionStep() call
                // and wait for response.
            } else {
                if (getLogLevel() > 4) {
                    SE_LOG_DEBUG(NULL, "before SessionStep: %s", Step2String(stepCmd).c_str());
                }
                m_engine.SessionStep(session, stepCmd, &progressInfo);
                if (getLogLevel() > 4) {
                    SE_LOG_DEBUG(NULL, "after SessionStep: %s", Step2String(stepCmd).c_str());
                }
                reportStepCmd(stepCmd);
            }

            if (stepCmd == sysync::STEPCMD_SENDDATA &&
                checkForScriptAbort(session)) {
                SE_LOG_DEBUG(NULL, "aborting after SessionStep() in STEPCMD_SENDDATA as requested by script");

                // Catch outgoing message and abort if requested by script.
                // Report which sources are affected, based on their status code.
                set<string> sources;
                BOOST_FOREACH(SyncSource *source, *m_sourceListPtr) {
                    if (source->getStatus() == STATUS_UNEXPECTED_SLOW_SYNC) {
                        string name = source->getVirtualSource();
                        if (name.empty()) {
                            name = source->getName();
                        }
                        sources.insert(name);
                    }
                }
                string explanation = SyncReport::slowSyncExplanation(m_server,
                                                                     sources);
                if (!explanation.empty()) {
                    string sourceparam = boost::join(sources, " ");
                    SE_LOG_ERROR(NULL,
                                 "Aborting because of unexpected slow sync for datastore(s): %s",
                                 sourceparam.c_str());
                    SE_LOG_INFO(NULL, "%s", explanation.c_str());
                } else {
                    // we should not get here, but if we do, at least log something
                    SE_LOG_ERROR(NULL, "aborting as requested by script");
                }
                stepCmd = sysync::STEPCMD_ABORT;
                continue;
            } else if (stepCmd == sysync::STEPCMD_SENDDATA &&
                       flags.isAborted()) {
                // Catch outgoing message and abort if requested by user.
                SE_LOG_DEBUG(NULL, "aborting after SessionStep() in STEPCMD_SENDDATA as requested by user");
                stepCmd = sysync::STEPCMD_ABORT;
                continue;
            } else if (suspending == 1) {
                //During suspention we actually insert a STEPCMD_SUSPEND cmd
                //Should restore to the original step here
                stepCmd = previousStepCmd;
                continue;
            }

            switch (stepCmd) {
            case sysync::STEPCMD_OK:
                // no progress info, call step again
                stepCmd = sysync::STEPCMD_STEP;
                break;
            case sysync::STEPCMD_PROGRESS:
                // new progress info to show
                // Check special case of interactive display alert
                if (progressInfo.eventtype == sysync::PEV_DISPLAY100) {
                    // alert 100 received from remote, message text is in
                    // SessionKey's "displayalert" field
                    SharedKey sessionKey = m_engine.OpenSessionKey(session);
                    // get message from server to display
                    s = m_engine.GetStrValue(sessionKey,
                                             "displayalert");
                    displayServerMessage(s);
                } else {
                    switch (progressInfo.targetID) {
                    case sysync::KEYVAL_ID_UNKNOWN:
                    case 0 /* used with PEV_SESSIONSTART?! */:
                        displaySyncProgress(sysync::TProgressEventEnum(progressInfo.eventtype),
                                            progressInfo.extra1,
                                            progressInfo.extra2,
                                            progressInfo.extra3);
                        if (progressInfo.eventtype == sysync::PEV_SESSIONEND &&
                            !status) {
                            // remember sync result
                            status = SyncMLStatus(progressInfo.extra1);
                        }
                        break;
                    default: {
                        // specific for a certain sync source:
                        // find it...
                        SyncSource *source = m_sourceListPtr->lookupBySynthesisID(progressInfo.targetID);
                        if (source) {
                            displaySourceProgress(*source,
                                                  SyncSourceEvent(sysync::TProgressEventEnum(progressInfo.eventtype),
                                                                  progressInfo.extra1,
                                                                  progressInfo.extra2,
                                                                  progressInfo.extra3),
                                                  false);
                        } else {
                            Exception::throwError(SE_HERE, std::string("unknown target ") + s);
                        }
                        target.reset();
                        break;
                    }
                    }
                }
                stepCmd = sysync::STEPCMD_STEP;
                break;
            case sysync::STEPCMD_ERROR:
                // error, terminate (should not happen, as status is
                // already checked above)
                break;
            case sysync::STEPCMD_RESTART:
                // make sure connection is closed and will be re-opened for next request
                // tbd: close communication channel if still open to make sure it is
                //       re-opened for the next request
                stepCmd = sysync::STEPCMD_STEP;
                m_retries = 0;
                break;
            case sysync::STEPCMD_SENDDATA: {
                // We'll be busy for a while with network IO, so give
                // sources a chance to do some work in parallel and
                // flush pending progress notifications.
                if (m_sourceListPtr) {
                    bool needResults = true;
                    if (numItemsReceived.size() < m_sourceListPtr->size()) {
                        numItemsReceived.insert(numItemsReceived.end(),
                                                m_sourceListPtr->size() - numItemsReceived.size(),
                                                0);
                    }
                    for (size_t i = 0; i < numItemsReceived.size(); i++) {
                        SyncSource *source = (*m_sourceListPtr->getSourceSet())[i];
                        int received = source->getTotalNumItemsReceived();
                        SE_LOG_DEBUG(source->getDisplayName(), "total number of items received %d",
                                     received);
                        if (numItemsReceived[i] != received) {
                            numItemsReceived[i] = received;
                            needResults = false;
                        }
                    }

                    BOOST_FOREACH (SyncSource *source, *m_sourceListPtr) {
                        source->flushItemChanges();
                        if (needResults) {
                            source->finishItemChanges();
                        }
                        displaySourceProgress(*source, SyncSourceEvent(), false);
                    }
                }

                // send data to remote

                SharedKey sessionKey = m_engine.OpenSessionKey(session);
                if (m_serverMode) {
                    m_agent->setURL("");
                } else {
                    // use OpenSessionKey() and GetValue() to retrieve "connectURI"
                    // and "contenttype" to be used to send data to the server
                    s = m_engine.GetStrValue(sessionKey,
                                             "connectURI");
                    m_agent->setURL(s);
                }
                s = m_engine.GetStrValue(sessionKey,
                                         "contenttype");
                m_agent->setContentType(s);
                sessionKey.reset();

                sendStart = resendStart = Timespec::monotonic();
                requestNum ++;
                // use GetSyncMLBuffer()/RetSyncMLBuffer() to access the data to be
                // sent or have it copied into caller's buffer using
                // ReadSyncMLBuffer(), then send it to the server
                sendBuffer = m_engine.GetSyncMLBuffer(session, true);
                if (m_serverMode && m_quitSync) {
                    // When aborting prematurely, skip the server's
                    // last reply message and instead tell the client
                    // to quit.
                    m_agent->setContentType("quitsync");
                    m_agent->send(NULL, 0);
                } else {
                    m_agent->send(sendBuffer.get(), sendBuffer.size());
                }
                stepCmd = sysync::STEPCMD_SENTDATA; // we have sent the data
                break;
            }
            case sysync::STEPCMD_RESENDDATA: {
                SE_LOG_INFO(NULL, "resend previous message, retry #%d", m_retries);
                resendStart = Timespec::monotonic();
                /* We are resending previous message, just read from the
                 * previous buffer */
                m_agent->send(sendBuffer.get(), sendBuffer.size());
                stepCmd = sysync::STEPCMD_SENTDATA; // we have sent the data
                break;
            }
            case sysync::STEPCMD_NEEDDATA:
                if (!sendStart) {
                    // no message sent yet, record start of wait for data
                    sendStart = Timespec::monotonic();
                }
                switch (m_agent->wait()) {
                case TransportAgent::ACTIVE:
                    // Still sending the data?! Don't change anything,
                    // skip SessionStep() above.
                    break;
               
                case TransportAgent::TIME_OUT: {
                    double duration = (Timespec::monotonic() - sendStart).duration();
                    // HTTP SyncML servers cannot resend a HTTP POST
                    // reply.  Other server transports could in theory
                    // resend, but don't have the necessary D-Bus APIs
                    // (MB #6370).
                    // Same if() as below for FAILED.
                    if (m_serverMode ||
                        !m_retryInterval || duration + 0.9 >= m_retryDuration || requestNum == 1) {
                        SE_LOG_INFO(NULL,
                                    "Transport giving up after %d retries and %ld:%02ldmin",
                                    m_retries,
                                    (long)duration / 60,
                                    (long)duration % 60);
                        SE_THROW_EXCEPTION(TransportException, "timeout, retry period exceeded");
                    }else {
                        // Timeout must have been due to retryInterval having passed, resend
                        // immediately.
                        m_retries ++;
                        stepCmd = sysync::STEPCMD_RESENDDATA;
                    }
                    break;
                    }
                case TransportAgent::GOT_REPLY: {
                    const char *reply;
                    size_t replylen;
                    string contentType;
                    m_agent->getReply(reply, replylen, contentType);

                    // sanity check for reply: if known at all, it must be either XML or WBXML
                    if (contentType.empty() ||
                        contentType.find("application/vnd.syncml+wbxml") != contentType.npos ||
                        contentType.find("application/vnd.syncml+xml") != contentType.npos) {
                        // put answer received earlier into SyncML engine's buffer
                        m_retries = 0;
                        sendBuffer.reset();
                        m_engine.WriteSyncMLBuffer(session,
                                                   reply,
                                                   replylen);
                        if (m_serverMode) {
                            SharedKey sessionKey = m_engine.OpenSessionKey(session);
                            m_engine.SetStrValue(sessionKey,
                                                 "contenttype",
                                                 contentType);
                        }
                        stepCmd = sysync::STEPCMD_GOTDATA; // we have received response data
                        break;
                    } else if (contentType == "quitsync") {
                        SE_LOG_DEBUG(NULL, "server is asking us to quit the sync session");
                        // Fake "done" events for each active source.
                        BOOST_FOREACH(SyncSource *source, *m_sourceListPtr) {
                            if (source->getFinalSyncMode() != SYNC_NONE) {
                                displaySourceProgress(*source,
                                                      SyncSourceEvent(sysync::PEV_SYNCEND, 0, 0, 0),
                                                      true);
                            }
                        }
                        m_quitSync = true;
                        break;
                    } else {
                        SE_LOG_DEBUG(NULL, "unexpected content type '%s' in reply, %d bytes:\n%.*s",
                                     contentType.c_str(), (int)replylen, (int)replylen, reply);
                        SE_LOG_ERROR(NULL, "unexpected reply from peer; might be a temporary problem, try again later");
                      } //fall through to network failure case
                }
                /* If this is a network error, it usually failed quickly, retry
                 * immediately has likely no effect. Manually sleep here to wait a while
                 * before retry. Sleep time will be calculated so that the
                 * message sending interval equals m_retryInterval.
                 */
                case TransportAgent::FAILED: {
                    // Send might have failed because of abort or
                    // suspend request.
                    if (flags.isSuspended()) {
                        SE_LOG_DEBUG(NULL, "suspending after TransportAgent::FAILED as requested by user");
                        stepCmd = sysync::STEPCMD_SUSPEND;
                        break;
                    } else if (flags.isAborted()) {
                        SE_LOG_DEBUG(NULL, "aborting after TransportAgent::FAILED as requested by user");
                        stepCmd = sysync::STEPCMD_ABORT;
                        break;
                    }

                    Timespec curTime = Timespec::monotonic();
                    double duration = (curTime - sendStart).duration();
                    double resendDelay = m_retryInterval - (curTime - resendStart).duration();
                    if (resendDelay < 0) {
                        resendDelay = 0;
                    }
                    // Similar if() as above for TIME_OUT. In addition, we must check that
                    // the next resend won't happen after the retryDuration, because then
                    // we might as well give up now immediately. Include some fuzz factor
                    // in case we woke up slightly too early.
                    if (m_serverMode ||
                        !m_retryInterval || duration + resendDelay + 0.9 >= m_retryDuration || requestNum == 1) {
                        SE_LOG_INFO(NULL,
                                    "Transport giving up after %d retries and %ld:%02ldmin",
                                    m_retries,
                                    (long)duration / 60,
                                    (long)duration % 60);
                        SE_THROW_EXCEPTION(TransportException, "transport failed, retry period exceeded");
                    } else {
                        // Resend after having ensured that the retryInterval is over.
                        if (resendDelay > 0) {
                            if (Sleep(resendDelay) > 0) {
                                if (flags.isSuspended()) {
                                    SE_LOG_DEBUG(NULL, "suspending after premature exit from sleep() caused by user suspend");
                                    stepCmd = sysync::STEPCMD_SUSPEND;
                                } else {
                                    SE_LOG_DEBUG(NULL, "aborting after premature exit from sleep() caused by user abort");
                                    stepCmd = sysync::STEPCMD_ABORT;
                                }
                                break;
                            } 
                        }

                        m_retries ++;
                        stepCmd = sysync::STEPCMD_RESENDDATA;
                    }
                    break;
                }
                case TransportAgent::CANCELED:
                    // Send might have failed because of abort or
                    // suspend request.
                    if (flags.isSuspended()) {
                        SE_LOG_DEBUG(NULL, "suspending after TransportAgent::CANCELED as requested by user");
                        stepCmd = sysync::STEPCMD_SUSPEND;
                        break;
                    } else if (flags.isAborted()) {
                        SE_LOG_DEBUG(NULL, "aborting after TransportAgent::CANCELED as requested by user");
                        stepCmd = sysync::STEPCMD_ABORT;
                        break;
                    }
                    // not sure exactly why it is canceled
                    SE_THROW_EXCEPTION_STATUS(BadSynthesisResult,
                                              "transport canceled",
                                              sysync::LOCERR_USERABORT);
                    break;
                default:
                    stepCmd = sysync::STEPCMD_TRANSPFAIL; // communication with server failed
                    break;
                }
            }

            // Don't tell engine to abort when it already did.
            if (aborting && stepCmd == sysync::STEPCMD_ABORT) {
                stepCmd = sysync::STEPCMD_DONE;
            }

            previousStepCmd = stepCmd;
            // loop until session done or aborted with error
        } catch (const BadSynthesisResult &result) {
            if (result.result() == sysync::LOCERR_USERABORT && aborting) {
                SE_LOG_INFO(NULL, "Aborted as requested.");
                stepCmd = sysync::STEPCMD_DONE;
            } else if (result.result() == sysync::LOCERR_USERSUSPEND && suspending) {
                SE_LOG_INFO(NULL, "Suspended as requested.");
                stepCmd = sysync::STEPCMD_DONE;
            } else if (aborting) {
                // aborting very early can lead to results different from LOCERR_USERABORT
                // => don't treat this as error
                SE_LOG_INFO(NULL, "Aborted with unexpected result (%d)",
                            static_cast<int>(result.result()));
                stepCmd = sysync::STEPCMD_DONE;
            } else {
                Exception::handle(&status);
                SE_LOG_DEBUG(NULL, "aborting after catching fatal error");
                // Don't tell engine to abort when it already did.
                stepCmd = aborting ? sysync::STEPCMD_DONE : sysync::STEPCMD_ABORT;
            }
        } catch (...) {
            Exception::handle(&status);
            SE_LOG_DEBUG(NULL, "aborting after catching fatal error");
            // Don't tell engine to abort when it already did.
            stepCmd = aborting ? sysync::STEPCMD_DONE : sysync::STEPCMD_ABORT;
        }
    } while (stepCmd != sysync::STEPCMD_DONE && stepCmd != sysync::STEPCMD_ERROR);

    // If we get here without error, then close down connection normally.
    // Otherwise destruct the agent without further communication.
    if (!status && !flags.isAborted()) {
        try {
            m_agent->shutdown();
            // TODO: implement timeout for peers which fail to respond
            while (!flags.isAborted() &&
                   m_agent->wait(true) == TransportAgent::ACTIVE) {
                // TODO: allow aborting the sync here
            }
        } catch (...) {
            status = handleException();
        }
    }

    // Let session shut down before auto-destructing anything else
    // (like our signal blocker). This may take a while, because it
    // may involve shutting down the helper background thread which
    // opened our local datastore.
    SE_LOG_DEBUG(NULL, "closing session");
    // setFreeze() no longer has an effect and returns false from now on.
    m_syncFreeze = SYNC_FREEZE_NONE;
    m_initialMessage.reset();
    sessionSentinel.reset();
    sendBuffer.reset();
    session.reset();
    SE_LOG_DEBUG(NULL, "session closed");

    return status;
}

string SyncContext::getSynthesisDatadir()
{
    if (isEphemeral() && m_sourceListPtr) {
        // Suppress writing in libsynthesis binfile client.
        return "/dev/null";
    } else if (m_localSync && !m_serverMode) {
        return m_localClientRootPath + "/.synthesis";
    } else {
        return getRootPath() + "/.synthesis";
    }
}

SyncMLStatus SyncContext::handleException()
{
    SyncMLStatus res = Exception::handle();
    return res;
}

void SyncContext::status()
{
    checkConfig("status check");

    SourceList sourceList(*this, false);
    initSources(sourceList);
    PasswordConfigProperty::checkPasswords(getUserInterfaceNonNull(), *this,
                                           // Don't need sync passwords.
                                           PasswordConfigProperty::CHECK_PASSWORD_ALL & ~PasswordConfigProperty::CHECK_PASSWORD_SYNC,
                                           sourceList.getSourceNames());
    BOOST_FOREACH(SyncSource *source, sourceList) {
        source->open();
    }

    SyncReport changes;
    checkSourceChanges(sourceList, changes);

    stringstream out;
    changes.prettyPrint(out,
                        SyncReport::WITHOUT_SERVER|
                        SyncReport::WITHOUT_CONFLICTS|
                        SyncReport::WITHOUT_REJECTS|
                        SyncReport::WITH_TOTAL);
    SE_LOG_INFO(NULL, "Local item changes:\n%s",
                out.str().c_str());

    sourceList.accessSession(getLogDir());
    Logger::instance().setLevel(Logger::INFO);
    string prevLogdir = sourceList.getPrevLogdir();
    bool found = access(prevLogdir.c_str(), R_OK|X_OK) == 0;

    if (found) {
        if (!m_quiet && getPrintChanges()) {
            try {
                sourceList.setPath(prevLogdir);
                sourceList.dumpDatabases("current", NULL);
                sourceList.dumpLocalChanges("", "after", "current", "");
            } catch(...) {
                Exception::handle();
            }
        }
    } else {
        SE_LOG_SHOW(NULL, "Previous log directory not found.");
        if (getLogDir().empty()) {
            SE_LOG_SHOW(NULL, "Enable the 'logdir' option and synchronize to use this feature.");
        }
    }
}

void SyncContext::checkStatus(SyncReport &report)
{
    checkConfig("status check");

    SourceList sourceList(*this, false);
    initSources(sourceList);
    PasswordConfigProperty::checkPasswords(getUserInterfaceNonNull(), *this,
                                           // Don't need sync passwords.
                                           PasswordConfigProperty::CHECK_PASSWORD_ALL & ~PasswordConfigProperty::CHECK_PASSWORD_SYNC,
                                           sourceList.getSourceNames());
    BOOST_FOREACH(SyncSource *source, sourceList) {
        source->open();
    }

    checkSourceChanges(sourceList, report);
}

static void logRestoreReport(const SyncReport &report, bool dryrun)
{
    if (!report.empty()) {
        stringstream out;
        report.prettyPrint(out, SyncReport::WITHOUT_SERVER|SyncReport::WITHOUT_CONFLICTS|SyncReport::WITH_TOTAL);
        SE_LOG_INFO(NULL, "Item changes %s applied locally during restore:\n%s",
                    dryrun ? "to be" : "that were",
                    out.str().c_str());
        SE_LOG_INFO(NULL, "The same incremental changes will be applied to the server during the next sync.");
        SE_LOG_INFO(NULL, "Use -sync refresh-from-client to replace the complete data on the server.");
    }
}

void SyncContext::checkSourceChanges(SourceList &sourceList, SyncReport &changes)
{
    changes.setStart(time(NULL));
    BOOST_FOREACH(SyncSource *source, sourceList) {
        SyncSourceReport local;
        if (source->getOperations().m_checkStatus) {
            source->getOperations().m_checkStatus(local);
        } else {
            // no information available
            local.setItemStat(SyncSourceReport::ITEM_LOCAL,
                              SyncSourceReport::ITEM_ADDED,
                              SyncSourceReport::ITEM_TOTAL,
                              -1);
            local.setItemStat(SyncSourceReport::ITEM_LOCAL,
                              SyncSourceReport::ITEM_UPDATED,
                              SyncSourceReport::ITEM_TOTAL,
                              -1);
            local.setItemStat(SyncSourceReport::ITEM_LOCAL,
                              SyncSourceReport::ITEM_REMOVED,
                              SyncSourceReport::ITEM_TOTAL,
                              -1);
            local.setItemStat(SyncSourceReport::ITEM_LOCAL,
                              SyncSourceReport::ITEM_ANY,
                              SyncSourceReport::ITEM_TOTAL,
                              -1);
        }
        changes.addSyncSourceReport(source->getName(), local);
    }
    changes.setEnd(time(NULL));
}

bool SyncContext::checkForScriptAbort(SharedSession session)
{
    try {
        SharedKey sessionKey = m_engine.OpenSessionKey(session);
        SharedKey contextKey = m_engine.OpenKeyByPath(sessionKey, "/sessionvars");
        bool abort = m_engine.GetInt32Value(contextKey, "delayedabort");
        return abort;
    } catch (NoSuchKey) {
        // this is necessary because the session might already have
        // been closed, which removes the variable
        return false;
    } catch (BadSynthesisResult) {
        return false;
    }
}

void SyncContext::restore(const string &dirname, RestoreDatabase database)
{
    checkConfig("restore");

    SourceList sourceList(*this, false);
    sourceList.accessSession(dirname.c_str());
    Logger::instance().setLevel(Logger::INFO);
    initSources(sourceList);
    PasswordConfigProperty::checkPasswords(getUserInterfaceNonNull(), *this,
                                           // Don't need sync passwords.
                                           PasswordConfigProperty::CHECK_PASSWORD_ALL & ~PasswordConfigProperty::CHECK_PASSWORD_SYNC,
                                           sourceList.getSourceNames());
    string datadump = database == DATABASE_BEFORE_SYNC ? "before" : "after";

    BOOST_FOREACH(SyncSource *source, sourceList) {
        // fake a source alert event
        displaySourceProgress(*source, SyncSourceEvent(sysync::PEV_ALERTED, -1, 0, 0), true);
        source->open();
    }

    if (!m_quiet && getPrintChanges()) {
        sourceList.dumpDatabases("current", NULL);
        sourceList.dumpLocalChanges(dirname, "current", datadump, "",
                                    "Data changes to be applied locally during restore:\n",
                                    "CLIENT_TEST_LEFT_NAME='current data' "
                                    "CLIENT_TEST_REMOVED='after restore' " 
                                    "CLIENT_TEST_REMOVED='to be removed' "
                                    "CLIENT_TEST_ADDED='to be added'");
    }

    SyncReport report;
    try {
        BOOST_FOREACH(SyncSource *source, sourceList) {
            SyncSourceReport sourcereport;
            try {
                displaySourceProgress(*source, SyncSourceEvent(sysync::PEV_SYNCSTART, 0, 0, 0), true);
                sourceList.restoreDatabase(*source,
                                           datadump,
                                           m_dryrun,
                                           sourcereport);
                displaySourceProgress(*source, SyncSourceEvent(sysync::PEV_SYNCEND, 0, 0, 0), true);
                report.addSyncSourceReport(source->getName(), sourcereport);
            } catch (...) {
                sourcereport.recordStatus(STATUS_FATAL);
                report.addSyncSourceReport(source->getName(), sourcereport);
                throw;
            }
        }
    } catch (...) {
        logRestoreReport(report, m_dryrun);
        throw;
    }
    logRestoreReport(report, m_dryrun);
}

void SyncContext::getSessions(vector<string> &dirs)
{
    LogDir::create(*this)->previousLogdirs(dirs);
}

string SyncContext::readSessionInfo(const string &dir, SyncReport &report)
{
    boost::shared_ptr<LogDir> logging(LogDir::create(*this));
    logging->openLogdir(dir);
    logging->readReport(report);
    return logging->getPeerNameFromLogdir(dir);
}

#ifdef ENABLE_UNIT_TESTS
/**
 * This class works LogDirTest as scratch directory.
 * LogDirTest/[file_event|file_contact]_[one|two|empty] contain different
 * sets of items for use in a FileSyncSource.
 *
 * With that setup and a fake SyncContext it is possible to simulate
 * sessions and test the resulting logdirs.
 */
class LogDirTest : public CppUnit::TestFixture
{
    class LogContext : public SyncContext, public Logger
    {
    public:
        LogContext() :
            SyncContext("nosuchconfig@nosuchcontext")
        {}

        ostringstream m_out;

        /** capture output produced while test ran */
        void messagev(const MessageOptions &options,
                      const char *format,
                      va_list args)
        {
            std::string str = StringPrintfV(format, args);
            m_out << '[' << levelToStr(options.m_level) << ']' << str;
            if (!boost::ends_with(str, "\n")) {
                m_out << std::endl;
            }
        }
    };

    boost::shared_ptr<LogContext> m_logContext;

public:
    LogDirTest() :
        m_maxLogDirs(10)
    {
    }

    ~LogDirTest() {
    }

    void setUp() {
        static const char *vcard_1 =
            "BEGIN:VCARD\n"
            "VERSION:2.1\n"
            "TITLE:tester\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "X-MOZILLA-HTML:FALSE\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "EMAIL:john.doe@work.com\n"
            "X-AIM:AIM JOHN\n"
            "END:VCARD\n";
        static const char *vcard_2 =
            "BEGIN:VCARD\n"
            "VERSION:2.1\n"
            "TITLE:developer\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "X-MOZILLA-HTML:TRUE\n"
            "BDAY:2006-01-08\n"
            "END:VCARD\n";
        static const char *ical_1 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201\n"
            "CREATED:20060409T213201\n"
            "LOCATION:calling from home\n"
            "DESCRIPTION:let's talk\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:1\n"
            "BEGIN:VALARM\n"
            "DESCRIPTION:alarm\n"
            "ACTION:DISPLAY\n"
            "TRIGGER;VALUE=DURATION;RELATED=START:-PT15M\n"
            "END:VALARM\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        static const char *ical_2 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201\n"
            "CREATED:20060409T213201\n"
            "LOCATION:my office\n"
            "CATEGORIES:WORK\n"
            "DESCRIPTION:what the heck\\, let's even shout a bit\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:1\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        rm_r("LogDirTest");
        dump("file_event.one", "1", ical_1);
        dump("file_event.two", "1", ical_1);
        dump("file_event.two", "2", ical_2);
        mkdir_p(getLogData() + "/file_event.empty");
        dump("file_contact.one", "1", vcard_1);
        dump("file_contact.two", "1", vcard_1);
        dump("file_contact.two", "2", vcard_2);
        mkdir_p(getLogData() + "/file_contact.empty");

        mkdir_p(getLogDir());
        m_maxLogDirs = 0;

        // Suppress output by redirecting into LogContext::m_out.
        // It's not tested at the moment.
        m_logContext.reset(new LogContext);
        Logger::addLogger(m_logContext);
    }

    void tearDown() {
        Logger::removeLogger(m_logContext.get());
        m_logContext.reset();
    }

private:

    string getLogData() { return "LogDirTest/data"; }
    virtual InitStateString getLogDir() const { return "LogDirTest/cache/syncevolution"; }
    int m_maxLogDirs;

    void dump(const char *dir, const char *file, const char *data) {
        string name = getLogData();
        name += "/";
        name += dir;
        mkdir_p(name);
        name += "/";
        name += file;
        ofstream out(name.c_str());
        out << data;
    }

    CPPUNIT_TEST_SUITE(LogDirTest);
    CPPUNIT_TEST(testQuickCompare);
    CPPUNIT_TEST(testSessionNoChanges);
    CPPUNIT_TEST(testSessionChanges);
    CPPUNIT_TEST(testMultipleSessions);
    CPPUNIT_TEST(testExpire);
    CPPUNIT_TEST_SUITE_END();

    /**
     * Simulate a session involving one or more sources.
     *
     * @param changeServer   pretend that peer got changed
     * @param status         result of session
     * @param varargs        sourcename ("file_event"),
     *                       statebefore (NULL for no dump, or suffix like "_one"),
     *                       stateafter (NULL for same as before), ..., NULL
     * @return logdir created for the session
     */
    string session(bool changeServer, SyncMLStatus status, ...) {
        Logger::Level level = Logger::instance().getLevel();
        SourceList list(*m_logContext, true);
        list.setLogLevel(SourceList::LOGGING_QUIET);
        SyncReport report;
        list.startSession("", m_maxLogDirs, 0, &report);
        va_list ap;
        va_start(ap, status);
        while (true) {
            const char *sourcename = va_arg(ap, const char *);
            if (!sourcename) {
                break;
            }
            const char *type = NULL;
            if (!strcmp(sourcename, "file_event")) {
                type = "file:text/calendar:2.0";
            } else if (!strcmp(sourcename, "file_contact")) {
                type = "file:text/vcard:3.0";
            }
            CPPUNIT_ASSERT(type);
            string datadir = getLogData() + "/";
            cxxptr<SyncSource> source(SyncSource::createTestingSource(sourcename, type, true,
                                                                      (string("file://") + datadir).c_str()));
            datadir += sourcename;
            datadir += "_1";
            source->open();
            if (changeServer) {
                // fake one added item on server
                source->setItemStat(SyncSourceReport::ITEM_REMOTE,
                                    SyncSourceReport::ITEM_ADDED,
                                    SyncSourceReport::ITEM_TOTAL,
                                    1);
            }
            list.addSource(source);
            const char *before = va_arg(ap, const char *);
            const char *after = va_arg(ap, const char *);
            if (before) {
                // do a "before" dump after directing the source towards the desired data
                rm_r(datadir);
                CPPUNIT_ASSERT_EQUAL(0, symlink((string(sourcename) + before).c_str(),
                                                datadir.c_str()));
                list.syncPrepare(sourcename);
                if (after) {
                    rm_r(datadir);
                    CPPUNIT_ASSERT_EQUAL(0, symlink((string(sourcename) + after).c_str(),
                                                    datadir.c_str()));
                }
            }
        }
        list.syncDone(status, &report);

        Logger::instance().setLevel(level);
        return list.getLogdir();
    }

    typedef vector<string> Sessions_t;
    // full paths to all sessions, sorted
    Sessions_t listSessions() {
        Sessions_t sessions;
        string logdir = getLogDir();
        ReadDir dirs(logdir);
        BOOST_FOREACH(const string &dir, dirs) {
            sessions.push_back(RealPath(logdir + "/" + dir));
        }
        sort(sessions.begin(), sessions.end());
        return sessions;
    }

    void testQuickCompare() {
        // identical dirs => identical files
        CPPUNIT_ASSERT(!LogDir::haveDifferentContent("file_event",
                                                     getLogData(), "empty",
                                                     getLogData(), "empty"));
        CPPUNIT_ASSERT(!LogDir::haveDifferentContent("file_event",
                                                     getLogData(), "one",
                                                     getLogData(), "one"));
        CPPUNIT_ASSERT(!LogDir::haveDifferentContent("file_event",
                                                     getLogData(), "two",
                                                     getLogData(), "two"));
        // some files shared
        CPPUNIT_ASSERT(!system("cp -l -r LogDirTest/data/file_event.two LogDirTest/data/file_event.copy && rm LogDirTest/data/file_event.copy/2"));
        CPPUNIT_ASSERT(LogDir::haveDifferentContent("file_event",
                                                    getLogData(), "two",
                                                    getLogData(), "copy"));
        CPPUNIT_ASSERT(LogDir::haveDifferentContent("file_event",
                                                    getLogData(), "copy",
                                                    getLogData(), "one"));
    }

    void testSessionNoChanges() {
        ScopedEnvChange config("XDG_CONFIG_HOME", "LogDirTest/config");
        ScopedEnvChange cache("XDG_CACHE_HOME", "LogDirTest/cache");

        // simple session with no changes
        string dir = session(false, STATUS_OK, "file_event", ".one", ".one", (char *)0);
        Sessions_t sessions = listSessions();
        CPPUNIT_ASSERT_EQUAL((size_t)1, sessions.size());
        CPPUNIT_ASSERT_EQUAL(dir, sessions[0]);
        IniFileConfigNode status(dir, "status.ini", true);
        CPPUNIT_ASSERT(status.exists());
        CPPUNIT_ASSERT_EQUAL(string("1"), status.readProperty("source-file__event-backup-before").get());
        CPPUNIT_ASSERT_EQUAL(string("1"), status.readProperty("source-file__event-backup-after").get());
        CPPUNIT_ASSERT_EQUAL(string("200"), status.readProperty("status").get());
        CPPUNIT_ASSERT(!LogDir::haveDifferentContent("file_event",
                                                     dir, "before",
                                                     dir, "after"));
    }

    void testSessionChanges() {
        ScopedEnvChange config("XDG_CONFIG_HOME", "LogDirTest/config");
        ScopedEnvChange cache("XDG_CACHE_HOME", "LogDirTest/cache");

        // session with local changes
        string dir = session(false, STATUS_OK, "file_event", ".one", ".two", (char *)0);
        Sessions_t sessions = listSessions();
        CPPUNIT_ASSERT_EQUAL((size_t)1, sessions.size());
        CPPUNIT_ASSERT_EQUAL(dir, sessions[0]);
        IniFileConfigNode status(dir, "status.ini", true);
        CPPUNIT_ASSERT(status.exists());
        CPPUNIT_ASSERT_EQUAL(string("1"), status.readProperty("source-file__event-backup-before").get());
        CPPUNIT_ASSERT_EQUAL(string("2"), status.readProperty("source-file__event-backup-after").get());
        CPPUNIT_ASSERT_EQUAL(string("200"), status.readProperty("status").get());
        CPPUNIT_ASSERT(LogDir::haveDifferentContent("file_event",
                                                    dir, "before",
                                                    dir, "after"));
    }

    void testMultipleSessions() {
        ScopedEnvChange config("XDG_CONFIG_HOME", "LogDirTest/config");
        ScopedEnvChange cache("XDG_CACHE_HOME", "LogDirTest/cache");

        // two sessions, starting with 1 item, adding 1 during the sync, then
        // removing it again during the second
        string dir = session(false, STATUS_OK,
                             "file_event", ".one", ".two",
                             "file_contact", ".one", ".two",
                             (char *)0);
        {
            Sessions_t sessions = listSessions();
            CPPUNIT_ASSERT_EQUAL((size_t)1, sessions.size());
            CPPUNIT_ASSERT_EQUAL(dir, sessions[0]);
            IniFileConfigNode status(dir, "status.ini", true);
            CPPUNIT_ASSERT(status.exists());
            CPPUNIT_ASSERT_EQUAL(string("1"), status.readProperty("source-file__event-backup-before").get());
            CPPUNIT_ASSERT_EQUAL(string("2"), status.readProperty("source-file__event-backup-after").get());
            CPPUNIT_ASSERT_EQUAL(string("1"), status.readProperty("source-file__contact-backup-before").get());
            CPPUNIT_ASSERT_EQUAL(string("2"), status.readProperty("source-file__contact-backup-after").get());
            CPPUNIT_ASSERT_EQUAL(string("200"), status.readProperty("status").get());
            CPPUNIT_ASSERT(LogDir::haveDifferentContent("file_event",
                                                        dir, "before",
                                                        dir, "after"));
            CPPUNIT_ASSERT(LogDir::haveDifferentContent("file_contact",
                                                        dir, "before",
                                                        dir, "after"));
        }

        string seconddir = session(false, STATUS_OK,
                                   "file_event", ".two", ".one",
                                   "file_contact", ".two", ".one",
                                   (char *)0);
        {
            Sessions_t sessions = listSessions();
            CPPUNIT_ASSERT_EQUAL((size_t)2, sessions.size());
            CPPUNIT_ASSERT_EQUAL(dir, sessions[0]);
            CPPUNIT_ASSERT_EQUAL(seconddir, sessions[1]);
            IniFileConfigNode status(seconddir, "status.ini", true);
            CPPUNIT_ASSERT(status.exists());
            CPPUNIT_ASSERT_EQUAL(string("2"), status.readProperty("source-file__event-backup-before").get());
            CPPUNIT_ASSERT_EQUAL(string("1"), status.readProperty("source-file__event-backup-after").get());
            CPPUNIT_ASSERT_EQUAL(string("2"), status.readProperty("source-file__contact-backup-before").get());
            CPPUNIT_ASSERT_EQUAL(string("1"), status.readProperty("source-file__contact-backup-after").get());
            CPPUNIT_ASSERT_EQUAL(string("200"), status.readProperty("status").get());
            CPPUNIT_ASSERT(LogDir::haveDifferentContent("file_event",
                                                        seconddir, "before",
                                                        seconddir, "after"));
            CPPUNIT_ASSERT(LogDir::haveDifferentContent("file_contact",
                                                        seconddir, "before",
                                                        seconddir, "after"));
        }

        CPPUNIT_ASSERT(!LogDir::haveDifferentContent("file_event",
                                                     dir, "after",
                                                     seconddir, "before"));
        CPPUNIT_ASSERT(!LogDir::haveDifferentContent("file_contact",
                                                     dir, "after",
                                                     seconddir, "before"));
    }

    void testExpire() {
        ScopedEnvChange config("XDG_CONFIG_HOME", "LogDirTest/config");
        ScopedEnvChange cache("XDG_CACHE_HOME", "LogDirTest/cache");

        string dirs[5];
        Sessions_t sessions;

        m_maxLogDirs = 1;

        // The latest session always must be preserved, even if it
        // is normally considered less important (no error in this case).
        dirs[0] = session(false, STATUS_FATAL, (char *)0);
        dirs[0] = session(false, STATUS_OK, (char *)0);
        sessions = listSessions();
        CPPUNIT_ASSERT_EQUAL((size_t)1, sessions.size());
        CPPUNIT_ASSERT_EQUAL(dirs[0], sessions[0]);

        // all things being equal, then expire the oldest session,
        // leaving us with two here
        m_maxLogDirs = 2;
        dirs[0] = session(false, STATUS_OK, (char *)0);
        dirs[1] = session(false, STATUS_OK, (char *)0);
        sessions = listSessions();
        CPPUNIT_ASSERT_EQUAL((size_t)2, sessions.size());
        CPPUNIT_ASSERT_EQUAL(dirs[0], sessions[0]);
        CPPUNIT_ASSERT_EQUAL(dirs[1], sessions[1]);

        // When syncing first file_event, then file_contact, both sessions
        // must be preserved despite m_maxLogDirs = 1, otherwise
        // we would loose the only existent backup of file_event.
        dirs[0] = session(false, STATUS_OK, "file_event", ".two", ".one", (char *)0);
        dirs[1] = session(false, STATUS_OK, "file_contact", ".two", ".one", (char *)0);
        sessions = listSessions();
        CPPUNIT_ASSERT_EQUAL((size_t)2, sessions.size());
        CPPUNIT_ASSERT_EQUAL(dirs[0], sessions[0]);
        CPPUNIT_ASSERT_EQUAL(dirs[1], sessions[1]);

        // after synchronizing both, we can expire both the old sessions
        m_maxLogDirs = 1;
        dirs[0] = session(false, STATUS_OK,
                          "file_event", ".two", ".one",
                          "file_contact", ".two", ".one",
                          (char *)0);
        sessions = listSessions();
        CPPUNIT_ASSERT_EQUAL((size_t)1, sessions.size());
        CPPUNIT_ASSERT_EQUAL(dirs[0], sessions[0]);

        // when doing multiple failed syncs without dumps, keep the sessions
        // which have database dumps
        m_maxLogDirs = 2;
        dirs[1] = session(false, STATUS_FATAL, (char *)0);
        dirs[1] = session(false, STATUS_FATAL, (char *)0);
        sessions = listSessions();
        CPPUNIT_ASSERT_EQUAL((size_t)2, sessions.size());
        CPPUNIT_ASSERT_EQUAL(dirs[0], sessions[0]);
        CPPUNIT_ASSERT_EQUAL(dirs[1], sessions[1]);

        // when doing syncs which don't change data, keep the sessions which
        // did change something: keep oldest backup because it created the
        // backups for the first time
        dirs[1] = session(false, STATUS_OK,
                          "file_event", ".one", ".one",
                          "file_contact", ".one", ".one",
                          (char *)0);
        dirs[1] = session(false, STATUS_OK,
                          "file_event", ".one", ".one",
                          "file_contact", ".one", ".one",
                          (char *)0);
        sessions = listSessions();
        CPPUNIT_ASSERT_EQUAL((size_t)2, sessions.size());
        CPPUNIT_ASSERT_EQUAL(dirs[0], sessions[0]);
        CPPUNIT_ASSERT_EQUAL(dirs[1], sessions[1]);

        // when making a change in each sync, we end up with the two
        // most recent sessions eventually: first change server,
        // then local
        dirs[1] = session(true, STATUS_OK,
                          "file_event", ".one", ".one",
                          "file_contact", ".one", ".one",
                          (char *)0);
        sessions = listSessions();
        CPPUNIT_ASSERT_EQUAL((size_t)2, sessions.size());
        CPPUNIT_ASSERT_EQUAL(dirs[0], sessions[0]);
        CPPUNIT_ASSERT_EQUAL(dirs[1], sessions[1]);
        dirs[0] = dirs[1];
        dirs[1] = session(false, STATUS_OK,
                          "file_event", ".one", ".two",
                          "file_contact", ".one", ".two",
                          (char *)0);
        sessions = listSessions();
        CPPUNIT_ASSERT_EQUAL((size_t)2, sessions.size());
        CPPUNIT_ASSERT_EQUAL(dirs[0], sessions[0]);
        CPPUNIT_ASSERT_EQUAL(dirs[1], sessions[1]);
    }
};
SYNCEVOLUTION_TEST_SUITE_REGISTRATION(LogDirTest);
#endif // ENABLE_UNIT_TESTS

SE_END_CXX
