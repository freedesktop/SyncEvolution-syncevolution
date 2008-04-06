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

#include "FileConfigNode.h"
#include "EvolutionSyncClient.h"
#include "SyncEvolutionUtil.h"

#include <boost/scoped_array.hpp>

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

/** @TODO: replace stdio.h with streams */

FileConfigNode::FileConfigNode(const string &path, const string &fileName) :
    m_path(path),
    m_fileName(fileName),
    m_modified(false),
    m_exists(false)
{
    read();
}

void FileConfigNode::read()
{
    string filename = m_path + "/" + m_fileName;

    FILE *file = fopen(filename.c_str(), "r");
    char buffer[512];

    m_lines.clear();
    if (file) {
        while (fgets(buffer, sizeof(buffer), file)) {
            char *eol = strchr(buffer, '\n');
            if (eol) {
                *eol = 0;
            }
            m_lines.push_back(buffer);
        }
        m_exists = true;
        fclose(file);
    }
    m_modified = false;
}

void FileConfigNode::flush()
{
    if (!m_modified) {
        return;
    }

    mkdir_p(m_path);

    string filename = m_path + "/" + m_fileName;
    string tmpFilename = m_path + "/.#" + m_fileName;

    FILE *file = fopen(tmpFilename.c_str(), "w");
    if (file) {
        for (list<string>::const_iterator it = m_lines.begin();
             it != m_lines.end();
             it++) {
            fprintf(file, "%s\n", it->c_str());
        }
        fflush(file);
        bool failed = ferror(file);
        if (fclose(file)) {
            failed = true;
        }
        if (failed ||
            rename(tmpFilename.c_str(), filename.c_str())) {
            EvolutionSyncClient::throwError(tmpFilename + ": " + strerror(errno));
        }
    } else {
        EvolutionSyncClient::throwError(tmpFilename + ": " + strerror(errno));
    }

    m_modified = false;
    m_exists = true;
}

/**
 * get property and value from line, if any present
 */
static bool getContent(const string &line,
                       string &property,
                       string &value,
                       bool &isComment,
                       bool fuzzyComments)
{
    size_t start = 0;
    while (start < line.size() &&
           isspace(line[start])) {
        start++;
    }

    // empty line?
    if (start == line.size()) {
        return false;
    }

    // Comment? Potentially keep reading, might be commented out assignment.
    isComment = false;
    if (line[start] == '#') {
        if (!fuzzyComments) {
            return false;
        }
        isComment = true;
    }

    // recognize # <word> = <value> as commented out (= default) value
    if (isComment) {
        start++;
        while (start < line.size() &&
               isspace(line[start])) {
            start++;
        }
    }

    // extract property
    size_t end = start;
    while (end < line.size() &&
           !isspace(line[end])) {
        end++;
    }
    property = line.substr(start, end - start);

    // skip assignment 
    start = end;
    while (start < line.size() &&
           isspace(line[start])) {
        start++;
    }
    if (start == line.size() ||
        line[start] != '=') {
        // invalid syntax or we tried to read a comment as assignment
        return false;
    }

    // extract value
    start++;
    while (start < line.size() &&
           isspace(line[start])) {
        start++;
    }

    value = line.substr(start);
    // remove trailing white space: usually it is
    // added accidentally by users
    size_t numspaces = 0;
    while (numspaces < value.size() &&
           isspace(value[value.size() - 1 - numspaces])) {
        numspaces++;
    }
    value.erase(value.size() - numspaces);

    // @TODO: strip quotation marks around value?!
    
    return true;    
}

/**
 * check whether the line contains the property and if so, extract its value
 */
static bool getValue(const string &line,
                     const string &property,
                     string &value,
                     bool &isComment,
                     bool fuzzyComments)

{
    string curProp;
    return getContent(line, curProp, value, isComment, fuzzyComments) &&
        !strcasecmp(curProp.c_str(), property.c_str());
}

string FileConfigNode::readProperty(const string &property) const {
    string value;

    for (list<string>::const_iterator it = m_lines.begin();
         it != m_lines.end();
         it++) {
        const string &line = *it;
        bool isComment;

        if (getValue(line, property, value, isComment, false)) {
            return value;
        }
    }
    return "";
}



map<string, string> FileConfigNode::readProperties() const {
    map<string, string> res;
    string value, property;

    for (list<string>::const_iterator it = m_lines.begin();
         it != m_lines.end();
         it++) {
        const string &line = *it;
        bool isComment;
        if (getContent(line, property, value, isComment, false)) {
            // don't care about the result: only the first instance
            // of the property counts, so it doesn't matter when
            // inserting it again later fails
            res.insert(pair<string, string>(property, value));
        }
    }
    return res;
}

void FileConfigNode::removeProperty(const string &property)
{
    string value;

    list<string>::iterator it = m_lines.begin();
    while (it != m_lines.end()) {
        const string &line = *it;
        bool isComment;
        if (getValue(line, property, value, isComment, false)) {
            it = m_lines.erase(it);
            m_modified = true;
        } else {
            it++;
        }
    }
}

void FileConfigNode::setProperty(const string &property,
                                 const string &newvalue,
                                 const string &comment,
                                 const string *defValue) {
    string newstr;
    string oldvalue;
    bool isDefault = false;

    if (defValue &&
        *defValue == newvalue) {
        newstr += "# ";
        isDefault = true;
    }
    newstr += property + " = " + newvalue;

    for (list<string>::iterator it = m_lines.begin();
         it != m_lines.end();
         it++) {
        const string &line = *it;
        bool isComment;

        if (getValue(line, property, oldvalue, isComment, true)) {
            if (newvalue != oldvalue ||
                isComment && !isDefault) {
                *it = newstr;
                m_modified = true;
            }
            return;
        }
    }

    // add each line of the comment as separate line in .ini file
    if (comment.size()) {
        list<string> commentLines;
        ConfigProperty::splitComment(comment, commentLines);
        if (m_lines.size()) {
            m_lines.push_back("");
        }
        for (list<string>::const_iterator it = commentLines.begin();
             it != commentLines.end();
             ++it) {
            m_lines.push_back(string("# " + *it));
        }
    }

    m_lines.push_back(newstr);
    m_modified = true;
}

