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

#include <syncevo/SyncML.h>
#include <syncevo/ConfigNode.h>
#include <syncevo/util.h>
#include <syncevo/StringDataBlob.h>
#include <syncevo/IniConfigNode.h>
#include <sstream>
#include <iomanip>
#include <vector>

#include <boost/foreach.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/join.hpp>

#include <synthesis/syerror.h>

#include <syncevo/declarations.h>
using namespace std;
SE_BEGIN_CXX

SimpleSyncMode SimplifySyncMode(SyncMode mode, bool peerIsClient)
{
    switch (mode) {
    case SYNC_NONE:
    case SYNC_TWO_WAY:
    case SYNC_SLOW:
    case SYNC_RESTORE_FROM_BACKUP:
    case SYNC_ONE_WAY_FROM_LOCAL:
    case SYNC_REFRESH_FROM_LOCAL:
    case SYNC_ONE_WAY_FROM_REMOTE:
    case SYNC_REFRESH_FROM_REMOTE:
        return static_cast<SimpleSyncMode>(mode);

    case SA_SYNC_ONE_WAY_FROM_CLIENT:
    case SYNC_ONE_WAY_FROM_CLIENT:
        return peerIsClient ? SIMPLE_SYNC_ONE_WAY_FROM_REMOTE : SIMPLE_SYNC_ONE_WAY_FROM_LOCAL;

    case SA_SYNC_REFRESH_FROM_CLIENT:
    case SYNC_REFRESH_FROM_CLIENT:
        return peerIsClient ? SIMPLE_SYNC_REFRESH_FROM_REMOTE : SIMPLE_SYNC_REFRESH_FROM_LOCAL;

    case SA_SYNC_ONE_WAY_FROM_SERVER:
    case SYNC_ONE_WAY_FROM_SERVER:
        return peerIsClient ? SIMPLE_SYNC_ONE_WAY_FROM_LOCAL : SIMPLE_SYNC_ONE_WAY_FROM_REMOTE;

    case SA_SYNC_REFRESH_FROM_SERVER:
    case SYNC_REFRESH_FROM_SERVER:
        return peerIsClient ? SIMPLE_SYNC_REFRESH_FROM_LOCAL : SIMPLE_SYNC_REFRESH_FROM_REMOTE;

    case SA_SYNC_TWO_WAY:
        return SIMPLE_SYNC_TWO_WAY;

    case SYNC_LOCAL_CACHE_SLOW:
        return SIMPLE_SYNC_LOCAL_CACHE_SLOW;

    case SYNC_LOCAL_CACHE_INCREMENTAL:
        return SIMPLE_SYNC_LOCAL_CACHE_INCREMENTAL;

    case SYNC_LAST:
    case SYNC_INVALID:
        return SIMPLE_SYNC_INVALID;
    }

    return SIMPLE_SYNC_INVALID;
}

SANSyncMode AlertSyncMode(SyncMode mode, bool peerIsClient)
{
    switch(mode) {
    case SYNC_RESTORE_FROM_BACKUP:
    case SYNC_NONE:
    case SYNC_LAST:
    case SYNC_INVALID:
        return SA_INVALID;

    case SYNC_SLOW:
    case SYNC_LOCAL_CACHE_SLOW:
        return SA_SLOW;

    case SYNC_TWO_WAY:
    case SA_SYNC_TWO_WAY:
    case SYNC_LOCAL_CACHE_INCREMENTAL: // use two-way because it is more likely to be implemented
        return SA_TWO_WAY;

    case SYNC_ONE_WAY_FROM_CLIENT:
    case SA_SYNC_ONE_WAY_FROM_CLIENT:
        return SA_ONE_WAY_FROM_CLIENT;

    case SYNC_REFRESH_FROM_CLIENT:
    case SA_SYNC_REFRESH_FROM_CLIENT:
        return SA_REFRESH_FROM_CLIENT;

    case SYNC_ONE_WAY_FROM_SERVER:
    case SA_SYNC_ONE_WAY_FROM_SERVER:
        return SA_ONE_WAY_FROM_SERVER;

    case SYNC_REFRESH_FROM_SERVER:
    case SA_SYNC_REFRESH_FROM_SERVER:
        return SA_REFRESH_FROM_SERVER;

    case SYNC_ONE_WAY_FROM_LOCAL:
        return peerIsClient ? SA_ONE_WAY_FROM_SERVER : SA_ONE_WAY_FROM_CLIENT;

    case SYNC_REFRESH_FROM_LOCAL:
        return peerIsClient ? SA_REFRESH_FROM_SERVER : SA_REFRESH_FROM_CLIENT;

    case SYNC_ONE_WAY_FROM_REMOTE:
        return peerIsClient ? SA_ONE_WAY_FROM_CLIENT : SA_ONE_WAY_FROM_SERVER;

    case SYNC_REFRESH_FROM_REMOTE:
        return peerIsClient ? SA_REFRESH_FROM_CLIENT : SA_REFRESH_FROM_SERVER;
    }

    return SA_INVALID;
}


