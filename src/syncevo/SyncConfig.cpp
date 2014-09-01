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

#include <syncevo/SyncConfig.h>
#include <syncevo/SyncSource.h>
#include <syncevo/SyncContext.h>
#include <syncevo/FileConfigTree.h>
#include <syncevo/VolatileConfigTree.h>
#include <syncevo/VolatileConfigNode.h>
#include <syncevo/DevNullConfigNode.h>
#include <syncevo/MultiplexConfigNode.h>
#include <syncevo/SingleFileConfigTree.h>
#include <syncevo/IniConfigNode.h>
#include <syncevo/Cmdline.h>
#include <syncevo/lcs.h>
#include <syncevo/ThreadSupport.h>
#include <syncevo/IdentityProvider.h>
#include <test.h>
#include <synthesis/timeutil.h>

#include <boost/foreach.hpp>
#include <iterator>
#include <algorithm>
#include <functional>
#include <queue>

#include <unistd.h>
#include "config.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

const char *const SourceAdminDataName = "adminData";
const char *const SyncMaxMsgSize = "maxMsgSize";

int ConfigVersions[CONFIG_LEVEL_MAX][CONFIG_VERSION_MAX] =
{
    { CONFIG_ROOT_MIN_VERSION, CONFIG_ROOT_CUR_VERSION },
    { CONFIG_CONTEXT_MIN_VERSION, CONFIG_CONTEXT_CUR_VERSION },
    { CONFIG_PEER_MIN_VERSION, CONFIG_PEER_CUR_VERSION },
};    

std::string ConfigLevel2String(ConfigLevel level)
{
    switch (level) {
    case CONFIG_LEVEL_ROOT:
        return "config root";
        break;
    case CONFIG_LEVEL_CONTEXT:
        return "context config";
        break;
    case CONFIG_LEVEL_PEER:
        return "peer config";
        break;
    default:
        return StringPrintf("config level %d (?)", level);
        break;
    }
}

PropertySpecifier PropertySpecifier::StringToPropSpec(const std::string &spec, int flags)
{
    PropertySpecifier res;

    size_t slash = spec.find('/');
    if (slash != spec.npos) {
        // no normalization needed at the moment
        res.m_source = spec.substr(0, slash);
        slash++;
    } else {
        slash = 0;
    }
    size_t at = spec.find('@', slash);
    if (at != spec.npos) {
        // Context or config?
        if (spec.find('@', at + 1) != spec.npos) {
            // has a second @ sign, must be config name
            res.m_config = spec.substr(at + 1);
        } else {
            // context, include leading @ sign
            res.m_config = spec.substr(at);
        }
        if (flags & NORMALIZE_CONFIG) {
            res.m_config = SyncConfig::normalizeConfigString(res.m_config, SyncConfig::NORMALIZE_LONG_FORMAT);
        }
    } else {
        at = spec.size();
    }
    res.m_property = spec.substr(slash, at - slash);

    return res;
}

UserIdentity::UserIdentity() :
    m_provider(InitStateString(USER_IDENTITY_PLAIN_TEXT, false))
{
}

UserIdentity UserIdentity::fromString(const InitStateString &idString)
{
    UserIdentity id;
    if (idString.wasSet()) {
        size_t off = idString.find(':');
        if (off != idString.npos) {
            id.m_provider = off ?
                idString.substr(0, off) :
                InitStateString(USER_IDENTITY_PLAIN_TEXT, false);
            id.m_identity = idString.substr(off + 1);
        } else {
            id.m_provider = InitStateString(USER_IDENTITY_PLAIN_TEXT, false);
            id.m_identity = idString;
        }
    }
    return id;
}

InitStateString UserIdentity::toString() const
{
    std::string str;
    if (m_provider.wasSet()) {
        str += m_provider;
        str += ':';
    }
    str += m_identity;
    return InitStateString(str, m_provider.wasSet() || m_identity.wasSet());
}

std::string PropertySpecifier::toString()
{
    std::string res;
    res.reserve(m_source.size() + 1 + m_property.size() + 1 + m_config.size());
    res += m_source;
    if (!m_source.empty()) {
        res += '/';
    }
    res += m_property;
    if (!m_config.empty()) {
        if (m_config[0] != '@') {
            res += '@';
        }
        res += m_config;
    }

    return res;
}

string ConfigProperty::getName(const ConfigNode &node) const
{
    if (m_names.empty()) {
        // shouldn't happen
        return "???";
    }
    if (m_names.size() == 1) {
        // typical case for most properties
        return m_names.front();
    }
    // pick the name already used in the node
    BOOST_FOREACH(const std::string &name, m_names) {
        string value;
        if (node.getProperty(name, value)) {
            return name;
        }
    }

    // main name as fallback
    return m_names.front();
}

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
    Exception::throwError(SE_HERE, node.getName() + ": " + name + " = " + value + ": " + error);
}

std::string ConfigProperty::sharing2str(Sharing sharing)
{
    switch (sharing) {
    case GLOBAL_SHARING:
        return "global";
        break;
    case SOURCE_SET_SHARING:
        return "shared";
        break;
    case NO_SHARING:
        return "unshared";
        break;
    }
    return "???";
}

string SyncConfig::normalizeConfigString(const string &config, NormalizeFlags flags)
{
    string normal = config;
    boost::to_lower(normal);
    BOOST_FOREACH(char &character, normal) {
        if (!isprint(character) ||
            character == '/' ||
            character == '\\' ||
            character == ':') {
            character = '_';
        }
    }
    if (boost::ends_with(normal, "@default")) {
        if (flags & NORMALIZE_SHORTHAND) {
            normal.resize(normal.size() - strlen("@default"));
        }
    } else if (boost::ends_with(normal, "@")) {
        normal.resize(normal.size() - 1);
    } else {
        size_t at = normal.rfind('@');
        if (at == normal.npos &&
            !(flags & NORMALIZE_IS_NEW)) {
            // No explicit context. Pick the first server which matches
            // when ignoring their context. Peer list is sorted by name,
            // therefore shorter config names (= without context) are
            // found first, as intended.
            BOOST_FOREACH(const StringPair &entry, getConfigs()) {
                string entry_peer, entry_context;
                splitConfigString(entry.first, entry_peer, entry_context);
                if (normal == entry_peer) {
                    // found a matching, existing config, use it
                    normal = entry.first;
                    break;
                }
            }
        }
        if (!(flags & NORMALIZE_SHORTHAND) && normal.find('@') == normal.npos) {
            // explicitly include @default context specifier
            normal += "@default";
        }
    }

    if (normal.empty()) {
        // default context is meant with the empty string,
        // better make that explicit
        normal = "@default";
    }

    return normal;
}

std::string SyncConfig::DeviceDescription::getFingerprint() const
{
    std::string fingerprint;

    /** In the case that we have the PnpInformation we prefer it over
     *  the mutable device name. The is true even if we only found the
     *  vendor component of the PnpInformation.
     */
    if (m_pnpInformation) {
        if(m_pnpInformation->isKnownProduct())
            fingerprint = m_pnpInformation->m_product;
        else
            fingerprint = m_pnpInformation->m_vendor;
    }
    else {
        fingerprint = m_deviceName;
    }

    return fingerprint;
}

bool SyncConfig::splitConfigString(const string &config, string &peer, string &context)
{
    string::size_type at = config.rfind('@');
    if (at != config.npos) {
        peer = config.substr(0, at);
        context = config.substr(at + 1);
        return true;
    } else {
        peer = config;
        context = "default";
        return false;
    }    
}

static SyncConfig::ConfigWriteMode defaultConfigWriteMode()
{
    return SyncContext::isStableRelease() ?
        SyncConfig::MIGRATE_AUTOMATICALLY :
        SyncConfig::ASK_USER_TO_MIGRATE;
}

SyncConfig::SyncConfig() :
    m_layout(HTTP_SERVER_LAYOUT), // use more compact layout with shorter paths and less source nodes
    m_configWriteMode(defaultConfigWriteMode())
{
    // initialize properties
    SyncConfig::getRegistry();
    SyncSourceConfig::getRegistry();

    m_peerPath =
        m_contextPath = "volatile";
    makeVolatile();
}

void SyncConfig::makeVolatile()
{
    m_tree.reset(new VolatileConfigTree());
    m_fileTree.reset();
    m_peerNode.reset(new VolatileConfigNode());
    m_hiddenPeerNode = m_peerNode;
    m_globalNode = m_peerNode;
    m_contextNode = m_peerNode;
    m_contextHiddenNode = m_peerNode;
    m_props[false] = m_peerNode;
    m_props[true] = m_peerNode;
}

void SyncConfig::makeEphemeral()
{
    m_ephemeral = true;
    // m_hiddenPeerNode.reset(new VolatileConfigNode());
    // m_contextHiddenNode = m_hiddenPeerNode;
}

/**
 * The goal is to have only one FileConfigTree instance per file system
 * location. This ensures that in-memory representations remain in sync
 * when instantiating a SyncConfig is created multiple times. In addition,
 * FilterConfigNodes also must only exit once per underlying node. This
 * allows sharing cached passwords between configs when using indirect
 * password lookup.
 *
 * The implementation in both cases is the same: keep a cache with
 * weak pointers. When asked for an instance, consult the cache first.
 * If the instance still exists, we can return that shared pointer. If
 * not, we create a new one.
 *
 * It is necessary to garbage-collect obsolete entries, because the
 * lookup parameters might never be used again.
 */
class ConfigCache
{
    typedef std::map< std::pair<std::string, SyncConfig::Layout>, boost::weak_ptr<FileConfigTree> > TreeMap;
    TreeMap m_trees;
    typedef std::map< ConfigNode *,  boost::weak_ptr<FilterConfigNode> > NodeMap;
    NodeMap m_nodes;

    template<class M> void purge(M &map)
    {
        typename M::iterator it = map.begin();
        while (it != map.end()) {
            if (!it->second.lock()) {
                typename M::iterator next = it;
                ++next;
                map.erase(it);
                it = next;
            } else {
                ++it;
            }
        }
    }
    void purge();

public:
    boost::shared_ptr<FileConfigTree> createTree(const std::string &root,
                                                 SyncConfig::Layout layout);
    /**
     * The filter is only installed when creating a new node. It is
     * assumed to be the same when reusing the node.
     */
    boost::shared_ptr<FilterConfigNode> createNode(const boost::shared_ptr<ConfigNode> &node,
                                                   const FilterConfigNode::ConfigFilter &filter = FilterConfigNode::ConfigFilter());
    static ConfigCache &singleton();
};

ConfigCache &ConfigCache::singleton()
{
    static ConfigCache instance;
    return instance;
}

void ConfigCache::purge()
{
    purge(m_trees);
    purge(m_nodes);
}

boost::shared_ptr<FileConfigTree> ConfigCache::createTree(const std::string &root,
                                                          SyncConfig::Layout layout)
{
    TreeMap::mapped_type &entry = m_trees[TreeMap::key_type(root, layout)];
    boost::shared_ptr<FileConfigTree> result;
    result = entry.lock();
    if (!result) {
        result.reset(new FileConfigTree(root, layout));
        entry = result;
    }

    purge();
    return result;
}

boost::shared_ptr<FilterConfigNode> ConfigCache::createNode(const boost::shared_ptr<ConfigNode> &node,
                                                            const FilterConfigNode::ConfigFilter &filter)
{
    NodeMap::mapped_type &entry = m_nodes[node.get()];
    boost::shared_ptr<FilterConfigNode> result;
    result = entry.lock();
    if (!result) {
        result.reset(new FilterConfigNode(node, filter));
        entry = result;
    }

    purge();
    return result;
}

SyncConfig::SyncConfig(const string &peer,
                       boost::shared_ptr<ConfigTree> tree,
                       const string &redirectPeerRootPath) :
    m_layout(SHARED_LAYOUT),
    m_redirectPeerRootPath(redirectPeerRootPath),
    m_configWriteMode(defaultConfigWriteMode())
{
    // initialize properties
    SyncConfig::getRegistry();
    SyncSourceConfig::getRegistry();

    string root;

    m_peer = normalizeConfigString(peer);

    // except for SHARED_LAYOUT (set below),
    // everything is below the directory called like
    // the peer
    m_peerPath =
        m_contextPath = 
        m_peer;

    if (tree.get() != NULL) {
        // existing tree points into simple configuration
        m_tree = tree;
        m_layout = HTTP_SERVER_LAYOUT;
        m_peerPath =
            m_contextPath = "";
    } else {
        // search for configuration in various places...
        root = getOldRoot();
        string path = root + "/" + m_peerPath;
        if (!access((path + "/spds/syncml/config.txt").c_str(), F_OK)) {
            m_layout = SYNC4J_LAYOUT;
        } else {
            m_layout = SHARED_LAYOUT;
            root = getNewRoot();
            path = root + "/" + m_peerPath;
            if (!access((path + "/config.ini").c_str(), F_OK) &&
                !access((path + "/sources").c_str(), F_OK) &&
                access((path + "/peers").c_str(), F_OK)) {
                m_layout = HTTP_SERVER_LAYOUT;
            } else {
                // check whether config name specifies a context,
                // otherwise use "default"
                splitConfigString(m_peer, m_peerPath, m_contextPath);
                if (!m_peerPath.empty()) {
                    m_peerPath = m_contextPath + "/peers/" + m_peerPath;
                }
            }
        }
        m_fileTree = ConfigCache::singleton().createTree(root, m_layout);
        m_tree = m_fileTree;
    }

    string path;
    boost::shared_ptr<ConfigNode> node;
    switch (m_layout) {
    case SYNC4J_LAYOUT:
        // all properties reside in the same node
        path = m_peerPath + "/spds/syncml";
        node = m_tree->open(path, ConfigTree::visible);
        m_peerNode = ConfigCache::singleton().createNode(node);
        m_globalNode =
            m_contextNode = m_peerNode;
        m_hiddenPeerNode =
            m_contextHiddenNode =
            m_globalHiddenNode =
            node;
        m_props[false] = m_peerNode;
        m_props[true] = ConfigCache::singleton().createNode(m_hiddenPeerNode);
        break;
    case HTTP_SERVER_LAYOUT: {
        // properties which are normally considered shared are
        // stored in the same nodes as the per-peer properties,
        // except for global ones
        path = "";
        node = m_tree->open(path, ConfigTree::visible);
        m_globalNode = ConfigCache::singleton().createNode(node);
        node = m_tree->open(path, ConfigTree::hidden);
        m_globalHiddenNode = node;

        path = m_peerPath;      
        node = m_tree->open(path, ConfigTree::visible);
        m_peerNode = ConfigCache::singleton().createNode(node);
        m_contextNode = m_peerNode;
        m_hiddenPeerNode =
            m_contextHiddenNode =
            m_tree->open(path, ConfigTree::hidden);

        // similar multiplexing as for SHARED_LAYOUT,
        // with two nodes underneath
        boost::shared_ptr<MultiplexConfigNode> mnode;
        mnode.reset(new MultiplexConfigNode(m_peerNode->getName(),
                                            getRegistry(),
                                            false));
        m_props[false] = mnode;
        mnode->setNode(false, ConfigProperty::GLOBAL_SHARING,
                       m_globalNode);
        mnode->setNode(false, ConfigProperty::SOURCE_SET_SHARING,
                       m_peerNode);
        mnode->setNode(false, ConfigProperty::NO_SHARING,
                       m_peerNode);
        mnode.reset(new MultiplexConfigNode(m_peerNode->getName(),
                                            getRegistry(),
                                            true));
        m_props[true] = mnode;
        mnode->setNode(true, ConfigProperty::GLOBAL_SHARING,
                       m_globalHiddenNode);
        mnode->setNode(true, ConfigProperty::SOURCE_SET_SHARING,
                       m_peerNode);
        mnode->setNode(true, ConfigProperty::NO_SHARING,
                       m_peerNode);
        break;
    }
    case SHARED_LAYOUT:
        // really use different nodes for everything
        path = "";
        node = m_tree->open(path, ConfigTree::visible);
        m_globalNode = ConfigCache::singleton().createNode(node);
        node = m_tree->open(path, ConfigTree::hidden);
        m_globalHiddenNode = node;

        path = m_peerPath;
        if (path.empty()) {
            if (!m_redirectPeerRootPath.empty()) {
                node.reset(new IniFileConfigNode(m_redirectPeerRootPath,
                                                 ".internal.ini",
                                                 false));
                node = m_tree->add(m_redirectPeerRootPath + "/.internal.ini",
                                   node);
            } else {
                node.reset(new DevNullConfigNode(m_contextPath + " without peer config"));
            }
        } else {
            node = m_tree->open(path, ConfigTree::visible);
        }
        m_peerNode = ConfigCache::singleton().createNode(node);
        if (path.empty()) {
            m_hiddenPeerNode = m_peerNode;
        } else {
            m_hiddenPeerNode = m_tree->open(path, ConfigTree::hidden);
        }

        path = m_contextPath;
        node = m_tree->open(path, ConfigTree::visible);
        m_contextNode = ConfigCache::singleton().createNode(node);
        m_contextHiddenNode = m_tree->open(path, ConfigTree::hidden);

        // Instantiate multiplexer with the most specific node name in
        // the set, the peer node's name. This is slightly inaccurate:
        // error messages generated for this node in will reference
        // the wrong config.ini file for shared properties. But
        // there no shared properties which can trigger such an error
        // at the moment, so this is good enough for now (MB#8037).
        boost::shared_ptr<MultiplexConfigNode> mnode;
        mnode.reset(new MultiplexConfigNode(m_peerNode->getName(),
                                            getRegistry(),
                                            false));
        mnode->setHavePeerNodes(!m_peerPath.empty());
        m_props[false] = mnode;
        mnode->setNode(false, ConfigProperty::GLOBAL_SHARING,
                       m_globalNode);
        mnode->setNode(false, ConfigProperty::SOURCE_SET_SHARING,
                       m_contextNode);
        mnode->setNode(false, ConfigProperty::NO_SHARING,
                       m_peerNode);

        mnode.reset(new MultiplexConfigNode(m_hiddenPeerNode->getName(),
                                            getRegistry(),
                                            true));
        mnode->setHavePeerNodes(!m_peerPath.empty());
        m_props[true] = mnode;
        mnode->setNode(true, ConfigProperty::SOURCE_SET_SHARING,
                       m_contextHiddenNode);
        mnode->setNode(true, ConfigProperty::NO_SHARING,
                       m_hiddenPeerNode);
        mnode->setNode(true, ConfigProperty::GLOBAL_SHARING,
                       m_globalHiddenNode);
        break;
    }

    // read version check
    for (ConfigLevel level = CONFIG_LEVEL_ROOT;
         level < CONFIG_LEVEL_MAX;
         level = (ConfigLevel)(level + 1)) {
        if (exists(level)) {
            if (getConfigVersion(level, CONFIG_MIN_VERSION) > ConfigVersions[level][CONFIG_CUR_VERSION]) {
                SE_LOG_INFO(NULL, "config version check failed: %s has format %d, but this SyncEvolution release only supports format %d",
                            ConfigLevel2String(level).c_str(),
                            getConfigVersion(level, CONFIG_MIN_VERSION),
                            ConfigVersions[level][CONFIG_CUR_VERSION]);
                // our code is too old to read the config, reject it
                SE_THROW_EXCEPTION_STATUS(StatusException,
                                          StringPrintf("SyncEvolution %s is too old to read configuration '%s', please upgrade SyncEvolution.",
                                                       VERSION, peer.c_str()),
                                          STATUS_RELEASE_TOO_OLD);
            }
        }
    }

    // Note that the version check does not reject old configs because
    // they are too old; so far, any release must be able to read any
    // older config.
}

