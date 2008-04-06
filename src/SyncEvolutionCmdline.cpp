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

#include "SyncEvolutionCmdline.h"
#include "FilterConfigNode.h"
#include "VolatileConfigNode.h"
#include "EvolutionSyncSource.h"
#include "EvolutionSyncClient.h"
#include "SyncEvolutionUtil.h"

#include <unistd.h>
#include <errno.h>

#include <iostream>
#include <sstream>
#include <memory>
#include <set>
#include <algorithm>
using namespace std;

#include <boost/shared_ptr.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>

SyncEvolutionCmdline::SyncEvolutionCmdline(int argc, const char * const * argv, ostream &out, ostream &err) :
    m_argc(argc),
    m_argv(argv),
    m_out(out),
    m_err(err),
    m_validSyncProps(EvolutionSyncConfig::getRegistry()),
    m_validSourceProps(EvolutionSyncSourceConfig::getRegistry())
{}

bool SyncEvolutionCmdline::parse()
{
    int opt = 1;
    while (opt < m_argc) {
        if (m_argv[opt][0] != '-') {
            break;
        }
        if (boost::iequals(m_argv[opt], "--sync") ||
            boost::iequals(m_argv[opt], "-s")) {
            opt++;
            string param;
            string cmdopt(m_argv[opt - 1]);
            if (!parseProp(m_validSourceProps, m_sourceProps,
                           m_argv[opt - 1], opt == m_argc ? NULL : m_argv[opt],
                           "sync")) {
                return false;
            }
        } else if(boost::iequals(m_argv[opt], "--sync-property") ||
                  boost::iequals(m_argv[opt], "-y")) {
                opt++;
                if (!parseProp(m_validSyncProps, m_syncProps,
                               m_argv[opt - 1], opt == m_argc ? NULL : m_argv[opt])) {
                    return false;
                }
        } else if(boost::iequals(m_argv[opt], "--source-property") ||
                  boost::iequals(m_argv[opt], "-z")) {
            opt++;
            if (!parseProp(m_validSourceProps, m_sourceProps,
                           m_argv[opt - 1], opt == m_argc ? NULL : m_argv[opt])) {
                return false;
            }
        } else if(boost::iequals(m_argv[opt], "--properties") ||
                  boost::iequals(m_argv[opt], "-r")) {
            opt++;
            /* TODO */
            m_err << "ERROR: not implemented yet: " << m_argv[opt - 1] << endl;
            return false;
        } else if(boost::iequals(m_argv[opt], "--template") ||
                  boost::iequals(m_argv[opt], "-l")) {
            opt++;
            if (opt >= m_argc) {
                usage(true, string("missing parameter for ") + cmdOpt(m_argv[opt - 1]));
                return false;
            }
            m_template = m_argv[opt];
            m_configure = true;
            if (boost::trim_copy(m_template) == "?") {
                dumpServers("Available configuration templates:",
                            EvolutionSyncConfig::getServerTemplates());
                m_dontrun = true;
            }
        } else if(boost::iequals(m_argv[opt], "--print-servers")) {
            m_printServers = true;
        } else if(boost::iequals(m_argv[opt], "--print-config") ||
                  boost::iequals(m_argv[opt], "-p")) {
            m_printConfig = true;
        } else if(boost::iequals(m_argv[opt], "--configure") ||
                  boost::iequals(m_argv[opt], "-c")) {
            m_configure = true;
        } else if(boost::iequals(m_argv[opt], "--migrate")) {
            m_migrate = true;
        } else if(boost::iequals(m_argv[opt], "--status") ||
                  boost::iequals(m_argv[opt], "-t")) {
            m_status = true;
        } else if(boost::iequals(m_argv[opt], "--quiet") ||
                  boost::iequals(m_argv[opt], "-q")) {
            m_quiet = true;
        } else if(boost::iequals(m_argv[opt], "--help") ||
                  boost::iequals(m_argv[opt], "-h")) {
            m_usage = true;
        } else if(boost::iequals(m_argv[opt], "--version")) {
            m_version = true;
        } else {
            usage(false, string(m_argv[opt]) + ": unknown parameter");
            return false;
        }
        opt++;
    }

    if (opt < m_argc) {
        m_server = m_argv[opt++];
        while (opt < m_argc) {
            m_sources.insert(m_argv[opt++]);
        }
    }

    return true;
}