std::string PrettyPrintSyncMode(SyncMode mode, bool userVisible)
{
    switch (mode) {
    case SYNC_NONE:
        return userVisible ? "disabled" : "SYNC_NONE";
    case SYNC_TWO_WAY:
    case SA_SYNC_TWO_WAY:
        return userVisible ? "two-way" : "SYNC_TWO_WAY";
    case SYNC_SLOW:
        return userVisible ? "slow" : "SYNC_SLOW";
    case SYNC_ONE_WAY_FROM_CLIENT:
    case SA_SYNC_ONE_WAY_FROM_CLIENT:
        return userVisible ? "one-way-from-client" : "SYNC_ONE_WAY_FROM_CLIENT";
    case SYNC_REFRESH_FROM_CLIENT:
    case SA_SYNC_REFRESH_FROM_CLIENT:
        return userVisible ? "refresh-from-client" : "SYNC_REFRESH_FROM_CLIENT";
    case SYNC_ONE_WAY_FROM_SERVER:
    case SA_SYNC_ONE_WAY_FROM_SERVER:
        return userVisible ? "one-way-from-server" : "SYNC_ONE_WAY_FROM_SERVER";
    case SYNC_REFRESH_FROM_SERVER:
    case SA_SYNC_REFRESH_FROM_SERVER:
        return userVisible ? "refresh-from-server" : "SYNC_REFRESH_FROM_SERVER";
    case SYNC_RESTORE_FROM_BACKUP:
        return userVisible ? "restore-from-backup" : "SYNC_RESTORE_FROM_BACKUP";
    case SYNC_ONE_WAY_FROM_LOCAL:
        return userVisible ? "one-way-from-local" : "SYNC_REFRESH_FROM_LOCAL";
    case SYNC_REFRESH_FROM_LOCAL:
        return userVisible ? "refresh-from-local" : "SYNC_REFRESH_FROM_LOCAL";
    case SYNC_ONE_WAY_FROM_REMOTE:
        return userVisible ? "one-way-from-remote" : "SYNC_ONE_WAY_FROM_REMOTE";
    case SYNC_REFRESH_FROM_REMOTE:
        return userVisible ? "refresh-from-remote" : "SYNC_REFRESH_FROM_REMOTE";
    case SYNC_LOCAL_CACHE_SLOW:
        return userVisible ? "local-cache-slow" : "SYNC_LOCAL_CACHE_SLOW";
    case SYNC_LOCAL_CACHE_INCREMENTAL:
        return userVisible ? "local-cache-incremental" : "SYNC_LOCAL_CACHE_INCREMENTAL";
    default:
        std::stringstream res;

        res << (userVisible ? "sync-mode-" : "SYNC_") << int(mode);
        return res.str();
    }
}

SyncMode StringToSyncMode(const std::string &mode, bool serverAlerted)
{
    if (boost::iequals(mode, "slow") || boost::iequals(mode, "SYNC_SLOW")) {
        return SYNC_SLOW;
    } else if (boost::iequals(mode, "two-way") || boost::iequals(mode, "SYNC_TWO_WAY")) {
        return serverAlerted ?SA_SYNC_TWO_WAY: SYNC_TWO_WAY;
    } else if (boost::iequals(mode, "refresh-from-server") || boost::iequals(mode, "SYNC_REFRESH_FROM_SERVER")) {
        return serverAlerted? SA_SYNC_REFRESH_FROM_SERVER: SYNC_REFRESH_FROM_SERVER;
    } else if (boost::iequals(mode, "refresh-from-client") || boost::iequals(mode, "SYNC_REFRESH_FROM_CLIENT")) {
        return serverAlerted? SA_SYNC_REFRESH_FROM_CLIENT: SYNC_REFRESH_FROM_CLIENT;
    } else if (boost::iequals(mode, "one-way-from-server") || boost::iequals(mode, "SYNC_ONE_WAY_FROM_SERVER")) {
        return serverAlerted? SA_SYNC_ONE_WAY_FROM_SERVER: SYNC_ONE_WAY_FROM_SERVER;
    } else if (boost::iequals(mode, "one-way-from-client") || boost::iequals(mode, "SYNC_ONE_WAY_FROM_CLIENT")) {
        return serverAlerted? SA_SYNC_ONE_WAY_FROM_CLIENT: SYNC_ONE_WAY_FROM_CLIENT;
    } else if (boost::iequals(mode, "refresh-from-remote") || boost::iequals(mode, "SYNC_REFRESH_FROM_REMOTE")) {
        return SYNC_REFRESH_FROM_REMOTE;
    } else if (boost::iequals(mode, "refresh-from-local") || boost::iequals(mode, "SYNC_REFRESH_FROM_LOCAL")) {
        return SYNC_REFRESH_FROM_LOCAL;
    } else if (boost::iequals(mode, "one-way-from-remote") || boost::iequals(mode, "SYNC_ONE_WAY_FROM_REMOTE")) {
        return SYNC_ONE_WAY_FROM_REMOTE;
    } else if (boost::iequals(mode, "one-way-from-local") || boost::iequals(mode, "SYNC_ONE_WAY_FROM_LOCAL")) {
        return SYNC_ONE_WAY_FROM_LOCAL;
    } else if (boost::iequals(mode, "disabled") || boost::iequals(mode, "SYNC_NONE")) {
        return SYNC_NONE;
    } else if (boost::iequals(mode, "local-cache-slow") || boost::iequals(mode, "SYNC_LOCAL_CACHE_SLOW")) {
        return SYNC_LOCAL_CACHE_SLOW;
    } else if (boost::iequals(mode, "local-cache-incremental") || boost::iequals(mode, "SYNC_LOCAL_CACHE_INCREMENTAL")) {
        return SYNC_LOCAL_CACHE_INCREMENTAL;
    } else {
        return SYNC_INVALID;
    }
}