void SyncConfig::prepareConfigForWrite()
{
    // check versions before bumping to something incompatible with the
    // previous user of the config
    for (ConfigLevel level = CONFIG_LEVEL_ROOT;
         level < CONFIG_LEVEL_MAX;
         level = (ConfigLevel)(level + 1)) {
        if (getLayout() < SHARED_LAYOUT &&
            level < CONFIG_LEVEL_PEER) {
            // old configs do not have explicit root or context,
            // only check peer config itself
            continue;
        }
        if (exists(level)) {
            if (getConfigVersion(level, CONFIG_CUR_VERSION) < ConfigVersions[level][CONFIG_MIN_VERSION]) {
                // release which created config will no longer be able to read
                // updated config; either alert user or migrate automatically
                string config;
                switch (level) {
                case CONFIG_LEVEL_CONTEXT:
                    config = getContextName();
                    break;
                case CONFIG_LEVEL_PEER:
                    config = getConfigName();
                    break;
                case CONFIG_LEVEL_ROOT:
                case CONFIG_LEVEL_MAX:
                    // keep compiler happy, not reached for _MAX
                    break;
                }
                SE_LOG_INFO(NULL, "must change format of %s '%s' in backward-incompatible way",
                            ConfigLevel2String(level).c_str(),
                            config.c_str());
                if (m_configWriteMode == MIGRATE_AUTOMATICALLY) {
                    // migrate config and anything beneath it,
                    // so no further checking needed
                    migrate(config);
                    break;
                } else {
                    SE_THROW_EXCEPTION_STATUS(StatusException,
                                              StringPrintf("Proceeding would modify config '%s' such "
                                                           "that the previous SyncEvolution release "
                                                           "will not be able to use it. Stopping now. "
                                                           "Please explicitly acknowledge this step by "
                                                           "running the following command on the command "
                                                           "line: syncevolution --migrate '%s'",
                                                           config.c_str(),
                                                           config.c_str()),
                                              STATUS_MIGRATION_NEEDED);
                }
            }
        }
    }

    // now set current versions at all levels,
    // but without reducing versions: if a config has format
    // "cur = 10", then properties or features added in that
    // format remain even if the config is (temporarily?) used
    // by a SyncEvolution binary which has "cur = 5".
    for (ConfigLevel level = CONFIG_LEVEL_ROOT;
         level < CONFIG_LEVEL_MAX;
         level = (ConfigLevel)(level + 1)) {
        if (level == CONFIG_LEVEL_PEER &&
            m_peerPath.empty()) {
            // no need (and no possibility) to set per-peer version)
            break;
        }
        for (ConfigLimit limit = CONFIG_MIN_VERSION;
             limit < CONFIG_VERSION_MAX;
             limit = (ConfigLimit)(limit + 1)) {
            // set if equal to ensure that version == 0 (the default)
            // is set explicitly
            if (getConfigVersion(level, limit) <= ConfigVersions[level][limit]) {
                setConfigVersion(level, limit, ConfigVersions[level][limit]);
            }
        }
    }
    flush();
}

void SyncConfig::migrate(const std::string &config)
{
    if (config.empty()) {
        // migrating root not yet supported
        SE_THROW("internal error, migrating config root not implemented");
    } else {
        // migrate using the higher-level logic in the Cmdline class
        Cmdline migrate(m_peer.c_str(),
                        "--migrate",
                        config.c_str(),
                        NULL);
        bool res = migrate.parse() && migrate.run();
        if (!res) {
            SE_THROW(StringPrintf("migration of config '%s' failed", config.c_str()));
        }

        // files that our tree access may have changed, refresh our
        // in-memory copy
        m_tree->reload();
    }
}

string SyncConfig::getRootPath() const
{
    return m_fileTree ?
        normalizePath(m_fileTree->getRoot() + "/" +
                      ((m_layout == SYNC4J_LAYOUT || hasPeerProperties()) ? m_peerPath : m_contextPath)) :
        "";
}

void SyncConfig::addPeers(const string &root,
                          const std::string &configname,
                          SyncConfig::ConfigList &res) {
    FileConfigTree tree(root, SyncConfig::HTTP_SERVER_LAYOUT);
    list<string> servers = tree.getChildren("");
    BOOST_FOREACH(const string &server, servers) {
        // sanity check: only list server directories which actually
        // contain a configuration. To distinguish between a context
        // (~/.config/syncevolution/default) and an HTTP server config
        // (~/.config/syncevolution/scheduleworld), we check for the
        // "peer" subdirectory that is only in the former.
        //
        // Contexts which don't have a peer are therefore incorrectly
        // listed as a peer. Short of adding a special hidden file
        // this can't be fixed. This is probably overkill and thus not
        // done yet.
        string peerPath = server + "/peers";
        if (!access((root + "/" + peerPath).c_str(), F_OK)) {
            // not a real HTTP server, search for peers
            BOOST_FOREACH(const string &peer, tree.getChildren(peerPath)) {
                res.push_back(pair<string, string> (normalizeConfigString(peer + "@" + server),
                                                  root + "/" + peerPath + "/" + peer));
            }
        } else if (!access((root + "/" + server + "/" + configname).c_str(), F_OK)) {
            res.push_back(pair<string, string> (server, root + "/" + server));
        }
    }
}

/** returns true if a precedes b (strict weak ordering) */
static bool cmpConfigEntries(const StringPair &a, const StringPair &b)
{
    string peerA, contextA, peerB, contextB;
    SyncConfig::splitConfigString(a.first, peerA, contextA);
    SyncConfig::splitConfigString(b.first, peerB, contextB);
    int res;
    res = contextA.compare(contextB);
    if (res == 0) {
        res = peerA.compare(peerB);
        if (res == 0) {
            res = a.second.compare(b.second);
        }
    }
    return res < 0;
}

SyncConfig::ConfigList SyncConfig::getConfigs()
{
    ConfigList res;

    addPeers(getOldRoot(), "config.txt", res);
    addPeers(getNewRoot(), "config.ini", res);

    // Sort the list by (context, peer name, path);
    // better than returning it in random order.
    // This sort order (compared to simple lexical
    // sorting based on the full config name) has
    // the advantage that peer names or contexts with
    // suffix (foo.old vs. foo) come later.
    res.sort(cmpConfigEntries);

    return res;
}

static string SyncEvolutionTemplateDir()
{
    string templateDir(TEMPLATE_DIR);
    const char *envvar = getenv("SYNCEVOLUTION_TEMPLATE_DIR");
    if (envvar) {
        templateDir = envvar;
    }
    return templateDir;
}

SyncConfig::TemplateList SyncConfig::matchPeerTemplates(const DeviceList &peers, bool fuzzyMatch)
{
    TemplateList result;
    // match against all possible templates without any assumption on directory
    // layout, the match is entirely based on the metadata template.ini
    string templateDir(SyncEvolutionTemplateDir());
    std::queue <std::string, std::list<std::string> > directories;

    directories.push(templateDir);
    templateDir = SubstEnvironment("${XDG_CONFIG_HOME}/syncevolution-templates");
    directories.push(templateDir);
    while (!directories.empty()) {
        string sDir = directories.front();
        directories.pop();
        if (isDir(sDir)) {
            // check all sub directories
            ReadDir dir(sDir);
            BOOST_FOREACH(const string &entry, dir) {
                // ignore hidden files, . and ..
                if (!boost::starts_with(entry, ".")) {
                    directories.push(sDir + "/" + entry);
                }
            }
        } else {
            TemplateConfig templateConf (sDir);
            if (boost::ends_with(sDir, "~") ||
                !templateConf.isTemplateConfig()) {
                // ignore temporary files and files which do
                // not contain a valid template
                continue;
            }
            BOOST_FOREACH (const DeviceList::value_type &entry, peers){
                std::string fingerprint(entry.getFingerprint());
                // peerName should be empty if no reliable device info is on hand.
                std::string peerName = entry.m_pnpInformation ? fingerprint : "";

                int rank = templateConf.metaMatch (entry.getFingerprint(), entry.m_matchMode);
                if (fuzzyMatch){
                    if (rank > TemplateConfig::NO_MATCH) {
                        result.push_back (boost::shared_ptr<TemplateDescription>(
                                    new TemplateDescription(templateConf.getTemplateId(),
                                                            templateConf.getDescription(),
                                                            rank,
                                                            peerName,
                                                            entry.m_deviceId,
                                                            entry.m_deviceName,
                                                            sDir,
                                                            templateConf.getFingerprint(),
                                                            templateConf.getTemplateName()
                                                            )
                                    ));
                    }
                } else if (rank == TemplateConfig::BEST_MATCH){
                    result.push_back (boost::shared_ptr<TemplateDescription>(
                                new TemplateDescription(templateConf.getTemplateId(),
                                                        templateConf.getDescription(),
                                                        rank,
                                                        peerName,
                                                        entry.m_deviceId,
                                                        entry.m_deviceName,
                                                        sDir,
                                                        templateConf.getFingerprint(),
                                                        templateConf.getTemplateName())
                                ));
                    break;
                }
            }
        }
    }

    result.sort (TemplateDescription::compare_op);
    return result;
}


boost::shared_ptr<SyncConfig> SyncConfig::createPeerTemplate(const string &server)
{
    if (server.empty()) {
        // Empty template name => no such template. This check is
        // necessary because otherwise we end up with SyncConfig(""),
        // which is a configuration where peer-specific properties
        // cannot be set, triggering an errror in config->setDevID().
        return boost::shared_ptr<SyncConfig>();
    }

    // case insensitive search for read-only file template config
    string templateConfig(SyncEvolutionTemplateDir());

    // before starting another fuzzy match process, first try to load the
    // template directly taking the parameter as the path
    if (server == "none") {
        // nothing to read from, just set some defaults below
    } else if (TemplateConfig::isTemplateConfig(server)) {
        templateConfig = server;
    } else {
        SyncConfig::DeviceList devices;
        devices.push_back (DeviceDescription("", server, MATCH_ALL));
        templateConfig = "";
        TemplateList templates = matchPeerTemplates (devices, false);
        if (!templates.empty()) {
            templateConfig = templates.front()->m_path;
        }
        if (templateConfig.empty()) {
            // return "not found"
            return boost::shared_ptr<SyncConfig>();
        }
    }
    
    boost::shared_ptr<ConfigTree> tree(new SingleFileConfigTree(templateConfig));
    boost::shared_ptr<SyncConfig> config(new SyncConfig(server, tree));
    boost::shared_ptr<PersistentSyncSourceConfig> source;

    config->setDefaults(false);
    config->setDevID(string("syncevolution-") + UUID());

    // leave the rest empty for special "none" template
    if (server == "none") {
        return config;
    }

    // check for icon
    if (config->getIconURI().empty()) {
        string dirname, filename;
        splitPath(templateConfig, dirname, filename);
        ReadDir dir(getDirname(dirname));

        // remove last suffix, regardless what it is
        size_t pos = filename.rfind('.');
        if (pos != filename.npos) {
            filename.resize(pos);
        }
        filename += "-icon";

        BOOST_FOREACH(const string &entry, dir) {
            if (boost::istarts_with(entry, filename)) {
                config->setIconURI("file://" + dirname + "/" + entry);
                break;
            }
        }
    }

    // "default" maps to SyncEvolution server template, which is not
    // consumer ready. When used as "default" by the GTK sync UI,
    // the UI expects the "consumer ready" flag to be set. Do that
    // here. Also unset the peer name, because otherwise it shows
    // up in the UI.
    if (server == "default") {
        config->setConsumerReady(true);
        config->setUserPeerName(InitStateString());
    }

    return config;
}

bool SyncConfig::exists() const
{
    return m_peerPath.empty() ?
        m_contextNode->exists() :
        m_peerNode->exists();
}

bool SyncConfig::exists(ConfigLevel level) const
{
    switch (level) {
    case CONFIG_LEVEL_ROOT:
        return m_globalNode->exists();
        break;
    case CONFIG_LEVEL_CONTEXT:
        return m_contextNode->exists();
        break;
    case CONFIG_LEVEL_PEER:
        return m_peerNode->exists();
        break;
    default:
        return false;
    }
}

string SyncConfig::getContextName() const
{
    string peer, context;
    splitConfigString(getConfigName(), peer, context);
    return string("@") + context;
}

string SyncConfig::getPeerName() const
{
    string peer, context;
    splitConfigString(getConfigName(), peer, context);
    return peer;
}

list<string> SyncConfig::getPeers() const
{
    list<string> res;

    if (!hasPeerProperties()) {
        std::string rootPath = getRootPath();
        if (!rootPath.empty()) {
            FileConfigTree tree(getRootPath(), SHARED_LAYOUT);
            res = tree.getChildren("peers");
        }
    }

    return res;
}

void SyncConfig::preFlush(UserInterface &ui)
{
    /* Iterator over all sync global and source properties 
     * one by one and check whether they need to save password */

    /* save password in the global config node */
    ConfigPropertyRegistry& registry = getRegistry();
    BOOST_FOREACH(const ConfigProperty *prop, registry) {
        prop->savePassword(ui, *this);
    }

    /** grep each source and save their password */
    list<string> configuredSources = getSyncSources();
    BOOST_FOREACH(const string &sourceName, configuredSources) {
        //boost::shared_ptr<SyncSourceConfig> sc = getSyncSourceConfig(sourceName);
        ConfigPropertyRegistry& registry = SyncSourceConfig::getRegistry();
        SyncSourceNodes sourceNodes = getSyncSourceNodes(sourceName);

        BOOST_FOREACH(const ConfigProperty *prop, registry) {
            prop->savePassword(ui, *this, sourceName);
        }
    }
}

void SyncConfig::flush()
{
    if (!isEphemeral()) {
        if (m_fileTree && m_layout == SHARED_LAYOUT && !hasPeerProperties()) {
            // Ensure that "peers" directory exists for new-style
            // configs.  It would not get created when flushing nodes
            // for pure context configs otherwise (it's empty), and we
            // need it to detect new-syle configs.
            mkdir_p(m_fileTree->getRoot() + "/" + m_contextPath + "/peers");
        }
        m_tree->flush();
    }
}

void SyncConfig::remove()
{
    boost::shared_ptr<ConfigTree> tree = m_tree;

    // stop using the config nodes, they might get removed now
    makeVolatile();

    tree->remove(m_peerPath.empty() ?
                 m_contextPath :
                 m_peerPath);
}

boost::shared_ptr<PersistentSyncSourceConfig> SyncConfig::getSyncSourceConfig(const string &name)
{
    SyncSourceNodes nodes = getSyncSourceNodes(name);
    return boost::shared_ptr<PersistentSyncSourceConfig>(new PersistentSyncSourceConfig(name, nodes));
}

list<string> SyncConfig::getSyncSources() const
{
    // Return *all* sources configured in this context,
    // not just those configured for the peer. This
    // is necessary so that sources created for some other peer
    // show up for the current one, to prevent overwriting
    // existing properties unintentionally.
    // Returned sources are an union of:
    // 1. contextpath/sources
    // 2. peers/[one-peer]/sources
    // 3. sources in source filter
    set<string> sources;
    list<string> sourceList;
    if (m_layout == SHARED_LAYOUT) {
        // get sources in context
        sourceList = m_tree->getChildren(m_contextPath + "/sources");
        sources.insert(sourceList.begin(), sourceList.end());
        // get sources from peer if it's not empty and merge into
        // full set of sources
        if (!m_peerPath.empty()) {
            sourceList = m_tree->getChildren(m_peerPath + "/sources");
            sources.insert(sourceList.begin(), sourceList.end());
        }
    } else {
        // get sources from peer
        sourceList = m_tree->getChildren(m_peerPath +
                                         (m_layout == SYNC4J_LAYOUT ? 
                                          "/spds/sources" :
                                          "/sources"));
        sources.insert(sourceList.begin(), sourceList.end());
    }
    // get sources from filter and union them into returned sources
    BOOST_FOREACH(const SourceProps::value_type &value, m_sourceFilters) {
        if (value.first.empty()) {
            // ignore filter for all sources
            continue;
        }
        sources.insert(value.first);
    }

    // Convert back to simple list. As a nice side-effect of
    // temporarily using a set, the final list is sorted.
    return list<string>(sources.begin(), sources.end());
}

