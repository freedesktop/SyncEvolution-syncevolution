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

#include <memory>
using namespace std;

#include "config.h"

#ifdef ENABLE_ECAL

#include "EvolutionSyncClient.h"
#include "EvolutionCalendarSource.h"
#include "EvolutionMemoSource.h"
#include "EvolutionSmartPtr.h"

#include <common/base/Log.h>
#include <common/vocl/VObject.h>
#include <common/vocl/VConverter.h>

static const string
EVOLUTION_CALENDAR_PRODID("PRODID:-//ACME//NONSGML SyncEvolution//EN"),
EVOLUTION_CALENDAR_VERSION("VERSION:2.0");

EvolutionCalendarSource::EvolutionCalendarSource( ECalSourceType type,
                                                  const string &name,
                                                  SyncSourceConfig *sc,
                                                  const string &changeId,
                                                  const string &id ) :
    EvolutionSyncSource(name, sc, changeId, id),
    m_type(type)
{
}

EvolutionCalendarSource::EvolutionCalendarSource( const EvolutionCalendarSource &other ) :
    EvolutionSyncSource(other),
    m_type(other.m_type)
{
    switch (m_type) {
     case E_CAL_SOURCE_TYPE_EVENT:
        m_typeName = "calendar";
        m_newSystem = e_cal_new_system_calendar;
        break;
     case E_CAL_SOURCE_TYPE_TODO:
        m_typeName = "task list";
        m_newSystem = e_cal_new_system_tasks;
        break;
     case E_CAL_SOURCE_TYPE_JOURNAL:
        m_typeName = "memo list";
        // This is not available in older Evolution versions.
        // A configure check could detect that, but as this isn't
        // important the functionality is simply disabled.
        m_newSystem = NULL /* e_cal_new_system_memos */;
        break;
     default:
        EvolutionSyncClient::throwError("internal error, invalid calendar type");
        break;
    }
}

EvolutionSyncSource::sources EvolutionCalendarSource::getSyncBackends()
{
    ESourceList *sources = NULL;
    GError *gerror = NULL;

    if (!e_cal_get_sources(&sources, m_type, &gerror)) {
        throwError("unable to access calendars", gerror);
    }

    EvolutionSyncSource::sources result;

    for (GSList *g = e_source_list_peek_groups (sources); g; g = g->next) {
        ESourceGroup *group = E_SOURCE_GROUP (g->data);
        for (GSList *s = e_source_group_peek_sources (group); s; s = s->next) {
            ESource *source = E_SOURCE (s->data);
            result.push_back( EvolutionSyncSource::source( e_source_peek_name(source),
                                                           e_source_get_uri(source) ) );
        }
    }
    return result;
}

char *EvolutionCalendarSource::authenticate(const char *prompt,
                                            const char *key)
{
    string user, passwd;
    getAuthentication(user, passwd);
    
    LOG.debug("%s: authentication requested, prompt \"%s\", key \"%s\" => %s",
              getName(), prompt, key,
              passwd.size() ? "returning configured password" : "no password configured");
    return passwd.size() ? strdup(passwd.c_str()) : NULL;
}

void EvolutionCalendarSource::open()
{
    ESourceList *sources;
    GError *gerror = NULL;

    if (!e_cal_get_sources(&sources, m_type, &gerror)) {
        throwError("unable to access calendars", gerror);
    }
    
    ESource *source = findSource(sources, m_id);
    bool onlyIfExists = true;
    if (!source) {
        // might have been special "<<system>>" or "<<default>>", try that and
        // creating address book from file:// URI before giving up
        if (m_id == "<<system>>" && m_newSystem) {
            m_calendar.set(m_newSystem(), "system calendar/tasks/memos");
        } else if (!m_id.compare(0, 7, "file://")) {
            m_calendar.set(e_cal_new_from_uri(m_id.c_str(), m_type), "creating calendar/tasks/memos");
        } else {
            throwError(string("not found: '") + m_id + "'");
        }
        onlyIfExists = false;
    } else {
        m_calendar.set(e_cal_new(source, m_type), m_typeName.c_str());
    }

    e_cal_set_auth_func(m_calendar, eCalAuthFunc, this);
    
    if (!e_cal_open(m_calendar, onlyIfExists, &gerror)) {
        // opening newly created address books often failed, perhaps that also applies to calendars - try again
        g_clear_error(&gerror);
        sleep(5);
        if (!e_cal_open(m_calendar, onlyIfExists, &gerror)) {
            throwError( "opening calendar", gerror );
        }
    }

    g_signal_connect_after(m_calendar,
                           "backend-died",
                           G_CALLBACK(EvolutionSyncClient::fatalError),
                           (void *)"Evolution Data Server has died unexpectedly, database no longer available.");
}