ContentType StringToContentType(const std::string &type, bool force) {
    if (boost::iequals (type, "text/x-vcard") || boost::iequals (type, "text/x-vcard:2.1")) {
        return WSPCTC_XVCARD;
    } else if (boost::iequals (type, "text/vcard") ||boost::iequals (type, "text/vcard:3.0")) {
        return force ? WSPCTC_VCARD : WSPCTC_XVCARD;
    } else if (boost::iequals (type, "text/x-vcalendar") ||boost::iequals (type, "text/x-vcalendar:1.0")
              ||boost::iequals (type, "text/x-calendar") || boost::iequals (type, "text/x-calendar:1.0")) {
        return WSPCTC_XVCALENDAR;
    } else if (boost::iequals (type, "text/calendar") ||boost::iequals (type, "text/calendar:2.0")) {
        return force ? WSPCTC_ICALENDAR : WSPCTC_XVCALENDAR;
    } else if (boost::iequals (type, "text/plain") ||boost::iequals (type, "text/plain:1.0")) {
        return WSPCTC_TEXT_PLAIN;
    } else {
        return WSPCTC_UNKNOWN;
    }
}

std::string GetLegacyMIMEType (const std::string &type, bool force) {
    if (boost::iequals (type, "text/x-vcard") || boost::iequals (type, "text/x-vcard:2.1")) {
        return  "text/x-vcard";
    } else if (boost::iequals (type, "text/vcard") ||boost::iequals (type, "text/vcard:3.0")) {
        return force ? "text/vcard" : "text/x-vcard";
    } else if (boost::iequals (type, "text/x-vcalendar") ||boost::iequals (type, "text/x-vcalendar:1.0")
              ||boost::iequals (type, "text/x-calendar") || boost::iequals (type, "text/x-calendar:1.0")) {
        return "text/x-vcalendar";
    } else if (boost::iequals (type, "text/calendar") ||boost::iequals (type, "text/calendar:2.0")) {
        return force ? "text/vcalendar" : "text/x-vcalendar";
    } else if (boost::iequals (type, "text/plain") ||boost::iequals (type, "text/plain:1.0")) {
        return "text/plain";
    } else {
        return "";
    }
}