SyncSourceNodes SyncConfig::getSyncSourceNodes(const string &name,
                                               const string &changeId)
{
    if (m_nodeCache.find(name) != m_nodeCache.end()) {
        // reuse existing set of nodes
        return m_nodeCache[name];
    }

    /** shared source properties */
    boost::shared_ptr<FilterConfigNode> sharedNode;
    /** per-peer source properties */
    boost::shared_ptr<FilterConfigNode> peerNode;
    /** per-peer internal properties and meta data */
    boost::shared_ptr<ConfigNode> hiddenPeerNode,
        serverNode,
        trackingNode;
    string cacheDir;

    // store configs lower case even if the UI uses mixed case
    string lower = name;
    boost::to_lower(lower);

    boost::shared_ptr<ConfigNode> node;
    string sharedPath, peerPath;
    switch (m_layout) {
    case SYNC4J_LAYOUT:
        peerPath = m_peerPath + "/spds/sources/" + lower;
        break;
    case HTTP_SERVER_LAYOUT:
        peerPath = m_peerPath + "/sources/" + lower;
        break;
    case SHARED_LAYOUT:
        if (!m_peerPath.empty()) {
            peerPath = m_peerPath + "/sources/" + lower;
        }
        sharedPath = m_contextPath + string("/sources/") + lower;
        break;
    }

    // Compatibility mode for reading configs which have "type" instead
    // of "backend/databaseFormat/syncFormat/forceSyncFormat": determine
    // the new values based on the old property, then inject the new
    // values into the SyncSourceNodes by adding an intermediate layer
    // of FilterConfigNodes. The top FilterConfigNode layer is the one
    // which might get modified, the one underneath remains hidden and
    // thus preserves the new values even if the caller does a setFilter().
    bool compatMode = getConfigVersion(CONFIG_LEVEL_CONTEXT, CONFIG_CUR_VERSION) < 1;
    SourceType sourceType;
    if (compatMode) {
        node = m_tree->open(peerPath.empty() ? sharedPath : peerPath, ConfigTree::visible);
        string type;
        if (node->getProperty("type", type)) {
            sourceType = SourceType(type);
        } else {
            // not set: avoid compatibility mode
            compatMode = false;
        }
    }

    if (peerPath.empty()) {
        node.reset(new DevNullConfigNode(m_contextPath + " without peer configuration"));
        peerNode = ConfigCache::singleton().createNode(node);
        hiddenPeerNode =
            trackingNode =
            serverNode = node;
    } else {
        // Here we assume that m_tree is a FileConfigTree. Otherwise getRootPath()
        // will not point into a normal file system. We fall back to not allowing
        // the usage of a cache dir by using /dev/null in that case.
        std::string rootPath = getRootPath();
        if (rootPath.empty()) {
            cacheDir = "/dev/null";
        } else {
            cacheDir = rootPath + "/" + peerPath + "/.cache";
        }

        node = m_tree->open(peerPath, ConfigTree::visible);
        if (compatMode) {
            boost::shared_ptr<FilterConfigNode> compat(new FilterConfigNode(node));
            compat->addFilter("syncFormat",
                              InitStateString(sourceType.m_format, !sourceType.m_format.empty()));
            compat->addFilter("forceSyncFormat",
                              sourceType.m_forceFormat ?
                              InitStateString("1", true) :
                              InitStateString("0", false));
            if (sharedPath.empty()) {
                compat->addFilter("databaseFormat",
                                  InitStateString(sourceType.m_localFormat, !sourceType.m_localFormat.empty()));
                compat->addFilter("backend",
                                  InitStateString(sourceType.m_backend, !sourceType.m_backend.empty()));
            }
            node = compat;
        }
        peerNode = ConfigCache::singleton().createNode(node, m_sourceFilters.createSourceFilter(name));
        hiddenPeerNode = m_tree->open(peerPath, ConfigTree::hidden);
        trackingNode = m_tree->open(peerPath, ConfigTree::other, changeId);
        serverNode = m_tree->open(peerPath, ConfigTree::server, changeId);
    }

    if (isEphemeral()) {
        // Throw away meta data.
        trackingNode.reset(new VolatileConfigNode);
        hiddenPeerNode.reset(new VolatileConfigNode);
        serverNode.reset(new VolatileConfigNode);
    } else if (!m_redirectPeerRootPath.empty()) {
        // Local sync: overwrite per-peer nodes with nodes inside the
        // parents tree. Otherwise different configs syncing locally
        // against the same context end up sharing .internal.ini and
        // .other.ini files inside that context.
        string path = m_redirectPeerRootPath + "/sources/" + lower;
        trackingNode.reset(new IniHashConfigNode(path,
                                                 ".other.ini",
                                                 false));
        trackingNode = m_tree->add(path + "/.other.ini", trackingNode);
        if (peerPath.empty()) {
            hiddenPeerNode = peerNode;
        } else {
            hiddenPeerNode = boost::static_pointer_cast<FilterConfigNode>(m_tree->add(path + "/.internal.ini", peerNode));
        }
    }

    if (sharedPath.empty()) {
        sharedNode = peerNode;
    } else {
        node = m_tree->open(sharedPath, ConfigTree::visible);
        if (compatMode) {
            boost::shared_ptr<FilterConfigNode> compat(new FilterConfigNode(node));
            compat->addFilter("databaseFormat",
                              InitStateString(sourceType.m_localFormat, !sourceType.m_localFormat.empty()));
            compat->addFilter("backend",
                              InitStateString(sourceType.m_backend, !sourceType.m_backend.empty()));
            node = compat;
        }
        sharedNode = ConfigCache::singleton().createNode(node, m_sourceFilters.createSourceFilter(name));
    }

    SyncSourceNodes nodes(!peerPath.empty(), sharedNode, peerNode, hiddenPeerNode, trackingNode, serverNode, cacheDir);
    m_nodeCache.insert(make_pair(name, nodes));
    return nodes;
}

ConstSyncSourceNodes SyncConfig::getSyncSourceNodes(const string &name,
                                                    const string &changeId) const
{
    return const_cast<SyncConfig *>(this)->getSyncSourceNodes(name, changeId);
}

SyncSourceNodes SyncConfig::getSyncSourceNodesNoTracking(const string &name)
{
    SyncSourceNodes nodes = getSyncSourceNodes(name);
    boost::shared_ptr<ConfigNode> dummy(new VolatileConfigNode());
    return SyncSourceNodes(nodes.m_havePeerNode,
                           nodes.m_sharedNode,
                           nodes.m_peerNode,
                           nodes.m_hiddenPeerNode,
                           dummy,
                           nodes.m_serverNode,
                           nodes.m_cacheDir);
}

static ConfigProperty syncPropSyncURL("syncURL",
                                      "Identifies how to contact the peer,\n"
                                      "best explained with some examples.\n\n"
                                      "HTTP(S) SyncML servers::\n\n"
                                      "  http://example.com/sync\n\n"
                                      "OBEX over Bluetooth uses the MAC address, with\n"
                                      "the channel chosen automatically::\n\n"
                                      "  obex-bt://00:0A:94:03:F3:7E\n\n"
                                      "If the automatism fails, the channel can also be specified::\n\n"
                                      "  obex-bt://00:0A:94:03:F3:7E+16\n\n"
                                      "For peers contacting us via Bluetooth, the MAC address is\n"
                                      "used to identify it before the sync starts. Multiple\n"
                                      "urls can be specified in one syncURL property::\n\n"
                                      "  obex-bt://00:0A:94:03:F3:7E obex-bt://00:01:02:03:04:05\n\n"
                                      "In the future this might be used to contact the peer\n"
                                      "via one of several transports; right now, only the first\n"
                                      "one is tried." // MB #9446
                                      );

static ConfigProperty syncPropDevID("deviceId",
                                    "The SyncML server gets this string and will use it to keep track of\n"
                                    "changes that still need to be synchronized with this particular\n"
                                    "client; it must be set to something unique (like the pseudo-random\n"
                                    "string created automatically for new configurations) among all clients\n"
                                    "accessing the same server.\n"
                                    "myFUNAMBOL also requires that the string starts with sc-pim-");
static ConfigProperty syncPropUsername("username",
                                       "user name used for authorization with the SyncML server",
                                       "");

static BoolConfigProperty syncPropPeerIsClient("PeerIsClient",
                                          "Indicates whether this configuration is about a\n"
                                          "client peer or server peer.\n",
                                          "FALSE");
static SafeConfigProperty syncPropRemoteDevID("remoteDeviceId",
                                              "SyncML ID of our peer, empty if unknown; must be set only when\n"
                                              "the peer is a SyncML client contacting us via HTTP.\n"
                                              "Clients contacting us via OBEX/Bluetooth can be identified\n"
                                              "either via this remoteDeviceId property or by their MAC\n"
                                              "address, if that was set in the syncURL property.\n"
                                              "\n"
                                              "If this property is empty and the peer synchronizes with\n"
                                              "this configuration chosen by some other means, then its ID\n"
                                              "is recorded here automatically and later used to verify that\n"
                                              "the configuration is not accidentally used by a different\n"
                                              "peer.");

static class SyncPasswordConfigProperty : public PasswordConfigProperty
{
public:
    SyncPasswordConfigProperty() :
        PasswordConfigProperty("password",
                               "password used for authorization with the peer;\n"
                               "in addition to specifying it directly as plain text, it can\n"
                               "also be read from the standard input or from an environment\n"
                               "variable of your choice::\n\n"
                               "  plain text  : password = <insert your password here>\n"
                               "  ask         : password = -\n"
                               "  env variable: password = ${<name of environment variable>}\n")
    {}

    virtual void checkPassword(UserInterface &ui,
                               SyncConfig &config,
                               int flags,
                               const std::string &sourceName = "") const {
        PasswordConfigProperty::checkPassword(ui, config, flags, syncPropUsername, sourceName);
    }

    virtual void savePassword(UserInterface &ui,
                              SyncConfig &config,
                              const std::string &sourceName = "") const {
        PasswordConfigProperty::savePassword(ui, config, syncPropUsername, sourceName);
    }

    ConfigPasswordKey getPasswordKey(const string &descr,
                                     const string &serverName,
                                     FilterConfigNode &globalConfigNode,
                                     const string &sourceName,
                                     const boost::shared_ptr<FilterConfigNode> &sourceConfigNode) const {
        ConfigPasswordKey key;

        bool peerIsClient = syncPropPeerIsClient.getPropertyValue(globalConfigNode);
        key.server = syncPropSyncURL.getProperty(globalConfigNode);
        key.description = StringPrintf("sync password for %s", descr.c_str());
        if (peerIsClient && key.server.empty()) {
            /**
             * Fall back to username/remoteDeviceId as key.
             */
            key.server = syncPropRemoteDevID.getProperty(globalConfigNode);
        } else {
            /**
             * Here we use server sync url without protocol prefix and
             * user account name as the key in the keyring.
             * The URL must not be empty, otherwise we end up
             * overwriting the password of some other service just
             * because it happens to have the same username.
             */
            size_t start = key.server.find("://");
            /* we don't preserve protocol prefix for it may change */
            if (start != key.server.npos) {
                key.server = key.server.substr(start + 3);
            }
        }
        key.user = getUsername(syncPropUsername, globalConfigNode);
        // User domain part of a username which looks like an email address
        // as server name if we don't have something better. Needed for
        // WebDAV configs using just an email address to locate the server.
        if (key.server.empty()) {
            size_t offset = key.user.find('@');
            if (offset != key.user.npos &&
                offset != key.user.size() - 1) {
                key.server = key.user.substr(offset + 1);
                key.user.resize(offset);
            }
        }
        return key;
    }
} syncPropPassword;

static BoolConfigProperty syncPropPreventSlowSync("preventSlowSync",
                                                  "During a slow sync, the SyncML server must match all items\n"
                                                  "of the client with its own items and detect which ones it\n"
                                                  "already has based on properties of the items. This is slow\n"
                                                  "(client must send all its data) and can lead to duplicates\n"
                                                  "(when the server fails to match correctly).\n"
                                                  "It is therefore sometimes desirable to wipe out data on one\n"
                                                  "side with a refresh-from-client/server sync instead of doing\n"
                                                  "a slow sync.\n"
                                                  "When this option is enabled, slow syncs that could cause problems\n"
                                                  "are not allowed to proceed. Instead, the affected datastores are\n"
                                                  "skipped, allowing the user to choose a suitable sync mode in\n"
                                                  "the next run (slow sync selected explicitly, refresh sync).\n"
                                                  "The following situations are handled:\n\n"
                                                  "- running as client with no local data => unproblematic,\n"
                                                  "  slow sync is allowed to proceed automatically\n"
                                                  "- running as client with local data => client has no\n"
                                                  "  information about server, so slow sync might be problematic\n"
                                                  "  and is prevented\n"
                                                  "- client has data, server asks for slow sync because all its data\n"
                                                  "  was deleted (done by Memotoo and Mobical, because they treat\n"
                                                  "  this as 'user wants to start from scratch') => the sync would\n"
                                                  "  recreate all the client's data, even if the user really wanted\n"
                                                  "  to have it deleted, therefore slow sync is prevented\n",
                                                  "TRUE");
static BoolConfigProperty syncPropUseProxy("useProxy",
                                           "set to T to choose an HTTP proxy explicitly; otherwise the default\n"
                                           "proxy settings of the underlying HTTP transport mechanism are used;\n"
                                           "only relevant when contacting the peer via HTTP",
                                           "FALSE");
static ConfigProperty syncPropProxyHost("proxyHost",
                                        "proxy URL (``http://<host>:<port>``)");
static ConfigProperty syncPropProxyUsername("proxyUsername",
                                            "authentication for proxy: username");

static class ProxyPasswordConfigProperty : public PasswordConfigProperty {
public:
    ProxyPasswordConfigProperty() :
        PasswordConfigProperty("proxyPassword",
                               "proxy password, can be specified in different ways,\n"
                               "see SyncML server password for details\n",
                               "",
                               "proxy")
    {}
    /**
     * re-implement this function for it is necessary to do a check 
     * before retrieving proxy password
     */
    virtual void checkPassword(UserInterface &ui,
                               SyncConfig &config,
                               int flags,
                               const std::string &sourceName = std::string()) const {
        /* if useProxy is set 'true', then check proxypassword */
        if (config.getUseProxy()) {
            PasswordConfigProperty::checkPassword(ui, config, flags, syncPropProxyUsername, sourceName);
        }
    }
    virtual void savePassword(UserInterface &ui,
                               SyncConfig &config,
                               const std::string &sourceName = std::string()) const {
        PasswordConfigProperty::savePassword(ui, config, syncPropProxyUsername, sourceName);
    }
    virtual ConfigPasswordKey getPasswordKey(const std::string &descr,
                                             const std::string &serverName,
                                             FilterConfigNode &globalConfigNode,
                                             const std::string &sourceName,
                                             const boost::shared_ptr<FilterConfigNode> &sourceConfigNode) const {
        ConfigPasswordKey key;
        key.server = syncPropProxyHost.getProperty(globalConfigNode);
        key.user = getUsername(syncPropProxyUsername, globalConfigNode);
        key.description = StringPrintf("proxy password for %s", descr.c_str());
        return key;
    }
} syncPropProxyPassword;

static StringConfigProperty syncPropClientAuthType("clientAuthType",
                                                   "- empty or \"md5\" for secure method (recommended)\n"
                                                   "- \"basic\" for insecure method\n"
                                                   "\n"
                                                   "This setting is only for debugging purpose and only\n"
                                                   "has an effect during the initial sync of a client.\n"
                                                   "Later it remembers the method that was supported by\n"
                                                   "the server and uses that. When acting as server,\n"
                                                   "clients contacting us can use both basic and md5\n"
                                                   "authentication.\n",
                                                   "md5",
                                                   "",
                                                   Values() +
                                                   (Aliases("basic") + "syncml:auth-basic") +
                                                   (Aliases("md5") + "syncml:auth-md5" + ""));
static ULongConfigProperty syncPropMaxMsgSize(SyncMaxMsgSize,
                                              "The maximum size of each message can be set (maxMsgSize) and the\n"
                                              "peer can be told to never sent items larger than a certain\n"
                                              "threshold (maxObjSize). Presumably the peer has to truncate or\n"
                                              "skip larger items. Sizes are specified as number of bytes.",
                                              "150000");
static UIntConfigProperty syncPropMaxObjSize("maxObjSize", "", "4000000");

static BoolConfigProperty syncPropWBXML("enableWBXML",
                                        "use the more compact binary XML (WBXML) for messages between client and server;\n"
                                        "not applicable when the peer is a SyncML client, because then the client\n"
                                        "chooses the encoding",
                                        "TRUE");
static BoolConfigProperty syncPropRefreshSync("enableRefreshSync",
                                              "Use the more advanced refresh-from-server sync mode to\n"
                                              "implement the refresh-from-remote operation. Some SyncML\n"
                                              "servers do not support this. Therefore the default is to\n"
                                              "delete local data before doing a slow sync, which has the\n"
                                              "same effect. However, some servers work better when they\n"
                                              "are told explicitly that the sync is a refresh sync. For\n"
                                              "example, Funambol's One Media server rejects too many slow\n"
                                              "syncs in a row with a 417 'retry later' error.\n",
                                              "FALSE");
static ConfigProperty syncPropLogDir("logdir",
                                     "full path to directory where automatic backups and logs\n"
                                     "are stored for all synchronizations; if unset, then\n"
                                     "\"${XDG_CACHE_HOME}/syncevolution/<server>\" (which\n"
                                     "usually expands to ${HOME}/.cache/...) will be used;\n"
                                     "if \"none\", then no backups of the databases are made and any\n"
                                     "output is printed directly to the screen");
static UIntConfigProperty syncPropMaxLogDirs("maxlogdirs",
                                             "Controls how many session directories are kept at most in the logdir.\n"
                                             "Unless set to zero, SyncEvolution will remove old directories and\n"
                                             "all their content to prevent the number of log directories from\n"
                                             "growing beyond the given limit. It tries to be intelligent and will\n"
                                             "remove sessions in which nothing interesting happened (no errors,\n"
                                             "no data changes) in favor of keeping sessions where something\n"
                                             "happened, even if those sessions are older.",
                                             "10");
static UIntConfigProperty syncPropLogLevel("loglevel",
                                          "level of detail for log messages:\n"
                                          "- 0 (or unset) = INFO messages without log file, DEBUG with log file\n"
                                          "- 1 = only ERROR messages\n"
                                          "- 2 = also INFO messages\n"
                                          "- 3 = also DEBUG messages\n"
                                          "> 3 = increasing amounts of debug messages for developers");
