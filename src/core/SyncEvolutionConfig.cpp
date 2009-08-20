/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#include "config.h"

#include "SyncEvolutionConfig.h"
#include "EvolutionSyncSource.h"
#include "EvolutionSyncClient.h"
#include "FileConfigTree.h"
#include "VolatileConfigTree.h"
#include "VolatileConfigNode.h"
#include "synthesis/timeutil.h"

#include <boost/foreach.hpp>
#include <iterator>
#include <algorithm>
#include <functional>

#include <unistd.h>
#include "config.h"

static bool SourcePropSourceTypeIsSet(boost::shared_ptr<EvolutionSyncSourceConfig> source);
static bool SourcePropURIIsSet(boost::shared_ptr<EvolutionSyncSourceConfig> source);

void ConfigProperty::splitComment(const string &comment, list<string> &commentLines)
{
    size_t start = 0;

    while (true) {
        size_t end = comment.find('\n', start);
        if (end == comment.npos) {
            commentLines.push_back(comment.substr(start));
            break;
        } else {
            commentLines.push_back(comment.substr(start, end - start));
            start = end + 1;
        }
    }
}

void ConfigProperty::throwValueError(const ConfigNode &node, const string &name, const string &value, const string &error) const
{
    EvolutionSyncClient::throwError(node.getName() + ": " + name + " = " + value + ": " + error);
}

EvolutionSyncConfig::EvolutionSyncConfig() :
    m_oldLayout(false)
{
    m_tree.reset(new VolatileConfigTree());
    m_configNode.reset(new VolatileConfigNode());
    m_hiddenNode = m_configNode;
}

EvolutionSyncConfig::EvolutionSyncConfig(const string &server,
                                         boost::shared_ptr<ConfigTree> tree) :
    m_server(server),
    m_oldLayout(false)
{
    string root;

    if (tree.get() != NULL) {
        m_tree = tree;
    } else {
        // search for configuration in various places...
        string lower = server;
        boost::to_lower(lower);
        string confname;
        root = getOldRoot() + "/" + lower;
        confname = root + "/spds/syncml/config.txt";
        if (!access(confname.c_str(), F_OK)) {
            m_oldLayout = true;
        } else {
            root = getNewRoot() + "/" + lower;
        }
        m_tree.reset(new FileConfigTree(root, m_oldLayout));
    }

    string path(m_oldLayout ? "spds/syncml" : "");
    boost::shared_ptr<ConfigNode> node;
    node = m_tree->open(path, ConfigTree::visible);
    m_configNode.reset(new FilterConfigNode(node));
    m_hiddenNode = m_tree->open(path, ConfigTree::hidden);
}

string EvolutionSyncConfig::getRootPath() const
{
    return m_tree->getRootPath();
}

static void addServers(const string &root, EvolutionSyncConfig::ServerList &res) {
    FileConfigTree tree(root, false);
    list<string> servers = tree.getChildren("");
    BOOST_FOREACH(const string &server, servers) {
        // sanity check: only list server directories which actually
        // contain a configuration
        EvolutionSyncConfig config(server);
        if (config.exists()) {
            res.push_back(pair<string, string>(server, root + "/" + server));
        }
    }
}

EvolutionSyncConfig::ServerList EvolutionSyncConfig::getServers()
{
    ServerList res;

    addServers(getOldRoot(), res);
    addServers(getNewRoot(), res);

    return res;
}

EvolutionSyncConfig::ServerList EvolutionSyncConfig::getServerTemplates()
{
    class TmpList : public ServerList {
    public:
        void addDefaultTemplate(const string &server, const string &url) {
            BOOST_FOREACH(const value_type &entry, static_cast<ServerList &>(*this)) {
                if (boost::iequals(entry.first, server)) {
                    // already present
                    return;
                }
            }
            push_back(value_type(server, url));
        }
    } result;

    // scan TEMPLATE_DIR for templates
    string templateDir(TEMPLATE_DIR);
    if (isDir(templateDir)) {
        ReadDir dir(templateDir);
        BOOST_FOREACH(const string &entry, dir) {
            if (isDir(templateDir + "/" + entry)) {
                boost::shared_ptr<EvolutionSyncConfig> config = EvolutionSyncConfig::createServerTemplate(entry);
                string comment = config->getWebURL();
                if (comment.empty()) {
                    comment = templateDir + "/" + entry;
                }
                result.push_back(ServerList::value_type(entry, comment));
            }
        }
    }

    // builtin templates if not present
    result.addDefaultTemplate("Funambol", "http://my.funambol.com");
    result.addDefaultTemplate("ScheduleWorld", "http://sync.scheduleworld.com");
    result.addDefaultTemplate("Synthesis", "http://www.synthesis.ch");
    result.addDefaultTemplate("Memotoo", "http://www.memotoo.com");
    result.addDefaultTemplate("Google", "http://m.google.com/sync");
    result.addDefaultTemplate("ZYB", "http://www.zyb.com");
    result.addDefaultTemplate("Mobical", "http://www.mobical.net");

    result.sort();
    return result;
}