bool SyncEvolutionCmdline::run() {
    if (m_usage) {
        usage(true);
    } else if (m_version) {
        printf("SyncEvolution %s\n", VERSION);
    } else if (m_printServers || boost::trim_copy(m_server) == "?") {
        dumpServers("Configured servers:",
                    EvolutionSyncConfig::getServers());
    } else if (m_dontrun) {
        // user asked for information
    } else if (m_argc == 1) {
        const SourceRegistry &registry(EvolutionSyncSource::getSourceRegistry());
        boost::shared_ptr<FilterConfigNode> configNode(new VolatileConfigNode());
        boost::shared_ptr<FilterConfigNode> hiddenNode(new VolatileConfigNode());
        boost::shared_ptr<FilterConfigNode> trackingNode(new VolatileConfigNode());
        SyncSourceNodes nodes(configNode, hiddenNode, trackingNode);
        EvolutionSyncSourceParams params("list", nodes, "");
        
        for (SourceRegistry::const_iterator source = registry.begin();
             source != registry.end();
             ++source) {
            for (Values::const_iterator alias = (*source)->m_typeValues.begin();
                 alias != (*source)->m_typeValues.end();
                 ++alias) {
                if (!alias->empty() && (*source)->m_enabled) {
                    configNode->setProperty("type", *alias->begin());
                    auto_ptr<EvolutionSyncSource> source(EvolutionSyncSource::createSource(params, false));
                    if (source.get() != NULL) {
                        listSources(*source, join(" = ", alias->begin(), alias->end()));
                        m_out << "\n";
                    }
                }
            }
        }

        usage(false);
    } else if (m_printConfig) {
        boost::shared_ptr<EvolutionSyncConfig> config;

        if (m_template.empty()) {
            if (m_server.empty()) {
                m_err << "ERROR: --print-config requires either a --template or a server name." << endl;
                return false;
            }
            config.reset(new EvolutionSyncConfig(m_server));
            if (!config->exists()) {
                m_err << "ERROR: server '" << m_server << "' has not been configured yet." << endl;
                return false;
            }
        } else {
            config = EvolutionSyncConfig::createServerTemplate(m_template);
            if (!config.get()) {
                m_err << "ERROR: no configuration template for '" << m_template << "' available." << endl;
                return false;
            }
        }

        if (m_sources.empty()) {
            boost::shared_ptr<FilterConfigNode> syncProps(config->getProperties());
            syncProps->setFilter(m_syncProps);
            dumpProperties(*syncProps, config->getRegistry());
        }

        list<string> sourceList = config->getSyncSources();
        vector<string> sources;
        append(sources, sourceList);
        sort(sources.begin(), sources.end());
        for (vector<string>::const_iterator it = sources.begin();
             it != sources.end();
             ++it) {
            if (m_sources.empty() ||
                m_sources.find(*it) != m_sources.end()) {
                m_out << endl << "[" << *it << "]" << endl;
                ConstSyncSourceNodes nodes = config->getSyncSourceNodes(*it);
                boost::shared_ptr<FilterConfigNode> sourceProps(new FilterConfigNode(boost::shared_ptr<const ConfigNode>(nodes.m_configNode)));
                sourceProps->setFilter(m_sourceProps);
                dumpProperties(*sourceProps, EvolutionSyncSourceConfig::getRegistry());
            }
        }
    } else if (m_server == "" && m_argc > 1) {
        // Options given, but no server - not sure what the user wanted?!
        usage(true, "server name missing");
        return false;
    } else if (m_configure || m_migrate) {
        bool fromScratch = false;

        // Both config changes and migration are implemented as copying from
        // another config (template resp. old one). Migration also moves
        // the old config.
        boost::shared_ptr<EvolutionSyncConfig> from;
        if (m_migrate) {
            from.reset(new EvolutionSyncConfig(m_server));
            if (!from->exists()) {
                m_err << "ERROR: server '" << m_server << "' has not been configured yet." << endl;
                return false;
            }

            int counter = 0;
            string oldRoot = from->getRootPath();
            string suffix;
            while (true) {
                string newname;
                ostringstream newsuffix;
                newsuffix << ".old";
                if (counter) {
                    newsuffix << "." << counter;
                }
                suffix = newsuffix.str();
                newname = oldRoot + suffix;
                if (!rename(oldRoot.c_str(),
                            newname.c_str())) {
                    break;
                } else if (errno != EEXIST && errno != ENOTEMPTY) {
                    m_err << "ERROR: renaming " << oldRoot << " to " <<
                        newname << ": " << strerror(errno) << endl;
                    return false;
                }
                counter++;
            }

            from.reset(new EvolutionSyncConfig(m_server + suffix));
        } else {
            from.reset(new EvolutionSyncConfig(m_server));
            if (!from->exists()) {
                // creating from scratch, look for template
                fromScratch = true;
                string configTemplate = m_template.empty() ? m_server : m_template;
                from = EvolutionSyncConfig::createServerTemplate(configTemplate);
                if (!from.get()) {
                    m_err << "ERROR: no configuration template for '" << configTemplate << "' available." << endl;
                    dumpServers("Available configuration templates:",
                                EvolutionSyncConfig::getServerTemplates());
                    return false;
                }
            }
        }

        // apply config changes on-the-fly
        from->setConfigFilter(true, m_syncProps);
        from->setConfigFilter(false, m_sourceProps);

        // write into the requested configuration, creating it if necessary
        boost::shared_ptr<EvolutionSyncConfig> to(new EvolutionSyncConfig(m_server));
        to->copy(*from, !fromScratch && !m_sources.empty() ? &m_sources : NULL);

        // activate only the selected sources
        if (fromScratch && !m_sources.empty()) {
            list<string> configuredSources = to->getSyncSources();
            BOOST_FOREACH(const string &source, configuredSources) {
                boost::shared_ptr<PersistentEvolutionSyncSourceConfig> sourceConfig(to->getSyncSourceConfig(source));
                sourceConfig->setSync(m_sources.find(source) != m_sources.end() ? "two-way" : "disabled");
            }
        }

        // done, now write it
        to->flush();
    } else {
        EvolutionSyncClient client(m_server, true, m_sources);
        client.setQuiet(m_quiet);
        client.setConfigFilter(true, m_syncProps);
        client.setConfigFilter(false, m_sourceProps);
        if (m_status) {
            client.status();
        } else {
            return client.sync() == 0;
        }
    }

    return true;
}

string SyncEvolutionCmdline::cmdOpt(const char *opt, const char *param)
{
    string res = "'";
    res += opt;
    if (param) {
        res += " ";
        res += param;
    }
    res += "'";
    return res;
}

bool SyncEvolutionCmdline::parseProp(const ConfigPropertyRegistry &validProps,
                                     FilterConfigNode::ConfigFilter &props,
                                     const char *opt,
                                     const char *param,
                                     const char *propname)
{
    if (!param) {
        usage(true, string("missing parameter for ") + cmdOpt(opt, param));
        return false;
    } else if (boost::trim_copy(string(param)) == "?") {
        m_dontrun = true;
        if (propname) {
            return listPropValues(validProps, propname, opt);
        } else {
            return listProperties(validProps, opt);
        }
    } else {
        string propstr;
        string paramstr;
        if (propname) {
            propstr = propname;
            paramstr = param;
        } else {
            const char *equal = strchr(param, '=');
            if (!equal) {
                usage(true, string("the '=<value>' part is missing in: ") + cmdOpt(opt, param));
                return false;
            }
            propstr.assign(param, equal - param);
            paramstr.assign(equal + 1);
        }

        boost::trim(propstr);
        boost::trim_left(paramstr);

        if (boost::trim_copy(paramstr) == "?") {
            m_dontrun = true;
            return listPropValues(validProps, propstr, cmdOpt(opt, param));
        } else {
            const ConfigProperty *prop = validProps.find(propstr);
            if (!prop) {
                m_err << "ERROR: " << cmdOpt(opt, param) << ": no such property" << endl;
                return false;
            } else {
                string error;
                if (!prop->checkValue(paramstr, error)) {
                    m_err << "ERROR: " << cmdOpt(opt, param) << ": " << error << endl;
                    return false;
                } else {
                    props.set(propstr, paramstr);
                    return true;                        
                }
            }
        }
    }
}

bool SyncEvolutionCmdline::listPropValues(const ConfigPropertyRegistry &validProps,
                                          const string &propName,
                                          const string &opt)
{
    const ConfigProperty *prop = validProps.find(propName);
    if (!prop) {
        m_err << "ERROR: "<< opt << ": no such property" << endl;
        return false;
    } else {
        m_out << opt << endl;
        string comment = prop->getComment();

        if (comment != "") {
            list<string> commentLines;
            ConfigProperty::splitComment(comment, commentLines);
            for (list<string>::const_iterator line = commentLines.begin();
                 line != commentLines.end();
                 ++line) {
                m_out << "   " << *line << endl;
            }
        } else {
            m_out << "   no documentation available" << endl;
        }
        return true;
    }
}