static UIntConfigProperty syncPropNotifyLevel("notifyLevel",
                                              "Level of detail for desktop notifications. Currently such\n"
                                              "notifications are generated only for automatically started\n"
                                              "sync sessions.\n"
                                              "\n"
                                              "0 - suppress all notifications\n"
                                              "1 - show only errors\n"
                                              "2 - show information about changes and errors (in practice currently the same as level 3)\n"
                                              "3 - show all notifications, including starting a sync\n",
                                              "3");
static BoolConfigProperty syncPropPrintChanges("printChanges",
                                               "enables or disables the detailed (and sometimes slow) comparison\n"
                                               "of database content before and after a sync session",
                                               "TRUE");
static BoolConfigProperty syncPropDumpData("dumpData",
                                           "enables or disables the automatic backup of database content\n"
                                           "before and after a sync session (always enabled if printChanges is enabled)",
                                           "TRUE");
static SecondsConfigProperty syncPropRetryDuration("RetryDuration",
                                          "The total amount of time in seconds in which the SyncML\n"
                                          "client tries to get a response from the server.\n"
                                          "During this time, the client will resend messages\n"
                                          "in regular intervals (RetryInterval) if no response\n"
                                          "is received or the message could not be delivered due\n"
                                          "to transport problems. When this time is exceeded\n"
                                          "without a response, the synchronization aborts without\n"
                                          "sending further messages to the server.\n"
                                          "\n"
                                          "When acting as server, this setting controls how long\n"
                                          "a client is allowed to not send a message before the\n"
                                          "synchronization is aborted."
                                          ,"5M");
static SecondsConfigProperty syncPropRetryInterval("RetryInterval",
                                          "The number of seconds between the start of SyncML message sending\n"
                                          "and the start of the retransmission. If the interval has\n"
                                          "already passed when a message send returns, the\n"
                                          "message is resent immediately. Resending without\n"
                                          "any delay will never succeed and therefore specifying 0\n"
                                          "disables retries.\n"
                                          "\n"
                                          "Servers cannot resend messages, so this setting has no\n"
                                          "effect in that case.\n"
                                          "\n"
                                          "The WebDAV backend also resends messages after a temporary\n"
                                          "network error. It uses exponential backoff to determine when\n"
                                          "the server is available again. This setting is divided by 24\n"
                                          "to obtain the initial delay (default: 2m => 5s), which is then\n"
                                          "doubled for each retry."
                                          ,"2M");
static SafeConfigProperty syncPropPeerName("PeerName",
                                           "An arbitrary name for the peer referenced by this config.\n"
                                           "Might be used by a GUI. The command line tool always uses the\n"
                                           "the configuration name.");
static ConfigProperty syncPropSyncMLVersion("SyncMLVersion",
                                            "On a client, the latest commonly supported SyncML version\n"
                                            "is used when contacting a server. One of '1.0/1.1/1.2' can\n"
                                            "be used to pick a specific version explicitly.\n"
                                            "\n"
                                            "On a server, this option controls what kind of Server Alerted\n"
                                            "Notification is sent to the client to start a synchronization.\n"
                                            "By default, first the format from 1.2 is tried, then in case\n"
                                            "of failure, the older one from 1.1. 1.2/1.1 can be set\n"
                                            "explicitly, which disables the automatism.\n"
                                            "\n"
                                            "Instead or in adddition to the version, several keywords can\n"
                                            "be set in this property (separated by spaces or commas):\n"
                                            "\n"
                                            "- NOCTCAP - avoid sending CtCap meta information\n"
                                            "- NORESTART - disable the sync mode extension that SyncEvolution\n"
                                            "  client and server use to negotiate whether both sides support\n"
                                            "  running multiple sync iterations in the same session\n"
                                            "- REQUESTMAXTIME=<time> - override the rate at which the\n"
                                            "  SyncML server sends preliminary replies while preparing\n"
                                            "  local storages in the background. This helps to avoid timeouts\n"
                                            "  in the SyncML client. Depends on multithreading.\n"
#ifdef HAVE_THREAD_SUPPORT
                                            // test-dbus.py checks for 'is thread-safe'!
                                            "  This SyncEvolution binary is thread-safe and thus this feature\n"
                                            "  is enabled by default for HTTP servers, with a delay of 2 minutes\n"
                                            "  between messages. Other servers (Bluetooth, local sync) should not\n"
                                            "  need preliminary replies and the feature is disabled, although\n"
                                            "  it can be enabled by setting the time explicitly.\n"
#else
                                            "  This SyncEvolution binary is not thread-safe and thus this feature\n"
                                            "  is disabled by default, although it can be enabled if absolutely\n"
                                            "  needed by setting the time explicitly.\n"
#endif
                                            "  <time> can be specified like other durations in the config,\n"
                                            "  for example as REQUESTMAXTIME=2m.\n"
                                            "\n"
                                            "Setting these flags should only be necessary as workaround for\n"
                                            "broken peers.\n"
                                            );

static ConfigProperty syncPropRemoteIdentifier("remoteIdentifier",
                                      "the identifier sent to the remote peer for a server initiated sync.\n"
                                      "if not set, deviceId will be used instead\n",
                                      "");
static ConfigProperty syncPropSSLServerCertificates("SSLServerCertificates",
                                                    "A string specifying the location of the certificates\n"
                                                    "used to authenticate the server. When empty, the\n"
                                                    "system's default location will be searched.\n"
                                                    "\n"
                                                    "SSL support when acting as HTTP server is implemented\n"
                                                    "by the HTTP server frontend, not with these properties.",
                                                    SYNCEVOLUTION_SSL_SERVER_CERTIFICATES);
static BoolConfigProperty syncPropSSLVerifyServer("SSLVerifyServer",
                                                  "The client refuses to establish the connection unless\n"
                                                  "the server presents a valid certificate. Disabling this\n"
                                                  "option considerably reduces the security of SSL\n"
                                                  "(man-in-the-middle attacks become possible) and is not\n"
                                                  "recommended.\n",
                                                  "TRUE");
static BoolConfigProperty syncPropSSLVerifyHost("SSLVerifyHost",
                                                "The client refuses to establish the connection unless the\n"
                                                "server's certificate matches its host name. In cases where\n"
                                                "the certificate still seems to be valid it might make sense\n"
                                                "to disable this option and allow such connections.\n",
                                                "TRUE");

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
                                                "FALSE");

/**
 * Some guidelines for peerType = WebDAV:
 * - Such templates may only be used to create the 'target-config@<target>.
 *   configurations. Typically <target> can be the same as the template's
 *   name.
 * - Because determining the default database in WebDAV can be difficult,
 *   the GUI should allow the user to choose and set the "uri"
 *   properties accordingly.
 * - A GUI should also create a <target> configuration for synchronizing
 *   against the WebDAV backends.
 */
static ConfigProperty syncPropPeerType("peerType",
                                       "Defines what a configuration is meant to be used for.\n"
                                       "Used in templates and the resulting configs to tell a GUI\n"
                                       "that special handling may be necessary. GUIs should ignore\n"
                                       "unknown types.\n"
                                       "The traditional SyncML configs use an empty value.\n"
                                       "\"WebDAV\" is used for the WebDAV side in a local synchronization.\n");

static ULongConfigProperty syncPropHashCode("HashCode", "used by the SyncML library internally; do not modify");

static ConfigProperty syncPropConfigDate("ConfigDate", "used by the SyncML library internally; do not modify");

static SafeConfigProperty syncPropNonce("lastNonce",
                                        "MD5 nonce of our peer, empty if not set yet; do not edit, used internally");

// used both as source and sync property, internal in both cases
static SafeConfigProperty syncPropDeviceData("deviceData",
                                             "information about the peer in the format described in the\n"
                                             "Synthesis SDK manual under 'Session_SaveDeviceInfo'");

static SafeConfigProperty globalPropDefaultPeer("defaultPeer",
                                                "the peer which is used by default in some frontends, like the sync-UI");

static ConfigProperty globalPropKeyring("keyring",
                                        "Explicitly selects a certain safe password storage.\n"
                                        "Depending on how SyncEvolution was compiled and installed\n"
                                        "the following values are possible:\n"
                                        "\n"
                                        "GNOME\n  GNOME Keyring\n"
                                        "KDE\n  KWallet\n"
                                        "yes/true/1\n  pick one automatically\n"
                                        "no/false/0\n  store passwords in SyncEvolution config files\n"
                                        "\n"
                                        "If unset, the default is to pick one automatically if support\n"
                                        "for any kind of password storage was enabled and use the config files\n"
                                        "otherwise. When choosing automatically, GNOME keyring is tried\n"
                                        "first because distinguishing between KDE and GNOME sessions\n"
                                        "automatically is tricky.\n"
                                        "\n"
                                        "Note that using this option applies to *all* passwords in\n"
                                        "a configuration and that the --keyring command line option\n"
                                        "is merely an alias for setting the global property, so setting\n"
                                        "a single password as follows sets both `keyring` and\n"
                                        "`proxyPasswords`, and also moves the other passwords into the\n"
                                        "keyring, even if they were not stored there already:\n"
                                        "\n"
                                        "     --keyring --configure proxyPassword=foo\n"
                                        "\n"
                                        "When passwords were stored in a safe storage, their value is set to a single\n"
                                        "hyphen (\"-\") in the configuration. This means that when running a\n"
                                        "synchronization without using the storage, the password has to be\n"
                                        "entered interactively. The --print-config output always shows \"-\" instead\n"
                                        "of retrieving the password from the keyring.\n",
#ifdef HAVE_KEYRING
                                        "yes"
#else
                                        "no"
#endif
                                        );

static StringConfigProperty syncPropAutoSync("autoSync",
                                             "Controls automatic synchronization. Currently,\n"
                                             "automatic synchronization is done by running\n"
                                             "a synchronization at regular intervals. This\n"
                                             "may drain the battery, in particular when\n"
                                             "using Bluetooth!\n"
                                             "Because a peer might be reachable via different\n"
                                             "transports at some point, this option provides\n"
                                             "detailed control over which transports may\n"
                                             "be used for automatic synchronization:\n\n"
                                             "0\n  don't do auto sync\n"
                                             "1\n  do automatic sync, using whatever transport\n"
                                             "    is available\n"
                                             "http\n  only via HTTP transport\n"
                                             "obex-bt\n  only via Bluetooth transport\n"
                                             "http,obex-bt\n  pick one of these\n",
                                             "0");

static SecondsConfigProperty syncPropAutoSyncInterval("autoSyncInterval",
                                                      "This is the minimum number of seconds since the start of\n"
                                                      "the last synchronization that has to pass before starting\n"
                                                      "an automatic synchronization. Can be specified using\n"
                                                      "a 1h30m5s format.\n"
                                                      "\n"
                                                      "Before reducing this interval, consider that it will\n"
                                                      "increase resource consumption on the local and remote\n"
                                                      "side. Some SyncML server operators only allow a\n"
                                                      "certain number of sessions per day.\n"
                                                      "The value 0 has the effect of only running automatic\n"
                                                      "synchronization when changes are detected (not\n"
                                                      "implemented yet, therefore it basically disables\n"
                                                      "automatic synchronization).\n",
                                                      "30M");

static SecondsConfigProperty syncPropAutoSyncDelay("autoSyncDelay",
                                                   "An automatic sync will not be started unless the peer\n"
                                                   "has been available for this duration, specified in seconds\n"
                                                   "or 1h30m5s format.\n"
                                                   "\n"
                                                   "This prevents running a sync when network connectivity\n"
                                                   "is unreliable or was recently established for some\n"
                                                   "other purpose. It is also a heuristic that attempts\n"
                                                   "to predict how long connectivity be available in the\n"
                                                   "future, because it should better be available long\n"
                                                   "enough to complete the synchronization.\n",
                                                   "5M");

/* config and on-disk file versionsing */
static IntConfigProperty propRootMinVersion("rootMinVersion", "");
static IntConfigProperty propRootCurVersion("rootCurVersion", "");
static IntConfigProperty propContextMinVersion("contextMinVersion", "");
static IntConfigProperty propContextCurVersion("contextCurVersion", "");
static IntConfigProperty propPeerMinVersion("peerMinVersion", "");
static IntConfigProperty propPeerCurVersion("peerCurVersion", "");

static const IntConfigProperty *configVersioning[CONFIG_LEVEL_MAX][CONFIG_VERSION_MAX] = {
    { &propRootMinVersion, &propRootCurVersion },
    { &propContextMinVersion, &propContextCurVersion },
    { &propPeerMinVersion, &propPeerCurVersion }
};

static const IntConfigProperty &getConfigVersionProp(ConfigLevel level, ConfigLimit limit)
{
    if (level < 0 || level >= CONFIG_LEVEL_MAX ||
        limit < 0 || limit >= CONFIG_VERSION_MAX) {
        SE_THROW("getConfigVersionProp: invalid args");
    }
    return *configVersioning[level][limit];
}

int SyncConfig::getConfigVersion(ConfigLevel level, ConfigLimit limit) const
{
    const IntConfigProperty &prop = getConfigVersionProp(level, limit);
    return prop.getPropertyValue(*getNode(prop));
}

void SyncConfig::setConfigVersion(ConfigLevel level, ConfigLimit limit, int version)
{
    if (m_layout != SHARED_LAYOUT) {
        // old-style layouts have version 0 by default, no need
        // (and sometimes no possibility) to set this explicitly
        if (version != 0) {
            SE_THROW(StringPrintf("cannot bump config version in old-style config %s", m_peer.c_str()));
        }
    } else {
        const IntConfigProperty &prop = getConfigVersionProp(level, limit);
        prop.setProperty(*getNode(prop), version);
    }
}



/**
 * This constructor updates some of the properties above and then adds
 * them to the registry. This cannot be done inside getRegistry()
 * itself because that function may be invoked by other global
 * instances before the properties above were constructed (BMC
 * #19464).
 */
static class RegisterSyncConfigProperties
{
public:
    RegisterSyncConfigProperties()
    {
        ConfigPropertyRegistry &registry = SyncConfig::getRegistry();

	// temporarily move existing properties away so that the important
	// standard properties come first when using the traditional
	// push_back() way of adding them
	ConfigPropertyRegistry tmp;
	std::swap(registry, tmp);

        registry.push_back(&syncPropSyncURL);
        registry.push_back(&syncPropUsername);
        registry.push_back(&syncPropPassword);
        registry.push_back(&syncPropLogDir);
        registry.push_back(&syncPropLogLevel);
        registry.push_back(&syncPropNotifyLevel);
        registry.push_back(&syncPropPrintChanges);
        registry.push_back(&syncPropDumpData);
        registry.push_back(&syncPropMaxLogDirs);
        registry.push_back(&syncPropAutoSync);
        registry.push_back(&syncPropAutoSyncInterval);
        registry.push_back(&syncPropAutoSyncDelay);
        registry.push_back(&syncPropPreventSlowSync);
        registry.push_back(&syncPropUseProxy);
        registry.push_back(&syncPropProxyHost);
        registry.push_back(&syncPropProxyUsername);
        registry.push_back(&syncPropProxyPassword);
        registry.push_back(&syncPropClientAuthType);
        registry.push_back(&syncPropRetryDuration);
        registry.push_back(&syncPropRetryInterval);
        registry.push_back(&syncPropRemoteIdentifier);
        registry.push_back(&syncPropPeerIsClient);
        registry.push_back(&syncPropSyncMLVersion);
        registry.push_back(&syncPropPeerName);
        registry.push_back(&syncPropDevID);
        registry.push_back(&syncPropRemoteDevID);
        registry.push_back(&syncPropWBXML);
        registry.push_back(&syncPropRefreshSync);
        registry.push_back(&syncPropMaxMsgSize);
        registry.push_back(&syncPropMaxObjSize);
        registry.push_back(&syncPropSSLServerCertificates);
        registry.push_back(&syncPropSSLVerifyServer);
        registry.push_back(&syncPropSSLVerifyHost);
        registry.push_back(&syncPropWebURL);
        registry.push_back(&syncPropIconURI);
        registry.push_back(&syncPropConsumerReady);
        registry.push_back(&syncPropPeerType);
        registry.push_back(&syncPropHashCode);
        registry.push_back(&syncPropConfigDate);
        registry.push_back(&syncPropNonce);
        registry.push_back(&syncPropDeviceData);
        registry.push_back(&globalPropDefaultPeer);
        registry.push_back(&globalPropKeyring);

#if 0
        // Must not be registered! Not valid for --sync-property and
        // must not be copied between configs.
        registry.push_back(&syncPropRootMinVersion);
        registry.push_back(&syncPropRootCurVersion);
        registry.push_back(&syncPropContextMinVersion);
        registry.push_back(&syncPropContextCurVersion);
        registry.push_back(&syncPropPeerMinVersion);
        registry.push_back(&syncPropPeerCurVersion);
#endif

        BOOST_FOREACH (const ConfigProperty *prop, tmp) {
            registry.push_back(prop);
        }

        // obligatory sync properties
        //
        // username/password used to be
        // considered obligatory, but are not anymore because there are
        // cases where they are not needed (local sync, Bluetooth)
        // syncPropUsername.setObligatory(true);
        // syncPropPassword.setObligatory(true);
        //
        // created if not given:
        // syncPropDevID.setObligatory(true);
        syncPropSyncURL.setObligatory(true);

        // hidden sync properties
        syncPropHashCode.setHidden(true);
        syncPropConfigDate.setHidden(true);
        syncPropNonce.setHidden(true);
        syncPropDeviceData.setHidden(true);
        propRootMinVersion.setHidden(true);
        propRootCurVersion.setHidden(true);
        propContextMinVersion.setHidden(true);
        propContextCurVersion.setHidden(true);
        propPeerMinVersion.setHidden(true);
        propPeerCurVersion.setHidden(true);

        // global sync properties
        globalPropDefaultPeer.setSharing(ConfigProperty::GLOBAL_SHARING);
        globalPropKeyring.setSharing(ConfigProperty::GLOBAL_SHARING);
        propRootMinVersion.setSharing(ConfigProperty::GLOBAL_SHARING);
        propRootCurVersion.setSharing(ConfigProperty::GLOBAL_SHARING);

        // peer independent sync properties
        syncPropLogDir.setSharing(ConfigProperty::SOURCE_SET_SHARING);
        syncPropMaxLogDirs.setSharing(ConfigProperty::SOURCE_SET_SHARING);
        syncPropDevID.setSharing(ConfigProperty::SOURCE_SET_SHARING);
        propContextMinVersion.setSharing(ConfigProperty::SOURCE_SET_SHARING);
        propContextCurVersion.setSharing(ConfigProperty::SOURCE_SET_SHARING);
    }
} RegisterSyncConfigProperties;