boost::shared_ptr<EvolutionSyncConfig> EvolutionSyncConfig::createServerTemplate(const string &server)
{
    // case insensitive search for read-only file template config
    string templateConfig(TEMPLATE_DIR);
    if (isDir(templateConfig)) {
        ReadDir dir(templateConfig);
        templateConfig = dir.find(boost::iequals(server, "default") ?
                                  string("ScheduleWorld") : server,
                                  false);
    } else {
        templateConfig = "";
    }

    if (templateConfig.empty()) {
        // not found, avoid reading current directory by using one which doesn't exist
        templateConfig = "/dev/null";
    }
    boost::shared_ptr<FileConfigTree> tree(new FileConfigTree(templateConfig, false));
    tree->setReadOnly(true);
    boost::shared_ptr<EvolutionSyncConfig> config(new EvolutionSyncConfig(server, tree));
    boost::shared_ptr<PersistentEvolutionSyncSourceConfig> source;

    config->setDefaults(false);
    // The prefix is important: without it, myFUNAMBOL 6.x and 7.0 map
    // all SyncEvolution instances to the single phone that they support,
    // which leads to unwanted slow syncs when switching between multiple
    // instances.
    config->setDevID(string("sc-pim-") + UUID());

    // create sync source configs and set non-default values
    config->setSourceDefaults("addressbook", false);
    config->setSourceDefaults("calendar", false);
    config->setSourceDefaults("todo", false);
    config->setSourceDefaults("memo", false);

    source = config->getSyncSourceConfig("addressbook");
    if (!SourcePropSourceTypeIsSet(source)) {
        source->setSourceType("addressbook");
    }
    if (!SourcePropURIIsSet(source)) {
        source->setURI("card");
    }
    source = config->getSyncSourceConfig("calendar");
    if (!SourcePropSourceTypeIsSet(source)) {
        source->setSourceType("calendar");
    }
    if (!SourcePropURIIsSet(source)) {
        source->setURI("event");
    }
    source = config->getSyncSourceConfig("todo");
    if (!SourcePropSourceTypeIsSet(source)) {
        source->setSourceType("todo");
    }
    if (!SourcePropURIIsSet(source)) {
        source->setURI("task");
    }
    source = config->getSyncSourceConfig("memo");
    if (!SourcePropSourceTypeIsSet(source)) {
        source->setSourceType("memo");
    }
    if (!SourcePropURIIsSet(source)) {
        source->setURI("note");
    }

    if (isDir(templateConfig)) {
        // directory exists, check for icon?
        if (config->getIconURI().empty()) {
            ReadDir dir(templateConfig);
            BOOST_FOREACH(const string &entry, dir) {
                if (boost::istarts_with(entry, "icon")) {
                    config->setIconURI("file://" + templateConfig + "/" + entry);
                    break;
                }
            }
        }

        // leave the source configs alone and return the config as it is:
        // in order to have sources configured as part of the template,
        // the template directory must have directories for all
        // sources under "sources"
        return config;
    }

    if (boost::iequals(server, "scheduleworld") ||
        boost::iequals(server, "default")) {
        config->setSyncURL("http://sync.scheduleworld.com/funambol/ds");
        config->setWebURL("http://sync.scheduleworld.com");
        config->setConsumerReady(true);
        source = config->getSyncSourceConfig("addressbook");
        source->setURI("card3");
        source->setSourceType("addressbook:text/vcard");
        source = config->getSyncSourceConfig("calendar");
        source->setURI("cal2");
        source = config->getSyncSourceConfig("todo");
        source->setURI("task2");
        source = config->getSyncSourceConfig("memo");
        source->setURI("note");
    } else if (boost::iequals(server, "funambol")) {
        config->setSyncURL("http://my.funambol.com/sync");
        config->setWebURL("http://my.funambol.com");
        config->setWBXML(false);
        config->setConsumerReady(true);
        source = config->getSyncSourceConfig("calendar");
        source->setSync("two-way");
        source->setURI("event");
        source->setSourceType("calendar:text/calendar!");
        source = config->getSyncSourceConfig("todo");
        source->setSync("two-way");
        source->setURI("task");
        source->setSourceType("todo:text/calendar!");
    } else if (boost::iequals(server, "synthesis")) {
        config->setSyncURL("http://www.synthesis.ch/sync");
        config->setWebURL("http://www.synthesis.ch");
        source = config->getSyncSourceConfig("addressbook");
        source->setURI("contacts");
        source = config->getSyncSourceConfig("calendar");
        source->setURI("events");
        source->setSync("disabled");
        source = config->getSyncSourceConfig("todo");
        source->setURI("tasks");
        source->setSync("disabled");
        source = config->getSyncSourceConfig("memo");
        source->setURI("notes");
    } else if (boost::iequals(server, "memotoo")) {
        config->setSyncURL("http://sync.memotoo.com/syncML");
        config->setWebURL("http://www.memotoo.com");
        source = config->getSyncSourceConfig("addressbook");
        source->setURI("con");
        source = config->getSyncSourceConfig("calendar");
        source->setURI("cal");
        source = config->getSyncSourceConfig("todo");
        source->setURI("task");
        source = config->getSyncSourceConfig("memo");
        source->setURI("note");
    } else if (boost::iequals(server, "google")) {
        config->setSyncURL("https://m.google.com/syncml");
        config->setWebURL("http://m.google.com/sync");
        config->setClientAuthType("syncml:auth-basic");
        config->setWBXML(true);
        config->setConsumerReady(true);
        // temporarily (?) disabled certificate checking because
        // libsoup/gnutls do not accept the Verisign certificate
        // (GNOME Bugzilla #589323)
        config->setSSLVerifyServer(false);
        config->setSSLVerifyHost(false);
        source = config->getSyncSourceConfig("addressbook");
        source->setURI("contacts");
        source->setSourceType("addressbook:text/x-vcard");
        /* Google support only addressbook sync via syncml */
        source = config->getSyncSourceConfig("calendar");
        source->setSync("none");
        source = config->getSyncSourceConfig("todo");
        source->setSync("none");
        source = config->getSyncSourceConfig("memo");
        source->setSync("none");
    } else if (boost::iequals(server, "zyb")) {
        config->setSyncURL("http://sync.zyb.com");
        config->setWebURL("http://www.zyb.com");
        source = config->getSyncSourceConfig("addressbook");
        source->setURI("contacts");
        source = config->getSyncSourceConfig("calendar");
        source->setURI("calendar");
        source = config->getSyncSourceConfig("todo");
        source->setURI("task");
        source->setSync("disabled");
        source = config->getSyncSourceConfig("memo");
        source->setURI("note");
        source->setSync("disabled");
    } else if (boost::iequals(server, "mobical")) {
        config->setSyncURL("http://www.mobical.net/sync/server");
        config->setWebURL("http://www.mobical.net");
        source = config->getSyncSourceConfig("addressbook");
        source->setURI("con");
        source = config->getSyncSourceConfig("calendar");
        source->setURI("cal");
        source = config->getSyncSourceConfig("todo");
        source->setURI("task");
        source = config->getSyncSourceConfig("memo");
        source->setURI("pnote");
    } else {
        config.reset();
    }

    return config;
}