void EvolutionCalendarSource::getChanges()
{
    GError *gerror = NULL;
    bool foundChanges;

    m_newItems.clear();
    m_updatedItems.clear();
    m_deletedItems.clear();

    // This is repeated in a loop because it was observed that in
    // Evolution 2.0.6 the deleted items were not all reported in one
    // chunk. Instead each invocation of e_cal_get_changes() reported
    // additional changes.
    do {
        GList *nextItem;

        foundChanges = false;
        if (!e_cal_get_changes(m_calendar, (char *)m_changeId.c_str(), &nextItem, &gerror)) {
            throwError( "reading changes", gerror );
        }
        while (nextItem) {
            ECalChange *ecc = (ECalChange *)nextItem->data;

            if (ecc->comp) {
                const char *uid = NULL;

                // does not have a return code which could be checked
                e_cal_component_get_uid(ecc->comp, &uid);

                if (uid) {
                    switch (ecc->type) {
                    case E_CAL_CHANGE_ADDED:
                        if (m_newItems.addItem(uid)) {
                            foundChanges = true;
                        }
                        break;
                    case E_CAL_CHANGE_MODIFIED:
                        if (m_updatedItems.addItem(uid)) {
                            foundChanges = true;
                        }
                        break;
                    case E_CAL_CHANGE_DELETED:
                        if (m_deletedItems.addItem(uid)) {
                            foundChanges = true;
                        }
                        break;
                    }
                }
            }

            nextItem = nextItem->next;
        }
    } while(foundChanges);

    // Sanitize result: it is possible that items are listed
    // as new and updated. That occurred when creating
    // a recurring event and then updating the detached exceptions.
    // Only send them as new in that case.
    set<string>::iterator it = m_updatedItems.begin();
    while (it != m_updatedItems.end()) {
        if (m_newItems.find(*it) != m_newItems.end()) {
            set<string>::iterator old = it;
            ++it;
            m_updatedItems.erase(old);
        } else {
            ++it;
        }
    }
}

void EvolutionCalendarSource::beginSyncThrow(bool needAll,
                                             bool needPartial,
                                             bool deleteLocal)
{
    GError *gerror = NULL;

    if (deleteLocal) {
        GList *nextItem;
        
        if (!e_cal_get_object_list_as_comp(m_calendar,
                                           "(contains? \"any\" \"\")",
                                           &nextItem,
                                           &gerror)) {
            throwError( "reading all items", gerror );
        }
        while (nextItem) {
            const char *uid;

            e_cal_component_get_uid(E_CAL_COMPONENT(nextItem->data), &uid);
            if (!e_cal_remove_object(m_calendar, uid, &gerror) ) {
                throwError( string( "deleting calendar entry " ) + uid,
                            gerror );
            }
            nextItem = nextItem->next;
        }
    }

    if (needAll) {
        GList *nextItem;
        
        if (!e_cal_get_object_list_as_comp(m_calendar, "(contains? \"any\" \"\")", &nextItem, &gerror)) {
            throwError( "reading all items", gerror );
        }
        while (nextItem) {
            const char *uid;

            e_cal_component_get_uid(E_CAL_COMPONENT(nextItem->data), &uid);
            m_allItems.addItem(uid);
            nextItem = nextItem->next;
        }
    }

    if (needPartial) {
        getChanges();
    }
}

void EvolutionCalendarSource::endSyncThrow()
{
    if (m_isModified) {
        getChanges();
    }
    resetItems();
    m_isModified = false;
}

void EvolutionCalendarSource::close()
{
    endSyncThrow();
    m_calendar = NULL;
}