ConfigPropertyRegistry &SyncConfig::getRegistry()
{
    static ConfigPropertyRegistry registry;
    return registry;
}

UserIdentity SyncConfig::getSyncUser() const {
    InitStateString user = syncPropUsername.getProperty(*getNode(syncPropUsername));
    UserIdentity id(UserIdentity::fromString(user));
    return id;
}
void SyncConfig::setSyncUsername(const string &value, bool temporarily) { syncPropUsername.setProperty(*getNode(syncPropUsername), value, temporarily); }
InitStateString SyncConfig::getSyncPassword() const {
    return syncPropPassword.getProperty(*getNode(syncPropPassword));
}
void PasswordConfigProperty::checkPassword(UserInterface &ui,
                                           SyncConfig &config,
                                           int flags,
                                           const ConfigProperty &usernameProperty,
                                           const std::string &sourceName) const
{
    std::string serverName = config.getConfigName();
    boost::shared_ptr<FilterConfigNode> globalConfigNode = config.getProperties();
    boost::shared_ptr<FilterConfigNode> sourceConfigNode;
    if (!sourceName.empty()) {
        sourceConfigNode = config.getSyncSourceNodes(sourceName).getNode(*this);
    }
    FilterConfigNode &configNode = sourceConfigNode ? *sourceConfigNode : *globalConfigNode;
    InitStateString username = usernameProperty.getProperty(configNode);
    if (sourceName.empty()) {
        SE_LOG_DEBUG(NULL, "checking password property '%s' in config '%s' with user identity '%s'",
                     getMainName().c_str(),
                     serverName.c_str(),
                     username.c_str());
    } else {
        SE_LOG_DEBUG(NULL, "checking password property '%s' in datastore '%s' of config '%s' with user identity '%s'",
                     getMainName().c_str(),
                     sourceName.c_str(),
                     serverName.c_str(),
                     username.c_str());
    }
    UserIdentity identity(UserIdentity::fromString(username));

    InitStateString passwordToSave;
    InitStateString usernameToSave;
    if (identity.m_provider == USER_IDENTITY_SYNC_CONFIG) {
        const std::string &credConfigName = identity.m_identity;
        SE_LOG_INFO(NULL, "using %s/%s from config '%s' as credentials for %s%s%s",
                    syncPropUsername.getMainName().c_str(),
                    syncPropPassword.getMainName().c_str(),
                    credConfigName.c_str(),
                    serverName.c_str(),
                    sourceName.empty() ? "" : "/",
                    sourceName.c_str());
        // Actual username/password are stored in a different config. Go find it...
        SyncConfig::Layout layout = config.getLayout();
        if (layout != SyncConfig::SHARED_LAYOUT) {
            SE_THROW(StringPrintf("%s = %s: only supported in configs using the current config storage, please migrate config %s",
                                  usernameProperty.getMainName().c_str(),
                                  username.c_str(),
                                  config.getConfigName().c_str()));
        }
        boost::shared_ptr<SyncConfig> credConfig(new SyncConfig(credConfigName));
        if (!credConfig->exists()) {
            SE_THROW(StringPrintf("%s = %s: config '%s' not found, cannot look up credentials",
                                  usernameProperty.getMainName().c_str(),
                                  username.c_str(),
                                  credConfigName.c_str()));
        }
        syncPropPassword.checkPassword(ui, *credConfig, flags);
        // Always store the new values.
        passwordToSave = InitStateString(credConfig->getSyncPassword(), true);
        usernameToSave = InitStateString(credConfig->getSyncUser().toString(), true);
    } else if (identity.m_provider != USER_IDENTITY_PLAIN_TEXT) {
        // Can some provider give us the plain text password? Not at the moment,
        // so we've got nothing to do here.
    } else {
        // Default, internal password handling.
        InitStateString password = getProperty(configNode);

        string descr = getDescr(serverName,*globalConfigNode,sourceName,sourceConfigNode);
        if (password == "-") {
            ConfigPasswordKey key = getPasswordKey(descr,serverName,*globalConfigNode,sourceName,sourceConfigNode);
            SE_LOG_DEBUG(NULL, "loading password from keyring with key %s", key.toString().c_str());
            std::string uiPassword = ui.askPassword(getMainName(),descr, key);
            // Empty means "no response". askPassword() pre-dates the
            // InitStateString class, and probably should be changed
            // to use it to avoid this kind of ambiguity, but for now
            // keep this semantic. Therefore don't use the result if
            // empty.
            if (!uiPassword.empty()) {
                passwordToSave = uiPassword;
            }
        } else if(boost::starts_with(password, "${") &&
                  boost::ends_with(password, "}")) {
            string envname = password.substr(2, password.size() - 3);
            const char *envval = getenv(envname.c_str());
            SE_LOG_DEBUG(NULL, "using password from env var %s", envname.c_str());
            if (!envval) {
                Exception::throwError(SE_HERE, string("the environment variable '") +
                                        envname +
                                        "' for the '" +
                                        descr +
                                        "' password is not set");
            } else {
                passwordToSave = envval;
            }
        }
    }

    // If we found a password, then set it in the config node
    // temporarily.  That way, all following "get password" calls will
    // be able to return it without having to make all callers away of
    // password handling.
    if (passwordToSave.wasSet() && (flags & CHECK_PASSWORD_RESOLVE_PASSWORD)) {
        configNode.addFilter(getMainName(), InitStateString(passwordToSave, true));
    }
    if (usernameToSave.wasSet() && (flags & CHECK_PASSWORD_RESOLVE_USERNAME)) {
        configNode.addFilter(usernameProperty.getMainName(), InitStateString(usernameToSave, true));
    }
}

void PasswordConfigProperty::checkPasswords(UserInterface &ui,
                                            SyncConfig &config,
                                            int flags,
                                            const std::list<std::string> &sourceNames)
{
    if (flags & CHECK_PASSWORD_SYNC) {
        ConfigPropertyRegistry& registry = SyncConfig::getRegistry();
        BOOST_FOREACH(const ConfigProperty *prop, registry) {
            prop->checkPassword(ui, config, flags);
        }
    }
    if (flags & CHECK_PASSWORD_SOURCE) {
        BOOST_FOREACH (const std::string &sourceName, sourceNames) {
            ConfigPropertyRegistry &registry = SyncSourceConfig::getRegistry();
            BOOST_FOREACH(const ConfigProperty *prop, registry) {
                prop->checkPassword(ui, config, flags, sourceName);
            }
        }
    }
}

std::string PasswordConfigProperty::getUsername(const ConfigProperty &usernameProperty,
                                                const FilterConfigNode &node)
{
    InitStateString username = usernameProperty.getProperty(node);
    UserIdentity id(UserIdentity::fromString(username));
    return id.m_identity;
}

void PasswordConfigProperty::savePassword(UserInterface &ui,
                                          SyncConfig &config,
                                          const ConfigProperty &usernameProperty,
                                          const std::string &sourceName) const
{
    std::string serverName = config.getConfigName();
    boost::shared_ptr<FilterConfigNode> globalConfigNode = config.getProperties();
    boost::shared_ptr<FilterConfigNode> sourceConfigNode;
    if (!sourceName.empty()) {
        sourceConfigNode = config.getSyncSourceNodes(sourceName).getNode(*this);
    }
    FilterConfigNode &configNode = sourceConfigNode ? *sourceConfigNode : *globalConfigNode;

    InitStateString username = usernameProperty.getProperty(configNode);
    // In checkPassword() we retrieve from background storage and store as temporary value.
    // Here we use the temporary value and move it in the background storage.
    // We allow empty passwords to be stored in the config although
    // that might leak some information, because that is how SyncEvolution
    // traditionally worked. Changing this now breaks tests and possibly
    // causes problems for users depending on the old behavior.
    InitStateString password = getProperty(configNode);
    if (!password.wasSet() || password.empty()) {
        return;
    }

    if (sourceName.empty()) {
        SE_LOG_DEBUG(NULL, "possibly saving password property '%s' in config '%s' with user identity '%s'",
                     getMainName().c_str(),
                     serverName.c_str(),
                     username.c_str());
    } else {
        SE_LOG_DEBUG(NULL, "possibly saving password property '%s' in datastore '%s' of config '%s' with user identity '%s'",
                     getMainName().c_str(),
                     sourceName.c_str(),
                     serverName.c_str(),
                     username.c_str());
    }
    UserIdentity identity(UserIdentity::fromString(username));


    bool updatePassword = false;
    InitStateString passwordToSave;
    if (identity.m_provider == USER_IDENTITY_SYNC_CONFIG) {
        // Store in the other config and unset it here.
        updatePassword = true;

        const std::string &credConfigName = identity.m_identity;
        SE_LOG_INFO(NULL, "setting %s in config '%s' as part of credentials for %s%s%s",
                    syncPropPassword.getMainName().c_str(),
                    credConfigName.c_str(),
                    serverName.c_str(),
                    sourceName.empty() ? "" : "/",
                    sourceName.c_str());
        // Actual username/password are stored in a different config. Go find it...
        SyncConfig::Layout layout = config.getLayout();
        if (layout != SyncConfig::SHARED_LAYOUT) {
            SE_THROW(StringPrintf("%s = %s: only supported in configs using the current config storage, please migrate config %s",
                                  usernameProperty.getMainName().c_str(),
                                  username.c_str(),
                                  config.getConfigName().c_str()));
        }
        boost::shared_ptr<SyncConfig> credConfig(new SyncConfig(credConfigName));
        if (!credConfig->exists()) {
            SE_THROW(StringPrintf("%s = %s: config '%s' not found, cannot look up credentials",
                                  usernameProperty.getMainName().c_str(),
                                  username.c_str(),
                                  credConfigName.c_str()));
        }
        credConfig->setSyncPassword(password, false);
        syncPropPassword.savePassword(ui, *credConfig);
    } else if (identity.m_provider != USER_IDENTITY_PLAIN_TEXT) {
        // Cannot store passwords in providers.
        if (password.wasSet() && !password.empty()) {
            SE_THROW(StringPrintf("setting property '%s' not supported for provider '%s' from property '%s'",
                                  getMainName().c_str(),
                                  identity.m_provider.c_str(),
                                  usernameProperty.getMainName().c_str()));
        }
    } else {
        if (password == "-" || password == "" ||
            (boost::starts_with(password, "${") && boost::ends_with(password, "}"))) {
            // Nothing to do, leave it as is.
            SE_LOG_DEBUG(NULL, "no need to save, interactive or env var password");
        } else {
            string descr = getDescr(serverName,*globalConfigNode,sourceName,sourceConfigNode);
            ConfigPasswordKey key = getPasswordKey(descr,serverName,*globalConfigNode,sourceName,sourceConfigNode);
            SE_LOG_DEBUG(NULL, "saving password in keyring with key %s", key.toString().c_str());
            if (ui.savePassword(getMainName(), password, key)) {
                passwordToSave = "-";
                updatePassword = true;
            }
        }
    }
    if (updatePassword) {
        setProperty(configNode, passwordToSave);
    }
}

void SyncConfig::setSyncPassword(const string &value, bool temporarily) { syncPropPassword.setProperty(*getNode(syncPropPassword), value, temporarily); }

InitState<bool> SyncConfig::getPreventSlowSync() const {
    return syncPropPreventSlowSync.getPropertyValue(*getNode(syncPropPreventSlowSync));
}
void SyncConfig::setPreventSlowSync(bool value, bool temporarily) { syncPropPreventSlowSync.setProperty(*getNode(syncPropPreventSlowSync), value, temporarily); }

static const char *ProxyString = "http_proxy";

/* Reads http_proxy from environment, if not available returns configured value */
InitState<bool> SyncConfig::getUseProxy() const {
    InitState<bool> res = syncPropUseProxy.getPropertyValue(*getNode(syncPropUseProxy));
    if (!res.wasSet()) {
        // Not configured. Check environment.
        char *proxy = getenv(ProxyString);
        if (proxy && *proxy) {
            // Environment has it. Assume that it applies to us and
            // use it. TODO: check no_proxy against our sync URL.
            res = InitState<bool>(true, true);
        }
    }
    return res;
}

void SyncConfig::setUseProxy(bool value, bool temporarily) { syncPropUseProxy.setProperty(*getNode(syncPropUseProxy), value, temporarily); }

/* If http_proxy set in the environment returns it, otherwise configured value */
InitStateString SyncConfig::getProxyHost() const {
    char *proxy = getenv(ProxyString);
    if (!proxy) {
        return syncPropProxyHost.getProperty(*getNode(syncPropUseProxy)); 
    } else {
        return InitStateString(proxy, true);
    }
}

void SyncConfig::setProxyHost(const string &value, bool temporarily) { syncPropProxyHost.setProperty(*getNode(syncPropProxyHost), value, temporarily); }

UserIdentity SyncConfig::getProxyUser() const {
    InitStateString username = syncPropProxyUsername.getProperty(*getNode(syncPropProxyUsername));
    UserIdentity id(UserIdentity::fromString(username));
    return id;
}
void SyncConfig::setProxyUsername(const string &value, bool temporarily) { syncPropProxyUsername.setProperty(*getNode(syncPropProxyUsername), value, temporarily); }

InitStateString SyncConfig::getProxyPassword() const {
    return syncPropProxyPassword.getProperty(*getNode(syncPropProxyPassword));
}
void SyncConfig::setProxyPassword(const string &value, bool temporarily) { syncPropProxyPassword.setProperty(*getNode(syncPropProxyPassword), value, temporarily); }
InitState< vector<string> > SyncConfig::getSyncURL() const { 
    InitStateString s = syncPropSyncURL.getProperty(*getNode(syncPropSyncURL));
    vector<string> urls;
    if (!s.empty()) {
        // workaround for g++ 4.3/4.4:
        // http://stackoverflow.com/questions/1168525/c-gcc4-4-warning-array-subscript-is-above-array-bounds
        static const string sep(" \t");
        boost::split(urls, s.get(), boost::is_any_of(sep));
    }
    return InitState< vector<string> >(urls, s.wasSet());
}
void SyncConfig::setSyncURL(const string &value, bool temporarily) { syncPropSyncURL.setProperty(*getNode(syncPropSyncURL), value, temporarily); }
void SyncConfig::setSyncURL(const vector<string> &value, bool temporarily) { 
    stringstream urls;
    BOOST_FOREACH (string url, value) {
        urls<<url<<" ";
    }
    return setSyncURL (urls.str(), temporarily);
}
InitStateString SyncConfig::getClientAuthType() const { return syncPropClientAuthType.getProperty(*getNode(syncPropClientAuthType)); }
void SyncConfig::setClientAuthType(const string &value, bool temporarily) { syncPropClientAuthType.setProperty(*getNode(syncPropClientAuthType), value, temporarily); }

template<class T> InitState<T> EnsureMinSize(const InitState<T> &current, T min)
{
    if (current >= min) {
        return current;
    } else {
        return InitState<T>(min);
    }
}

InitState<unsigned long > SyncConfig::getMaxMsgSize() const {
    // Sanitize value: don't run with buffer sizes smaller than 10KB.
    return EnsureMinSize(syncPropMaxMsgSize.getPropertyValue(*getNode(syncPropMaxMsgSize)),
                         10 * 1024ul);
}
void SyncConfig::setMaxMsgSize(unsigned long value, bool temporarily) { syncPropMaxMsgSize.setProperty(*getNode(syncPropMaxMsgSize), value, temporarily); }
InitState<unsigned int > SyncConfig::getMaxObjSize() const {
    return EnsureMinSize(syncPropMaxObjSize.getPropertyValue(*getNode(syncPropMaxObjSize)),
                         1024u);
}
void SyncConfig::setMaxObjSize(unsigned int value, bool temporarily) { syncPropMaxObjSize.setProperty(*getNode(syncPropMaxObjSize), value, temporarily); }
InitStateString SyncConfig::getDevID() const { return syncPropDevID.getProperty(*getNode(syncPropDevID)); }
void SyncConfig::setDevID(const string &value, bool temporarily) { syncPropDevID.setProperty(*getNode(syncPropDevID), value, temporarily); }
InitState<bool> SyncConfig::getWBXML() const { return syncPropWBXML.getPropertyValue(*getNode(syncPropWBXML)); }
void SyncConfig::setWBXML(bool value, bool temporarily) { syncPropWBXML.setProperty(*getNode(syncPropWBXML), value, temporarily); }
InitState<bool> SyncConfig::getRefreshSync() const { return syncPropRefreshSync.getPropertyValue(*getNode(syncPropRefreshSync)); }
void SyncConfig::setRefreshSync(bool value, bool temporarily) { syncPropRefreshSync.setProperty(*getNode(syncPropRefreshSync), value, temporarily); }
InitStateString SyncConfig::getLogDir() const { return syncPropLogDir.getProperty(*getNode(syncPropLogDir)); }
void SyncConfig::setLogDir(const string &value, bool temporarily) { syncPropLogDir.setProperty(*getNode(syncPropLogDir), value, temporarily); }
InitState<unsigned int> SyncConfig::getMaxLogDirs() const { return syncPropMaxLogDirs.getPropertyValue(*getNode(syncPropMaxLogDirs)); }
void SyncConfig::setMaxLogDirs(unsigned int value, bool temporarily) { syncPropMaxLogDirs.setProperty(*getNode(syncPropMaxLogDirs), value, temporarily); }
InitState<unsigned int> SyncConfig::getLogLevel() const { return syncPropLogLevel.getPropertyValue(*getNode(syncPropLogLevel)); }
void SyncConfig::setLogLevel(unsigned int value, bool temporarily) { syncPropLogLevel.setProperty(*getNode(syncPropLogLevel), value, temporarily); }
InitState<SyncConfig::NotifyLevel> SyncConfig::getNotifyLevel() const {
    InitState<unsigned int> res = syncPropNotifyLevel.getPropertyValue(*getNode(syncPropNotifyLevel));
    return InitState<SyncConfig::NotifyLevel>(static_cast<SyncConfig::NotifyLevel>(res.get()), res.wasSet());
}
void SyncConfig::setNotifyLevel(SyncConfig::NotifyLevel value, bool temporarily) { syncPropNotifyLevel.setProperty(*getNode(syncPropNotifyLevel), value, temporarily); }
InitState<unsigned int> SyncConfig::getRetryDuration() const {return syncPropRetryDuration.getPropertyValue(*getNode(syncPropRetryDuration));}
void SyncConfig::setRetryDuration(unsigned int value, bool temporarily) { syncPropRetryDuration.setProperty(*getNode(syncPropRetryDuration), value, temporarily); }
InitState<unsigned int> SyncConfig::getRetryInterval() const { return syncPropRetryInterval.getPropertyValue(*getNode(syncPropRetryInterval)); }
void SyncConfig::setRetryInterval(unsigned int value, bool temporarily) { return syncPropRetryInterval.setProperty(*getNode(syncPropRetryInterval),value,temporarily); }