bool EvolutionSyncConfig::exists() const
{
    return m_configNode->exists();
}

void EvolutionSyncConfig::flush()
{
    m_tree->flush();
}

void EvolutionSyncConfig::remove()
{
    m_tree->remove();
    m_tree.reset(new VolatileConfigTree());
}

boost::shared_ptr<PersistentEvolutionSyncSourceConfig> EvolutionSyncConfig::getSyncSourceConfig(const string &name)
{
    SyncSourceNodes nodes = getSyncSourceNodes(name);
    return boost::shared_ptr<PersistentEvolutionSyncSourceConfig>(new PersistentEvolutionSyncSourceConfig(name, nodes));
}

list<string> EvolutionSyncConfig::getSyncSources() const
{
    return m_tree->getChildren(m_oldLayout ? "spds/sources" : "sources");
}

SyncSourceNodes EvolutionSyncConfig::getSyncSourceNodes(const string &name,
                                                        const string &changeId)
{
    boost::shared_ptr<FilterConfigNode> configNode;
    boost::shared_ptr<ConfigNode> hiddenNode,
        trackingNode;

    boost::shared_ptr<ConfigNode> node;
    string path = string(m_oldLayout ? "spds/sources/" : "sources/");
    // store configs lower case even if the UI uses mixed case
    string lower = name;
    boost::to_lower(lower);
    path += lower;

    node = m_tree->open(path, ConfigTree::visible);
    configNode.reset(new FilterConfigNode(node, m_sourceFilter));
    hiddenNode = m_tree->open(path, ConfigTree::hidden);
    trackingNode = m_tree->open(path, ConfigTree::other, changeId);

    return SyncSourceNodes(configNode, hiddenNode, trackingNode);
}

ConstSyncSourceNodes EvolutionSyncConfig::getSyncSourceNodes(const string &name,
                                                             const string &changeId) const
{
    return const_cast<EvolutionSyncConfig *>(this)->getSyncSourceNodes(name, changeId);
}


static ConfigProperty syncPropSyncURL("syncURL",
                                      "the base URL of the SyncML server which is to be used for SyncML;\n"
                                      "some examples:\n"
                                      "- http://my.funambol.com/sync\n"
                                      "- http://sync.scheduleworld.com/funambol/ds\n"
                                      "- http://www.synthesis.ch/sync\n");
static ConfigProperty syncPropDevID("deviceId",
                                    "The SyncML server gets this string and will use it to keep track of\n"
                                    "changes that still need to be synchronized with this particular\n"
                                    "client; it must be set to something unique (like the pseudo-random\n"
                                    "string created automatically for new configurations) among all clients\n"
                                    "accessing the same server.\n"
                                    "myFUNAMBOL also requires that the string starts with sc-pim-");
static ConfigProperty syncPropUsername("username",
                                       "user name used for authorization with the SyncML server",
                                       "your SyncML server account name");
static PasswordConfigProperty syncPropPassword("password",
                                               "password used for authorization with the SyncML server;\n"
                                               "in addition to specifying it directly as plain text, it can\n"
                                               "also be read from the standard input or from an environment\n"
                                               "variable of your choice:\n"
                                               "  plain text: password = <insert your password here>\n"
                                               "         ask: password = -\n"
                                               "env variable: password = ${<name of environment variable>}\n",
                                               "your SyncML server password");
static BoolConfigProperty syncPropUseProxy("useProxy",
                                           "set to T to choose an HTTP proxy explicitly; otherwise the default\n"
                                           "proxy settings of the underlying HTTP transport mechanism are used");
static ConfigProperty syncPropProxyHost("proxyHost",
                                        "proxy URL (http://<host>:<port>)");
static ConfigProperty syncPropProxyUsername("proxyUsername",
                                            "authentication for proxy: username");
static PasswordConfigProperty syncPropProxyPassword("proxyPassword",
                                                    "proxy password, can be specified in different ways,\n"
                                                    "see SyncML server password for details\n");
static StringConfigProperty syncPropClientAuthType("clientAuthType",
                                                   "- empty or \"md5\" for secure method (recommended)\n"
                                                   "- \"basic\" for insecure method\n"
                                                   "\n"
                                                   "This setting is only for debugging purpose and only\n"
                                                   "has an effect during the initial sync of a client.\n"
                                                   "Later it remembers the method that was supported by\n"
                                                   "the server and uses that.",
                                                   "md5",
                                                   Values() +
                                                   (Aliases("basic") + "syncml:auth-basic") +
                                                   (Aliases("md5") + "syncml:auth-md5" + ""));
static ULongConfigProperty syncPropMaxMsgSize("maxMsgSize",
                                              "The maximum size of each message can be set (maxMsgSize) and the\n"
                                              "server can be told to never sent items larger than a certain\n"
                                              "threshold (maxObjSize). Presumably the server has to truncate or\n"
                                              "skip larger items. Sizes are specified as number of bytes.",
                                              "20000");
static UIntConfigProperty syncPropMaxObjSize("maxObjSize", "", "4000000");

static BoolConfigProperty syncPropCompression("enableCompression", "enable compression of network traffic (not currently supported)");
static BoolConfigProperty syncPropWBXML("enableWBXML",
                                        "use the more compact binary XML (WBXML) for messages between client and server",
                                        "TRUE");