void EvolutionCalendarSource::exportData(ostream &out)
{
    GList *nextItem;
    GError *gerror = NULL;

    if (!e_cal_get_object_list_as_comp(m_calendar,
                                       "(contains? \"any\" \"\")",
                                       &nextItem,
                                       &gerror)) {
        throwError( "reading all items", gerror );
    }
    while (nextItem) {
        const char *uid;
        e_cal_component_get_uid(E_CAL_COMPONENT(nextItem->data), &uid);
        out << retrieveItemAsString(uid);
        out << "\r\n";
        nextItem = nextItem->next;
    }
}

SyncItem *EvolutionCalendarSource::createItem( const string &uid, SyncState state )
{
    logItem( uid, "extracting from EV", true );
        
    string icalstr = retrieveItemAsString(uid);

    auto_ptr<SyncItem> item(new SyncItem(uid.c_str()));
    item->setData(icalstr.c_str(), icalstr.size());
    item->setDataType("text/calendar");
    item->setModificationTime(0);
    item->setState(state);

    return item.release();
}

void EvolutionCalendarSource::setItemStatusThrow(const char *key, int status)
{
    switch (status) {
     case STC_CONFLICT_RESOLVED_WITH_SERVER_DATA: {
         LOG.error("%s: calendar item %.80s: conflict, will be replaced by server\n",
                   getName(), key);

        // uids make the item unique, so it cannot be copied;
        // the following code does not work:
        //
        // ECalComponent *comp = retrieveItem(key);
        // ECalComponent *copy = e_cal_component_clone(comp);
        // if (!copy) {
        //    LOG.error("copying %s: making copy failed\n", key);
        // }
        break;
     }
     default:
        EvolutionSyncSource::setItemStatusThrow(key, status);
        break;
    }
}

int EvolutionCalendarSource::addItemThrow(SyncItem& item)
{
    return insertItem(item, false);
}

int EvolutionCalendarSource::insertItem(SyncItem& item, bool update)
{
    bool fallback = false;
    bool created = false;
    string data = getData(item);

    /*
     * Evolution/libical can only deal with \, as separator.
     * Replace plain , in incoming event CATEGORIES with \, -
     * based on simple text search/replace and thus will not work
     * in all cases...
     *
     * Inverse operation in extractItemAsString().
     */
    size_t propstart = data.find("\nCATEGORIES");
    bool modified = false;
    while (propstart != data.npos) {
        size_t eol = data.find('\n', propstart + 1);
        size_t comma = data.find(',', propstart);

        while (eol != data.npos &&
               comma != data.npos &&
               comma < eol) {
            if (data[comma-1] != '\\') {
                data.insert(comma, "\\");
                comma++;
                modified = true;
            }
            comma = data.find(',', comma + 1);
        }
        propstart = data.find("\nCATEGORIES", propstart + 1);
    }
    if (modified) {
        LOG.debug("after replacing , with \\, in CATEGORIES:\n%s", data.c_str());
    }

    eptr<icalcomponent> icomp(icalcomponent_new_from_string((char *)data.c_str()));

    if( !icomp ) {
        throwError( string( "parsing ical" ) + data,
                    NULL );
    }

    GError *gerror = NULL;
    char *uid = NULL;
    int status = STC_OK;

    // insert before adding/updating the event so that the new VTIMEZONE is
    // immediately available should anyone want it
    for (icalcomponent *tcomp = icalcomponent_get_first_component(icomp, ICAL_VTIMEZONE_COMPONENT);
         tcomp;
         tcomp = icalcomponent_get_next_component(icomp, ICAL_VTIMEZONE_COMPONENT)) {
        eptr<icaltimezone> zone(icaltimezone_new(), "icaltimezone");
        icaltimezone_set_component(zone, tcomp);

        GError *gerror = NULL;
        gboolean success = e_cal_add_timezone(m_calendar, zone, &gerror);
        if (!success) {
            throwError(string("error adding VTIMEZONE ") + icaltimezone_get_tzid(zone),
                       gerror);
        }
    }

    // the component to update/add must be the
    // ICAL_VEVENT/VTODO_COMPONENT of the item,
    // e_cal_create/modify_object() fail otherwise
    icalcomponent *subcomp = icalcomponent_get_first_component(icomp,
                                                               getCompType());
    if (!subcomp) {
        throwError("extracting event");
    }
    do {
        if (!update) {
            const char *olduid = icalcomponent_get_uid(subcomp);
            string olduidstr(olduid ? olduid : "");

            if(!e_cal_create_object(m_calendar, subcomp, &uid, &gerror)) {
                if (gerror->domain == E_CALENDAR_ERROR &&
                    gerror->code == E_CALENDAR_STATUS_OBJECT_ID_ALREADY_EXISTS) {
                    // Deal with error due to adding already existing item, that can happen,
                    // for example with a "dumb" server which cannot pair items by UID.
                    logItem(item, "exists already, updating instead");
                    fallback = true;
                    g_clear_error(&gerror);
                    
                    // Starting with Evolution 2.12, the old UID was removed during
                    // e_cal_create_object(). Restore it so that the updating below works.
                    icalcomponent_set_uid(subcomp, olduidstr.c_str());
                } else {
                    throwError( "storing new calendar item", gerror );
                }
            } else {
                created = true;
                if (uid) {
                    item.setKey(uid);
                }
            }
        }

        if (update || fallback) {
            // ensure that the component has the right UID - some servers replace it
            // inside the VEVENT, but luckily the SyncML standard requires them to
            // provide the original UID in an update
            if (update && item.getKey() && item.getKey()[0]) {
                icalcomponent_set_uid(subcomp, item.getKey());
            }
        
            if (!e_cal_modify_object(m_calendar, subcomp, CALOBJ_MOD_THIS, &gerror)) {
                throwError(string("updating calendar item ") + item.getKey(), gerror);
            }
            string uid = getCompUID(subcomp);
            if (uid.size()) {
                item.setKey(uid.c_str());
            }
            if (fallback && !created) {
                status = STC_CONFLICT_RESOLVED_WITH_MERGE;
            }
        }
        subcomp = icalcomponent_get_next_component(icomp, getCompType());
    } while(subcomp);

    return status;
}