/* used by Server Alerted Sync */
InitStateString SyncConfig::getRemoteIdentifier() const { return syncPropRemoteIdentifier.getProperty(*getNode(syncPropRemoteIdentifier));}
void SyncConfig::setRemoteIdentifier (const string &value, bool temporarily) { return syncPropRemoteIdentifier.setProperty (*getNode(syncPropRemoteIdentifier), value, temporarily); }

InitState<bool> SyncConfig::getPeerIsClient() const { return syncPropPeerIsClient.getPropertyValue(*getNode(syncPropPeerIsClient)); }
void SyncConfig::setPeerIsClient(bool value, bool temporarily) { syncPropPeerIsClient.setProperty(*getNode(syncPropPeerIsClient), value, temporarily); }

InitStateString SyncConfig::getSyncMLVersion() const {
    InitState< std::set<std::string> > flags = getSyncMLFlags();
    static const char * const versions[] = { "1.2", "1.1", "1.0" };
    BOOST_FOREACH (const char *version, versions) {
        if (flags.find(version) != flags.end()) {
            return InitStateString(version, flags.wasSet());
        }
    }
    return InitStateString("", flags.wasSet());
}
InitState<unsigned int> SyncConfig::getRequestMaxTime() const {
    InitState<unsigned int> requestmaxtime;
    InitState< std::set<std::string> > flags = getSyncMLFlags();
    BOOST_FOREACH(const std::string &flag, flags) {
        size_t offset = flag.find('=');
        if (offset != flag.npos) {
            std::string key = flag.substr(0, offset);
            if (boost::iequals(key, "RequestMaxTime")) {
                std::string value = flag.substr(offset + 1);
                unsigned int seconds;
                std::string error;
                if (!SecondsConfigProperty::parseDuration(value, error, seconds)) {
                    SE_THROW("invalid RequestMaxTime value in SyncMLVersion property: " + error);
                }
                requestmaxtime = seconds;
                break;
            }
        }
    }
    return requestmaxtime;
}
InitState< std::set<std::string> > SyncConfig::getSyncMLFlags() const {
    InitStateString value = syncPropSyncMLVersion.getProperty(*getNode(syncPropSyncMLVersion));
    std::list<std::string> keywords;
    // Work around g++ 4.4 + "array out of bounds" when using is_any_of() on plain string.
    static const std::string delim(" ,");
    boost::split(keywords, value, boost::is_any_of(delim));
    std::set<std::string> flags;
    BOOST_FOREACH (std::string &keyword, keywords) {
        boost::to_lower(keyword);
        flags.insert(keyword);
    }
    return InitState< std::set<std::string> >(flags, value.wasSet());
}

InitStateString SyncConfig::getUserPeerName() const { return syncPropPeerName.getProperty(*getNode(syncPropPeerName)); }
void SyncConfig::setUserPeerName(const InitStateString &name) { syncPropPeerName.setProperty(*getNode(syncPropPeerName), name); }

InitState<bool> SyncConfig::getPrintChanges() const { return syncPropPrintChanges.getPropertyValue(*getNode(syncPropPrintChanges)); }
void SyncConfig::setPrintChanges(bool value, bool temporarily) { syncPropPrintChanges.setProperty(*getNode(syncPropPrintChanges), value, temporarily); }
InitState<bool> SyncConfig::getDumpData() const { return syncPropDumpData.getPropertyValue(*getNode(syncPropDumpData)); }
void SyncConfig::setDumpData(bool value, bool temporarily) { syncPropDumpData.setProperty(*getNode(syncPropDumpData), value, temporarily); }
InitStateString SyncConfig::getWebURL() const { return syncPropWebURL.getProperty(*getNode(syncPropWebURL)); }
void SyncConfig::setWebURL(const std::string &url, bool temporarily) { syncPropWebURL.setProperty(*getNode(syncPropWebURL), url, temporarily); }
InitStateString SyncConfig::getIconURI() const { return syncPropIconURI.getProperty(*getNode(syncPropIconURI)); }
InitState<bool> SyncConfig::getConsumerReady() const { return syncPropConsumerReady.getPropertyValue(*getNode(syncPropConsumerReady)); }
void SyncConfig::setConsumerReady(bool ready) { return syncPropConsumerReady.setProperty(*getNode(syncPropConsumerReady), ready); }
void SyncConfig::setIconURI(const std::string &uri, bool temporarily) { syncPropIconURI.setProperty(*getNode(syncPropIconURI), uri, temporarily); }
InitState<unsigned long> SyncConfig::getHashCode() const { return syncPropHashCode.getPropertyValue(*getNode(syncPropHashCode)); }
void SyncConfig::setHashCode(unsigned long code) { syncPropHashCode.setProperty(*getNode(syncPropHashCode), code); }
InitStateString SyncConfig::getConfigDate() const { return syncPropConfigDate.getProperty(*getNode(syncPropConfigDate)); }
void SyncConfig::setConfigDate() { 
    /* Set current timestamp as configdate */
    char buffer[17]; 
    time_t ts = time(NULL);
    strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", gmtime(&ts));
    const std::string date(buffer);
    syncPropConfigDate.setProperty(*getNode(syncPropConfigDate), date);
}

InitStateString SyncConfig::getSSLServerCertificates() const { return syncPropSSLServerCertificates.getProperty(*getNode(syncPropSSLServerCertificates)); }
void SyncConfig::setSSLServerCertificates(const string &value, bool temporarily) { syncPropSSLServerCertificates.setProperty(*getNode(syncPropSSLServerCertificates), value, temporarily); }
InitState<bool> SyncConfig::getSSLVerifyServer() const { return syncPropSSLVerifyServer.getPropertyValue(*getNode(syncPropSSLVerifyServer)); }
void SyncConfig::setSSLVerifyServer(bool value, bool temporarily) { syncPropSSLVerifyServer.setProperty(*getNode(syncPropSSLVerifyServer), value, temporarily); }
InitState<bool> SyncConfig::getSSLVerifyHost() const { return syncPropSSLVerifyHost.getPropertyValue(*getNode(syncPropSSLVerifyHost)); }
void SyncConfig::setSSLVerifyHost(bool value, bool temporarily) { syncPropSSLVerifyHost.setProperty(*getNode(syncPropSSLVerifyHost), value, temporarily); }
InitStateString SyncConfig::getRemoteDevID() const { return syncPropRemoteDevID.getProperty(*getNode(syncPropRemoteDevID)); }
void SyncConfig::setRemoteDevID(const string &value) { syncPropRemoteDevID.setProperty(*getNode(syncPropRemoteDevID), value); }
InitStateString SyncConfig::getNonce() const { return syncPropNonce.getProperty(*getNode(syncPropNonce)); }
void SyncConfig::setNonce(const string &value) { syncPropNonce.setProperty(*getNode(syncPropNonce), value); }
InitStateString SyncConfig::getDeviceData() const { return syncPropDeviceData.getProperty(*getNode(syncPropDeviceData)); }
void SyncConfig::setDeviceData(const string &value) { syncPropDeviceData.setProperty(*getNode(syncPropDeviceData), value); }
InitStateString SyncConfig::getDefaultPeer() const { return globalPropDefaultPeer.getProperty(*getNode(globalPropDefaultPeer)); }
void SyncConfig::setDefaultPeer(const string &value) { globalPropDefaultPeer.setProperty(*getNode(globalPropDefaultPeer), value); }
InitStateTri SyncConfig::getKeyring() const { return globalPropKeyring.getProperty(*getNode(globalPropKeyring)); }
void SyncConfig::setKeyring(const string &value) { globalPropKeyring.setProperty(*getNode(globalPropKeyring), value); }

InitStateString SyncConfig::getAutoSync() const { return syncPropAutoSync.getProperty(*getNode(syncPropAutoSync)); }
void SyncConfig::setAutoSync(const string &value, bool temporarily) { syncPropAutoSync.setProperty(*getNode(syncPropAutoSync), value, temporarily); }
InitState<unsigned int> SyncConfig::getAutoSyncInterval() const { return syncPropAutoSyncInterval.getPropertyValue(*getNode(syncPropAutoSyncInterval)); }
void SyncConfig::setAutoSyncInterval(unsigned int value, bool temporarily) { syncPropAutoSyncInterval.setProperty(*getNode(syncPropAutoSyncInterval), value, temporarily); }
InitState<unsigned int> SyncConfig::getAutoSyncDelay() const { return syncPropAutoSyncDelay.getPropertyValue(*getNode(syncPropAutoSyncDelay)); }
void SyncConfig::setAutoSyncDelay(unsigned int value, bool temporarily) { syncPropAutoSyncDelay.setProperty(*getNode(syncPropAutoSyncDelay), value, temporarily); }

std::string SyncConfig::findSSLServerCertificate()
{
    std::string paths = getSSLServerCertificates();
    std::vector< std::string > files;
    boost::split(files, paths, boost::is_any_of(":"));
    BOOST_FOREACH(std::string file, files) {
        if (!file.empty() && !access(file.c_str(), R_OK)) {
            return file;
        }
    }

    return "";
}

void SyncConfig::setConfigFilter(bool sync,
                                 const std::string &source,
                                 const FilterConfigNode::ConfigFilter &filter)
{
    if (sync) {
        m_peerNode->setFilter(filter);
        if (m_peerNode != m_contextNode) {
            m_contextNode->setFilter(filter);
        }
        if (m_globalNode != m_contextNode) {
            m_globalNode->setFilter(filter);
        }
    } else {
        m_nodeCache.clear();
        m_sourceFilters[source] = filter;
    }
}

boost::shared_ptr<FilterConfigNode>
SyncConfig::getNode(const ConfigProperty &prop)
{
    switch (prop.getSharing()) {
    case ConfigProperty::GLOBAL_SHARING:
        if (prop.isHidden()) {
            return boost::shared_ptr<FilterConfigNode>(new FilterConfigNode(m_globalHiddenNode));
        } else {
            return m_globalNode;
        }
        break;
    case ConfigProperty::SOURCE_SET_SHARING:
        if (prop.isHidden()) {
            return boost::shared_ptr<FilterConfigNode>(new FilterConfigNode(m_contextHiddenNode));
        } else {
            return m_contextNode;
        }
        break;
    case ConfigProperty::NO_SHARING:
        if (prop.isHidden()) {
            return boost::shared_ptr<FilterConfigNode>(new FilterConfigNode(m_hiddenPeerNode));
        } else {
            return m_peerNode;
        }
        break;
    }
    // should not be reached
    return boost::shared_ptr<FilterConfigNode>(new FilterConfigNode(boost::shared_ptr<ConfigNode>(new DevNullConfigNode("unknown sharing state of property"))));
}

boost::shared_ptr<FilterConfigNode>
SyncConfig::getNode(const std::string &propName)
{
    ConfigPropertyRegistry &registry = getRegistry();
    const ConfigProperty *prop = registry.find(propName);
    if (prop) {
        return getNode(*prop);
    } else {
        return boost::shared_ptr<FilterConfigNode>();
    }
}

static void setDefaultProps(const ConfigPropertyRegistry &registry,
                            boost::shared_ptr<FilterConfigNode> node,
                            bool force,
                            bool unshared,
                            bool useObligatory = true)
{
    BOOST_FOREACH(const ConfigProperty *prop, registry) {
        InitStateString value = prop->getProperty(*node);
        if (!prop->isHidden() &&
            (unshared || prop->getSharing() != ConfigProperty::NO_SHARING) &&
            (force || !value.wasSet())) {
            if (useObligatory) {
                prop->setDefaultProperty(*node, prop->isObligatory());
            } else {
                prop->setDefaultProperty(*node, false);
            }
        }
    }
}

void SyncConfig::setDefaults(bool force)
{
    setDefaultProps(getRegistry(), getProperties(),
                    force,
                    !m_peerPath.empty());
}

void SyncConfig::setSourceDefaults(const string &name, bool force)
{
    SyncSourceNodes nodes = getSyncSourceNodes(name);
    setDefaultProps(SyncSourceConfig::getRegistry(),
                    nodes.getProperties(),
                    force,
                    !m_peerPath.empty());
}

void SyncConfig::removeSyncSource(const string &name)
{
    string lower = name;
    boost::to_lower(lower);
    string pathName;

    if (m_layout == SHARED_LAYOUT) {
        if (m_peerPath.empty()) {
            // removed shared source properties...
            pathName = m_contextPath + "/sources/" + lower;
            m_tree->remove(pathName);
            // ... and the peer-specific ones of *all* peers
            BOOST_FOREACH(const std::string peer,
                          m_tree->getChildren(m_contextPath + "/peers")) {
                m_tree->remove(m_contextPath + "/peers/" + peer + "/sources/" + lower);
            }
        } else {
            // remove only inside the selected peer
            m_tree->remove(m_peerPath + "/sources/" + lower);
        }
    } else {
        // remove the peer-specific ones
        pathName = m_peerPath +
            (m_layout == SYNC4J_LAYOUT ? "spds/sources/" : "sources/") +
            lower;
        m_tree->remove(pathName);
    }
}

void SyncConfig::clearSyncSourceProperties(const string &name)
{
    SyncSourceNodes nodes = getSyncSourceNodes(name);
    setDefaultProps(SyncSourceConfig::getRegistry(),
                    nodes.getProperties(),
                    true,
                    !m_peerPath.empty(),
                    false);
}

void SyncConfig::clearSyncProperties()
{
    setDefaultProps(getRegistry(), getProperties(),
                    true,
                    !m_peerPath.empty(),
                    false);
}

static void copyProperties(const ConfigNode &fromProps,
                           ConfigNode &toProps,
                           bool hidden,
                           bool unshared,
                           const ConfigPropertyRegistry &allProps)
{
    BOOST_FOREACH(const ConfigProperty *prop, allProps) {
        if (prop->isHidden() == hidden &&
            (unshared ||
             prop->getSharing() != ConfigProperty::NO_SHARING)) {
            InitStateString value = prop->getProperty(fromProps);
            string name = prop->getName(toProps);
            toProps.setProperty(name,
                                value,
                                prop->getComment());
        }
    }
}

static void copyProperties(const ConfigNode &fromProps,
                           ConfigNode &toProps)
{
    ConfigProps props;
    fromProps.readProperties(props);
    toProps.writeProperties(props);
}

void SyncConfig::copy(const SyncConfig &other,
                      const set<string> *sourceSet)
{
    for (int i = 0; i < 2; i++ ) {
        boost::shared_ptr<const FilterConfigNode> fromSyncProps(other.getProperties(i));
        boost::shared_ptr<FilterConfigNode> toSyncProps(this->getProperties(i));
        copyProperties(*fromSyncProps,
                       *toSyncProps,
                       i,
                       !m_peerPath.empty(),
                       SyncConfig::getRegistry());
    }

    list<string> sources;
    if (!sourceSet) {
        sources = other.getSyncSources();
    } else {
        BOOST_FOREACH(const string &sourceName, *sourceSet) {
            sources.push_back(sourceName);
        }
    }
    BOOST_FOREACH(const string &sourceName, sources) {
        ConstSyncSourceNodes fromNodes = other.getSyncSourceNodes(sourceName);
        SyncSourceNodes toNodes = this->getSyncSourceNodes(sourceName);

        for (int i = 0; i < 2; i++ ) {
            copyProperties(*fromNodes.getProperties(i),
                           *toNodes.getProperties(i),
                           i,
                           !m_peerPath.empty(),
                           SyncSourceConfig::getRegistry());
        }
        copyProperties(*fromNodes.getTrackingNode(),
                       *toNodes.getTrackingNode());
        copyProperties(*fromNodes.getServerNode(),
                       *toNodes.getServerNode());
    }
}

InitStateString SyncConfig::getSwv() const { return VERSION; }
InitStateString SyncConfig::getDevType() const { return DEVICE_TYPE; }

                     
SyncSourceConfig::SyncSourceConfig(const string &name, const SyncSourceNodes &nodes) :
    m_name(name),
    m_nodes(nodes)
{
}