bool SyncEvolutionCmdline::listProperties(const ConfigPropertyRegistry &validProps,
                                          const string &opt)
{
    // The first of several related properties has a comment.
    // Remember that comment and print it as late as possible,
    // that way related properties preceed their comment.
    string comment;
    for (ConfigPropertyRegistry::const_iterator prop = validProps.begin();
         prop != validProps.end();
         ++prop) {
        if (!(*prop)->isHidden()) {
            string newComment = (*prop)->getComment();

            if (newComment != "") {
                if (!comment.empty()) {
                    dumpComment(m_out, "   ", comment);
                    m_out << endl;
                }
                comment = newComment;
            }
            m_out << (*prop)->getName() << ":" << endl;
        }
    }
    dumpComment(m_out, "   ", comment);
    return true;
}

void SyncEvolutionCmdline::listSources(EvolutionSyncSource &syncSource, const string &header)
{
    m_out << header << ":\n";
    EvolutionSyncSource::sources sources = syncSource.getSyncBackends();

    for (EvolutionSyncSource::sources::const_iterator it = sources.begin();
         it != sources.end();
         it++) {
        m_out << "   " << it->m_name << " (" << it->m_uri << ")";
        if (it->m_isDefault) {
            m_out << " <default>";
        }
        m_out << endl;
    }
}

void SyncEvolutionCmdline::dumpServers(const string &preamble,
                                       const EvolutionSyncConfig::ServerList &servers)
{
    m_out << preamble << endl;
    for (EvolutionSyncConfig::ServerList::const_iterator it = servers.begin();
         it != servers.end();
         ++it) {
        m_out << "   "  << it->first << " = " << it->second << endl;
    }
    if (!servers.size()) {
        m_out << "   none" << endl;
    }
}

void SyncEvolutionCmdline::dumpProperties(const ConfigNode &configuredProps,
                                          const ConfigPropertyRegistry &allProps)
{
    for (ConfigPropertyRegistry::const_iterator it = allProps.begin();
         it != allProps.end();
         ++it) {
        if ((*it)->isHidden()) {
            continue;
        }
        if (!m_quiet) {
            string comment = (*it)->getComment();
            if (!comment.empty()) {
                m_out << endl;
                dumpComment(m_out, "# ", comment);
            }
        }
        bool isDefault;
        (*it)->getProperty(configuredProps, &isDefault);
        if (isDefault) {
            m_out << "# ";
        }
        m_out << (*it)->getName() << " = " << (*it)->getProperty(configuredProps) << endl;
    }
}

void SyncEvolutionCmdline::dumpComment(ostream &stream,
                                       const string &prefix,
                                       const string &comment)
{
    list<string> commentLines;
    ConfigProperty::splitComment(comment, commentLines);
    for (list<string>::const_iterator line = commentLines.begin();
         line != commentLines.end();
         ++line) {
        stream << prefix << *line << endl;
    }
}

void SyncEvolutionCmdline::usage(bool full, const string &error, const string &param)
{
    ostream &out(error.empty() ? m_out : m_err);

    out << m_argv[0] << endl;
    out << m_argv[0] << " [<options>] <server> [<source> ...]" << endl;
    out << m_argv[0] << " --help|-h" << endl;
    out << m_argv[0] << " --version" << endl;
    if (full) {
        out << endl <<
            "Options:" << endl <<
            "  --sync|-s <mode>" << endl <<
            "    Temporarily synchronize the active sources in that mode. Useful" << endl <<
            "    for a 'refresh-from-server' or 'refresh-from-client' sync which" << endl <<
            "    clears all data at one end and copies all items from the other." << endl <<
            "  " << endl <<
            "  --status|-t" << endl <<
            "    The changes made to local data since the last synchronization are" << endl <<
            "    shown without starting a new one. This can be used to see in advance" << endl <<
            "    whether the local data needs to be synchronized with the server." << endl <<
            "  " << endl <<
            "  --quiet|-q" << endl <<
            "    Suppresses most of the normal output during a synchronization. The" << endl <<
            "    log file still contains all the information." << endl <<
            "  " << endl <<
            "  --help|-h" << endl <<
            "    Prints usage information." << endl <<
            "  " << endl <<
            "  --version" << endl <<
            "    Prints the SyncEvolution version." << endl;
    }

    if (error != "") {
        out << endl << "ERROR: " << error << endl;
    }
    if (param != "") {
        out << "INFO: use '" << param << (param[param.size() - 1] == '=' ? "" : " ") <<
            "?' to get a list of valid parameters" << endl;
    }
}

#ifdef ENABLE_INTEGRATION_TESTS

/** simple line-by-line diff */
static string diffStrings(const string &lhs, const string &rhs)
{
    ostringstream res;

    typedef boost::split_iterator<string::const_iterator> string_split_iterator;
    string_split_iterator lit =
        boost::make_split_iterator(lhs, boost::first_finder("\n", boost::is_iequal()));
    string_split_iterator rit =
        boost::make_split_iterator(rhs, boost::first_finder("\n", boost::is_iequal()));
    while (lit != string_split_iterator() &&
           rit != string_split_iterator()) {
        if (*lit != *rit) {
            res << "< " << *lit << endl;
            res << "> " << *rit << endl;
        }
        ++lit;
        ++rit;
    }

    while (lit != string_split_iterator()) {
        res << "< " << *lit << endl;
        ++lit;
    }

    while (rit != string_split_iterator()) {
        res << "> " << *rit << endl;
        ++rit;
    }

    return res.str();
}

# define CPPUNIT_ASSERT_EQUAL_DIFF( expected, actual )      \
    do { \
        string expected_ = (expected);                                  \
        string actual_ = (actual);                                      \
        if (expected_ != actual_) {                                     \
            CPPUNIT_NS::Message cpputMsg_(string("expected:\n") +       \
                                          expected_);                   \
            cpputMsg_.addDetail(string("actual:\n") +                   \
                                actual_);                               \
            cpputMsg_.addDetail(string("diff:\n") +                     \
                                diffStrings(expected_, actual_));       \
            CPPUNIT_NS::Asserter::fail( cpputMsg_,                      \
                                        CPPUNIT_SOURCELINE() );         \
        } \
    } while ( false )

// returns last line, including trailing line break, empty if input is empty
static string lastLine(const string &buffer)
{
    if (buffer.size() < 2) {
        return buffer;
    }

    size_t line = buffer.rfind("\n", buffer.size() - 2);
    if (line == buffer.npos) {
        return buffer;
    }

    return buffer.substr(line + 1);
}

// true if <word> =
static bool isPropAssignment(const string &buffer) {
    size_t start = 0;
    while (start < buffer.size() &&
           !isspace(buffer[start])) {
        start++;
    }
    if (start + 3 <= buffer.size() &&
        buffer.substr(start, 3) == " = ") {
        return true;
    } else {
        return false;
    }
}

// remove pure comment lines from buffer,
// also empty lines
static string filterConfig(const string &buffer)
{
    ostringstream res;

    typedef boost::split_iterator<string::const_iterator> string_split_iterator;
    for (string_split_iterator it =
             boost::make_split_iterator(buffer, boost::first_finder("\n", boost::is_iequal()));
         it != string_split_iterator();
         ++it) {
        string line = boost::copy_range<string>(*it);
        if (!line.empty() &&
            (!boost::starts_with(line, "# ") ||
             isPropAssignment(line.substr(2)))) {
            res << line << endl;
        }
    }

    return res.str();
}