int EvolutionCalendarSource::updateItemThrow(SyncItem& item)
{
    return insertItem(item, true);
}

int EvolutionCalendarSource::deleteItemThrow(SyncItem& item)
{
    int status = STC_OK;
    GError *gerror = NULL;

    if (!e_cal_remove_object(m_calendar, item.getKey(), &gerror)) {
        if (gerror->domain == E_CALENDAR_ERROR &&
            gerror->code == E_CALENDAR_STATUS_OBJECT_NOT_FOUND) {
            LOG.debug("%s: %s: request to delete non-existant item ignored",
                      getName(), item.getKey());
            g_clear_error(&gerror);
        } else {
            throwError( string( "deleting calendar item " ) + item.getKey(),
                        gerror );
        }
    }
    return status;
}

void EvolutionCalendarSource::logItem(const string &uid, const string &info, bool debug)
{
    if (LOG.getLevel() >= (debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO)) {
        (LOG.*(debug ? &Log::debug : &Log::info))("%s: %s: %s", getName(), uid.c_str(), info.c_str());
    }
}

void EvolutionCalendarSource::logItem( SyncItem &item, const string &info, bool debug)
{
    if (LOG.getLevel() >= (debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO)) {
        (LOG.*(debug ? &Log::debug : &Log::info))("%s: %s: %s", getName(), item.getKey(), info.c_str());
    }
}