static ConfigProperty syncPropLogDir("logdir",
                                     "full path to directory where automatic backups and logs\n"
                                     "are stored for all synchronizations; if unset, then\n"
                                     "\"${XDG_CACHE_HOME}/syncevolution/<server>\" (which\n"
                                     "usually expands to ${HOME}/.cache/...) will be used;\n"
                                     "if \"none\", then no backups of the databases are made and any\n"
                                     "output is printed directly to the screen");
static IntConfigProperty syncPropMaxLogDirs("maxlogdirs",
                                            "Unless this option is set, SyncEvolution will never delete\n"
                                            "anything in the \"logdir\". If set, the oldest directories and\n"
                                            "all their content will be removed after a successful sync\n"
                                            "to prevent the number of log directories from growing beyond\n"
                                            "the given limit.",
                                            "10");
static IntConfigProperty syncPropLogLevel("loglevel",
                                          "level of detail for log messages:\n"
                                          "- 0 (or unset) = INFO messages without log file, DEBUG with log file\n"
                                          "- 1 = only ERROR messages\n"
                                          "- 2 = also INFO messages\n"
                                          "- 3 = also DEBUG messages\n"
                                          "> 3 = increasing amounts of debug messages for developers");
static BoolConfigProperty syncPropPrintChanges("printChanges",
                                               "enables or disables the detailed (and sometimes slow) comparison\n"
                                               "of database content before and after a sync session",
                                               "1");
static ConfigProperty syncPropSSLServerCertificates("SSLServerCertificates",
                                                    "A string specifying the location of the certificates\n"
                                                    "used to authenticate the server. When empty, the\n"
                                                    "system's default location will be searched.",
                                                    SYNCEVOLUTION_SSL_SERVER_CERTIFICATES);
static BoolConfigProperty syncPropSSLVerifyServer("SSLVerifyServer",
                                                  "The client refuses to establish the connection unless\n"
                                                  "the server presents a valid certificate. Disabling this\n"
                                                  "option considerably reduces the security of SSL\n"
                                                  "(man-in-the-middle attacks become possible) and is not\n"
                                                  "recommended.\n",
                                                  "1");
static BoolConfigProperty syncPropSSLVerifyHost("SSLVerifyHost",
                                                "The client refuses to establish the connection unless the\n"
                                                "server's certificate matches its host name. In cases where\n"
                                                "the certificate still seems to be valid it might make sense\n"
                                                "to disable this option and allow such connections.\n",
                                                "1");

static ConfigProperty syncPropWebURL("WebURL",
                                     "The URL of a web page with further information about the server.\n"
                                     "Used only by the GUI."
                                     "");

static ConfigProperty syncPropIconURI("IconURI",
                                      "The URI of an icon representing the server graphically.\n"
                                      "Should be a 48x48 pixmap or a SVG (preferred).\n"
                                      "Used only by the GUI.");

static BoolConfigProperty syncPropConsumerReady("ConsumerReady",
                                                "Set to true in a configuration template to indicate\n"
                                                "that the server works well enough and is available\n"
                                                "for normal users. Used by the GUI to limit the choice\n"
                                                "of configurations offered to users.\n"
                                                "Has no effect in a user's server configuration.\n",
                                                "0");

static ULongConfigProperty syncPropHashCode("HashCode", "used by the SyncML library internally; do not modify");

static ConfigProperty syncPropConfigDate("ConfigDate", "used by the SyncML library internally; do not modify");

ConfigPropertyRegistry &EvolutionSyncConfig::getRegistry()
{
    static ConfigPropertyRegistry registry;
    static bool initialized;

    if (!initialized) {
        registry.push_back(&syncPropSyncURL);
        syncPropSyncURL.setObligatory(true);
        registry.push_back(&syncPropUsername);
        syncPropUsername.setObligatory(true);
        registry.push_back(&syncPropPassword);
        syncPropPassword.setObligatory(true);
        registry.push_back(&syncPropLogDir);
        registry.push_back(&syncPropLogLevel);
        registry.push_back(&syncPropPrintChanges);
        registry.push_back(&syncPropMaxLogDirs);
        registry.push_back(&syncPropUseProxy);
        registry.push_back(&syncPropProxyHost);
        registry.push_back(&syncPropProxyUsername);
        registry.push_back(&syncPropProxyPassword);
        registry.push_back(&syncPropClientAuthType);
        registry.push_back(&syncPropDevID);
        syncPropDevID.setObligatory(true);
        registry.push_back(&syncPropWBXML);
        registry.push_back(&syncPropMaxMsgSize);
        registry.push_back(&syncPropMaxObjSize);
        registry.push_back(&syncPropCompression);
        registry.push_back(&syncPropSSLServerCertificates);
        registry.push_back(&syncPropSSLVerifyServer);
        registry.push_back(&syncPropSSLVerifyHost);
        registry.push_back(&syncPropWebURL);
        registry.push_back(&syncPropIconURI);
        registry.push_back(&syncPropConsumerReady);
        registry.push_back(&syncPropHashCode);
        syncPropHashCode.setHidden(true);
        registry.push_back(&syncPropConfigDate);
        syncPropConfigDate.setHidden(true);
        initialized = true;
    }

    return registry;
}

const char *EvolutionSyncConfig::getUsername() const { return m_stringCache.getProperty(*m_configNode, syncPropUsername); }
void EvolutionSyncConfig::setUsername(const string &value, bool temporarily) { syncPropUsername.setProperty(*m_configNode, value, temporarily); }
const char *EvolutionSyncConfig::getPassword() const {
    string password = syncPropPassword.getCachedProperty(*m_configNode, m_cachedPassword);
    return m_stringCache.storeString(syncPropPassword.getName(), password);
}
void EvolutionSyncConfig::checkPassword(ConfigUserInterface &ui) {
    syncPropPassword.checkPassword(*m_configNode, ui, "SyncML server", m_cachedPassword);
}