std::string Status2String(SyncMLStatus status)
{
    string error;
    bool local;
    int code = status;

    local = status >= static_cast<int>(sysync::LOCAL_STATUS_CODE);
    if (local &&
        status <= static_cast<int>(sysync::LOCAL_STATUS_CODE_END)) {
        code = status - static_cast<int>(sysync::LOCAL_STATUS_CODE);
    } else {
        code = status;
    }

    switch (code) {
    case STATUS_OK:
    case STATUS_HTTP_OK:
        error = "no error";
        break;
    case STATUS_NO_CONTENT:
        error = "no content/end of data";
        break;
    case STATUS_DATA_MERGED:
        error = "data merged";
        break;
    case STATUS_UNAUTHORIZED:
        error = "authorization failed";
        break;
    case STATUS_FORBIDDEN:
        error = "access denied";
        break;
    case STATUS_NOT_FOUND:
        error = "object not found";
        break;
    case STATUS_COMMAND_NOT_ALLOWED:
        error = "operation not allowed";
        break;
    case STATUS_OPTIONAL_FEATURE_NOT_SUPPORTED:
        error = "optional feature not supported";
        break;
    case STATUS_AUTHORIZATION_REQUIRED:
        error = "authorization required";
        break;
    case STATUS_COMMAND_GONE:
        error = "command gone";
        break;
    case STATUS_SIZE_REQUIRED:
        error = "size required";
        break;
    case STATUS_INCOMPLETE_COMMAND:
        error = "incomplete command";
        break;
    case STATUS_REQUEST_ENTITY_TOO_LARGE:
        error = "request entity too large";
        break;
    case STATUS_UNSUPPORTED_MEDIA_TYPE_OR_FORMAT:
        error = "unsupported media type or format";
        break;
    case STATUS_REQUESTED_SIZE_TOO_BIG:
        error = "requested size too big";
        break;
    case STATUS_RETRY_LATER:
        error = "retry later";
        break;
    case STATUS_ALREADY_EXISTS:
        error = "object exists already";
        break;
    case STATUS_UNKNOWN_SEARCH_GRAMMAR:
        error = "unknown search grammar";
        break;
    case STATUS_BAD_CGI_OR_FILTER_QUERY:
        error = "bad CGI or filter query";
        break;
    case STATUS_SOFT_DELETE_CONFLICT:
        error = "soft-delete conflict";
        break;
    case STATUS_PARTIAL_ITEM_NOT_ACCEPTED:
        error = "partial item not accepted";
        break;
    case STATUS_ITEM_NOT_EMPTY:
        error = "item not empty";
        break;
    case STATUS_MOVE_FAILED:
        error = "move failed";
        break;
    case STATUS_FATAL:
        error = "fatal error";
        break;
    case STATUS_DATASTORE_FAILURE:
        error = "database failure";
        break;
    case STATUS_FULL:
        error = "out of space";
        break;

    case STATUS_UNEXPECTED_SLOW_SYNC:
        error = "unexpected slow sync";
        break;

    case STATUS_PARTIAL_FAILURE:
        error = "some changes could not be transferred";
        break;

    case STATUS_PASSWORD_TIMEOUT:
        error = "password request timed out";
        break;

    case STATUS_RELEASE_TOO_OLD:
        error = "SyncEvolution binary V" VERSION " too old to use configuration";
        break;

    case STATUS_MIGRATION_NEEDED:
        error = "proceeding would make backward incompatible changes, aborted";
        break;

    case STATUS_SYNC_END_SHORTCUT:
        error = "intentionally skip sync session shutdown";
        break;

    case sysync::LOCERR_BADPROTO:
        error = "bad or unknown protocol";
        break;
    case sysync::LOCERR_SMLFATAL:
        error = "fatal problem with SML";
        break;
    case sysync::LOCERR_COMMOPEN:
        error = "cannot open communication";
        break;
    case sysync::LOCERR_SENDDATA:
        error = "cannot send data";
        break;
    case sysync::LOCERR_RECVDATA:
        error = "cannot receive data";
        break;
    case sysync::LOCERR_BADCONTENT:
        error = "bad content in response";
        break;
    case sysync::LOCERR_PROCESSMSG:
        error = "SML (or SAN) error processing incoming message";
        break;
    case sysync::LOCERR_COMMCLOSE:
        error = "cannot close communication";
        break;
    case sysync::LOCERR_AUTHFAIL:
        error = "transport layer authorisation failed";
        break;
    case sysync::LOCERR_CFGPARSE:
        error = "error parsing config file";
        break;
    case sysync::LOCERR_CFGREAD:
        error = "error reading config file";
        break;
    case sysync::LOCERR_NOCFG:
        error = "no config found";
        break;
    case sysync::LOCERR_NOCFGFILE:
        error = "config file could not be found";
        break;
    case sysync::LOCERR_EXPIRED:
        error = "expired";
        break;
    case sysync::LOCERR_WRONGUSAGE:
        error = "bad usage";
        break;
    case sysync::LOCERR_BADHANDLE:
        error = "bad handle";
        break;
    case sysync::LOCERR_USERABORT:
        error = "aborted on behalf of user";
        break;
    case sysync::LOCERR_BADREG:
        error = "bad registration";
        break;
    case sysync::LOCERR_LIMITED:
        error = "limited trial version";
        break;
    case sysync::LOCERR_TIMEOUT:
        error = "connection timeout";
        break;
    case sysync::LOCERR_CERT_EXPIRED:
        error = "connection SSL certificate expired";
        break;
    case sysync::LOCERR_CERT_INVALID:
        error = "connection SSL certificate invalid";
        break;
    case sysync::LOCERR_INCOMPLETE:
        error = "incomplete sync session";
        break;
    case sysync::LOCERR_RETRYMSG:
        error = "retry sending message";
        break;
    case sysync::LOCERR_OUTOFMEM:
        error = "out of memory";
        break;
    case sysync::LOCERR_NOCONN:
        error = "no means to open a connection";
        break;
    case sysync::LOCERR_CONN:
        error = "connection cannot be established";
        break;
    case sysync::LOCERR_ALREADY:
        error = "element is already installed";
        break;
    case sysync::LOCERR_TOONEW:
        error = "this build is too new for this license";
        break;
    case sysync::LOCERR_NOTIMP:
        error = "function not implemented";
        break;
    case sysync::LOCERR_WRONGPROD:
        error = "this license code is valid, but not for this product";
        break;
    case sysync::LOCERR_USERSUSPEND:
        error = "explicitly suspended on behalf of user";
        break;
    case sysync::LOCERR_TOOOLD:
        error = "this build is too old for this SDK/plugin";
        break;
    case sysync::LOCERR_UNKSUBSYSTEM:
        error = "unknown subsystem";
        break;
    case sysync::LOCERR_SESSIONRST:
        error = "next message will be a session restart";
        break;
    case sysync::LOCERR_LOCDBNOTRDY:
        error = "local datastore is not ready";
        break;
    case sysync::LOCERR_RESTART:
        error = "session should be restarted from scratch";
        break;
    case sysync::LOCERR_PIPECOMM:
        error = "internal pipe communication problem";
        break;
    case sysync::LOCERR_BUFTOOSMALL:
        error = "buffer too small for requested value";
        break;
    case sysync::LOCERR_TRUNCATED:
        error = "value truncated to fit into field or buffer";
        break;
    case sysync::LOCERR_BADPARAM:
        error = "bad parameter";
        break;
    case sysync::LOCERR_OUTOFRANGE:
        error = "out of range";
        break;
    case sysync::LOCERR_TRANSPFAIL:
        error = "external transport failure";
        break;
    case sysync::LOCERR_CLASSNOTREG:
        error = "class not registered";
        break;
    case sysync::LOCERR_IIDNOTREG:
        error = "interface not registered";
        break;
    case sysync::LOCERR_BADURL:
        error = "bad URL";
        break;
    case sysync::LOCERR_SRVNOTFOUND:
        error = "server not found";
        break;

    case STATUS_MAX:
        break;
    }

    string statusstr = StringPrintf("%s, status %d",
                                    local ? "local" : "remote",
                                    status);
    string description;
    if (error.empty()) {
        description = statusstr;
    } else {
        description = StringPrintf("%s (%s)",
                                   error.c_str(),
                                   statusstr.c_str());
    }

    return description;
}

namespace {
    const char * const locNames[] = { "local", "remote", NULL };
    const char * const stateNames[] = { "added", "updated", "removed", "any", NULL };
    const char * const resultNames[] = { "total", "reject", "match",
                                         "conflict_server_won",
                                         "conflict_client_won",
                                         "conflict_duplicated",
                                         "sent",
                                         "received",
                                         NULL };

    int toIndex(const char * const names[],
                const std::string &name) {
        int i;
        for (i = 0;
             names[i] && name != names[i];
             i++)
            {}
        return i;
    }
    std::string toString(const char * const names[],
                    int index) {
        for (int i = 0;
             names[i];
             i++) {
            if (i == index) {
                return names[i];
            }
        }
        return "unknown";
    }
}

