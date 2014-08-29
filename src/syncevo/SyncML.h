/*
 * Copyright (C) 2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#ifndef INCL_SYNCML
#define INCL_SYNCML

#include <string>
#include <map>
#include <list>
#include <set>
#include <ostream>
#include <string.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * only the codes which are valid for a server alerted sync (SAN) message
 */
enum SANSyncMode {
    SA_SLOW = 201,
    SA_FIRST = SA_SLOW,
    SA_TWO_WAY = 206,
    SA_ONE_WAY_FROM_CLIENT = 207,
    SA_REFRESH_FROM_CLIENT = 208,
    SA_ONE_WAY_FROM_SERVER = 209,
    SA_REFRESH_FROM_SERVER = 210,
    SA_LAST = SA_REFRESH_FROM_SERVER,

    SA_INVALID = 255
};

/**
 * unambiguous sync modes:
 * - data direction is independent of client/server role
 * - no server alerted variants
 */
enum SimpleSyncMode {
    SIMPLE_SYNC_NONE,
    SIMPLE_SYNC_TWO_WAY = 200,
    SIMPLE_SYNC_SLOW = 201,

    SIMPLE_SYNC_RESTORE_FROM_BACKUP = 211,

    SIMPLE_SYNC_ONE_WAY_FROM_LOCAL = 212,
    SIMPLE_SYNC_REFRESH_FROM_LOCAL = 213,
    SIMPLE_SYNC_ONE_WAY_FROM_REMOTE = 214,
    SIMPLE_SYNC_REFRESH_FROM_REMOTE = 215,

    // custom modes in SyncEvolution

    /** mirror data on server side, slow variant (client sends all items) */
    SIMPLE_SYNC_LOCAL_CACHE_SLOW = 218,
    /** mirror data on server side, incremental variant (client sends only changes) */
    SIMPLE_SYNC_LOCAL_CACHE_INCREMENTAL = 219,

    SIMPLE_SYNC_INVALID = 255
};

/** all kinds of sync modes */
enum SyncMode {
    /** unset or disabled */
    SYNC_NONE,

    SYNC_FIRST = 200,
    SYNC_TWO_WAY = 200,
    SYNC_SLOW = 201,
    SYNC_ONE_WAY_FROM_CLIENT = 202,
    SYNC_REFRESH_FROM_CLIENT = 203,
    SYNC_ONE_WAY_FROM_SERVER = 204,
    SYNC_REFRESH_FROM_SERVER = 205,

    // used by Server Alerted Sync, same as SA_ in SANSyncMode
    SA_SYNC_TWO_WAY = 206,
    SA_SYNC_ONE_WAY_FROM_CLIENT = 207,
    SA_SYNC_REFRESH_FROM_CLIENT = 208,
    SA_SYNC_ONE_WAY_FROM_SERVER = 209,
    SA_SYNC_REFRESH_FROM_SERVER = 210,

    // used by restore backend with backup data, a pseudo mode
    SYNC_RESTORE_FROM_BACKUP = 211,

    // modes which always transfer in the same direction,
    // regardless which side acts as client or server
    SYNC_ONE_WAY_FROM_LOCAL = 212,
    SYNC_REFRESH_FROM_LOCAL = 213,
    SYNC_ONE_WAY_FROM_REMOTE = 214,
    SYNC_REFRESH_FROM_REMOTE = 215,

    // custom mode
    SYNC_LOCAL_CACHE_SLOW = 218,
    SYNC_LOCAL_CACHE_INCREMENTAL = 219,

    SYNC_LAST = 220,
    /** error situation (in contrast to SYNC_NONE) */
    SYNC_INVALID = 255
};

/**
 * maps to normal SYNC_ variants (no SA_*) and unambiguous direction
 * (LOCAL/REMOTE instead of CLIENT/SERVER)
 */
SimpleSyncMode SimplifySyncMode(SyncMode mode, bool peerIsClient);

/**
 * maps to the server alerted variants
 */
SANSyncMode AlertSyncMode(SyncMode mode, bool peerIsClient);

/* According to OMNA WSP Content Type Numbers*/
enum ContentType {
    WSPCTC_TEXT_PLAIN = 0x03,
    WSPCTC_XVCALENDAR = 0x06,
    WSPCTC_XVCARD = 0x07,
    WSPCTC_ICALENDAR = 0x0305,
    WSPCTC_VCARD = 0x0309,
    WSPCTC_UNKNOWN =0xFFFFFF
};

/**
 * Return string for sync mode. User-visible strings are the ones used
 * in a sync source config ("two-way", "refresh-from-server", etc.).
 * Otherwise the constants above are returned ("SYNC_NONE").
 */
std::string PrettyPrintSyncMode(SyncMode mode, bool userVisible = true);

/**
 * Parse user-visible mode names.
 */