void PasswordConfigProperty::checkPassword(ConfigNode &node,
                                           ConfigUserInterface &ui,
                                           const string &descr,
                                           string &cachedPassword)
{
    string password = getProperty(node);

    if (password == "-") {
        password = ui.askPassword(descr);
    } else if(boost::starts_with(password, "${") &&
              boost::ends_with(password, "}")) {
        string envname = password.substr(2, password.size() - 3);
        const char *envval = getenv(envname.c_str());
        if (!envval) {
            EvolutionSyncClient::throwError(string("the environment variable '") +
                                            envname +
                                            "' for the '" +
                                            descr +
                                            "' password is not set");
        } else {
            password = envval;
        }
    }
    cachedPassword = password;
}

string PasswordConfigProperty::getCachedProperty(ConfigNode &node,
                                                 const string &cachedPassword)
{
    string password;

    if (!cachedPassword.empty()) {
        password = cachedPassword;
    } else {
        password = getProperty(node);
    }
    return password;
}

void EvolutionSyncConfig::setPassword(const string &value, bool temporarily) { m_cachedPassword = ""; syncPropPassword.setProperty(*m_configNode, value, temporarily); }
bool EvolutionSyncConfig::getUseProxy() const { return syncPropUseProxy.getProperty(*m_configNode); }
void EvolutionSyncConfig::setUseProxy(bool value, bool temporarily) { syncPropUseProxy.setProperty(*m_configNode, value, temporarily); }
const char *EvolutionSyncConfig::getProxyHost() const { return m_stringCache.getProperty(*m_configNode, syncPropProxyHost); }
void EvolutionSyncConfig::setProxyHost(const string &value, bool temporarily) { syncPropProxyHost.setProperty(*m_configNode, value, temporarily); }
const char *EvolutionSyncConfig::getProxyUsername() const { return m_stringCache.getProperty(*m_configNode, syncPropProxyUsername); }
void EvolutionSyncConfig::setProxyUsername(const string &value, bool temporarily) { syncPropProxyUsername.setProperty(*m_configNode, value, temporarily); }
const char *EvolutionSyncConfig::getProxyPassword() const {
    string password = syncPropProxyPassword.getCachedProperty(*m_configNode, m_cachedProxyPassword);
    return m_stringCache.storeString(syncPropProxyPassword.getName(), password);
}
void EvolutionSyncConfig::checkProxyPassword(ConfigUserInterface &ui) {
    syncPropProxyPassword.checkPassword(*m_configNode, ui, "proxy", m_cachedProxyPassword);
}
void EvolutionSyncConfig::setProxyPassword(const string &value, bool temporarily) { m_cachedProxyPassword = ""; syncPropProxyPassword.setProperty(*m_configNode, value, temporarily); }
const char *EvolutionSyncConfig::getSyncURL() const { return m_stringCache.getProperty(*m_configNode, syncPropSyncURL); }
void EvolutionSyncConfig::setSyncURL(const string &value, bool temporarily) { syncPropSyncURL.setProperty(*m_configNode, value, temporarily); }
const char *EvolutionSyncConfig::getClientAuthType() const { return m_stringCache.getProperty(*m_configNode, syncPropClientAuthType); }
void EvolutionSyncConfig::setClientAuthType(const string &value, bool temporarily) { syncPropClientAuthType.setProperty(*m_configNode, value, temporarily); }
unsigned long  EvolutionSyncConfig::getMaxMsgSize() const { return syncPropMaxMsgSize.getProperty(*m_configNode); }
void EvolutionSyncConfig::setMaxMsgSize(unsigned long value, bool temporarily) { syncPropMaxMsgSize.setProperty(*m_configNode, value, temporarily); }
unsigned int  EvolutionSyncConfig::getMaxObjSize() const { return syncPropMaxObjSize.getProperty(*m_configNode); }
void EvolutionSyncConfig::setMaxObjSize(unsigned int value, bool temporarily) { syncPropMaxObjSize.setProperty(*m_configNode, value, temporarily); }
bool EvolutionSyncConfig::getCompression() const { return syncPropCompression.getProperty(*m_configNode); }
void EvolutionSyncConfig::setCompression(bool value, bool temporarily) { syncPropCompression.setProperty(*m_configNode, value, temporarily); }
const char *EvolutionSyncConfig::getDevID() const { return m_stringCache.getProperty(*m_configNode, syncPropDevID); }
void EvolutionSyncConfig::setDevID(const string &value, bool temporarily) { syncPropDevID.setProperty(*m_configNode, value, temporarily); }
bool EvolutionSyncConfig::getWBXML() const { return syncPropWBXML.getProperty(*m_configNode); }
void EvolutionSyncConfig::setWBXML(bool value, bool temporarily) { syncPropWBXML.setProperty(*m_configNode, value, temporarily); }
const char *EvolutionSyncConfig::getLogDir() const { return m_stringCache.getProperty(*m_configNode, syncPropLogDir); }
void EvolutionSyncConfig::setLogDir(const string &value, bool temporarily) { syncPropLogDir.setProperty(*m_configNode, value, temporarily); }
int EvolutionSyncConfig::getMaxLogDirs() const { return syncPropMaxLogDirs.getProperty(*m_configNode); }
void EvolutionSyncConfig::setMaxLogDirs(int value, bool temporarily) { syncPropMaxLogDirs.setProperty(*m_configNode, value, temporarily); }
int EvolutionSyncConfig::getLogLevel() const { return syncPropLogLevel.getProperty(*m_configNode); }
void EvolutionSyncConfig::setLogLevel(int value, bool temporarily) { syncPropLogLevel.setProperty(*m_configNode, value, temporarily); }
bool EvolutionSyncConfig::getPrintChanges() const { return syncPropPrintChanges.getProperty(*m_configNode); }
void EvolutionSyncConfig::setPrintChanges(bool value, bool temporarily) { syncPropPrintChanges.setProperty(*m_configNode, value, temporarily); }
std::string EvolutionSyncConfig::getWebURL() const { return syncPropWebURL.getProperty(*m_configNode); }
void EvolutionSyncConfig::setWebURL(const std::string &url, bool temporarily) { syncPropWebURL.setProperty(*m_configNode, url, temporarily); }
std::string EvolutionSyncConfig::getIconURI() const { return syncPropIconURI.getProperty(*m_configNode); }
bool EvolutionSyncConfig::getConsumerReady() const { return syncPropConsumerReady.getProperty(*m_configNode); }
void EvolutionSyncConfig::setConsumerReady(bool ready) { return syncPropConsumerReady.setProperty(*m_configNode, ready); }
void EvolutionSyncConfig::setIconURI(const std::string &uri, bool temporarily) { syncPropIconURI.setProperty(*m_configNode, uri, temporarily); }
unsigned long EvolutionSyncConfig::getHashCode() const { return syncPropHashCode.getProperty(*m_hiddenNode); }
void EvolutionSyncConfig::setHashCode(unsigned long code) { syncPropHashCode.setProperty(*m_hiddenNode, code); }
std::string EvolutionSyncConfig::getConfigDate() const { return syncPropConfigDate.getProperty(*m_hiddenNode); }
void EvolutionSyncConfig::setConfigDate() { 
    /* Set current timestamp as configdate */
    char buffer[17]; 
    time_t ts = time(NULL);
    strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", gmtime(&ts));
    const std::string date(buffer);
    syncPropConfigDate.setProperty(*m_hiddenNode, date);
}
const char* EvolutionSyncConfig::getSSLServerCertificates() const { return m_stringCache.getProperty(*m_configNode, syncPropSSLServerCertificates); }
void EvolutionSyncConfig::setSSLServerCertificates(const string &value, bool temporarily) { syncPropSSLServerCertificates.setProperty(*m_configNode, value, temporarily); }
bool EvolutionSyncConfig::getSSLVerifyServer() const { return syncPropSSLVerifyServer.getProperty(*m_configNode); }
void EvolutionSyncConfig::setSSLVerifyServer(bool value, bool temporarily) { syncPropSSLVerifyServer.setProperty(*m_configNode, value, temporarily); }
bool EvolutionSyncConfig::getSSLVerifyHost() const { return syncPropSSLVerifyHost.getProperty(*m_configNode); }
void EvolutionSyncConfig::setSSLVerifyHost(bool value, bool temporarily) { syncPropSSLVerifyHost.setProperty(*m_configNode, value, temporarily); }

