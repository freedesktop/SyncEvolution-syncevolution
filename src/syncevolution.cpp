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
#include <stddef.h>

#include <base/Log.h>
#include <posix/base/posixlog.h>
#include <spds/spdsutils.h>

#include <iostream>
#include <memory>
using namespace std;

#include <libgen.h>
#ifdef HAVE_GLIB
#include <glib-object.h>
#endif

#include "SyncEvolutionCmdline.h"
#include "EvolutionSyncSource.h"
#include "EvolutionSyncClient.h"

#if defined(ENABLE_MAEMO) && defined (ENABLE_EBOOK)

#include <dlfcn.h>

extern "C" EContact *e_contact_new_from_vcard(const char *vcard)
{
    static typeof(e_contact_new_from_vcard) *impl;

    if (!impl) {
        impl = (typeof(impl))dlsym(RTLD_NEXT, "e_contact_new_from_vcard");
    }

    // Old versions of EDS-DBus parse_changes_array() call
    // e_contact_new_from_vcard() with a pointer which starts
    // with a line break; Evolution is not happy with that and
    // refuses to parse it. This code forwards until it finds
    // the first non-whitespace, presumably the BEGIN:VCARD.
    while (*vcard && isspace(*vcard)) {
        vcard++;
    }

    return impl ? impl(vcard) : NULL;
}
#endif

#ifdef LOG_HAVE_SET_LOGGER
class CmdLineLogger : public POSIXLog {
protected:
    virtual void printLine(bool firstLine,
                           time_t time,
                           const char *fullTime,
                           const char *shortTime,
                           const char *utcTime,
                           LogLevel level,
                           const char *levelPrefix,
                           const char *line) {
        POSIXLog::printLine(firstLine,
                            time,
                            fullTime,
                            shortTime,
                            utcTime,
                            level,
                            levelPrefix,
                            line);
        if (level <= LOG_LEVEL_INFO &&
            getLogFile()) {
            /* POSIXLog is printing to file, therefore print important lines to stdout */
            fprintf(stdout, "%s [%s] %s\n",
                    shortTime,
                    levelPrefix,
                    line);
        }
    }
};
#endif

int main( int argc, char **argv )
{
#ifdef ENABLE_MAEMO
    // EDS-DBus uses potentially long-running calls which may fail due
    // to the default 25s timeout. Some of these can be replaced by
    // their async version, but e_book_async_get_changes() still
    // triggered it.
    //
    // The workaround for this is to link the binary against a libdbus
    // which has the dbus-timeout.patch and thus let's users and
    // the application increase the default timeout.
    setenv("DBUS_DEFAULT_TIMEOUT", "600000", 0);
#endif
    
#if defined(HAVE_GLIB) && defined(HAVE_EDS)
    // this is required on Maemo and does not harm either on a normal
    // desktop system with Evolution
    g_type_init();
#endif

#ifdef LOG_HAVE_SET_LOGGER
    static CmdLineLogger logger;
    Log::setLogger(&logger);
#endif

#ifdef POSIX_LOG
    POSIX_LOG.
#endif
        setLogFile(NULL, "-");
    LOG.reset();
    LOG.setLevel(LOG_LEVEL_INFO);
    resetError();
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    // Expand PATH to cover the directory we were started from?
    // This might be needed to find normalize_vcard.
    char *exe = strdup(argv[0]);
    if (strchr(exe, '/') ) {
        char *dir = dirname(exe);
        string path;
        char *oldpath = getenv("PATH");
        if (oldpath) {
            path += oldpath;
            path += ":";
        }
        path += dir;
        setenv("PATH", path.c_str(), 1);
    }
    free(exe);

    try {
        SyncEvolutionCmdline cmdline(argc, argv, cout, cerr);
        if (cmdline.parse() &&
            cmdline.run()) {
            return 0;
        } else {
            return 1;
        }
    } catch ( const std::exception &ex ) {
        LOG.error( "%s", ex.what() );
    } catch (...) {
        LOG.error( "unknown error" );
    }

    return 1;
}