std::string SyncSourceReport::LocationToString(ItemLocation location) { return toString(locNames, location); }
SyncSourceReport::ItemLocation SyncSourceReport::StringToLocation(const std::string &location) { return static_cast<ItemLocation>(toIndex(locNames, location)); }
std::string SyncSourceReport::StateToString(ItemState state) { return toString(stateNames, state); }
SyncSourceReport::ItemState SyncSourceReport::StringToState(const std::string &state) { return static_cast<ItemState>(toIndex(stateNames, state)); }
std::string SyncSourceReport::ResultToString(ItemResult result) { return toString(resultNames, result); }
SyncSourceReport::ItemResult SyncSourceReport::StringToResult(const std::string &result) { return static_cast<ItemResult>(toIndex(resultNames, result)); }

std::string SyncSourceReport::StatTupleToString(ItemLocation location, ItemState state, ItemResult result)
{
    return std::string("") +
        LocationToString(location) + "-" +
        StateToString(state) + "-" +
        ResultToString(result);
}
void SyncSourceReport::StringToStatTuple(const std::string &str, ItemLocation &location, ItemState &state, ItemResult &result)
{
    std::vector< std::string > tokens;
    boost::split(tokens, str, boost::is_any_of("-"));
    location = tokens.size() > 0 ? StringToLocation(tokens[0]) : ITEM_LOCATION_MAX;
    state = tokens.size() > 1 ? StringToState(tokens[1]) : ITEM_STATE_MAX;
    result = tokens.size() > 2 ? StringToResult(tokens[2]) : ITEM_RESULT_MAX;
}

bool SyncSourceReport::wasChanged(ItemLocation location)
{
    for (int i = ITEM_ADDED; i < ITEM_ANY; i++) {
        if (getItemStat(location, (ItemState)i, ITEM_TOTAL) > 0) {
            return true;
        }
    }
    return false;
}


std::ostream &operator << (std::ostream &out, const SyncReport &report)
{
    report.prettyPrint(out, 0);
    return out;
}

namespace {
    string fill(char sep, size_t width) {
        string res;
        res.resize(width - 1, sep);
        return res;
    }
    string center(char sep, const string &str, size_t width) {
        if (str.size() + 1 >= width) {
            return str;
        } else {
            string res;
            res.resize(width - 1, sep);
            res.replace((width - 1 - str.size()) / 2, str.size(), str);
            return res;
        }
    }
    string right(char sep, const string &str, size_t width) {
        if (str.size() + 1 >= width) {
            return str;
        } else {
            string res;
            res.resize(width - 1, sep);
            res.replace(width - 2 - str.size(), str.size(), str);
            return res;
        }
    }
#if 0
    string left(char sep, const string &str, size_t width) {
        if (str.size() + 1 >= width) {
            return str;
        } else {
            string res;
            res.resize(width - 1, sep);
            res.replace(1, str.size(), str);
            return res;
        }
    }
#endif

    // insert string at column if it fits, otherwise flush right
    string align(char sep, const string &str, size_t width, size_t column) {
        if (column + str.size() + 1 >= width) {
            return right(sep, str, width);
        } else {
            string res;
            res.resize(width - 1, sep);
            res.replace(column, str.size(), str);
            return res;
        }
    }
}

SyncReport::SyncReport(const std::string &dump)
{
    boost::shared_ptr<std::string> data(new std::string(dump));
    boost::shared_ptr<StringDataBlob> blob(new StringDataBlob("sync report",
                                                              data,
                                                              true));
    IniHashConfigNode node(blob);
    node >> *this;
}

std::string SyncReport::toString() const
{
    boost::shared_ptr<std::string> data(new std::string);
    boost::shared_ptr<StringDataBlob> blob(new StringDataBlob("sync report",
                                                              data,
                                                              false));
    IniHashConfigNode node(blob);
    node << *this;
    node.flush();
    return *data;
}