static void setDefaultProps(const ConfigPropertyRegistry &registry,
                            boost::shared_ptr<FilterConfigNode> node,
                            bool force)
{
    BOOST_FOREACH(const ConfigProperty *prop, registry) {
        bool isDefault;
        prop->getProperty(*node, &isDefault);
        
        if (!prop->isHidden() &&
            (force || isDefault)) {
            prop->setDefaultProperty(*node, prop->isObligatory());
        }
    }    
}

void EvolutionSyncConfig::setDefaults(bool force)
{
    setDefaultProps(getRegistry(), m_configNode, force);
}

void EvolutionSyncConfig::setSourceDefaults(const string &name, bool force)
{
    SyncSourceNodes nodes = getSyncSourceNodes(name);
    setDefaultProps(EvolutionSyncSourceConfig::getRegistry(),
                    nodes.m_configNode, force);
}

static void copyProperties(const ConfigNode &fromProps,
                           ConfigNode &toProps,
                           bool hidden,
                           const ConfigPropertyRegistry &allProps)
{
    BOOST_FOREACH(const ConfigProperty *prop, allProps) {
        if (prop->isHidden() == hidden) {
            string name = prop->getName();
            bool isDefault;
            string value = prop->getProperty(fromProps, &isDefault);
            toProps.setProperty(name, value, prop->getComment(),
                                isDefault ? &value : NULL);
        }
    }
}

static void copyProperties(const ConfigNode &fromProps,
                           ConfigNode &toProps)
{
    map<string, string> props;
    fromProps.readProperties(props);

    BOOST_FOREACH(const StringPair &prop, props) {
        toProps.setProperty(prop.first, prop.second);
    }
}

void EvolutionSyncConfig::copy(const EvolutionSyncConfig &other,
                               const set<string> *sourceFilter)
{
    static const bool visibility[2] = { false, true };
    for (int i = 0; i < 2; i++ ) {
        boost::shared_ptr<const FilterConfigNode> fromSyncProps(other.getProperties(visibility[i]));
        boost::shared_ptr<FilterConfigNode> toSyncProps(this->getProperties(visibility[i]));
        copyProperties(*fromSyncProps, *toSyncProps, visibility[i], other.getRegistry());
    }

    list<string> sources = other.getSyncSources();
    BOOST_FOREACH(const string &sourceName, sources) {
        if (!sourceFilter ||
            sourceFilter->find(sourceName) != sourceFilter->end()) {
            ConstSyncSourceNodes fromNodes = other.getSyncSourceNodes(sourceName);
            SyncSourceNodes toNodes = this->getSyncSourceNodes(sourceName);
            copyProperties(*fromNodes.m_configNode, *toNodes.m_configNode, false, EvolutionSyncSourceConfig::getRegistry());
            copyProperties(*fromNodes.m_hiddenNode, *toNodes.m_hiddenNode, true, EvolutionSyncSourceConfig::getRegistry());
            copyProperties(*fromNodes.m_trackingNode, *toNodes.m_trackingNode);
        }
    }
}