string EvolutionCalendarSource::retrieveItemAsString(const string &uid)
{
    GList *nextItem;
    GError *gerror = NULL;

    if (!e_cal_get_objects_for_uid(m_calendar,
                                   uid.c_str(),
                                   &nextItem,
                                   &gerror)) {
        throwError(string("retrieving item: ") + uid, gerror);
    }

    string data = "BEGIN:VCALENDAR\n"
        "PRODID:-//Ohly//NONSGML SyncEvolution//EN\n"
        "VERSION:2.0\n";

    set<string> timezones;

    while (nextItem) {
        eptr<char> icalstr;
        
        // icalstr = e_cal_get_component_as_string(m_calendar, (icalcomponent *)nextItem->data);
        ECalComponent *ecomp = (ECalComponent *)nextItem->data;
        icalstr = e_cal_component_get_as_string(ecomp);
        if (!icalstr) {
            throwError(string("could not encode item as iCal: ") + uid);
        }
        data += (char *)icalstr;

        // remember which timezones were used to retrieve their definition later on
        ECalComponentDateTime dt;
        e_cal_component_get_dtend(ecomp, &dt);
        if (dt.tzid) {
            timezones.insert(dt.tzid);
        }
        e_cal_component_free_datetime(&dt);
        e_cal_component_get_dtstart(ecomp, &dt);
        if (dt.tzid) {
            timezones.insert(dt.tzid);
        }
        e_cal_component_free_datetime(&dt);
        e_cal_component_get_due(ecomp, &dt);
        if (dt.tzid) {
            timezones.insert(dt.tzid);
        }
        e_cal_component_free_datetime(&dt);
        
        nextItem = nextItem->next;
    }

    /*
     * Evolution/libical can only deal with \, as separator.
     * Replace plain \, in outgoing event CATEGORIES with , -
     * based on simple text search/replace and thus will not work
     * in all cases...
     *
     * Inverse operation in insertItem().
     */
    size_t propstart = data.find("\nCATEGORIES");
    bool modified = false;
    while (propstart != data.npos) {
        size_t eol = data.find('\n', propstart + 1);
        size_t comma = data.find(',', propstart);

        while (eol != data.npos &&
               comma != data.npos &&
               comma < eol) {
            if (data[comma-1] == '\\') {
                data.erase(comma - 1, 1);
                comma--;
                modified = true;
            }
            comma = data.find(',', comma + 1);
        }
        propstart = data.find("\nCATEGORIES", propstart + 1);
    }
    if (modified) {
        LOG.debug("after replacing \\, with , in CATEGORIES:\n%s", data.c_str());
    }

    // try to append all timezone definitions, but ignore error (in
    // some Evolution versions it was possible to import an event
    // without the corresponding timezones - SF #1777453)
    for (set<string>::const_iterator it = timezones.begin();
         it != timezones.end();
         ++it) {
        icaltimezone *zone;
        GError *gerror = NULL;

        if (e_cal_get_timezone(m_calendar,
                               it->c_str(),
                               &zone,
                               &gerror)) {
            icalcomponent *zonecomp = icaltimezone_get_component(zone);
            if (zonecomp) {
                const char *zonestr = icalcomponent_as_ical_string(zonecomp);
                if (zonestr) {
                    data += (char *)zonestr;
                }
            }
        } else {
            g_clear_error(&gerror);
        }
    }

    data += "END:VCALENDAR\n";
    
    return data;
}

string EvolutionCalendarSource::getCompUID(icalcomponent *icomp)
{
    // figure out what the UID is
    icalproperty *iprop = icalcomponent_get_first_property(icomp,
                                                           ICAL_UID_PROPERTY);
    if (!iprop) {
        throwError("cannot extract UID property");
    }
    const char *uid = icalproperty_get_uid(iprop);
    return string(uid ? uid : "");
}

#ifdef ENABLE_MODULES

extern "C" EvolutionSyncSource *SyncEvolutionCreateSource(const string &name,
                                                          SyncSourceConfig *sc,
                                                          const string &changeId,
                                                          const string &id,
                                                          const string &mimeType)
{
    if (mimeType == "text/x-todo") {
        return new EvolutionCalendarSource(E_CAL_SOURCE_TYPE_TODO, name, sc, changeId, id);
    } else if (mimeType == "text/x-journal") {
        return new EvolutionCalendarSource(E_CAL_SOURCE_TYPE_JOURNAL, name, sc, changeId, id);
    } else if (mimeType == "text/plain") {
        return new EvolutionMemoSource(E_CAL_SOURCE_TYPE_JOURNAL, name, sc, changeId, id);
    } else if (mimeType == "text/calendar" ||
               mimeType == "text/x-vcalendar") {
        return new EvolutionCalendarSource(E_CAL_SOURCE_TYPE_EVENT, name, sc, changeId, id);
    } else {
        return NULL;
    }
}

#endif /* ENABLE_MODULES */

#endif /* ENABLE_ECAL */
