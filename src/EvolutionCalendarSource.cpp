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

class unrefECalChanges {
 public:
    /** free list of ECalChange instances */
    static void unref(GList *pointer) {
        if (pointer) {
            GList *next = pointer;
            do {
                ECalChange *ecc = (ECalChange *)next->data;
                g_object_unref(ecc->comp);
                g_free(next->data);
                next = next->next;
            } while (next);
            g_list_free(pointer);
        }
    }
};

EvolutionCalendarSource::EvolutionCalendarSource(ECalSourceType type,
                                                 const EvolutionSyncSourceParams &params) :
    TrackingSyncSource(params),
    m_type(type)
{
}

EvolutionCalendarSource::EvolutionCalendarSource( const EvolutionCalendarSource &other ) :
    TrackingSyncSource(other),
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
    bool first = true;
    for (GSList *g = e_source_list_peek_groups (sources); g; g = g->next) {
        ESourceGroup *group = E_SOURCE_GROUP (g->data);
        for (GSList *s = e_source_group_peek_sources (group); s; s = s->next) {
            ESource *source = E_SOURCE (s->data);
            result.push_back( EvolutionSyncSource::source( e_source_peek_name(source),
                                                           e_source_get_uri(source),
                                                           first ) );
            first = false;
        }
    }
    return result;
}

char *EvolutionCalendarSource::authenticate(const char *prompt,
                                            const char *key)
{
    const char *passwd = getPassword();

    LOG.debug("%s: authentication requested, prompt \"%s\", key \"%s\" => %s",
              getName(), prompt, key,
              passwd && passwd[0] ? "returning configured password" : "no password configured");
    return passwd && passwd[0] ? strdup(passwd) : NULL;
}