SyncMode StringToSyncMode(const std::string &str, bool serverAlerted = false);

/*
 * Parse string based content type to WSPCTC encoded binary code
 * Always use older type unless forceType is true.
 */
ContentType StringToContentType (const std::string &str, bool forceType);

/*
 * return string based MIME Type for PIM, always use legacy type unless
 * forceType is set.
 * */
std::string GetLegacyMIMEType (const std::string &str, bool forceType);

/**
 * result of SyncML operations, same codes as in HTTP and the Synthesis engine
 */
enum SyncMLStatus {
    /** ok */
    STATUS_OK = 0,

    /** more explicit ok status in cases where 0 might mean "unknown" (SyncReport) */
    STATUS_HTTP_OK = 200,

    /** no content / end of file / end of iteration / empty/NULL value */
    STATUS_NO_CONTENT = 204,
    /** external data has been merged */
    STATUS_DATA_MERGED = 207,

    /** The request requires user authentication. */
    STATUS_UNAUTHORIZED = 401,
    /** forbidden / access denied */
    STATUS_FORBIDDEN = 403,
    /** object not found / unassigned field */
    STATUS_NOT_FOUND = 404,
    /** command not allowed */
    STATUS_COMMAND_NOT_ALLOWED = 405,

    STATUS_OPTIONAL_FEATURE_NOT_SUPPORTED = 406,
    STATUS_AUTHORIZATION_REQUIRED = 407,

    /**
     * "If the specified remote procedure call no longer exists on
     * the recipient, then the (410) Gone exception condition is
     * created by the command."
     */
    STATUS_COMMAND_GONE = 410,

    /**
     * "If the Put command did not include the size of the data item
     * to be transferred (i.e., in the Meta element type), then the
     * (411) Size required exception condition is created by the
     * command.
     */
    STATUS_SIZE_REQUIRED = 411,

    /**
     * "If the Alert command didn’t include all the correct parameters
     * in the Item element type, then the (412) Incomplete command
     * exception condition is created by the command."
     */
    STATUS_INCOMPLETE_COMMAND = 412,

    /**
     * "If the requested data item is too large to be transferred at
     * this time, then the (413) Request entity too large exception
     * condition is created by the command.
     */
    STATUS_REQUEST_ENTITY_TOO_LARGE = 413,

    STATUS_UNSUPPORTED_MEDIA_TYPE_OR_FORMAT = 415,

    /**
     * "If the Size specified in the Meta element type was too large
     * for the recipient (e.g., the recipient does not have sufficient
     * input buffer for the data), then the (416) Requested size too
     * big exception condition is created by the command."
     */
    STATUS_REQUESTED_SIZE_TOO_BIG = 416,

    STATUS_RETRY_LATER = 417,

    /** object exists already */
    STATUS_ALREADY_EXISTS = 418,

    /** database / memory full error */
    STATUS_FULL = 420,

    STATUS_UNKNOWN_SEARCH_GRAMMAR = 421,
    STATUS_BAD_CGI_OR_FILTER_QUERY = 422,

    /**
     * "In a two-way synchronization, if the OMA DS client specifies a
     * "Soft Delete" for an item that has already been "Hard Deleted"
     * on the OMA DS server, then a (423) Soft-delete conflict MUST be
     * returned in the Status command."
     */
    STATUS_SOFT_DELETE_CONFLICT = 423,

    STATUS_PARTIAL_ITEM_NOT_ACCEPTED = 426,
    STATUS_ITEM_NOT_EMPTY = 427,
    STATUS_MOVE_FAILED = 428,

    /** command failed / fatal DB error */
    STATUS_FATAL = 500,
    /** in Synthesis StartDataRead: slow sync forced by backend */
    STATUS_SLOW_SYNC_508 = 508,
    /** general DB error */
    STATUS_DATASTORE_FAILURE = 510,

    /* sysync error codes also used by SyncEvolution */

    /** transport failure, sysync::LOCERR_TRANSPFAIL */
    STATUS_TRANSPORT_FAILURE = 20043,

    /* error codes in the range reserved by Synthesis for the application follow */

    /** ran into an unexpected slow sync, refused to execute it */
    STATUS_UNEXPECTED_SLOW_SYNC = 22000,

    /** no error at the SyncML level, but some items did not transfer correctly */
    STATUS_PARTIAL_FAILURE = 22001,

    /**
     * Set by SyncEvolution in status.ini before starting an sync.
     * Replaced with the final status code if the sync completes.
     * Finding this code here in a session report implies that
     * the process responsible for the session died unexpectedly,
     * for unknown reasons.
     */
    STATUS_DIED_PREMATURELY = 22002,

    /**
     * Set by dbus server when it asks password from dbus clients
     * and no responsble is gotten in a specific time.
     */
    STATUS_PASSWORD_TIMEOUT = 22003,