void SyncReport::prettyPrint(std::ostream &out, int flags) const
{
    // table looks like this:
    // +-------------------+-------------------------------+-------------------------------|-CON-+
    // |                   |             LOCAL             |           REMOTE              | FLI |
    // |            Source | NEW | MOD | DEL | ERR | TOTAL | NEW | MOD | DEL | ERR | TOTAL | CTS |
    // +-------------------+-----+-----+-----+-----+-------+-----+-----+-----+-----+-------+-----+
    //
    // Most of the columns can be turned on or off dynamically.
    // Their width is calculated once (including right separators and spaces):
    // | name_width        |count_width|                   |                       |conflict_width|
    //                     |client_width                   | server_width          |
    // | text_width                                                                      |

    // name column is sized dynamically based on column header and actual names
    size_t name_width = strlen("Source");
    BOOST_FOREACH(const SyncReport::value_type &entry, *this) {
        const std::string &name = entry.first;
        if (name_width < name.size()) {
            name_width = name.size();
        }
    }
    name_width += 1; // separator
    if (name_width < 20) {
        // enough room for spaces
        name_width += 2;
    }

    int count_width = 6;
    int num_counts = 3;
    if (flags & WITH_TOTAL) {
        num_counts++;
    }
    if (!(flags & WITHOUT_REJECTS)) {
        num_counts++;
    }
    int client_width = (flags & WITHOUT_CLIENT) ? 0 :
        num_counts * count_width;
    int server_width = (flags & WITHOUT_SERVER) ? 0 :
        num_counts * count_width;
    int conflict_width = (flags & WITHOUT_CONFLICTS) ? 0 : 6;
    int text_width = name_width + client_width + server_width + conflict_width;

    if (text_width < 70) {
        // enlarge name column to make room for long lines of text
        name_width += 70 - text_width;
        text_width = 70;
    }

    out << "+" << fill('-', name_width);
    if (!(flags & WITHOUT_CLIENT)) {
        out << '|' << center('-', "", client_width);
    }
    if (!(flags & WITHOUT_SERVER)) {
        out << '|' << center('-', "", server_width);
    }
    if (!(flags & WITHOUT_CONFLICTS)) {
        out << '|' << center('-', "CON", conflict_width);
    }
    out << "+\n";

    if (!(flags & WITHOUT_REJECTS) || !(flags & WITHOUT_CONFLICTS)) {
        out << "|" << fill(' ', name_width);
        if (!(flags & WITHOUT_CLIENT)) {
            out << '|' << center(' ', m_localName, client_width);
        }
        if (!(flags & WITHOUT_SERVER)) {
            out << '|' << center(' ', m_remoteName, server_width);
        }
        if (!(flags & WITHOUT_CONFLICTS)) {
            out << '|' << center(' ', "FLI", conflict_width);
        }
        out << "|\n";
    }

    out << '|' << right(' ', "Source", name_width);
    if (!(flags & WITHOUT_CLIENT)) {
        out << '|' << center(' ', "NEW", count_width);
        out << '|' << center(' ', "MOD", count_width);
        out << '|' << center(' ', "DEL", count_width);
        if (!(flags & WITHOUT_REJECTS)) {
            out << '|' << center(' ', "ERR", count_width);
        }
        if (flags & WITH_TOTAL) {
            out << '|' << center(' ', "TOTAL", count_width);
        }
    }
    if (!(flags & WITHOUT_SERVER)) {
        out << '|' << center(' ', "NEW", count_width);
        out << '|' << center(' ', "MOD", count_width);
        out << '|' << center(' ', "DEL", count_width);
        if (!(flags & WITHOUT_REJECTS)) {
            out << '|' << center(' ', "ERR", count_width);
        }
        if (flags & WITH_TOTAL) {
            out << '|' << center(' ', "TOTAL", count_width);
        }
    }
    if (!(flags & WITHOUT_CONFLICTS)) {
        out << '|' << center(' ', "CTS", conflict_width);
    }
    out << "|\n";

    stringstream sepstream;
    sepstream << '+' << fill('-', name_width);
    if (!(flags & WITHOUT_CLIENT)) {
        sepstream << '+' << fill('-', count_width);
        sepstream << '+' << fill('-', count_width);
        sepstream << '+' << fill('-', count_width);
        if (!(flags & WITHOUT_REJECTS)) {
            sepstream << '+' << fill('-', count_width);
        }
        if (flags & WITH_TOTAL) {
            sepstream << '+' << fill('-', count_width);
        }
    }
    if (!(flags & WITHOUT_SERVER)) {
        sepstream << '+' << fill('-', count_width);
        sepstream << '+' << fill('-', count_width);
        sepstream << '+' << fill('-', count_width);
        if (!(flags & WITHOUT_REJECTS)) {
            sepstream << '+' << fill('-', count_width);
        }
        if (flags & WITH_TOTAL) {
            sepstream << '+' << fill('-', count_width);
        }
    }
    if (!(flags & WITHOUT_CONFLICTS)) {
        sepstream << '+' << fill('-', conflict_width);
    }
    sepstream << "+\n";
    string sep = sepstream.str();
    out << sep;

    BOOST_FOREACH(const SyncReport::value_type &entry, *this) {
        const std::string &name = entry.first;
        const SyncSourceReport &source = entry.second;
        out << '|' << right(' ', name, name_width);
        ssize_t name_column = name_width - 2 - name.size();
        if (name_column < 0) {
            name_column = 0;
        }
        for (SyncSourceReport::ItemLocation location =
                 ((flags & WITHOUT_CLIENT) ? SyncSourceReport::ITEM_REMOTE : SyncSourceReport::ITEM_LOCAL);
             location <= ((flags & WITHOUT_SERVER) ? SyncSourceReport::ITEM_LOCAL : SyncSourceReport::ITEM_REMOTE);
             location = SyncSourceReport::ItemLocation(int(location) + 1)) {
            for (SyncSourceReport::ItemState state = SyncSourceReport::ITEM_ADDED;
                 state <= SyncSourceReport::ITEM_REMOVED;
                 state = SyncSourceReport::ItemState(int(state) + 1)) {
                stringstream count;
                count << source.getItemStat(location, state, SyncSourceReport::ITEM_TOTAL);
                out << '|' << center(' ', count.str(), count_width);
            }
            if (!(flags & WITHOUT_REJECTS)) {
                stringstream count;
                count << source.getItemStat(location,
                                            SyncSourceReport::ITEM_ANY,
                                            SyncSourceReport::ITEM_REJECT);
                out << '|' << center(' ', count.str(), count_width);
            }
            if (flags & WITH_TOTAL) {
                stringstream count;
                count << source.getItemStat(location,
                                            SyncSourceReport::ITEM_ANY,
                                            SyncSourceReport::ITEM_TOTAL);
                out << '|' << center(' ', count.str(), count_width);
            }
        }

        int total_conflicts = 0;
        if (!(flags & WITHOUT_CONFLICTS)) {
            total_conflicts =
                source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                   SyncSourceReport::ITEM_ANY,
                                   SyncSourceReport::ITEM_CONFLICT_SERVER_WON) +
                source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                   SyncSourceReport::ITEM_ANY,
                                   SyncSourceReport::ITEM_CONFLICT_CLIENT_WON) +
                source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                   SyncSourceReport::ITEM_ANY,
                                   SyncSourceReport::ITEM_CONFLICT_DUPLICATED);
            stringstream conflicts;
            conflicts << total_conflicts;
            out << '|' << center(' ', conflicts.str(), conflict_width);
        }
        out << "|\n";

        std::stringstream line;

        if (source.getFinalSyncMode() != SYNC_NONE ||
            source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                               SyncSourceReport::ITEM_ANY,
                               SyncSourceReport::ITEM_SENT_BYTES) ||
            source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                               SyncSourceReport::ITEM_ANY,
                               SyncSourceReport::ITEM_RECEIVED_BYTES)) {
            line <<
                PrettyPrintSyncMode(source.getFinalSyncMode()) << ", ";
            if (source.getRestarts()) {
                line << source.getRestarts() + 1 << " cycles, ";
            }
            line <<
                source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                   SyncSourceReport::ITEM_ANY,
                                   SyncSourceReport::ITEM_SENT_BYTES) / 1024 <<
                " KB sent by client, " <<
                source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                   SyncSourceReport::ITEM_ANY,
                                   SyncSourceReport::ITEM_RECEIVED_BYTES) / 1024 <<
                " KB received";
            out << '|' << align(' ', line.str(), text_width, name_column) << "|\n";
        }

        if (total_conflicts > 0) {
            for (SyncSourceReport::ItemResult result = SyncSourceReport::ITEM_CONFLICT_SERVER_WON;
                 result <= SyncSourceReport::ITEM_CONFLICT_DUPLICATED;
                 result = SyncSourceReport::ItemResult(int(result) + 1)) {
                int count;
                if ((count = source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                                SyncSourceReport::ITEM_ANY,
                                                result)) != 0 || true) {
                    std::stringstream line;
                    line << count << " " <<
                        (result == SyncSourceReport::ITEM_CONFLICT_SERVER_WON ? "client item(s) discarded" :
                         result == SyncSourceReport::ITEM_CONFLICT_CLIENT_WON ? "server item(s) discarded" :
                     "item(s) duplicated");

                    out << '|' << align(' ', line.str(), text_width, name_column) << "|\n";
                }
            }
        }

        int total_matched = source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                               SyncSourceReport::ITEM_ANY,
                                               SyncSourceReport::ITEM_MATCH);
        if (total_matched) {
            stringstream line;
            line << total_matched << " item(s) matched";
            out << '|' << align(' ', line.str(), text_width, name_column)
                << "|\n";
        }

        if (source.m_backupBefore.isAvailable() ||
            source.m_backupAfter.isAvailable()) {
            std::stringstream backup;
            backup << "item(s) in database backup: ";
            if (source.m_backupBefore.isAvailable()) {
                backup << source.m_backupBefore.getNumItems() << " before sync, ";
            } else {
                backup << "no backup before sync, ";
            }
            if (source.m_backupAfter.isAvailable()) {
                backup << source.m_backupAfter.getNumItems() << " after it";
            } else {
                backup << "no backup after it";
            }
            out << '|' << align(' ', backup.str(), text_width, name_column) << "|\n";
        }
        if (source.getStatus()) {
            out  << '|' << align(' ',
                                 Status2String(source.getStatus()),
                                 text_width, name_column) << "|\n";
        }
        out << sep;
    }

    if (getStart()) {
        out << '|' << center(' ', formatSyncTimes(), text_width) << "|\n";
    }
    if (getStatus()) {
        out << '|' << center(' ',
                             getStatus() != STATUS_HTTP_OK ?
                             Status2String(getStatus()) :
                             "synchronization completed successfully",
                             text_width)
            << "|\n";
    }
    if (getStatus() || getStart()) {
        out << sep;
    }
    if (!getError().empty()) {
        out << "First ERROR encountered: " << getError() << endl;
    }
}