void EvolutionCalendarSource::open()
{
    ESourceList *sources;
    GError *gerror = NULL;

    if (!e_cal_get_sources(&sources, m_type, &gerror)) {
        throwError("unable to access calendars", gerror);
    }

    string id = getDatabaseID();    
    ESource *source = findSource(sources, id);
    bool onlyIfExists = true;
    if (!source) {
        // might have been special "<<system>>" or "<<default>>", try that and
        // creating address book from file:// URI before giving up
        if (id == "<<system>>" && m_newSystem) {
            m_calendar.set(m_newSystem(), "system calendar/tasks/memos");
        } else if (!id.compare(0, 7, "file://")) {
            m_calendar.set(e_cal_new_from_uri(id.c_str(), m_type), "creating calendar/tasks/memos");
        } else {
            throwError(string("not found: '") + id + "'");
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

void EvolutionCalendarSource::listAllItems(RevisionMap_t &revisions)
{
    GError *gerror = NULL;
    GList *nextItem;
        
    if (!e_cal_get_object_list_as_comp(m_calendar,
                                       "(contains? \"any\" \"\")",
                                       &nextItem,
                                       &gerror)) {
        throwError( "reading all items", gerror );
    }
    eptr<GList> listptr(nextItem);
    while (nextItem) {
        ECalComponent *ecomp = E_CAL_COMPONENT(nextItem->data);
        ItemID id = getItemID(ecomp);
        string luid = id.getLUID();
        string modTime = getItemModTime(ecomp);

        revisions.insert(make_pair(luid, modTime));
        nextItem = nextItem->next;
    }
}

void EvolutionCalendarSource::close()
{
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
    eptr<GList> listptr(nextItem);
    while (nextItem) {
        ItemID id = getItemID(E_CAL_COMPONENT(nextItem->data));
        out << retrieveItemAsString(id);
        out << "\r\n";
        nextItem = nextItem->next;
    }
}

SyncItem *EvolutionCalendarSource::createItem(const string &luid)
{
    logItem( luid, "extracting from EV", true );

    ItemID id = ItemID::parseLUID(luid);
    string icalstr = retrieveItemAsString(id);

    auto_ptr<SyncItem> item(new SyncItem(luid.c_str()));
    item->setData(icalstr.c_str(), icalstr.size());
    item->setDataType("text/calendar");
    item->setModificationTime(0);

    return item.release();
}

void EvolutionCalendarSource::setItemStatusThrow(const char *key, int status)
{
    switch (status) {
    case STC_CONFLICT_RESOLVED_WITH_SERVER_DATA:
         LOG.error("%s: calendar item %.80s: conflict, will be replaced by server\n",
                   getName(), key);
        break;
    }
    TrackingSyncSource::setItemStatusThrow(key, status);
}

string EvolutionCalendarSource::insertItem(string &luid, const SyncItem &item, bool &merged)
{
    bool update = !luid.empty();
    string data = (const char *)item.getData();
    string modTime;

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
    
    if (!update) {
        ItemID id = getItemID(subcomp);
        const char *uid = NULL;

        // TODO: create detached recurrences

        if(!e_cal_create_object(m_calendar, subcomp, (char **)&uid, &gerror)) {
            if (gerror->domain == E_CALENDAR_ERROR &&
                gerror->code == E_CALENDAR_STATUS_OBJECT_ID_ALREADY_EXISTS) {
                // Deal with error due to adding already existing item, that can happen,
                // for example with a "dumb" server which cannot pair items by UID.
                logItem(item, "exists already, updating instead");
                merged = true;
                g_clear_error(&gerror);

                // Starting with Evolution 2.12, the old UID was removed during
                // e_cal_create_object(). Restore it so that the updating below works.
                icalcomponent_set_uid(subcomp, id.m_uid.c_str());
            } else {
                throwError( "storing new calendar item", gerror );
            }
        } else {
            ItemID id(uid, "");
            luid = id.getLUID();
            modTime = getItemModTime(id);
        }
    }

    if (update || merged) {
        // TODO: update detached recurrence

        // ensure that the component has the right UID - some servers replace it
        // inside the VEVENT, but luckily the SyncML standard requires them to
        // provide the original UID in an update
        if (update && item.getKey() && item.getKey()[0]) {
            ItemID id = ItemID::parseLUID(item.getKey());
            icalcomponent_set_uid(subcomp, id.m_uid.c_str());
        }
        
        if (!e_cal_modify_object(m_calendar, subcomp, CALOBJ_MOD_ALL, &gerror)) {
            throwError(string("updating calendar item ") + item.getKey(), gerror);
        }
        ItemID id = getItemID(subcomp);
        luid = id.getLUID();
        modTime = getItemModTime(id);
    }

    return modTime;
}

void EvolutionCalendarSource::deleteItem(const string &luid)
{
    GError *gerror = NULL;
    ItemID id = ItemID::parseLUID(luid);

    // TODO: support detached recurrences

    if (!e_cal_remove_object(m_calendar, id.m_uid.c_str(), &gerror)) {
        if (gerror->domain == E_CALENDAR_ERROR &&
            gerror->code == E_CALENDAR_STATUS_OBJECT_NOT_FOUND) {
            LOG.debug("%s: %s: request to delete non-existant item ignored",
                      getName(), luid.c_str());
            g_clear_error(&gerror);
        } else {
            throwError( string( "deleting calendar item " ) + luid,
                        gerror );
        }
    }
}

void EvolutionCalendarSource::flush()
{
    // Flushing is not necessary, all changes are directly stored.
    // However, our change tracking depends on the resolution with which
    // Evolution stores modification times. Sleeping for a second here
    // ensures that any future operations on the same date generate
    // different modification time stamps.
    time_t start = time(NULL);
    do {
        sleep(1);
    } while (time(NULL) - start <= 0);
}

void EvolutionCalendarSource::logItem(const string &luid, const string &info, bool debug)
{
    if (LOG.getLevel() >= (debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO)) {
        (LOG.*(debug ? &Log::debug : &Log::info))("%s: %s: %s", getName(), luid.c_str(), info.c_str());
    }
}

void EvolutionCalendarSource::logItem(const SyncItem &item, const string &info, bool debug)
{
    if (LOG.getLevel() >= (debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO)) {
        (LOG.*(debug ? &Log::debug : &Log::info))("%s: %s: %s", getName(), item.getKey(), info.c_str());
    }
}

icalcomponent *EvolutionCalendarSource::retrieveItem(const ItemID &id)
{
    GError *gerror = NULL;
    icalcomponent *comp;

    if (!e_cal_get_object(m_calendar,
                          id.m_uid.c_str(),
                          !id.m_rid.empty() ? id.m_rid.c_str() : NULL,
                          &comp,
                          &gerror)) {
        throwError(string("retrieving item: ") + id.getLUID(), gerror);
    }
    if (!comp) {
        throwError(string("retrieving item: ") + id.getLUID());
    }

    return comp;
}

string EvolutionCalendarSource::retrieveItemAsString(const ItemID &id)
{
    eptr<icalcomponent> comp(retrieveItem(id));
    eptr<char> icalstr;

    icalstr = e_cal_get_component_as_string(m_calendar, comp);
    if (!icalstr) {
        throwError(string("could not encode item as iCal: ") + id.getLUID());
    }

    /*
     * Evolution/libical can only deal with \, as separator.
     * Replace plain \, in outgoing event CATEGORIES with , -
     * based on simple text search/replace and thus will not work
     * in all cases...
     *
     * Inverse operation in insertItem().
     */
    string data = string(icalstr);
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
    
    return data;
}

string EvolutionCalendarSource::ItemID::getLUID() const
{
    return getLUID(m_uid, m_rid);
}

string EvolutionCalendarSource::ItemID::getLUID(const string &uid, const string &rid)
{
    return uid + "-rid" + rid;
}

EvolutionCalendarSource::ItemID EvolutionCalendarSource::ItemID::parseLUID(const string &luid)
{
    size_t ridoff = luid.rfind("-rid");
    if (ridoff != luid.npos) {
        return ItemID(luid.substr(0, ridoff),
                      luid.substr(ridoff + strlen("-rid")));
    } else {
        return ItemID(luid, "");
    }
}

EvolutionCalendarSource::ItemID EvolutionCalendarSource::getItemID(ECalComponent *ecomp)
{
    icalcomponent *icomp = e_cal_component_get_icalcomponent(ecomp);
    if (!icomp) {
        throwError("internal error in getItemID(): ECalComponent without icalcomp");
    }
    return getItemID(icomp);
}

EvolutionCalendarSource::ItemID EvolutionCalendarSource::getItemID(icalcomponent *icomp)
{
    const char *uid;
    struct icaltimetype rid;

    uid = icalcomponent_get_uid(icomp);
    rid = icalcomponent_get_recurrenceid(icomp);
    return ItemID(uid ? uid : "",
                  icalTime2Str(rid));
}

string EvolutionCalendarSource::getItemModTime(ECalComponent *ecomp)
{
    struct icaltimetype *modTime;
    e_cal_component_get_last_modified(ecomp, &modTime);
    eptr<struct icaltimetype, struct icaltimetype, EvolutionUnrefFree<struct icaltimetype> > modTimePtr(modTime, "calendar item without modification time");
    return icalTime2Str(*modTimePtr);
}

string EvolutionCalendarSource::getItemModTime(const ItemID &id)
{
    eptr<icalcomponent> icomp(retrieveItem(id));
    icalproperty *lastModified = icalcomponent_get_first_property(icomp, ICAL_LASTMODIFIED_PROPERTY);
    if (!lastModified) {
        throwError("getItemModTime(): calendar item without modification time");
    }
    struct icaltimetype modTime = icalproperty_get_lastmodified(lastModified);
    return icalTime2Str(modTime);

}

string EvolutionCalendarSource::icalTime2Str(const icaltimetype &tt)
{
    static const struct icaltimetype null = { 0 };
    if (!memcmp(&tt, &null, sizeof(null))) {
        return "";
    } else {
        const char *timestr = icaltime_as_ical_string(tt);
        if (!timestr) {
            throwError("cannot convert to time string");
        }
        return timestr;
    }
}

#endif /* ENABLE_ECAL */

#ifdef ENABLE_MODULES
# include "EvolutionCalendarSourceRegister.cpp"
#endif