// remove lines indented with spaces
static string filterIndented(const string &buffer)
{
    ostringstream res;
    bool first = true;

    typedef boost::split_iterator<string::const_iterator> string_split_iterator;
    for (string_split_iterator it =
             boost::make_split_iterator(buffer, boost::first_finder("\n", boost::is_iequal()));
         it != string_split_iterator();
         ++it) {
        if (!boost::starts_with(*it, " ")) {
            if (!first) {
                res << endl;
            } else {
                first = false;
            }
            res << *it;
        }
    }

    return res.str();
}

// convert the internal config dump to .ini style
static string internalToIni(const string &config)
{
    ostringstream res;

    string section;
    typedef boost::split_iterator<string::const_iterator> string_split_iterator;
    for (string_split_iterator it =
             boost::make_split_iterator(config, boost::first_finder("\n", boost::is_iequal()));
         it != string_split_iterator();
         ++it) {
        string line(it->begin(), it->end());
        if (line.empty()) {
            continue;
        }

        size_t colon = line.find(':');
        string prefix = line.substr(0, colon);
        if (boost::contains(prefix, ".internal.ini") ||
            boost::contains(line, "= internal value")) {
            continue;
        }
        // sources/<name>/config.ini or
        // spds/sources/<name>/config.ini
        size_t endslash = prefix.rfind('/');
        if (endslash != line.npos && endslash > 1) {
            size_t slash = prefix.rfind('/', endslash - 1);
            if (slash != line.npos) {
                string newsource = prefix.substr(slash + 1, endslash - slash - 1);
                if (newsource != section &&
                    newsource != "syncml") {
                    res << endl << "[" << newsource << "]" << endl;
                    section = newsource;
                }
            }
        }
        string assignment = line.substr(colon + 1);
        // substitude aliases with generic values
        boost::replace_first(assignment, "= F", "= 0");
        boost::replace_first(assignment, "= T", "= 1");
        boost::replace_first(assignment, "= md5", "= syncml:auth-md5");
        res << assignment << endl;
    }

    return res.str();
}


/**
 * Testing is based on a text representation of a directory
 * hierarchy where each line is of the format
 * <file path>:<line in file>
 *
 * The order of files is alphabetical, of lines in the file as
 * in the file. Lines in the file without line break cannot
 * be represented.
 *
 * The root of the hierarchy is not part of the representation
 * itself.
 */
class SyncEvolutionCmdlineTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(SyncEvolutionCmdlineTest);
    CPPUNIT_TEST(testFramework);
    CPPUNIT_TEST(testSetupScheduleWorld);
    CPPUNIT_TEST(testSetupDefault);
    CPPUNIT_TEST(testSetupRenamed);
    CPPUNIT_TEST(testSetupFunambol);
    CPPUNIT_TEST(testSetupSynthesis);
    CPPUNIT_TEST(testPrintServers);
    CPPUNIT_TEST(testPrintConfig);
    CPPUNIT_TEST(testTemplate);
    CPPUNIT_TEST(testSync);
    CPPUNIT_TEST(testConfigure);
    CPPUNIT_TEST(testOldConfigure);
    CPPUNIT_TEST(testListSources);
    CPPUNIT_TEST(testMigrate);
    CPPUNIT_TEST_SUITE_END();
    
public:
    SyncEvolutionCmdlineTest() :
        m_testDir("SyncEvolutionCmdlineTest"),
        m_scheduleWorldConfig(".internal.ini:# serverNonce = \n"
                              ".internal.ini:# clientNonce = \n"
                              ".internal.ini:# devInfoHash = \n"
                              "config.ini:syncURL = http://sync.scheduleworld.com\n"
                              "config.ini:username = your SyncML server account name\n"
                              "config.ini:password = your SyncML server password\n"
                              "config.ini:# logdir = \n"
                              "config.ini:# loglevel = 0\n"
                              "config.ini:# maxlogdirs = 0\n"
                              "config.ini:# useProxy = 0\n"
                              "config.ini:# proxyHost = \n"
                              "config.ini:# proxyUsername = \n"
                              "config.ini:# proxyPassword = \n"
                              "config.ini:# clientAuthType = syncml:auth-md5\n"
                              "config.ini:deviceId = fixed-devid\n" /* this is not the default! */
                              "config.ini:# maxMsgSize = 8192\n"
                              "config.ini:# maxObjSize = 500000\n"
                              "config.ini:# loSupport = 1\n"
                              "config.ini:# enableCompression = 0\n"
                              "sources/addressbook/.internal.ini:# last = 0\n"
                              "sources/addressbook/config.ini:sync = two-way\n"
                              "sources/addressbook/config.ini:type = addressbook\n"
                              "sources/addressbook/config.ini:# evolutionsource = \n"
                              "sources/addressbook/config.ini:uri = card3\n"
                              "sources/addressbook/config.ini:# evolutionuser = \n"
                              "sources/addressbook/config.ini:# evolutionpassword = \n"
                              "sources/addressbook/config.ini:# encoding = \n"
                              "sources/calendar/.internal.ini:# last = 0\n"
                              "sources/calendar/config.ini:sync = two-way\n"
                              "sources/calendar/config.ini:type = calendar\n"
                              "sources/calendar/config.ini:# evolutionsource = \n"
                              "sources/calendar/config.ini:uri = event2\n"
                              "sources/calendar/config.ini:# evolutionuser = \n"
                              "sources/calendar/config.ini:# evolutionpassword = \n"
                              "sources/calendar/config.ini:# encoding = \n"
                              "sources/memo/.internal.ini:# last = 0\n"
                              "sources/memo/config.ini:sync = two-way\n"
                              "sources/memo/config.ini:type = memo\n"
                              "sources/memo/config.ini:# evolutionsource = \n"
                              "sources/memo/config.ini:uri = note\n"
                              "sources/memo/config.ini:# evolutionuser = \n"
                              "sources/memo/config.ini:# evolutionpassword = \n"
                              "sources/memo/config.ini:# encoding = \n"
                              "sources/todo/.internal.ini:# last = 0\n"
                              "sources/todo/config.ini:sync = two-way\n"
                              "sources/todo/config.ini:type = todo\n"
                              "sources/todo/config.ini:# evolutionsource = \n"
                              "sources/todo/config.ini:uri = task2\n"
                              "sources/todo/config.ini:# evolutionuser = \n"
                              "sources/todo/config.ini:# evolutionpassword = \n"
                              "sources/todo/config.ini:# encoding = \n")
    {}

