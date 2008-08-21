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

#ifndef INCL_EVOLUTIONSYNCCLIENT
#define INCL_EVOLUTIONSYNCCLIENT

#include <config.h>

#include "EvolutionSmartPtr.h"
#include "SyncEvolutionConfig.h"
#include <client/SyncClient.h>
#include <spds/SyncManagerConfig.h>

#include <string>
#include <set>
using namespace std;

class SourceList;

/*
 * This is the main class inside sync4jevolution which
 * looks at the configuration, activates all enabled
 * sources and executes the synchronization.
 *
 */
class EvolutionSyncClient : public SyncClient, public EvolutionSyncConfig, public ConfigUserInterface {
    const string m_server;
    const set<string> m_sources;
    const bool m_doLogging;
    SyncMode m_syncMode;
    bool m_quiet;

    /**
     * a pointer to the active SourceList instance if one exists; 
     * used for error handling in throwError() on the iPhone
     */
    static SourceList *m_sourceListPtr;

  public:
    /**
     * @param server     identifies the server config to be used
     * @param doLogging  write additional log and datatbase files about the sync
     * @param sources    overrides the set of active sources
     */
    EvolutionSyncClient(const string &server,
                        bool doLogging = false,
                        const set<string> &sources = set<string>());
    ~EvolutionSyncClient();

    /**
     * A helper function which interactively asks the user for
     * a certain password. May throw errors.
     *
     * The default implementation uses stdin/stdout to communicate
     * with the user.
     *
     * @param descr     A simple string explaining what the password is needed for,
     *                  e.g. "SyncML server". Has to be unique and understandable
     *                  by the user.
     * @return entered password
     */
    virtual string askPassword(const string &descr);

    /**
     * controls verbosity during sync
     */
    bool getQuiet() { return m_quiet; }
    void setQuiet(bool quiet) { m_quiet = quiet; }

    /**
     * override preferred sync mode of all active sources without
     * enabling sources which have no sync mode set in the config
     */
    SyncMode getSyncMode() { return m_syncMode; }
    void setSyncMode(SyncMode syncMode) { m_syncMode = syncMode; }

    /**
     * intercept config filters
     *
     * This call removes the "sync" source property and remembers
     * it separately because it has to be applied to only the active
     * sync sources; the generic config handling code would apply
     * it to all sources.
     */
    virtual void setConfigFilter(bool sync, const FilterConfigNode::ConfigFilter &filter);

    /**
     * Executes the sync, throws an exception in case of failure.
     * Handles automatic backups and report generation.
     */
    int sync();

    /**
     * Determines the log directory of the previous sync (either in
     * temp or logdir) and shows changes since then.
     */
    void status();

    /**
     * throws a runtime_error with the given string
     * or (on the iPhone, where exception handling is not
     * supported by the toolchain) prints an error directly
     * and aborts
     *
     * output format: <error>
     *
     * @param error     a string describing the error
     */
    static void throwError(const string &error);

    /**
     * throw an exception after an operation failed
     *
     * output format: <action>: <error string>
     *
     * @Param action   a string describing the operation or object involved
     * @param error    the errno error code for the failure
     */
    static void throwError(const string &action, int error);

    /**
     * An error handler which prints the error message and then
     * stops the program. Never returns.
     *
     * The API was chosen so that it can be used as libebook/libecal
     * "backend-dies" signal handler.
     */
    static void fatalError(void *object, const char *error);

    /**
     * When using Evolution this function starts a background thread
     * which drives the default event loop. Without that loop
     * "backend-died" signals are not delivered. The problem with
     * the thread is that it seems to interfere with gconf startup
     * when added to the main() function of syncevolution. Therefore
     * it is started by EvolutionSyncSource::beginSync() (for unit
     * testing of sync sources) and EvolutionSyncClient::sync() (for
     * normal operation).
     */
    static void startLoopThread();

    /* AbstractSyncConfig API */
    virtual AbstractSyncSourceConfig* getAbstractSyncSourceConfig(const char* name) const;
    virtual AbstractSyncSourceConfig* getAbstractSyncSourceConfig(unsigned int i) const;
    virtual unsigned int getAbstractSyncSourceConfigsCount() const;

  protected:
    /**
     * Callback for derived classes: called after setting up the client's
     * and sources' configuration. Can be used to reconfigure before
     * actually starting the synchronization.
     *
     * @param sources   a NULL terminated array of all active sources
     */
    virtual void prepare(SyncSource **sources);

 private:
    /**
     * the code common to init() and status():
     * populate source list with active sources and open
     * them for reading without changing their state yet
     */
    void initSources(SourceList &sourceList);

    /**
     * override sync mode of all active sync sources if set
     */
    string m_overrideMode;
};

#endif // INCL_EVOLUTIONSYNCCLIENT