std::string SyncReport::formatSyncTimes() const
{
    std::stringstream out;
    time_t duration = m_end - m_start;

    out << "start ";
    if (!m_start) {
        out << "unknown";
    } else {
        struct tm tmbuffer, *tm = localtime_r(&m_start, &tmbuffer);
        if (tm) {
            char buffer[160];
            strftime(buffer, sizeof(buffer), "%c", tm);
            out << buffer;
        } else {
            out << "???";
        }
        if (!m_end) {
            out << ", unknown duration (crashed?!)";
        } else {
            out << ", duration " << duration / 60 << ":"
                << std::setw(2) << std::setfill('0') << duration % 60
                << "min";
        }
    }
    return out.str();
}

std::string SyncReport::slowSyncExplanation(const std::string &peer,
                                            const std::set<std::string> &sources)
{
    if (sources.empty()) {
        return "";
    }

    string sourceparam = boost::join(sources, " ");
    std::string explanation =
        StringPrintf("Doing a slow synchronization may lead to duplicated items or\n"
                     "lost data when the server merges items incorrectly. Choosing\n"
                     "a different synchronization mode may be the better alternative.\n"
                     "Restart synchronization of affected source(s) with one of the\n"
                     "following sync modes to recover from this problem:\n"
                     "    slow, refresh-from-server, refresh-from-client\n\n"
                     "Analyzing the current state:\n"
                     "    syncevolution --status %s %s\n\n"
                     "Running with one of the three modes:\n"
                     "    syncevolution --sync [slow|refresh-from-remote|refresh-from-local] %s %s\n",
                     peer.c_str(), sourceparam.c_str(),
                     peer.c_str(), sourceparam.c_str());
    return explanation;
}