    /**
     * On-disk files (config, Synthesis binfiles) are too recent
     * to be read and/or used by this SyncEvolution release.
     * User must upgrade.
     */
    STATUS_RELEASE_TOO_OLD = 22004,

    /**
     * On-disk files would be changed in such a way that downgrading
     * becomes impossible. User must explicitly migrate config if
     * he wants to proceed.
     */
    STATUS_MIGRATION_NEEDED = 22005,

    /**
     * Skip writing of admin data after a sync did not change
     * anything.
     */
    STATUS_SYNC_END_SHORTCUT = 22006,

    STATUS_MAX = 0x7FFFFFF
};

/**
 * short (in the range of 80 characters or less) description of the status code,
 * followed by "(status xxxx)" because the mapping of description to code
 * might be ambiguous
 */
std::string Status2String(SyncMLStatus status);

/**
 * Information about a database dump.
 * Currently only records the number of items.
 * A negative number of items means no backup
 * available.
 */
class BackupReport {
 public:
    BackupReport() {
        clear();
    }

    bool isAvailable() const { return m_numItems >= 0; }
    long getNumItems() const { return m_numItems; }
    void setNumItems(long numItems) { m_numItems = numItems; }

    void clear() {
        m_numItems = -1;
    }

 private:
    long m_numItems;
};

class SyncSourceReport {
 public:
    SyncSourceReport() {
        memset(m_stat, 0, sizeof(m_stat));
        m_received = 0;
        m_sent = 0;
        m_first =
            m_resume = false;
        m_mode = SYNC_NONE;
        m_status = STATUS_OK;
        m_restarts = 0;
    }

    enum ItemLocation {
        ITEM_LOCAL,
        ITEM_REMOTE,
        ITEM_LOCATION_MAX
    };
    static std::string LocationToString(ItemLocation location);
    static ItemLocation StringToLocation(const std::string &location);
    enum ItemState {
        ITEM_ADDED,
        ITEM_UPDATED,
        ITEM_REMOVED,
        ITEM_ANY,
        ITEM_STATE_MAX
    };
    static std::string StateToString(ItemState state);
    static ItemState StringToState(const std::string &state);
    enum ItemResult {
        ITEM_TOTAL,               /**< total number ADDED/UPDATED/REMOVED */
        ITEM_REJECT,              /**< number of rejected items, ANY state */
        ITEM_MATCH,               /**< number of matched items, ANY state, REMOTE */
        ITEM_CONFLICT_SERVER_WON, /**< conflicts resolved by using server item, ANY state, REMOTE */
        ITEM_CONFLICT_CLIENT_WON, /**< conflicts resolved by using client item, ANY state, REMOTE */
        ITEM_CONFLICT_DUPLICATED, /**< conflicts resolved by duplicating item, ANY state, REMOTE */
        ITEM_SENT_BYTES,          /**< number of sent bytes, ANY, LOCAL */
        ITEM_RECEIVED_BYTES,      /**< number of received bytes, ANY, LOCAL */
        ITEM_RESULT_MAX
    };
    static std::string ResultToString(ItemResult result);
    static ItemResult StringToResult(const std::string &result);

    static std::string StatTupleToString(ItemLocation location, ItemState state, ItemResult result);
    static void StringToStatTuple(const std::string &str, ItemLocation &location, ItemState &state, ItemResult &result);

    /**
     * get item statistics
     *
     * @param location   either local or remote
     * @param state      added, updated or removed
     * @param success    either okay or failed
     */
    int getItemStat(ItemLocation location,
                    ItemState state,
                    ItemResult success) const {
        return m_stat[location][state][success];
    }
    void setItemStat(ItemLocation location,
                     ItemState state,
                     ItemResult success,
                     int count) {
        m_stat[location][state][success] = count;
    }
    void incrementItemStat(ItemLocation location,
                           ItemState state,
                           ItemResult success) {
        m_stat[location][state][success]++;
    }

    /** true if statistics indicate that peer or local was modified during sync */
    bool wasChanged(ItemLocation location);

    void recordFinalSyncMode(SyncMode mode) { m_mode = mode; }
    SyncMode getFinalSyncMode() const { return m_mode; }

    void recordRestart() { m_restarts++; }
    void setRestarts(int restarts) { m_restarts = restarts; }
    /**
     * number of times that the sync session was restarted
     * involving the source, usually zero
     */
    int getRestarts() const { return m_restarts; }

    void recordFirstSync(bool isFirstSync) { m_first = isFirstSync; }
    bool isFirstSync() const { return m_first; }

    void recordResumeSync(bool isResumeSync) { m_resume = isResumeSync; }
    bool isResumeSync() const { return m_resume; }

    void recordStatus(SyncMLStatus status ) { m_status = status; }
    SyncMLStatus getStatus() const { return m_status; }