const char *EvolutionSyncConfig::getSwv() const { return VERSION; }
const char *EvolutionSyncConfig::getDevType() const { return DEVICE_TYPE; }

                     
EvolutionSyncSourceConfig::EvolutionSyncSourceConfig(const string &name, const SyncSourceNodes &nodes) :
    m_name(name),
    m_nodes(nodes)
{
}

StringConfigProperty EvolutionSyncSourceConfig::m_sourcePropSync("sync",
                                           "requests a certain synchronization mode:\n"
                                           "  two-way             = only send/receive changes since last sync\n"
                                           "  slow                = exchange all items\n"
                                           "  refresh-from-client = discard all remote items and replace with\n"
                                           "                        the items on the client\n"
                                           "  refresh-from-server = discard all local items and replace with\n"
                                           "                        the items on the server\n"
                                           "  one-way-from-client = transmit changes from client\n"
                                           "  one-way-from-server = transmit changes from server\n"
                                           "  none (or disabled)  = synchronization disabled",
                                           "two-way",
                                           Values() +
                                           (Aliases("two-way")) +
                                           (Aliases("slow")) +
                                           (Aliases("refresh-from-client") + "refresh-client") +
                                           (Aliases("refresh-from-server") + "refresh-server" + "refresh") +
                                           (Aliases("one-way-from-client") + "one-way-client") +
                                           (Aliases("one-way-from-server") + "one-way-server" + "one-way") +
                                           (Aliases("disabled") + "none"));

static class SourceTypeConfigProperty : public StringConfigProperty {
public:
    SourceTypeConfigProperty() :
        StringConfigProperty("type",
                             "Specifies the SyncEvolution backend and thus the\n"
                             "data which is synchronized by this source. Some\n"
                             "backends can exchange data in multiple formats.\n"
                             "Some of them have a default format that is used\n"
                             "automatically unless specified differently.\n"
                             "Sometimes the format must be specified.\n"
                             "\n"
                             "In all cases the format of this configuration is\n"
                             "  <backend>[:format]\n"
                             "\n"
                             "Here are some valid examples:\n"
                             "  contacts - synchronize address book with default vCard 2.1 format\n"
                             "  contacts:text/vcard - address book with vCard 3.0 format\n"
                             "  calendar - synchronize events in iCalendar 2.0 format\n"
                             "  calendar:text/x-calendar - prefer legacy vCalendar 1.0 format\n"
                             "\n"
                             "Sending and receiving items in the same format as used by the server for\n"
                             "the uri selected below is essential. Normally, SyncEvolution and the server\n"
                             "negotiate the preferred format automatically. With some servers, it is\n"
                             "necessary to change the defaults (vCard 2.1 and iCalendar 2.0), typically\n"
                             "because the server does not implement the format selection or the format\n"
                             "itself correctly.\n"
                             "Errors while starting to sync and parsing and/or storing\n"
                             "items on either client or server can be caused by a mismatch between\n"
                             "type and uri.\n"
                             "\n"
                             "Here's the full list of potentially supported backends,\n"
                             "valid <backend> values for each of them, and possible\n"
                             "formats. Note that SyncEvolution installations usually\n"
                             "support only a subset of the backends; that's why e.g.\n"
                             "\"addressbook\" is unambiguous although there are multiple\n"
                             "address book backends.\n",
                             "select backend",
                             Values() +
                             (Aliases("calendar") + "events") +
                             (Aliases("calendar:text/calendar") + "text/calendar") +
                             (Aliases("calendar:text/x-vcalendar") + "text/x-vcalendar") +
                             (Aliases("addressbook") + "contacts") +
                             (Aliases("addressbook:text/x-vcard") + "text/x-vcard") +
                             (Aliases("addressbook:text/vcard") + "text/vcard") +
                             (Aliases("todo") + "tasks" + "text/x-todo") +
                             (Aliases("memo") + "memos" + "notes" + "text/plain") +
                             (Aliases("memo:text/calendar") + "text/x-journal"))
    {}

    virtual string getComment() const {
        stringstream enabled, disabled;
        stringstream res;

        SourceRegistry &registry(EvolutionSyncSource::getSourceRegistry());
        BOOST_FOREACH(const RegisterSyncSource *sourceInfos, registry) {
            const string &comment = sourceInfos->m_typeDescr;
            stringstream *curr = sourceInfos->m_enabled ? &enabled : &disabled;
            *curr << comment;
            if (comment.size() && comment[comment.size() - 1] != '\n') {
                *curr << '\n';
            }
        }

        res << StringConfigProperty::getComment();
        if (enabled.str().size()) {
            res << "\nCurrently active:\n" << enabled.str();
        }
        if (disabled.str().size()) {
            res << "\nCurrently inactive:\n" << disabled.str();
        }

        return boost::trim_right_copy(res.str());
    }

    virtual Values getValues() const {
        Values res(StringConfigProperty::getValues());

        const SourceRegistry &registry(EvolutionSyncSource::getSourceRegistry());
        BOOST_FOREACH(const RegisterSyncSource *sourceInfos, registry) {
            copy(sourceInfos->m_typeValues.begin(),
                 sourceInfos->m_typeValues.end(),
                 back_inserter(res));
        }

        return res;
    }

    /** relax string checking: only the part before a colon has to match one of the aliases */
    virtual bool checkValue(const string &value, string &error) const {
        size_t colon = value.find(':');
        if (colon != value.npos) {
            string backend = value.substr(0, colon);
            return StringConfigProperty::checkValue(backend, error);
        } else {
            return StringConfigProperty::checkValue(value, error);
        }
    }
} sourcePropSourceType;
static bool SourcePropSourceTypeIsSet(boost::shared_ptr<EvolutionSyncSourceConfig> source)
{
    return source->isSet(sourcePropSourceType);
}