std::string SyncReport::slowSyncExplanation(const std::string &peer) const
{
    std::set<std::string> sources;
    BOOST_FOREACH(const SyncReport::value_type &entry, *this) {
        const std::string &name = entry.first;
        const SyncSourceReport &source = entry.second;
        if (source.getStatus() == STATUS_UNEXPECTED_SLOW_SYNC) {
            string virtualsource = source.getVirtualSource();
            sources.insert(virtualsource.empty() ?
                           name :
                           virtualsource);
        }
    }
    return slowSyncExplanation(peer, sources);
}

ConfigNode &operator << (ConfigNode &node, const SyncReport &report)
{
    node.setProperty("start", static_cast<long>(report.getStart()));
    node.setProperty("end", static_cast<long>(report.getEnd()));
    node.setProperty("status", static_cast<int>(report.getStatus()));
    string error = report.getError();
    if (!error.empty()) {
        node.setProperty("error", error);
    } else {
        node.removeProperty("error");
    }

    BOOST_FOREACH(const SyncReport::value_type &entry, report) {
        const std::string &name = entry.first;
        const SyncSourceReport &source = entry.second;

        string prefix = name;
        boost::replace_all(prefix, "_", "__");
        boost::replace_all(prefix, "-", "_+");
        prefix = "source-" + prefix;

        string key;
        key = prefix + "-mode";
        node.setProperty(key, PrettyPrintSyncMode(source.getFinalSyncMode()));
        if (source.getRestarts()) {
            key = prefix + "-restarts";
            node.setProperty(key, source.getRestarts());
        }
        key = prefix + "-first";
        node.setProperty(key, source.isFirstSync());
        key = prefix + "-resume";
        node.setProperty(key, source.isResumeSync());
        key = prefix + "-status";
        node.setProperty(key, static_cast<long>(source.getStatus()));
        string virtualsource = source.getVirtualSource();
        if (!virtualsource.empty()) {
            key = prefix + "-virtualsource";
            node.setProperty(key, virtualsource);
        }
        key = prefix + "-backup-before";
        node.setProperty(key, source.m_backupBefore.getNumItems());
        key = prefix + "-backup-after";
        node.setProperty(key, source.m_backupAfter.getNumItems());

        for (int location = 0;
             location < SyncSourceReport::ITEM_LOCATION_MAX;
             location++) {
            for (int state = 0;
                 state < SyncSourceReport::ITEM_STATE_MAX;
                 state++) {
                for (int result = 0;
                     result < SyncSourceReport::ITEM_RESULT_MAX;
                     result++) {
                    int intval = source.getItemStat(SyncSourceReport::ItemLocation(location),
                                                    SyncSourceReport::ItemState(state),
                                                    SyncSourceReport::ItemResult(result));
                    if (intval) {
                        key = prefix + "-stat-" +
                            SyncSourceReport::StatTupleToString(SyncSourceReport::ItemLocation(location),
                                                                SyncSourceReport::ItemState(state),
                                                                SyncSourceReport::ItemResult(result));
                        node.setProperty(key, intval);
                    }
                }
            }
        }
    }

    return node;
}

ConfigNode &operator >> (ConfigNode &node, SyncReport &report)
{
    long ts;
    if (node.getProperty("start", ts)) {
        report.setStart(ts);
    }
    if (node.getProperty("end", ts)) {
        report.setEnd(ts);
    }
    int status;
    if (node.getProperty("status", status)) {
        report.setStatus(static_cast<SyncMLStatus>(status));
    }
    string error;
    if (node.getProperty("error", error)) {
        report.setError(error);
    }

    ConfigNode::PropsType props;
    node.readProperties(props);
    BOOST_FOREACH(const ConfigNode::PropsType::value_type &prop, props) {
        string key = prop.first;
        if (boost::starts_with(key, "source-")) {
            key.erase(0, strlen("source-"));
            size_t off = key.find('-');
            if (off != key.npos) {
                string sourcename = key.substr(0, off);
                boost::replace_all(sourcename, "_+", "-");
                boost::replace_all(sourcename, "__", "_");
                SyncSourceReport &source = report.getSyncSourceReport(sourcename);
                key.erase(0, off + 1);
                if (boost::starts_with(key, "stat-")) {
                    key.erase(0, strlen("stat-"));
                    SyncSourceReport::ItemLocation location;
                    SyncSourceReport::ItemState state;
                    SyncSourceReport::ItemResult result;
                    SyncSourceReport::StringToStatTuple(key, location, state, result);
                    stringstream in(prop.second);
                    int intval;
                    in >> intval;
                    source.setItemStat(location, state, result, intval);
                } else if (key == "mode") {
                    source.recordFinalSyncMode(StringToSyncMode(prop.second));
                } else if (key == "restarts") {
                    int value;
                    if (node.getProperty(prop.first, value)) {
                        source.setRestarts(value);
                    }
                } else if (key == "first") {
                    bool value = false;
                    if (node.getProperty(prop.first, value)) {
                        source.recordFirstSync(value);
                    }
                } else if (key == "resume") {
                    bool value = false;
                    if (node.getProperty(prop.first, value)) {
                        source.recordResumeSync(value);
                    }
                } else if (key == "status") {
                    long value;
                    if (node.getProperty(prop.first, value)) {
                        source.recordStatus(static_cast<SyncMLStatus>(value));
                    }
                } else if (key == "virtualsource") {
                    source.recordVirtualSource(node.readProperty(prop.first));
                } else if (key == "backup-before") {
                    long value;
                    if (node.getProperty(prop.first, value)) {
                        source.m_backupBefore.setNumItems(value);
                    }
                } else if (key == "backup-after") {
                    long value;
                    if (node.getProperty(prop.first, value)) {
                        source.m_backupAfter.setNumItems(value);
                    }
                }
            }
        }
    }

    return node;
}

SE_END_CXX