    /** counts each received add/update/delete */
    void recordTotalNumItemsReceived(int received) { m_received = received; }
    int getTotalNumItemsReceived() const { return m_received; }

    /** counts each sent add/update/delete */
    void recordTotalNumItemsSent(int sent) { m_sent = sent; }
    int getTotalNumItemsSent() const { return m_sent; }

    /**
     * if not empty, then this was the virtual source which cause the
     * current one to be included in the sync
     */
    void recordVirtualSource(const std::string &virtualsource) { m_virtualSource = virtualsource; }
    std::string getVirtualSource() const { return m_virtualSource; }

    /** information about database dump before and after session */
    BackupReport m_backupBefore, m_backupAfter;

 private:
    /** storage for getItemStat(): allow access with _MAX as index */
    int m_stat[ITEM_LOCATION_MAX + 1][ITEM_STATE_MAX + 1][ITEM_RESULT_MAX + 1];
    int m_received, m_sent;

    SyncMode m_mode;
    int m_restarts;
    bool m_first;
    bool m_resume;
    SyncMLStatus m_status;
    std::string m_virtualSource;
};

class SyncReport : public std::map<std::string, SyncSourceReport> {
    time_t m_start, m_end;
    SyncMLStatus m_status;
    std::string m_error;
    std::string m_localName, m_remoteName;

 public:
    SyncReport() :
        m_start(0),
        m_end(0),
        m_status(STATUS_OK),
        m_localName("LOCAL"),
        m_remoteName("REMOTE")
        {}

    /** construct from text dump */
    SyncReport(const std::string &dump);

    /** convert to text dump */
    std::string toString() const;

    void setLocalName(const std::string &name) { m_localName = name; }
    void setRemoteName(const std::string &name) { m_remoteName = name; }

    typedef std::pair<std::string, SyncSourceReport> SourceReport_t;

    void addSyncSourceReport(const std::string &name,
                             const SyncSourceReport &report) {
        (*this)[name] = report;
    }
    SyncSourceReport &getSyncSourceReport(const std::string &name) {
        return (*this)[name];
    }
    const SyncSourceReport *findSyncSourceReport(const std::string &name) const {
        const_iterator it = find(name);
        return it == end() ? NULL : &it->second;
    }

    /** start time of sync, 0 if unknown */
    time_t getStart() const { return m_start; }
    void setStart(time_t start) { m_start = start; }
    /** end time of sync, 0 if unknown (indicates a crash) */
    time_t getEnd() const { return m_end; }
    void setEnd(time_t end) { m_end = end; }

    /**
     * overall sync result
     *
     * STATUS_OK = 0 means unknown status (might have aborted prematurely),
     * STATUS_HTTP_OK = 200 means successful completion
     */
    SyncMLStatus getStatus() const { return m_status; }
    void setStatus(SyncMLStatus status) { m_status = status; }

    /**
     * Initial ERROR description as seen by SyncEvolution,
     * typically via the logging infrastructure. Not localized.
     */
    std::string getError() const { return m_error; }
    void setError(const std::string &error) { m_error = error; }

    void clear() {
        std::map<std::string, SyncSourceReport>::clear();
        m_start = m_end = 0;
        m_error = "";
        m_status = STATUS_OK;
    }

    /** generate short string representing start and duration of sync */
    std::string formatSyncTimes() const;

    /** pretty-print with options */
    void prettyPrint(std::ostream &out, int flags) const;
    enum {
        WITHOUT_CLIENT = 1 << 1,
        WITHOUT_SERVER = 1 << 2,
        WITHOUT_CONFLICTS = 1 << 3,
        WITHOUT_REJECTS = 1 << 4,
        WITH_TOTAL = 1 << 5
    };

    /**
     * Produces a non-localized explanation for recovering from unexpected 
     * slow syncs, targeted towards command line users.
     *
     * @param peer     config name used to select the affected peer (nor necessarily normalized)
     * @param sources  list of affected sources
     * @return explanation, empty string if list of sources is empty
     */
    static std::string slowSyncExplanation(const std::string &peer,
                                           const std::set<std::string> &sources);

    /**
     * Produces a non-localized explanation for recovering from unexpected 
     * slow syncs, targeted towards command line users. Uses the information
     * about sources stored in the report.
     *
     * @param peer     config name used to select the affected peer (nor necessarily normalized)
     * @return explanation, empty string if list of sources is empty
     */
    std::string slowSyncExplanation(const std::string &peer) const;
};

/** pretty-print the report as an ASCII table */
std::ostream &operator << (std::ostream &out, const SyncReport &report);

class ConfigNode;

/** write report into a ConfigNode */
ConfigNode &operator << (ConfigNode &node, const SyncReport &report);

/** read report from a ConfigNode */
ConfigNode &operator >> (ConfigNode &node, SyncReport &report);


SE_END_CXX
#endif // INCL_SYNCML