StringConfigProperty SyncSourceConfig::m_sourcePropSync("sync",
                                           "Requests a certain synchronization mode when initiating a sync:\n\n"
                                           "  two-way\n"
                                           "    only send/receive changes since last sync\n"
                                           "  slow\n"
                                           "    exchange all items\n"
                                           "  refresh-from-remote\n"
                                           "    discard all local items and replace with\n"
                                           "    the items on the peer\n"
                                           "  refresh-from-local\n"
                                           "    discard all items on the peer and replace\n"
                                           "    with the local items\n"
                                           "  one-way-from-remote\n"
                                           "    transmit changes from peer\n"
                                           "  one-way-from-local\n"
                                           "    transmit local changes\n"
                                           "  local-cache-slow (server only)\n"
                                           "    mirror remote data locally, transferring all data\n"
                                           "  local-cache-incremental (server only)\n"
                                           "    mirror remote data locally, transferring only changes;\n"
                                           "    falls back to local-cache-slow automatically if necessary\n"
                                           "  disabled (or none)\n"
                                           "    synchronization disabled\n"
                                           "\n"
                                           "refresh/one-way-from-server/client are also supported. Their use is\n"
                                           "discouraged because the direction of the data transfer depends\n"
                                           "on the role of the local side (can be server or client), which is\n"
                                           "not always obvious.\n"
                                           "\n"
                                           "When accepting a sync session in a SyncML server (HTTP server), only\n"
                                           "datastores with sync != disabled are made available to the client,\n"
                                           "which chooses the final sync mode based on its own configuration.\n"
                                           "When accepting a sync session in a SyncML client (local sync with\n"
                                           "the server contacting SyncEvolution on a device), the sync mode\n"
                                           "specified in the client is typically overriden by the server.\n",
                                           "disabled",
                                           "",
                                           Values() +
                                           (Aliases("two-way")) +
                                           (Aliases("slow")) +

                                           (Aliases("refresh-from-local")) +
                                           (Aliases("refresh-from-remote") + "refresh") +
                                           (Aliases("one-way-from-local")) +
                                           (Aliases("one-way-from-remote") + "one-way") +

                                           (Aliases("refresh-from-client") + "refresh-client") +
                                           (Aliases("refresh-from-server") + "refresh-server") +
                                           (Aliases("one-way-from-client") + "one-way-client") +
                                           (Aliases("one-way-from-server") + "one-way-server") +
                                           (Aliases("local-cache-slow")) +
                                           (Aliases("local-cache-incremental") + "local-cache") +
                                           (Aliases("disabled") + "none"));

static class SourceBackendConfigProperty : public StringConfigProperty {
public:
    SourceBackendConfigProperty() :
        StringConfigProperty("backend",
                             "Specifies the SyncEvolution backend and thus the\n"
                             "data which is synchronized by this datastore. Each\n"
                             "backend may support multiple databases (see 'database'\n"
                             "property), different formats inside that database (see\n"
                             "'databaseFormat'), and different formats when talking to\n"
                             "the sync peer (see 'syncFormat' and 'forceSyncFormat').\n"
                             "\n"
                             "A special 'virtual' backend combines several other\n"
                             "datastores and presents them as one set of items\n"
                             "to the peer. For example, Nokia phones typically\n"
                             "exchange tasks and events as part of one set of\n"
                             "calendar items.\n"
                             "\n"
                             "Right now such a virtual backend is limited to\n"
                             "combining one calendar datastore with events and one\n"
                             "task datastore. They have to be specified in the\n"
                             "``database`` property, typically like this:\n"
                             "``calendar,todo``\n"
                             "\n"
                             "Different datastores combined in one virtual datastore must\n"
                             "have a common format. As with other backends,\n"
                             "the preferred format can be influenced via the 'syncFormat'\n"
                             "attribute.\n"
                             "\n"
                             "Here's the full list of potentially supported backends,\n"
                             "valid 'backend' values for each of them, and possible\n"
                             "formats. Note that SyncEvolution installations usually\n"
                             "support only a subset of the backends; that's why e.g.\n"
                             "\"addressbook\" is unambiguous although there are multiple\n"
                             "address book backends.\n"
                             "\n",
                             "select backend",
                             "",
                             Values() +
                             (Aliases("virtual")) +
                             (Aliases("calendar") + "events") +
                             (Aliases("addressbook") + "contacts") +
                             (Aliases("todo") + "tasks") +
                             (Aliases("memo") + "memos" + "notes"))
    {}

    virtual string getComment() const {
        stringstream enabled, disabled;
        stringstream res;

        SourceRegistry &registry(SyncSource::getSourceRegistry());
        BOOST_FOREACH(const RegisterSyncSource *sourceInfos, registry) {
            static const std::string whitespace(" \t\n");
            string comment = boost::trim_right_copy_if(sourceInfos->m_typeDescr,
                                                       boost::is_any_of(whitespace));
            stringstream *curr = sourceInfos->m_enabled ? &enabled : &disabled;
            boost::replace_all(comment, "\n", "\n ");
            *curr << " " << comment << "\n";
        }

        res << StringConfigProperty::getComment();
        if (enabled.str().size()) {
            res << "\n\nCurrently active::\n\n" << enabled.str();
        }
        if (disabled.str().size()) {
            res << "\n\nCurrently inactive::\n\n" << disabled.str();
        }

        return boost::trim_right_copy(res.str());
    }

    virtual Values getValues() const {
        Values res(StringConfigProperty::getValues());

        const SourceRegistry &registry(SyncSource::getSourceRegistry());
        BOOST_FOREACH(const RegisterSyncSource *sourceInfos, registry) {
            copy(sourceInfos->m_typeValues.begin(),
                 sourceInfos->m_typeValues.end(),
                 back_inserter(res));
        }

        return res;
    }
} sourcePropBackend;

StringConfigProperty sourcePropSyncFormat("syncFormat",
                                          "When there are alternative formats for the same data,\n"
                                          "each side of a sync offers all that it supports and marks one as\n"
                                          "preferred. If set, this property overrides the format\n"
                                          "that would normally be marked as preferred by a backend.\n"
                                          "\n"
                                          "Valid values depend on the backend. Here are some examples:\n"
                                          "  contacts - text/vcard = vCard 3.0 format\n"
                                          "             text/x-vcard = legacy vCard 2.1 format\n"
                                          "  calendar - text/calendar = iCalendar 2.0 format\n"
                                          "             text/x-vcalendar = legacy vCalendar 1.0 format\n"
                                          "\n"
                                          "Errors while starting to sync and parsing and/or storing\n"
                                          "items on either client or server can be caused by a mismatch between\n"
                                          "the sync format and uri at the peer.\n");

static BoolConfigProperty sourcePropForceSyncFormat("forceSyncFormat",
                                                    "Some peers get confused when offered multiple choices\n"
                                                    "for the sync format or pick the less optimal one.\n"
                                                    "In such a case, setting this property enforces that the\n"
                                                    "preferred format specified with 'syncFormat' is\n"
                                                    "really used.",
                                                    "FALSE");

static ConfigProperty sourcePropDatabaseID(Aliases("database") + "evolutionsource",
                                           "Picks one of the backend's databases:\n"
                                           "depending on the backend, one can set the name\n"
                                           "and/or a unique identifier.\n\n"
                                           "Most backends have a default database,\n"
                                           "like for example the system address book.\n"
                                           "Not setting this property selects that default\n"
                                           "database.\n\n"
                                           "If the backend is a virtual data datastore,\n"
                                           "this field must contain comma seperated list of\n"
                                           "sub datasources actually used to store data.\n"
                                           "If your sub datastore has a comma in name, you\n"
                                           "must prevent taht comma from being mistaken as the\n"
                                           "separator by preceding it with a backslash, like this:\n"
                                           "``database=Source1PartA\\,PartB,Source2\\\\Backslash``\n"
                                           "\n"
                                           "To get a full list of available databases,\n"
                                           "run ``syncevolution --print-databases``. The name\n"
                                           "is printed in front of the colon, followed by\n"
                                           "an identifier in brackets. Usually the name is unique and can be\n"
                                           "used to reference the data datastore. The default\n"
                                           "data datastore is marked with <default> at the end\n"
                                           "of the line, if there is a default.\n");

static StringConfigProperty sourcePropDatabaseFormat("databaseFormat",
                                                     "Defines the data format to be used by the backend for its\n"
                                                     "own storage. Typically backends only support one format\n"
                                                     "and ignore this property, but for example the file backend\n"
                                                     "uses it. See the 'backend' property for more information.\n");

static ConfigProperty sourcePropURI("uri",
                                    "this is appended to the server's URL to identify the\n"
                                    "server's database; if unset, the datastore name is used as\n"
                                    "fallback");

static ConfigProperty sourcePropUser(Aliases("databaseUser") + "evolutionuser",
                                     "authentication for backend data datastore; password can be specified\n"
                                     "in multiple ways, see SyncML server password for details\n"
                                     "\n"
                                     "Warning: setting database user/password in cases where it is not\n"
                                     "needed, as for example with local Evolution calendars and addressbooks,\n"
                                     "can cause the Evolution backend to hang.");

static class DatabasePasswordConfigProperty : public PasswordConfigProperty {
public:
    DatabasePasswordConfigProperty() :
        PasswordConfigProperty(Aliases("databasePassword") + "evolutionpassword", "","", "backend")
    {}

    virtual void checkPassword(UserInterface &ui,
                               SyncConfig &config,
                               int flags,
                               const std::string &sourceName) const {
        PasswordConfigProperty::checkPassword(ui, config, flags, sourcePropUser, sourceName);
    }
    virtual void savePassword(UserInterface &ui,
                               SyncConfig &config,
                               const std::string &sourceName) const {
        PasswordConfigProperty::savePassword(ui, config, sourcePropUser, sourceName);
    }

    virtual ConfigPasswordKey getPasswordKey(const std::string &descr,
                                             const std::string &serverName,
                                             FilterConfigNode &globalConfigNode,
                                             const std::string &sourceName = std::string(),
                                             const boost::shared_ptr<FilterConfigNode> &sourceConfigNode=boost::shared_ptr<FilterConfigNode>()) const {
        ConfigPasswordKey key;
        key.user = getUsername(sourcePropUser, *sourceConfigNode);
        std::string configName = SyncConfig::normalizeConfigString(serverName, SyncConfig::NORMALIZE_LONG_FORMAT);
        std::string peer, context;
        SyncConfig::splitConfigString(configName, peer, context);
        key.object = "@";
        key.object += context;
        key.object += " ";
        key.object += sourceName;
        key.object += " backend";
        key.description = StringPrintf("databasePassword for %s in datastore %s",
                                       descr.c_str(), sourceName.c_str());
        return key;
    }
    virtual const std::string getDescr(const std::string &serverName,
                                  FilterConfigNode &globalConfigNode,
                                  const std::string &sourceName,
                                  const boost::shared_ptr<FilterConfigNode> &sourceConfigNode) const {
        std::string descr = sourceName;
        descr += " ";
        descr += ConfigProperty::getDescr();
        return descr;
    }
} sourcePropPassword;

static ConfigProperty sourcePropAdminData(SourceAdminDataName,
                                          "used by the Synthesis library internally; do not modify");

static IntConfigProperty sourcePropSynthesisID("synthesisID", "unique integer ID, necessary for libsynthesis", "0");

/**
 * Same as RegisterSyncConfigProperties, only for SyncSource properties.
 */
static class RegisterSyncSourceConfigProperties
{
public:
    RegisterSyncSourceConfigProperties()
    {
        ConfigPropertyRegistry &registry = SyncSourceConfig::getRegistry();

	// temporarily move existing properties away so that the important
	// standard properties come first when using the traditional
	// push_back() way of adding them
	ConfigPropertyRegistry tmp;
	std::swap(registry, tmp);

        registry.push_back(&SyncSourceConfig::m_sourcePropSync);
        registry.push_back(&sourcePropURI);
        registry.push_back(&sourcePropBackend);
        registry.push_back(&sourcePropSyncFormat);
        registry.push_back(&sourcePropForceSyncFormat);
        registry.push_back(&sourcePropDatabaseID);
        registry.push_back(&sourcePropDatabaseFormat);
        registry.push_back(&sourcePropUser);
        registry.push_back(&sourcePropPassword);
        registry.push_back(&sourcePropAdminData);
        registry.push_back(&sourcePropSynthesisID);

        BOOST_FOREACH (const ConfigProperty *prop, tmp) {
            registry.push_back(prop);
        }

        // obligatory source properties
        SyncSourceConfig::m_sourcePropSync.setObligatory(true);

        // hidden source properties - only possible for
        // non-shared properties (other hidden nodes don't
        // exist at the moment)
        sourcePropAdminData.setHidden(true);
        sourcePropSynthesisID.setHidden(true);

        // No global source properties. Does not make sense
        // conceptually.

        // peer independent source properties
        sourcePropBackend.setSharing(ConfigProperty::SOURCE_SET_SHARING);
        sourcePropDatabaseID.setSharing(ConfigProperty::SOURCE_SET_SHARING);
        sourcePropDatabaseFormat.setSharing(ConfigProperty::SOURCE_SET_SHARING);
        sourcePropUser.setSharing(ConfigProperty::SOURCE_SET_SHARING);
        sourcePropPassword.setSharing(ConfigProperty::SOURCE_SET_SHARING);
    }
} RegisterSyncSourceConfigProperties;


ConfigPropertyRegistry &SyncSourceConfig::getRegistry()
{
    static ConfigPropertyRegistry registry;
    return registry;
}

SyncSourceNodes::SyncSourceNodes(bool havePeerNode,
                                 const boost::shared_ptr<FilterConfigNode> &sharedNode,
                                 const boost::shared_ptr<FilterConfigNode> &peerNode,
                                 const boost::shared_ptr<ConfigNode> &hiddenPeerNode,
                                 const boost::shared_ptr<ConfigNode> &trackingNode,
                                 const boost::shared_ptr<ConfigNode> &serverNode,
                                 const string &cacheDir) :
    m_havePeerNode(havePeerNode),
    m_sharedNode(sharedNode),
    m_peerNode(peerNode),
    m_hiddenPeerNode(hiddenPeerNode),
    m_trackingNode(trackingNode),
    m_serverNode(serverNode),
    m_cacheDir(cacheDir)
{
    boost::shared_ptr<MultiplexConfigNode> mnode;
    mnode.reset(new MultiplexConfigNode(m_peerNode->getName(),
                                        SyncSourceConfig::getRegistry(),
                                        false));
    mnode->setHavePeerNodes(havePeerNode);
    m_props[false] = mnode;
    mnode->setNode(false, ConfigProperty::SOURCE_SET_SHARING,
                   m_sharedNode);
    mnode->setNode(false, ConfigProperty::NO_SHARING,
                   m_peerNode);
    // no multiplexing necessary for hidden peer properties yet
    m_props[true].reset(new FilterConfigNode(m_hiddenPeerNode));
}


boost::shared_ptr<FilterConfigNode>
SyncSourceNodes::getNode(const ConfigProperty &prop) const
{
    switch (prop.getSharing()) {
    case ConfigProperty::GLOBAL_SHARING:
        return boost::shared_ptr<FilterConfigNode>(new FilterConfigNode(boost::shared_ptr<ConfigNode>(new DevNullConfigNode("no global datastore properties"))));
        break;
    case ConfigProperty::SOURCE_SET_SHARING:
        if (prop.isHidden()) {
            return boost::shared_ptr<FilterConfigNode>(new FilterConfigNode(boost::shared_ptr<ConfigNode>(new DevNullConfigNode("no hidden datastore set properties"))));
        } else {
            return m_sharedNode;
        }
        break;
    case ConfigProperty::NO_SHARING:
        if (prop.isHidden()) {
            return boost::shared_ptr<FilterConfigNode>(new FilterConfigNode(m_hiddenPeerNode));
        } else {
            return m_peerNode;
        }
        break;
    }
    return boost::shared_ptr<FilterConfigNode>();
}

InitStateString SyncSourceConfig::getDatabaseID() const { return sourcePropDatabaseID.getProperty(*getNode(sourcePropDatabaseID)); }
void SyncSourceConfig::setDatabaseID(const string &value, bool temporarily) { sourcePropDatabaseID.setProperty(*getNode(sourcePropDatabaseID), value, temporarily); }
UserIdentity SyncSourceConfig::getUser() const {
    InitStateString user = sourcePropUser.getProperty(*getNode(sourcePropUser));
    UserIdentity id(UserIdentity::fromString(user));
    return id;
}

void SyncSourceConfig::setUsername(const string &value, bool temporarily) { sourcePropUser.setProperty(*getNode(sourcePropUser), value, temporarily); }
InitStateString SyncSourceConfig::getPassword() const {
    return sourcePropPassword.getProperty(*getNode(sourcePropPassword));
}
void SyncSourceConfig::setPassword(const string &value, bool temporarily) { sourcePropPassword.setProperty(*getNode(sourcePropPassword), value, temporarily); }
InitStateString SyncSourceConfig::getURI() const { return sourcePropURI.getProperty(*getNode(sourcePropURI)); }
InitStateString SyncSourceConfig::getURINonEmpty() const {
    InitStateString uri = sourcePropURI.getProperty(*getNode(sourcePropURI));
    if (uri.empty()) {
        uri = InitStateString(m_name, false);
    }
    return uri;
}
void SyncSourceConfig::setURI(const string &value, bool temporarily) { sourcePropURI.setProperty(*getNode(sourcePropURI), value, temporarily); }
InitStateString SyncSourceConfig::getSync() const { return m_sourcePropSync.getProperty(*getNode(m_sourcePropSync)); }
void SyncSourceConfig::setSync(const string &value, bool temporarily) { m_sourcePropSync.setProperty(*getNode(m_sourcePropSync), value, temporarily); }

SourceType::SourceType(const string &type)
{
    m_forceFormat = false;
    size_t colon = type.find(':');
    if (colon != type.npos) {
        m_backend = type.substr(0, colon);
        sourcePropBackend.normalizeValue(m_backend);
        string format = type.substr(colon + 1);
        if (boost::ends_with(format, "!")) {
            m_forceFormat = true;
            format.resize(format.size() - 1);
        }
        colon = format.find(':');
        if (colon != format.npos) {
            // ignore obsolete Mime version
            m_format = format.substr(0, colon);
        } else {
            m_format = format;
        }
        // no difference between remote and local format
        m_localFormat = m_format;
    } else {
        m_backend = type;
    }
}

string SourceType::toString() const
{
    string type = m_backend;
    if (!m_format.empty()) {
        type += ":";
        type += m_format;
        if (m_forceFormat) {
            type += "!";
        }
    }
    return type;
}

InitState<SourceType> SyncSourceConfig::getSourceType(const SyncSourceNodes &nodes)
{
    // legacy "type" property is tried if the backend property is not set
    InitStateString backend = sourcePropBackend.getProperty(*nodes.getNode(sourcePropBackend));
    if (!backend.wasSet()) {
        string type;
        if (nodes.getNode(sourcePropBackend)->getProperty("type", type)) {
            return InitState<SourceType>(SourceType(type), true);
        }
    }

    SourceType sourceType;
    sourceType.m_backend = backend;
    sourceType.m_localFormat = sourcePropDatabaseFormat.getProperty(*nodes.getNode(sourcePropDatabaseFormat));
    sourceType.m_format = sourcePropSyncFormat.getProperty(*nodes.getNode(sourcePropSyncFormat));
    sourceType.m_forceFormat = sourcePropForceSyncFormat.getPropertyValue(*nodes.getNode(sourcePropForceSyncFormat));
    return InitState<SourceType>(sourceType, backend.wasSet());
}
InitState<SourceType> SyncSourceConfig::getSourceType() const { return getSourceType(m_nodes); }