protected:

    /** verify that createFiles/scanFiles themselves work */
    void testFramework() {
        const string root(m_testDir);
        const string content("baz:line\n"
                             "caz/subdir:booh\n"
                             "caz/subdir2/sub:# comment\n"
                             "caz/subdir2/sub:# foo = bar\n"
                             "caz/subdir2/sub:# empty = \n"
                             "caz/subdir2/sub:# another comment\n"
                             "foo:bar1\n"
                             "foo:\n"
                             "foo: \n"
                             "foo:bar2\n");
        const string filtered("baz:line\n"
                              "caz/subdir:booh\n"
                              "caz/subdir2/sub:# foo = bar\n"
                              "caz/subdir2/sub:# empty = \n"
                              "foo:bar1\n"
                              "foo: \n"
                              "foo:bar2\n");
        createFiles(root, content);
        string res = scanFiles(root);
        CPPUNIT_ASSERT_EQUAL_DIFF(filtered, res);
    }

    void removeRandomUUID(string &buffer) {
        string uuidstr = "deviceId = uuid-";
        size_t uuid = buffer.find(uuidstr);
        CPPUNIT_ASSERT(uuid != buffer.npos);
        size_t end = buffer.find("\n", uuid + uuidstr.size());
        CPPUNIT_ASSERT(end != buffer.npos);
        buffer.replace(uuid, end - uuid, "deviceId = fixed-devid");
    }

    /** create new configurations */
    void testSetupScheduleWorld() {
        string root;
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        root = m_testDir;
        root += "/syncevolution/scheduleworld";

        {
            rm_r(root);
            TestCmdline cmdline("--configure",
                                "--sync-property", "proxyHost = proxy",
                                "scheduleworld",
                                "addressbook",
                                NULL);
            cmdline.doit();
            string res = scanFiles(root);
            removeRandomUUID(res);
            string expected = m_scheduleWorldConfig;
            boost::replace_first(expected,
                                 "# proxyHost = ",
                                 "proxyHost = proxy");
            boost::replace_all(expected,
                               "sync = two-way",
                               "sync = disabled");
            boost::replace_first(expected,
                                 "addressbook/config.ini:sync = disabled",
                                 "addressbook/config.ini:sync = two-way");
            CPPUNIT_ASSERT_EQUAL_DIFF(expected, res);
        }

        {
            rm_r(root);
            TestCmdline cmdline("--configure",
                                "--sync-property", "deviceID = fixed-devid",
                                "scheduleworld",
                                NULL);
            cmdline.doit();
            string res = scanFiles(root);
            CPPUNIT_ASSERT_EQUAL_DIFF(string(m_scheduleWorldConfig), res);
        }
    }

    void testSetupDefault() {
        string root;
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        root = m_testDir;
        root += "/syncevolution/some-other-server";
        rm_r(root);
        TestCmdline cmdline("--configure",
                            "--template", "default",
                            "--sync-property", "deviceID = fixed-devid",
                            "some-other-server",
                            NULL);
        cmdline.doit();
        string res = scanFiles(root);
        CPPUNIT_ASSERT_EQUAL_DIFF(string(m_scheduleWorldConfig), res);
    }
    void testSetupRenamed() {
        string root;
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        root = m_testDir;
        root += "/syncevolution/scheduleworld2";
        rm_r(root);
        TestCmdline cmdline("--configure",
                            "--template", "scheduleworld",
                            "--sync-property", "deviceID = fixed-devid",
                            "scheduleworld2",
                            NULL);
        cmdline.doit();
        string res = scanFiles(root);
        CPPUNIT_ASSERT_EQUAL_DIFF(string(m_scheduleWorldConfig), res);
    }
    void testSetupFunambol() {
        string root;
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        root = m_testDir;
        root += "/syncevolution/funambol";
        rm_r(root);
        TestCmdline cmdline("--configure",
                            "--sync-property", "deviceID = fixed-devid",
                            "funambol",
                            NULL);
        cmdline.doit();
        string res = scanFiles(root);
        CPPUNIT_ASSERT_EQUAL_DIFF(FunambolConfig(), res);
    }

    void testSetupSynthesis() {
        string root;
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        root = m_testDir;
        root += "/syncevolution/synthesis";
        rm_r(root);
        TestCmdline cmdline("--configure",
                            "--sync-property", "deviceID = fixed-devid",
                            "synthesis",
                            NULL);
        cmdline.doit();
        string res = scanFiles(root);
        CPPUNIT_ASSERT_EQUAL_DIFF(SynthesisConfig(), res);
    }

    void testTemplate() {
        TestCmdline failure("--template", NULL);
        CPPUNIT_ASSERT(!failure.m_cmdline->parse());
        CPPUNIT_ASSERT_EQUAL_DIFF("", failure.m_out.str());
        CPPUNIT_ASSERT_EQUAL(string("ERROR: missing parameter for '--template'\n"), lastLine(failure.m_err.str()));

        TestCmdline help("--template", "? ", NULL);
        help.doit();
        CPPUNIT_ASSERT_EQUAL_DIFF("Available configuration templates:\n"
                                  "   funambol = http://my.funambol.com\n"
                                  "   scheduleworld = http://sync.scheduleworld.com\n"
                                  "   synthesis = http://www.synthesis.ch\n",
                                  help.m_out.str());
        CPPUNIT_ASSERT_EQUAL_DIFF("", help.m_err.str());
    }

    void testPrintServers() {
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        rm_r(m_testDir);
        testSetupScheduleWorld();
        testSetupSynthesis();
        testSetupFunambol();

        TestCmdline cmdline("--print-servers", NULL);
        cmdline.doit();
        CPPUNIT_ASSERT_EQUAL_DIFF("Configured servers:\n"
                                  "   scheduleworld = SyncEvolutionCmdlineTest/syncevolution/scheduleworld\n"
                                  "   synthesis = SyncEvolutionCmdlineTest/syncevolution/synthesis\n"
                                  "   funambol = SyncEvolutionCmdlineTest/syncevolution/funambol\n",
                                  cmdline.m_out.str());
        CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
    }

    void testPrintConfig() {
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        rm_r(m_testDir);
        testSetupFunambol();

        {
            TestCmdline failure("--print-config", NULL);
            CPPUNIT_ASSERT(failure.m_cmdline->parse());
            CPPUNIT_ASSERT(!failure.m_cmdline->run());
            CPPUNIT_ASSERT_EQUAL_DIFF("", failure.m_out.str());
            CPPUNIT_ASSERT_EQUAL(string("ERROR: --print-config requires either a --template or a server name.\n"),
                                 lastLine(failure.m_err.str()));
        }

        {
            TestCmdline failure("--print-config", "foo", NULL);
            CPPUNIT_ASSERT(failure.m_cmdline->parse());
            CPPUNIT_ASSERT(!failure.m_cmdline->run());
            CPPUNIT_ASSERT_EQUAL_DIFF("", failure.m_out.str());
            CPPUNIT_ASSERT_EQUAL(string("ERROR: server 'foo' has not been configured yet.\n"),
                                 lastLine(failure.m_err.str()));
        }

        {
            TestCmdline failure("--print-config", "--template", "foo", NULL);
            CPPUNIT_ASSERT(failure.m_cmdline->parse());
            CPPUNIT_ASSERT(!failure.m_cmdline->run());
            CPPUNIT_ASSERT_EQUAL_DIFF("", failure.m_out.str());
            CPPUNIT_ASSERT_EQUAL(string("ERROR: no configuration template for 'foo' available.\n"),
                                 lastLine(failure.m_err.str()));
        }

        {
            TestCmdline cmdline("--print-config", "--template", "scheduleworld", NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            string actual = cmdline.m_out.str();
            removeRandomUUID(actual);
            string filtered = filterConfig(actual);
            CPPUNIT_ASSERT_EQUAL_DIFF(filterConfig(internalToIni(m_scheduleWorldConfig)),
                                      filtered);
            // there should have been comments
            CPPUNIT_ASSERT(actual.size() > filtered.size());
        }

        {
            TestCmdline cmdline("--print-config", "--template", "default", NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            string actual = filterConfig(cmdline.m_out.str());
            removeRandomUUID(actual);
            CPPUNIT_ASSERT_EQUAL_DIFF(filterConfig(internalToIni(m_scheduleWorldConfig)),
                                      actual);
        }

        {
            TestCmdline cmdline("--print-config", "funambol", NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF(filterConfig(internalToIni(FunambolConfig())),
                                      filterConfig(cmdline.m_out.str()));
        }

        {
            TestCmdline cmdline("--print-config", "--template", "scheduleworld",
                                "--sync-property", "syncURL=foo",
                                "--source-property", "sync=disabled",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            string expected = filterConfig(internalToIni(m_scheduleWorldConfig));
            boost::replace_first(expected,
                                 "syncURL = http://sync.scheduleworld.com",
                                 "syncURL = foo");
            boost::replace_all(expected,
                               "sync = two-way",
                               "sync = disabled");
            string actual = filterConfig(cmdline.m_out.str());
            removeRandomUUID(actual);
            CPPUNIT_ASSERT_EQUAL_DIFF(expected,
                                      actual);
        }

        {
            TestCmdline cmdline("--print-config", "--quiet",
                                "--template", "scheduleworld",
                                "funambol",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            string actual = cmdline.m_out.str();
            removeRandomUUID(actual);
            CPPUNIT_ASSERT_EQUAL_DIFF(internalToIni(m_scheduleWorldConfig),
                                      actual);
        }
        
    }

    void testSync() {
        TestCmdline failure("--sync", NULL);
        CPPUNIT_ASSERT(!failure.m_cmdline->parse());
        CPPUNIT_ASSERT_EQUAL_DIFF("", failure.m_out.str());
        CPPUNIT_ASSERT_EQUAL(string("ERROR: missing parameter for '--sync'\n"), lastLine(failure.m_err.str()));

        TestCmdline failure2("--sync", "foo", NULL);
        CPPUNIT_ASSERT(!failure2.m_cmdline->parse());
        CPPUNIT_ASSERT_EQUAL_DIFF("", failure2.m_out.str());
        CPPUNIT_ASSERT_EQUAL(string("ERROR: '--sync foo': not one of the valid values (two-way, slow, refresh-from-client = refresh-client, refresh-from-server = refresh-server = refresh, one-way-from-client = one-way-client, one-way-from-server = one-way-server = one-way, disabled = none)\n"), lastLine(failure2.m_err.str()));

        TestCmdline help("--sync", " ?", NULL);
        help.doit();
        CPPUNIT_ASSERT_EQUAL_DIFF("--sync\n"
                                  "   requests a certain synchronization mode:\n"
                                  "     two-way             = only send/receive changes since last sync\n"
                                  "     slow                = exchange all items\n"
                                  "     refresh-from-client = discard all remote items and replace with\n"
                                  "                           the items on the client\n"
                                  "     refresh-from-server = discard all local items and replace with\n"
                                  "                           the items on the server\n"
                                  "     one-way-from-client = transmit changes from client\n"
                                  "     one-way-from-server = transmit changes from server\n"
                                  "     none (or disabled)  = synchronization disabled\n",
                                  help.m_out.str());
        CPPUNIT_ASSERT_EQUAL_DIFF("", help.m_err.str());

        TestCmdline filter("--sync", "refresh-from-server", NULL);
        CPPUNIT_ASSERT(filter.m_cmdline->parse());
        CPPUNIT_ASSERT(!filter.m_cmdline->run());
        CPPUNIT_ASSERT_EQUAL_DIFF("", filter.m_out.str());
        CPPUNIT_ASSERT_EQUAL_DIFF("sync = refresh-from-server",
                                  string(filter.m_cmdline->m_sourceProps));
        CPPUNIT_ASSERT_EQUAL_DIFF("",
                                  string(filter.m_cmdline->m_syncProps));

        TestCmdline filter2("--source-property", "sync=refresh", NULL);
        CPPUNIT_ASSERT(filter2.m_cmdline->parse());
        CPPUNIT_ASSERT(!filter2.m_cmdline->run());
        CPPUNIT_ASSERT_EQUAL_DIFF("", filter2.m_out.str());
        CPPUNIT_ASSERT_EQUAL_DIFF("sync = refresh",
                                  string(filter2.m_cmdline->m_sourceProps));
        CPPUNIT_ASSERT_EQUAL_DIFF("",
                                  string(filter2.m_cmdline->m_syncProps));
    }

    void testConfigure() {
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        rm_r(m_testDir);
        testSetupScheduleWorld();
        doConfigure(ScheduleWorldConfig(), "sources/addressbook/config.ini:");

        string syncProperties("syncURL:\n"
                              "\n"
                              "username:\n"
                              "\n"
                              "password:\n"
                              "\n"
                              "logdir:\n"
                              "\n"
                              "loglevel:\n"
                              "\n"
                              "maxlogdirs:\n"
                              "\n"
                              "useProxy:\n"
                              "\n"
                              "proxyHost:\n"
                              "\n"
                              "proxyUsername:\n"
                              "\n"
                              "proxyPassword:\n"
                              "\n"
                              "clientAuthType:\n"
                              "\n"
                              "deviceId:\n"
                              "\n"
                              "maxMsgSize:\n"
                              "maxObjSize:\n"
                              "loSupport:\n"
                              "\n"
                              "enableCompression:\n");
        string sourceProperties("sync:\n"
                                "\n"
                                "type:\n"
                                "\n"
                                "evolutionsource:\n"
                                "\n"
                                "uri:\n"
                                "\n"
                                "evolutionuser:\n"
                                "evolutionpassword:\n"
                                "\n"
                                "encoding:\n");

        {
            TestCmdline cmdline("--sync-property", "?",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF(syncProperties,
                                      filterIndented(cmdline.m_out.str()));
        }

        {
            TestCmdline cmdline("--source-property", "?",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF(sourceProperties,
                                      filterIndented(cmdline.m_out.str()));
        }

        {
            TestCmdline cmdline("--source-property", "?",
                                "--sync-property", "?",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF(sourceProperties + syncProperties,
                                      filterIndented(cmdline.m_out.str()));
        }

        {
            TestCmdline cmdline("--sync-property", "?",
                                "--source-property", "?",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF(syncProperties + sourceProperties,
                                      filterIndented(cmdline.m_out.str()));
        }
    }

    void testOldConfigure() {
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        string oldConfig = OldScheduleWorldConfig();
        InitList<string> props = InitList<string>("serverNonce") +
            "clientNonce" +
            "devInfoHash" +
            "last";
        BOOST_FOREACH(string &prop, props) {
            boost::replace_all(oldConfig,
                               prop + " = ",
                               prop + " = internal value");
        }

        rm_r(m_testDir);
        createFiles(m_testDir + "/.sync4j/evolution/scheduleworld", oldConfig);
        doConfigure(oldConfig, "spds/sources/addressbook/config.txt:");
    }

    void doConfigure(const string &SWConfig, const string &addressbookPrefix) {
        string expected;

        {
            TestCmdline cmdline("--configure",
                                "--source-property", "sync = disabled",
                                "scheduleworld",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_out.str());
            expected = filterConfig(internalToIni(SWConfig));
            boost::replace_all(expected,
                               "sync = two-way",
                               "sync = disabled");
            CPPUNIT_ASSERT_EQUAL_DIFF(expected,
                                      filterConfig(printConfig("scheduleworld")));
        }

        {
            TestCmdline cmdline("--configure",
                                "--source-property", "sync = one-way-from-server",
                                "scheduleworld",
                                "addressbook",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_out.str());
            expected = SWConfig;
            boost::replace_all(expected,
                               "sync = two-way",
                               "sync = disabled");
            boost::replace_first(expected,
                                 addressbookPrefix + "sync = disabled",
                                 addressbookPrefix + "sync = one-way-from-server");
            expected = filterConfig(internalToIni(expected));
            CPPUNIT_ASSERT_EQUAL_DIFF(expected,
                                      filterConfig(printConfig("scheduleworld")));
        }

        {
            TestCmdline cmdline("--configure",
                                "--sync", "two-way",
                                "-z", "evolutionsource=source",
                                "--sync-property", "maxlogdirs=10",
                                "-y", "LOGDIR=logdir",
                                "scheduleworld",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_out.str());
            boost::replace_all(expected,
                               "sync = one-way-from-server",
                               "sync = two-way");
            boost::replace_all(expected,
                               "sync = disabled",
                               "sync = two-way");
            boost::replace_all(expected,
                               "# evolutionsource = ",
                               "evolutionsource = source");
            boost::replace_all(expected,
                               "# maxlogdirs = 0",
                               "maxlogdirs = 10");
            boost::replace_all(expected,
                               "# logdir = ",
                               "logdir = logdir");
            CPPUNIT_ASSERT_EQUAL_DIFF(expected,
                                      filterConfig(printConfig("scheduleworld")));
        }
    }

    void testListSources() {
        TestCmdline cmdline(NULL);
        cmdline.doit();
        CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
        // exact output varies, do not test
    }

    void testMigrate() {
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        rm_r(m_testDir);
        string oldRoot = m_testDir + "/.sync4j/evolution/scheduleworld";
        string newRoot = m_testDir + "/syncevolution/scheduleworld";

        string oldConfig = OldScheduleWorldConfig();

        {
            // migrate old config
            createFiles(oldRoot, oldConfig);
            string createdConfig = scanFiles(oldRoot);
            TestCmdline cmdline("--migrate",
                                "scheduleworld",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_out.str());

            string migratedConfig = scanFiles(newRoot);
            CPPUNIT_ASSERT_EQUAL_DIFF(m_scheduleWorldConfig, migratedConfig);
            string renamedConfig = scanFiles(oldRoot + ".old");
            CPPUNIT_ASSERT_EQUAL_DIFF(createdConfig, renamedConfig);
        }

        {
            // rewrite existing config
            createFiles(newRoot,
                        "config.ini:# obsolete comment\n"
                        "config.ini:obsoleteprop = foo\n",
                        true);
            string createdConfig = scanFiles(newRoot);

            TestCmdline cmdline("--migrate",
                                "scheduleworld",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_out.str());

            string migratedConfig = scanFiles(newRoot);
            CPPUNIT_ASSERT_EQUAL_DIFF(m_scheduleWorldConfig, migratedConfig);
            string renamedConfig = scanFiles(newRoot + ".old");
            CPPUNIT_ASSERT_EQUAL_DIFF(createdConfig, renamedConfig);
        }

        {
            // migrate old config with changes, a second time
            createFiles(oldRoot, oldConfig);
            createFiles(oldRoot,
                        "spds/sources/addressbook/changes/config.txt:foo = bar\n"
                        "spds/sources/addressbook/changes/config.txt:foo2 = bar2\n",
                        true);
            string createdConfig = scanFiles(oldRoot);
            rm_r(newRoot);
            TestCmdline cmdline("--migrate",
                                "scheduleworld",
                                NULL);
            cmdline.doit();
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
            CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_out.str());

            string migratedConfig = scanFiles(newRoot);
            string expected = m_scheduleWorldConfig;
            boost::replace_first(expected,
                                 "sources/addressbook/config.ini",
                                 "sources/addressbook/.other.ini:foo = bar\n"
                                 "sources/addressbook/.other.ini:foo2 = bar2\n"
                                 "sources/addressbook/config.ini");
            CPPUNIT_ASSERT_EQUAL_DIFF(expected, migratedConfig);
            string renamedConfig = scanFiles(oldRoot + ".old.1");
            CPPUNIT_ASSERT_EQUAL_DIFF(createdConfig, renamedConfig);
        }
    }

    const string m_testDir;
    const string m_scheduleWorldConfig;
        

private:

    /**
     * vararg constructor with NULL termination,
     * out and error stream into stringstream members
     */
    class TestCmdline {
    public:
        TestCmdline(const char *arg, ...) {
            va_list argList;
            va_start (argList, arg);
            for (const char *curr = arg;
                 curr;
                 curr = va_arg(argList, const char *)) {
                m_argvstr.push_back(curr);
            }
            va_end(argList);

            m_argv.reset(new const char *[m_argvstr.size() + 1]);
            m_argv[0] = "client-test";
            for (size_t index = 0;
                 index < m_argvstr.size();
                 ++index) {
                m_argv[index + 1] = m_argvstr[index].c_str();
            }

            m_cmdline.set(new SyncEvolutionCmdline(m_argvstr.size() + 1, m_argv.get(), m_out, m_err), "cmdline");
        }

        void doit() {
            bool success;
            success = m_cmdline->parse() &&
                m_cmdline->run();
            if (m_err.str().size()) {
                cerr << endl << m_err.str();
            }
            CPPUNIT_ASSERT(success);
        }

        ostringstream m_out, m_err;
        cxxptr<SyncEvolutionCmdline> m_cmdline;

    private:
        vector<string> m_argvstr;
        boost::scoped_array<const char *> m_argv;
    };

    string ScheduleWorldConfig() {
        return m_scheduleWorldConfig;
    }

    string OldScheduleWorldConfig() {
        string oldConfig = m_scheduleWorldConfig;
        boost::replace_all(oldConfig,
                           ".internal.ini",
                           "config.ini");
        InitList<string> sources = InitList<string>("addressbook") +
            "calendar" +
            "memo" +
            "todo";
        BOOST_FOREACH(string &source, sources) {
            boost::replace_all(oldConfig,
                               string("sources/") + source + "/config.ini",
                               string("spds/sources/") + source + "/config.txt");
        }
        boost::replace_all(oldConfig,
                           "config.ini",
                           "spds/syncml/config.txt");
        return oldConfig;
    }

    string FunambolConfig() {
        string config = m_scheduleWorldConfig;

        boost::replace_first(config,
                             "syncURL = http://sync.scheduleworld.com",
                             "syncURL = http://my.funambol.com");

        boost::replace_first(config,
                             "addressbook/config.ini:uri = card3",
                             "addressbook/config.ini:uri = card");
        boost::replace_first(config,
                             "addressbook/config.ini:type = addressbook",
                             "addressbook/config.ini:type = addressbook:text/x-vcard");

        boost::replace_first(config,
                             "calendar/config.ini:uri = event2",
                             "calendar/config.ini:uri = event");
        boost::replace_first(config,
                             "calendar/config.ini:sync = two-way",
                             "calendar/config.ini:sync = disabled");

        boost::replace_first(config,
                             "memo/config.ini:sync = two-way",
                             "memo/config.ini:sync = disabled");

        boost::replace_first(config,
                             "todo/config.ini:uri = task2",
                             "todo/config.ini:uri = task");
        boost::replace_first(config,
                             "todo/config.ini:sync = two-way",
                             "todo/config.ini:sync = disabled");

        return config;
    }

    string SynthesisConfig() {
        string config = m_scheduleWorldConfig;
        boost::replace_first(config,
                             "syncURL = http://sync.scheduleworld.com",
                             "syncURL = http://www.synthesis.ch/sync");

        boost::replace_first(config,
                             "addressbook/config.ini:uri = card3",
                             "addressbook/config.ini:uri = contacts");

        boost::replace_first(config,
                             "calendar/config.ini:uri = event2",
                             "calendar/config.ini:uri = events");
        boost::replace_first(config,
                             "calendar/config.ini:sync = two-way",
                             "calendar/config.ini:sync = disabled");

        boost::replace_first(config,
                             "memo/config.ini:uri = note",
                             "memo/config.ini:uri = notes");

        boost::replace_first(config,
                             "todo/config.ini:uri = task2",
                             "todo/config.ini:uri = tasks");
        boost::replace_first(config,
                             "todo/config.ini:sync = two-way",
                             "todo/config.ini:sync = disabled");

        return config;
    }

    /** temporarily set env variable, restore old value on destruction */
    class ScopedEnvChange {
    public:
        ScopedEnvChange(const string &var, const string &value) :
            m_var(var)
        {
            const char *oldval = getenv(var.c_str());
            if (oldval) {
                m_oldvalset = true;
                m_oldval = oldval;
            } else {
                m_oldvalset = false;
            }
            setenv(var.c_str(), value.c_str(), 1);
        }
        ~ScopedEnvChange()
        {
            if (m_oldvalset) {
                setenv(m_var.c_str(), m_oldval.c_str(), 1);
            } else {
                unsetenv(m_var.c_str());
            } 
        }
    private:
        string m_var, m_oldval;
        bool m_oldvalset;
    };
            

    /** create directory hierarchy, overwriting previous content */
    void createFiles(const string &root, const string &content, bool append = false) {
        if (!append) {
            rm_r(root);
        }

        size_t start = 0;
        ofstream out;
        string outname;

        out.exceptions(ios_base::badbit|ios_base::failbit);
        while (start < content.size()) {
            size_t delim = content.find(':', start);
            size_t end = content.find('\n', start);
            if (delim == content.npos ||
                end == content.npos) {
                // invalid content ?!
                break;
            }
            string newname = content.substr(start, delim - start);
            string line = content.substr(delim + 1, end - delim - 1);
            if (newname != outname) {
                if (out.is_open()) {
                    out.close();
                }
                string fullpath = root + "/" + newname;
                size_t fileoff = fullpath.rfind('/');
                mkdir_p(fullpath.substr(0, fileoff));
                out.open(fullpath.c_str(),
                         append ? ios_base::out : (ios_base::out|ios_base::trunc));
                outname = newname;
            }
            out << line << endl;
            start = end + 1;
        }
    }

    /** turn directory hierarchy into string */
    string scanFiles(const string &root, bool onlyProps = true) {
        ostringstream out;

        scanFiles(root, "", out, onlyProps);
        return out.str();
    }

    void scanFiles(const string &root, const string &dir, ostringstream &out, bool onlyProps) {
        string newroot = root;
        newroot += "/";
        newroot += dir;
        ReadDir readDir(newroot);
        sort(readDir.begin(), readDir.end());

        for (ReadDir::const_iterator it = readDir.begin();
             it != readDir.end();
             ++it) {
            if (isDir(newroot + "/" + *it)) {
                scanFiles(root, dir + (dir.empty() ? "" : "/") + *it, out, onlyProps);
            } else {
                ifstream in;
                in.exceptions(ios_base::badbit /* failbit must not trigger exception because is set when reaching eof ?! */);
                in.open((newroot + "/" + *it).c_str());
                string line;
                while (!in.eof()) {
                    getline(in, line);
                    if ((line.size() || !in.eof()) && 
                        (!onlyProps ||
                         (boost::starts_with(line, "# ") ?
                          isPropAssignment(line.substr(2)) :
                          !line.empty()))) {
                        if (dir.size()) {
                            out << dir << "/";
                        }
                        out << *it << ":";
                        out << line << '\n';
                    }
                }
            }
        }
    }

    string printConfig(const string &server) {
        ScopedEnvChange xdg("XDG_CONFIG_HOME", m_testDir);
        ScopedEnvChange home("HOME", m_testDir);

        TestCmdline cmdline("--print-config", server.c_str(), NULL);
        cmdline.doit();
        CPPUNIT_ASSERT_EQUAL_DIFF("", cmdline.m_err.str());
        return cmdline.m_out.str();
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(SyncEvolutionCmdlineTest);

#endif // ENABLE_UNIT_TESTS