static ConfigProperty sourcePropDatabaseID("evolutionsource",
                                           "Picks one of backend data sources:\n"
                                           "enter either the name or the full URL.\n"
                                           "Most backends have a default data source,\n"
                                           "like for example the system address book.\n"
                                           "Not setting this property selects that default\n"
                                           "data source.\n"
                                           "\n"
                                           "To get a full list of available data sources,\n"
                                           "run syncevolution without parameters. The name\n"
                                           "is printed in front of the colon, followed by\n"
                                           "the URL. Usually the name is unique and can be\n"
                                           "used to reference the data source. The default\n"
                                           "data source is marked with <default> after the\n"
                                           "URL, if there is a default.\n");
static ConfigProperty sourcePropURI("uri",
                                    "this is appended to the server's URL to identify the\n"
                                    "server's database");
static bool SourcePropURIIsSet(boost::shared_ptr<EvolutionSyncSourceConfig> source)
{
    return source->isSet(sourcePropURI);
}

static ConfigProperty sourcePropUser("evolutionuser",
                                     "authentication for backend data source; password can be specified\n"
                                     "in multiple ways, see SyncML server password for details\n"
                                     "\n"
                                     "Warning: setting evolutionuser/password in cases where it is not\n"
                                     "needed, as for example with local Evolution calendars and addressbooks,\n"
                                     "can cause the Evolution backend to hang.");
static PasswordConfigProperty sourcePropPassword("evolutionpassword", "");

static ULongConfigProperty sourcePropLast("last",
                                          "used by the SyncML library internally; do not modify");

ConfigPropertyRegistry &EvolutionSyncSourceConfig::getRegistry()
{
    static ConfigPropertyRegistry registry;
    static bool initialized;

    if (!initialized) {
        registry.push_back(&EvolutionSyncSourceConfig::m_sourcePropSync);
        EvolutionSyncSourceConfig::m_sourcePropSync.setObligatory(true);
        registry.push_back(&sourcePropSourceType);
        registry.push_back(&sourcePropDatabaseID);
        registry.push_back(&sourcePropURI);
        registry.push_back(&sourcePropUser);
        registry.push_back(&sourcePropPassword);
        registry.push_back(&sourcePropLast);
        sourcePropLast.setHidden(true);
        initialized = true;
    }

    return registry;
}

const char *EvolutionSyncSourceConfig::getDatabaseID() const { return m_stringCache.getProperty(*m_nodes.m_configNode, sourcePropDatabaseID); }
void EvolutionSyncSourceConfig::setDatabaseID(const string &value, bool temporarily) { sourcePropDatabaseID.setProperty(*m_nodes.m_configNode, value, temporarily); }
const char *EvolutionSyncSourceConfig::getUser() const { return m_stringCache.getProperty(*m_nodes.m_configNode, sourcePropUser); }
void EvolutionSyncSourceConfig::setUser(const string &value, bool temporarily) { sourcePropUser.setProperty(*m_nodes.m_configNode, value, temporarily); }
const char *EvolutionSyncSourceConfig::getPassword() const {
    string password = sourcePropPassword.getCachedProperty(*m_nodes.m_configNode, m_cachedPassword);
    return m_stringCache.storeString(sourcePropPassword.getName(), password);
}
void EvolutionSyncSourceConfig::checkPassword(ConfigUserInterface &ui) {
    sourcePropPassword.checkPassword(*m_nodes.m_configNode, ui, m_name + " backend", m_cachedPassword);
}
void EvolutionSyncSourceConfig::setPassword(const string &value, bool temporarily) { m_cachedPassword = ""; sourcePropPassword.setProperty(*m_nodes.m_configNode, value, temporarily); }
const char *EvolutionSyncSourceConfig::getURI() const { return m_stringCache.getProperty(*m_nodes.m_configNode, sourcePropURI); }
void EvolutionSyncSourceConfig::setURI(const string &value, bool temporarily) { sourcePropURI.setProperty(*m_nodes.m_configNode, value, temporarily); }
const char *EvolutionSyncSourceConfig::getSync() const { return m_stringCache.getProperty(*m_nodes.m_configNode, m_sourcePropSync); }
void EvolutionSyncSourceConfig::setSync(const string &value, bool temporarily) { m_sourcePropSync.setProperty(*m_nodes.m_configNode, value, temporarily); }
unsigned long EvolutionSyncSourceConfig::getLast() const { return sourcePropLast.getProperty(*m_nodes.m_hiddenNode); }
void EvolutionSyncSourceConfig::setLast(unsigned long timestamp) { sourcePropLast.setProperty(*m_nodes.m_hiddenNode, timestamp); }
string EvolutionSyncSourceConfig::getSourceTypeString(const SyncSourceNodes &nodes) { return sourcePropSourceType.getProperty(*nodes.m_configNode); }
SourceType EvolutionSyncSourceConfig::getSourceType(const SyncSourceNodes &nodes) {
    string type = getSourceTypeString(nodes);
    SourceType sourceType;
    size_t colon = type.find(':');
    if (colon != type.npos) {
        string backend = type.substr(0, colon);
        string format = type.substr(colon + 1);
        sourcePropSourceType.normalizeValue(backend);
        size_t formatLen = format.size();
        if(format[formatLen - 1] == '!') {
            sourceType.m_forceFormat = true;
            format = format.substr(0, formatLen - 1);
        }
        sourceType.m_backend = backend;
        sourceType.m_format  = format;
    } else {
        sourceType.m_backend = type;
        sourceType.m_format  = "";
    }
    return sourceType;
}
SourceType EvolutionSyncSourceConfig::getSourceType() const { return getSourceType(m_nodes); }
void EvolutionSyncSourceConfig::setSourceType(const string &value, bool temporarily) { sourcePropSourceType.setProperty(*m_nodes.m_configNode, value, temporarily); }