void SyncSourceConfig::setSourceType(const SourceType &type, bool temporarily)
{
    // writing always uses the new properties: the config must have
    // been converted to the new format before writing is allowed
    setBackend(type.m_backend, temporarily);
    setDatabaseFormat(type.m_localFormat, temporarily);
    setSyncFormat(InitStateString(type.m_format, !type.m_format.empty()), temporarily);
    setForceSyncFormat(type.m_forceFormat, temporarily);
}

void SyncSourceConfig::setBackend(const std::string &value, bool temporarily)
{
    sourcePropBackend.setProperty(*getNode(sourcePropBackend),
                                  value,
                                  temporarily);    
}
InitStateString SyncSourceConfig::getBackend() const
{
    return sourcePropBackend.getProperty(*getNode(sourcePropBackend));
}

void SyncSourceConfig::setDatabaseFormat(const std::string &value, bool temporarily)
{
    sourcePropDatabaseFormat.setProperty(*getNode(sourcePropDatabaseFormat),
                                         value,
                                         temporarily);
}
InitStateString SyncSourceConfig::getDatabaseFormat() const
{
    return sourcePropDatabaseFormat.getProperty(*getNode(sourcePropDatabaseFormat));
}

void SyncSourceConfig::setSyncFormat(const InitStateString &value, bool temporarily)
{
    sourcePropSyncFormat.setProperty(*getNode(sourcePropSyncFormat),
                                     value,
                                     temporarily);
}
InitStateString SyncSourceConfig::getSyncFormat() const
{
    return sourcePropSyncFormat.getProperty(*getNode(sourcePropSyncFormat));
}

void SyncSourceConfig::setForceSyncFormat(bool value, bool temporarily)
{
    sourcePropForceSyncFormat.setProperty(*getNode(sourcePropForceSyncFormat),
                                          value,
                                          temporarily);
}
InitState<bool> SyncSourceConfig::getForceSyncFormat() const
{
    return sourcePropForceSyncFormat.getPropertyValue(*getNode(sourcePropForceSyncFormat));
}

InitState<int> SyncSourceConfig::getSynthesisID() const {
    if (!m_synthesisID.wasSet()) {
        const_cast<InitState<int> &>(m_synthesisID) = sourcePropSynthesisID.getPropertyValue(*getNode(sourcePropSynthesisID));
    }
    return m_synthesisID;
}
void SyncSourceConfig::setSynthesisID(int value, bool temporarily) {
    m_synthesisID = value;
    sourcePropSynthesisID.setProperty(*getNode(sourcePropSynthesisID), value, temporarily);
}


// Used for built-in templates
SyncConfig::TemplateDescription::TemplateDescription (const std::string &name, const std::string &description)
:   m_templateId (name), m_description (description)
{
    m_rank = TemplateConfig::LEVEL3_MATCH;
    m_deviceName = "";
    m_path = "";
    m_matchedModel = name;
}

/* Ranking of template description is controled by the rank field, larger the
 * better
 */
bool SyncConfig::TemplateDescription::compare_op (boost::shared_ptr<SyncConfig::TemplateDescription> &left, boost::shared_ptr<SyncConfig::TemplateDescription> &right)
{
    //first sort against the fingerprint string
    if (left->m_deviceName != right->m_deviceName) {
        return (left->m_deviceName < right->m_deviceName);
    }
    // sort against the rank
    if (right->m_rank != left->m_rank) {
        return (right->m_rank < left->m_rank);
    }
    // sort against the template id, case-insensitive (for eGroupware < Funambol)
    return boost::ilexicographical_compare(left->m_templateId, right->m_templateId);
}

TemplateConfig::TemplateConfig(const string &path) :
    m_template(new SingleFileConfigTree(path))
{
    boost::shared_ptr<ConfigNode> metaNode = m_template->open("template.ini");
    metaNode->readProperties(m_metaProps);
}

bool TemplateConfig::isTemplateConfig (const string &path) 
{
    SingleFileConfigTree templ(path);
    boost::shared_ptr<ConfigNode> metaNode = templ.open("template.ini");
    if (!metaNode->exists()) {
        return false;
    }
    ConfigProps props;
    metaNode->readProperties(props);
    return !props.empty();
}

bool TemplateConfig::isTemplateConfig() const
{
    return !m_metaProps.empty();
}

int TemplateConfig::serverModeMatch (SyncConfig::MatchMode mode)
{
    if (mode != SyncConfig::MATCH_FOR_SERVER_MODE &&
        mode != SyncConfig::MATCH_FOR_CLIENT_MODE) {
        // no need to read config, peerIsClient doesn't matter
        // => fall back to BEST_MATCH directly
        return BEST_MATCH;
    }

    boost::shared_ptr<ConfigNode> configNode = m_template->open("config.ini");
    std::string peerIsClient = configNode->readProperty ("peerIsClient");
    
    //not a match if serverMode does not match
    if ((peerIsClient.empty() || peerIsClient == "0") && mode == SyncConfig::MATCH_FOR_SERVER_MODE) {
        return NO_MATCH;
    }
    if (peerIsClient == "1" && mode == SyncConfig::MATCH_FOR_CLIENT_MODE){
        return NO_MATCH;
    }
    return BEST_MATCH;
}

/**
 * The matching is based on Least common string algorithm,
 * with space, hyphen and underscore being treated as equal.
 * */
int TemplateConfig::fingerprintMatch (const string &fingerprint)
{
    //if input "", match all
    if (fingerprint.empty()) {
        return LEVEL3_MATCH;
    }

    std::string fingerprintProp = m_metaProps["fingerprint"];
    std::vector <string> subfingerprints = unescapeJoinedString (fingerprintProp, ',');
    std::string input = fingerprint;
    boost::to_lower(input);
    boost::replace_all(input, " ", "_");
    boost::replace_all(input, "-", "_");
    //return the largest match value
    int max = NO_MATCH;
    BOOST_FOREACH (std::string sub, subfingerprints){
        std::vector< LCS::Entry <char> > result;
        std::string match = sub;
        boost::to_lower(match);
        boost::replace_all(match, " ", "_");
        boost::replace_all(match, "-", "_");
        LCS::lcs(match, input, std::back_inserter(result), LCS::accessor_sequence<std::string>());
        int score = result.size() *2 *BEST_MATCH /(sub.size() + fingerprint.size()) ;
        if (score > max) {
            max = score;
        }
    }
    return max;
}

int TemplateConfig::metaMatch (const std::string &fingerprint, SyncConfig::MatchMode mode)
{
    int serverMatch = serverModeMatch (mode);
    if (serverMatch == NO_MATCH){
        return NO_MATCH;
    }
    int fMatch = fingerprintMatch (fingerprint);
    return (serverMatch *1 + fMatch *3) >>2;
}

string TemplateConfig::getDescription(){
    return m_metaProps["description"];
}

string TemplateConfig::getFingerprint(){
    return m_metaProps["fingerprint"];
}

string TemplateConfig::getTemplateName() {
    return m_metaProps["templateName"];
}

/*
 * A unique identifier for this template, it must be unique and retrieveable.
 * We use the first entry in the "fingerprint" property for cmdline and
 * replace spaces with underscores, to make it more command line friendly.
 **/
string TemplateConfig::getTemplateId(){
    if (m_id.empty()){
        std::string fingerprintProp = m_metaProps["fingerprint"];
        if (!fingerprintProp.empty()){
            std::vector<std::string> subfingerprints = unescapeJoinedString (fingerprintProp, ',');
            m_id = subfingerprints[0];
        }
        boost::replace_all(m_id, " ", "_");
    }
    return m_id;
}

bool SecondsConfigProperty::checkValue(const string &value, string &error) const
{
    unsigned int seconds;
    return parseDuration(value, error, seconds);
}

InitState<unsigned int> SecondsConfigProperty::getPropertyValue(const ConfigNode &node) const
{
    string name = getName(node);
    bool wasSet;
    std::string value = node.readProperty(name);
    if (value.empty()) {
        wasSet = false;
        value = getDefValue();
    } else {
        wasSet = true;
    }
    string error;
    unsigned int seconds;
    if (!parseDuration(value, error, seconds)) {
        throwValueError(node, name, value, error);
    }
    return InitState<unsigned int>(seconds, wasSet);
}

bool SecondsConfigProperty::parseDuration(const string &value, string &error, unsigned int &seconds)
{
    seconds = 0;
    if (value.empty()) {
        // ambiguous - zero seconds?!
        error = "duration expected, empty string not valid";
        return false;
    }

    unsigned int current = 0;
    bool haveDigit = false;
    BOOST_FOREACH(char c, value) {
        if (isdigit(c)) {
            current = current * 10 + (c - '0');
            haveDigit = true;
        } else {
            unsigned int multiplier = 1;
            switch (toupper(c)) {
            case 'Y':
                multiplier = 365 * 24 * 60 * 60;
                break;
            case 'D':
                multiplier = 24 * 60 * 60;
                break;
            case 'H':
                multiplier = 60 * 60;
                break;
            case 'M':
                multiplier = 60;
                break;
            case 'S':
                break;
            case ' ':
            case '\t':
                continue;
            case '+':
                break;
            default:
                error = StringPrintf("invalid character '%c'", c);
                return false;
            }
            if (!haveDigit && c != '+') {
                error = StringPrintf("unit character without preceeding number: %c", c);
                return false;
            }
            seconds += current * multiplier;
            current = 0;
            haveDigit = false;
        }
    }
    seconds += current;
    return true;
}


#ifdef ENABLE_UNIT_TESTS

class SyncConfigTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(SyncConfigTest);
    CPPUNIT_TEST(normalize);
    CPPUNIT_TEST(parseDuration);
    CPPUNIT_TEST(propertySpec);
    CPPUNIT_TEST_SUITE_END();

private:
    void normalize()
    {
        // use same dir as CmdlineTest...
        ScopedEnvChange xdg("XDG_CONFIG_HOME", "CmdlineTest");
        ScopedEnvChange home("HOME", "CmdlineTest");

        rm_r("CmdlineTest");

        CPPUNIT_ASSERT_EQUAL(std::string("@default"),
                             SyncConfig::normalizeConfigString(""));
        CPPUNIT_ASSERT_EQUAL(std::string("@default"),
                             SyncConfig::normalizeConfigString("@default"));
        CPPUNIT_ASSERT_EQUAL(std::string("@default"),
                             SyncConfig::normalizeConfigString("@DeFaULT"));
        CPPUNIT_ASSERT_EQUAL(std::string("foobar"),
                             SyncConfig::normalizeConfigString("FooBar"));
        CPPUNIT_ASSERT_EQUAL(std::string("foobar@something"),
                             SyncConfig::normalizeConfigString("FooBar@Something"));
        CPPUNIT_ASSERT_EQUAL(std::string("foo_bar_x_y_z"),
                             SyncConfig::normalizeConfigString("Foo/bar\\x:y:z"));

        // keep @default if explicitly requested
        CPPUNIT_ASSERT_EQUAL(std::string("foobar@default"),
                             SyncConfig::normalizeConfigString("FooBar", SyncConfig::NORMALIZE_LONG_FORMAT));

        // Test config lookup. We expect all of these to exist on disk, so
        // set some property. Empty configs do not get written to disk.
        // This allows testing for existance.
        SyncConfig foo_default("foo"), foo_other("foo@other"), bar("bar@other");
        foo_default.setLogLevel(10);
        foo_other.setLogLevel(10);
        bar.setLogLevel(10);
        foo_default.flush();
        foo_other.flush();
        bar.flush();
        CPPUNIT_ASSERT_EQUAL(std::string("foo"),
                             SyncConfig::normalizeConfigString("foo"));
        CPPUNIT_ASSERT_EQUAL(std::string("foo"),
                             SyncConfig::normalizeConfigString("foo@default"));
        CPPUNIT_ASSERT_EQUAL(std::string("foo@default"),
                             SyncConfig::normalizeConfigString("foo", SyncConfig::NORMALIZE_LONG_FORMAT));
        CPPUNIT_ASSERT_EQUAL(std::string("foo@default"),
                             SyncConfig::normalizeConfigString("foo@default", SyncConfig::NORMALIZE_LONG_FORMAT));
        CPPUNIT_ASSERT_EQUAL(std::string("foo@other"),
                             SyncConfig::normalizeConfigString("foo@other"));
        foo_default.remove();
        CPPUNIT_ASSERT_EQUAL(std::string("foo@other"),
                             SyncConfig::normalizeConfigString("foo"));
        CPPUNIT_ASSERT_EQUAL(std::string("foo@other"),
                             SyncConfig::normalizeConfigString("foo", SyncConfig::NORMALIZE_LONG_FORMAT));
    }

    void parseDuration()
    {
        string error;
        unsigned int seconds;
        unsigned int expected;

        CPPUNIT_ASSERT(!SecondsConfigProperty::parseDuration("foo", error, seconds));
        CPPUNIT_ASSERT_EQUAL(error, string("invalid character 'f'"));
        CPPUNIT_ASSERT(!SecondsConfigProperty::parseDuration("1g", error, seconds));
        CPPUNIT_ASSERT_EQUAL(error, string("invalid character 'g'"));
        CPPUNIT_ASSERT(!SecondsConfigProperty::parseDuration("", error, seconds));
        CPPUNIT_ASSERT_EQUAL(error, string("duration expected, empty string not valid"));

        expected = 5;
        CPPUNIT_ASSERT(SecondsConfigProperty::parseDuration("5", error, seconds));
        CPPUNIT_ASSERT_EQUAL(expected, seconds);
        CPPUNIT_ASSERT(SecondsConfigProperty::parseDuration("05", error, seconds));
        CPPUNIT_ASSERT_EQUAL(expected, seconds);
        CPPUNIT_ASSERT(SecondsConfigProperty::parseDuration("05s", error, seconds));
        CPPUNIT_ASSERT_EQUAL(expected, seconds);
        CPPUNIT_ASSERT(SecondsConfigProperty::parseDuration("5s", error, seconds));
        CPPUNIT_ASSERT_EQUAL(expected, seconds);

        expected = (((1 * 365 + 2) * 24 + 3) * 60 + 4) * 60 + 5;
        CPPUNIT_ASSERT(SecondsConfigProperty::parseDuration("1y2d3H4M5s", error, seconds));
        CPPUNIT_ASSERT_EQUAL(expected, seconds);
        CPPUNIT_ASSERT(SecondsConfigProperty::parseDuration("5 + 1y+2d + 3 H4M", error, seconds));
        CPPUNIT_ASSERT_EQUAL(expected, seconds);

        CPPUNIT_ASSERT(!SecondsConfigProperty::parseDuration("m", error, seconds));
    }

    void propertySpec()
    {
        ScopedEnvChange xdg("XDG_CONFIG_HOME", "/dev/null");
        ScopedEnvChange home("HOME", "/dev/null");
        PropertySpecifier spec;

        spec = PropertySpecifier::StringToPropSpec("foo");
        CPPUNIT_ASSERT_EQUAL(string(""), spec.m_source);
        CPPUNIT_ASSERT_EQUAL(string("foo"), spec.m_property);
        CPPUNIT_ASSERT_EQUAL(string(""), spec.m_config);
        CPPUNIT_ASSERT_EQUAL(string("foo"), spec.toString());

        spec = PropertySpecifier::StringToPropSpec("source/foo@ContEXT");
        CPPUNIT_ASSERT_EQUAL(string("source"), spec.m_source);
        CPPUNIT_ASSERT_EQUAL(string("foo"), spec.m_property);
        CPPUNIT_ASSERT_EQUAL(string("@context"), spec.m_config);       
        CPPUNIT_ASSERT_EQUAL(string("source/foo@context"), spec.toString());

        spec = PropertySpecifier::StringToPropSpec("source/foo@ContEXT", PropertySpecifier::NO_NORMALIZATION);
        CPPUNIT_ASSERT_EQUAL(string("source"), spec.m_source);
        CPPUNIT_ASSERT_EQUAL(string("foo"), spec.m_property);
        CPPUNIT_ASSERT_EQUAL(string("@ContEXT"), spec.m_config);       
        CPPUNIT_ASSERT_EQUAL(string("source/foo@ContEXT"), spec.toString());

        spec = PropertySpecifier::StringToPropSpec("foo@peer@context");
        CPPUNIT_ASSERT_EQUAL(string(""), spec.m_source);
        CPPUNIT_ASSERT_EQUAL(string("foo"), spec.m_property);
        CPPUNIT_ASSERT_EQUAL(string("peer@context"), spec.m_config);
        CPPUNIT_ASSERT_EQUAL(string("foo@peer@context"), spec.toString());

        spec = PropertySpecifier::StringToPropSpec("foo@context");
        CPPUNIT_ASSERT_EQUAL(string(""), spec.m_source);
        CPPUNIT_ASSERT_EQUAL(string("foo"), spec.m_property);
        CPPUNIT_ASSERT_EQUAL(string("@context"), spec.m_config);
        CPPUNIT_ASSERT_EQUAL(string("foo@context"), spec.toString());

        spec = PropertySpecifier::StringToPropSpec("source/foo");
        CPPUNIT_ASSERT_EQUAL(string("source"), spec.m_source);
        CPPUNIT_ASSERT_EQUAL(string("foo"), spec.m_property);
        CPPUNIT_ASSERT_EQUAL(string(""), spec.m_config);
        CPPUNIT_ASSERT_EQUAL(string("source/foo"), spec.toString());

        spec = PropertySpecifier::StringToPropSpec("");
        CPPUNIT_ASSERT_EQUAL(string(""), spec.m_source);
        CPPUNIT_ASSERT_EQUAL(string(""), spec.m_property);
        CPPUNIT_ASSERT_EQUAL(string(""), spec.m_config);
        CPPUNIT_ASSERT_EQUAL(string(""), spec.toString());
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(SyncConfigTest);

#endif // ENABLE_UNIT_TESTS

SE_END_CXX
