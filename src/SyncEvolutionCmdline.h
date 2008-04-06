/*
 * Copyright (C) 2008 Patrick Ohly
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY, TITLE, NONINFRINGEMENT or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307  USA
 */

#ifndef INCL_SYNC_EVOLUTION_CMDLINE
# define INCL_SYNC_EVOLUTION_CMDLINE

#include "SyncEvolutionConfig.h"
#include "FilterConfigNode.h"
class EvolutionSyncSource;

#include <set>
using namespace std;

#include <boost/shared_ptr.hpp>

class SyncEvolutionCmdlineTest;

class SyncEvolutionCmdline {
public:
    /**
     * @param out      stdout stream for normal messages
     * @param err      stderr stream for error messages
     */
    SyncEvolutionCmdline(int argc, const char * const *argv, ostream &out, ostream &err);

    /**
     * parse the command line options
     *
     * @retval true if command line was okay
     */
    bool parse();

    bool run();

private:
    class Bool { 
    public:
        Bool(bool val = false) : m_value(val) {}
        operator bool () { return m_value; }
        Bool & operator = (bool val) { m_value = val; return *this; }
    private:
        bool m_value;
    };

    int m_argc;
    const char * const * m_argv;
    ostream &m_out, &m_err;

    Bool m_quiet;
    Bool m_status;
    Bool m_version;
    Bool m_usage;
    Bool m_configure;
    Bool m_migrate;
    Bool m_printServers;
    Bool m_printConfig;
    Bool m_dontrun;
    FilterConfigNode::ConfigFilter m_syncProps, m_sourceProps;
    const ConfigPropertyRegistry &m_validSyncProps;
    const ConfigPropertyRegistry &m_validSourceProps;

    string m_server;
    string m_template;
    set<string> m_sources;

    /** compose description of cmd line option with optional parameter */
    static string cmdOpt(const char *opt, const char *param = NULL);

    /**
     * parse sync or source property
     *
     * @param validProps     list of valid properties
     * @retval props         add property name/value pair here
     * @param opt            command line option as it appeard in argv (e.g. --sync|--sync-property|-z)
     * @param param          the parameter following the opt, may be NULL if none given (error!)
     * @param propname       if given, then this is the property name and param contains the param value (--sync <param>)
     */
    bool parseProp(const ConfigPropertyRegistry &validProps,
                   FilterConfigNode::ConfigFilter &props,
                   const char *opt,
                   const char *param,
                   const char *propname = NULL);

    bool listPropValues(const ConfigPropertyRegistry &validProps,
                        const string &propName,
                        const string &opt);

    bool listProperties(const ConfigPropertyRegistry &validProps,
                        const string &opt);

    /**
     * list all known data sources of a certain type
     */
    void listSources(EvolutionSyncSource &syncSource, const string &header);

    void dumpServers(const string &preamble,
                     const EvolutionSyncConfig::ServerList &servers);

    void dumpProperties(const ConfigNode &configuredProps,
                        const ConfigPropertyRegistry &allProps);

    void copyProperties(const ConfigNode &fromProps,
                        ConfigNode &toProps,
                        bool hidden,
                        const ConfigPropertyRegistry &allProps);

    void dumpComment(ostream &stream,
                     const string &prefix,
                     const string &comment);

    /** print usage information */
    void usage(bool full,
               const string &error = string(""),
               const string &param = string(""));

    friend class SyncEvolutionCmdlineTest;
};

#endif // INCL_SYNC_EVOLUTION_CMDLINE
