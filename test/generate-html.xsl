<?xml version='1.0'?>
<!--
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
-->
<xsl:stylesheet version="1.0" 
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
    <!-- This program is used to generate html to represent nightly test results by combining today's test results and comparison info -->
    <!-- TODO: Add more checkings to avoid generate un-welformed html files -->
    <!--xsl:strip-space elements="*"/-->
    <xsl:param name="cmp_result_file" select="''"/> <!-- comparsion result file, necessary -->
	<xsl:output method="html" indent="yes" encoding="UTF-8"/>
    
    <!-- comparison result strings -->
    <xsl:variable name="equal" select="'equal'"/> 
    <xsl:variable name="equal-in-semantics" select="'equal-in-semantics'"/>
    <xsl:variable name="regression" select="'regression'"/>
    <xsl:variable name="improved" select="'improved'"/>
    <xsl:variable name="skipped" select="'skipped'"/>
    <xsl:variable name="invalid-value" select="'invalid-value'"/>

    <!-- log file suffix name -->
    <xsl:variable name="log-file-suffix" select="'.log'"/>
	
    <xsl:template match="/">
        <xsl:choose>
            <xsl:when test="$cmp_result_file!=''"> <!-- check comparison file path -->
                <html>
                    <xsl:call-template name="generate-html-header"/>
                    <xsl:variable name="cmp_result_tree" select="document($cmp_result_file)"/>
                    <xsl:call-template name="generate-html-body">
                        <xsl:with-param name="cmp_result_tree" select="$cmp_result_tree"/>
                    </xsl:call-template>
                </html>
            </xsl:when>
            <xsl:otherwise>
                <xsl:text>No cmp_result_file, please set comparison result file by: 
                    --stringparam cmp_result_file [your file path]
                </xsl:text>
            </xsl:otherwise>
        </xsl:choose>
    </xsl:template>
    <xsl:template name="generate-html-body">
        <xsl:param name="cmp_result_tree"/>
        <body>
            <xsl:variable name="log-dir-uri" select="nightly-test/log-info/uri"/>
            <xsl:call-template name="generate-platform-info">
                <xsl:with-param name="platform" select="nightly-test/platform-info"/>
            </xsl:call-template>
            <xsl:call-template name="generate-prepare-info">
                <xsl:with-param name="source" select="nightly-test/prepare"/>
                <xsl:with-param name="cmp-result" select="$cmp_result_tree/comparison/prepare"/>
                <xsl:with-param name="log-path" select="$log-dir-uri"/>
            </xsl:call-template>
            <!-- todo: check compile status, if it fails, no need to generate client-test info -->
            <xsl:call-template name="generate-client-test-info">
                <xsl:with-param name="source" select="nightly-test/client-test"/>
                <xsl:with-param name="cmp-result" select="$cmp_result_tree/comparison/client-test"/>
                <xsl:with-param name="log-path" select="$log-dir-uri"/>
            </xsl:call-template>
            <xsl:call-template name="generate-html-notes"/>
        </body>
    </xsl:template>

    <xsl:template name="generate-client-test-info">
        <xsl:param name="source"/>
        <xsl:param name="cmp-result"/>
        <xsl:param name="log-path"/> <!-- log file path -->

        <h1><font color="Olive"> Client-test Results</font> </h1>
        <xsl:choose>
            <xsl:when test="$source">
                <h2> Client-test Summary </h2>
                <xsl:call-template name="generate-servers-summary">
                    <xsl:with-param name="unit-cases" select="$source/sync/template/*"/>
                    <xsl:with-param name="servers" select="$source/sync/*[name(.)!='template']"/>
                    <xsl:with-param name="cmp-result" select="$cmp-result/sync"/>
                </xsl:call-template>

                <xsl:call-template name="generate-source-summary">
                    <xsl:with-param name="source" select="$source/source"/>
                    <xsl:with-param name="cmp-result" select="$cmp-result/source"/>
                </xsl:call-template>

                <h2> Client-test Detail</h2>
                <xsl:call-template name="generate-client-test-sync">
                    <xsl:with-param name="source" select="$source/sync"/>
                    <xsl:with-param name="cmp-result" select="$cmp-result/sync"/>
                    <xsl:with-param name="log-path" select="$log-path"/>
                </xsl:call-template>

                <xsl:call-template name="generate-client-test-source">
                    <xsl:with-param name="source" select="$source/source"/>
                    <xsl:with-param name="cmp-result" select="$cmp-result/source"/>
                    <xsl:with-param name="log-path" select="$log-path"/>
                </xsl:call-template>
            </xsl:when>
            <xsl:otherwise>
                No client-test info!
                <br/>
            </xsl:otherwise>
        </xsl:choose>
    </xsl:template>

    <xsl:template name="generate-client-test-source">
        <xsl:param name="source"/>
        <xsl:param name="cmp-result"/>
        <xsl:param name="log-path"/> <!-- log file path -->

        <xsl:call-template name="generate-desc-and-table">
            <xsl:with-param name="source" select="$source"/>
            <xsl:with-param name="cmp-result" select="$cmp-result"/>
            <xsl:with-param name="log-path" select="concat($log-path,'/',string($source/@path), '/')"/>
            <xsl:with-param name="desc-size" select="'3'"/>
            <xsl:with-param name="description" select="'Client::Source Test Results'"/>
        </xsl:call-template>
    </xsl:template>

    <xsl:template name="generate-client-test-sync">
        <xsl:param name="source"/>
        <xsl:param name="cmp-result"/>
        <xsl:param name="log-path"/> <!-- log file path -->

        <xsl:variable name="list-of-unit-cases" select="$source/template/*"/>
        <xsl:variable name="servers" select="$source/*[name(.)!='template']"/>
        <xsl:variable name="target-servers" select="$cmp-result/*"/>

        <h3> Client::Sync Test Results </h3>
        <xsl:for-each select="$servers">
            <xsl:variable name="server" select="."/>
            <xsl:variable name="cmp-result" select="$cmp-result/*[name(.)=name($server)]"/>
            <xsl:call-template name="generate-server-table">
                <xsl:with-param name="source" select="$server"/>
                <xsl:with-param name="cmp-result" select="$cmp-result"/>
                <xsl:with-param name="log-path" select="concat($log-path,'/',$server/@path,'/')"/>
                <xsl:with-param name="desc-size" select="'2'"/>
                <xsl:with-param name="description" select="name($server)"/>
                <xsl:with-param name="list-of-unit-cases" select="$list-of-unit-cases"/>
            </xsl:call-template>
        </xsl:for-each>
    </xsl:template>
    
    <xsl:template name="generate-html-header">
        <head>
            <meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>
            <title>SyncEvolution NightlyTest</title>
        </head>
    </xsl:template>

    <xsl:template name="generate-platform-info">
        <xsl:param name="platform"/>
        <xsl:variable name="list" select="$platform/*"/>
        <xsl:comment>
            Generate platform information in a table
        </xsl:comment>
        <h1><font color="Olive"> Platform Information </font> </h1>
        <xsl:choose>
            <xsl:when test="count($list)">
                <table border="2">
                    <tr>
                        <th>Item</th>
                        <th>Value</th>
                    </tr>
                    <xsl:for-each select="$platform/*">
                        <tr>
                            <td><xsl:value-of select="name(.)"/></td>
                            <td><xsl:value-of select="."/></td>
                        </tr>
                    </xsl:for-each>
                </table>
            </xsl:when>
            <xsl:otherwise>
                <xsl:text> No platform information!</xsl:text>
                <br/>
            </xsl:otherwise>
        </xsl:choose>
    </xsl:template>

    <xsl:template name="generate-prepare-info">
        <xsl:param name="source"/>
        <xsl:param name="cmp-result"/>
        <xsl:param name="log-path"/> <!-- log file path -->

        <xsl:comment>
            Generate preparation information in a table
        </xsl:comment>
        <xsl:call-template name="generate-table-info">
            <xsl:with-param name="source" select="$source"/>
            <xsl:with-param name="cmp-result" select="$cmp-result"/>
            <xsl:with-param name="log-path" select="concat($log-path,'/')"/>
            <xsl:with-param name="log-name" select="'output.txt'"/>
            <xsl:with-param name="desc-size" select="'1'"/>
            <xsl:with-param name="description" select="'Preparation Results'"/>
            <xsl:with-param name="desc-color" select="'olive'"/>
        </xsl:call-template>
    </xsl:template>
    <xsl:template name="check-prepare">
        <xsl:variable name="nodes-all" select="nightly-test/prepare/*"/>
        <xsl:if test="count($nodes-all)!=0 and contains(string($nodes-all),'failed')">
            <xsl:text>error</xsl:text>
        </xsl:if>
    </xsl:template>
    <xsl:template name="generate-servers-summary">
        <xsl:param name="unit-cases"/>
        <xsl:param name="servers"/>
        <xsl:param name="cmp-result"/>

        <h3> Servers Interoperability Test Summary </h3>
        <table border="2">
            <tr>
                <th width="60">Server</th>
                <th width="60">Categories</th>
                <th width="60">Total Cases</th>
                <th width="60">Passed</th>
                <th width="60">Failed</th>
                <th width="60">Skipped</th>
                <th width="60">Passrate</th>
                <th width="60">Improved</th>
                <th width="60">Regression</th>
            </tr>

            <xsl:for-each select="$servers">
                <xsl:variable name="item" select="."/>
                <xsl:variable name="categories" select="$item/*"/>
                <xsl:variable name="all" select="count($unit-cases) * count($categories)"/>
                <xsl:variable name="passed" select="count($item/*/*[.='okay'])"/>
                <xsl:variable name="failed" select="count($item/*/*[.='failed'])"/>
                <xsl:variable name="status" select="$cmp-result/*[name(.)=name($item)]/@summary"/>
                <xsl:variable name="real-regression" select="$cmp-result/*[name(.)=name($item)]/*/*[.=$regression]"/>
                <xsl:variable name="real-improved" select="$cmp-result/*[name(.)=name($item)]/*/*[.=$improved]"/>
                <tr>
                    <td> <!-- server name -->
                        <a href="#{name($item)}">
                            <xsl:value-of select="name($item)"/>
                        </a>
                    </td>
                    <td>
                        <xsl:for-each select="$categories">
                            <xsl:value-of select="name(.)"/>
                            <xsl:if test="position()!=last()">
                                <xsl:value-of select="', '"/>
                            </xsl:if>
                        </xsl:for-each>
                    </td>
                    <td>
                        <xsl:value-of select="$all"/>
                    </td>
                    <td>
                        <xsl:value-of select="$passed"/>
                    </td>
                    <td>
                        <xsl:value-of select="$failed"/>
                    </td>
                    <td>
                        <xsl:value-of select="$all - $passed - $failed"/>
                    </td>
                    <td>
                        <xsl:variable name="passrate" select="$passed div ($passed+$failed) * 100"/>
                        <!--xsl:value-of select="concat(substring-before(string($passrate),'.'),'.', su)"/-->
                        <xsl:value-of select="substring(string($passrate),1,5)"/>
                        <xsl:value-of select="'%'"/>
                    </td>
                    <td>
                        <xsl:choose>
                            <xsl:when test="count($real-improved)">
                                <xsl:attribute name="bgcolor">
                                    <xsl:call-template name="generate-color">
                                        <xsl:with-param name="status" select="$improved"/>
                                    </xsl:call-template>
                                </xsl:attribute>
                                <xsl:value-of select="count($real-improved)"/>
                            </xsl:when>
                            <xsl:otherwise>
                                0
                            </xsl:otherwise>
                        </xsl:choose>
                    </td>
                    <td>
                        <xsl:choose>
                            <xsl:when test="count($real-regression)">
                                <xsl:attribute name="bgcolor">
                                    <xsl:call-template name="generate-color">
                                        <xsl:with-param name="status" select="$regression"/>
                                    </xsl:call-template>
                                </xsl:attribute>
                                <xsl:value-of select="count($real-regression)"/>
                            </xsl:when>
                            <xsl:otherwise>
                                0
                            </xsl:otherwise>
                        </xsl:choose>
                    </td>
                </tr>
            </xsl:for-each>

        </table>
    </xsl:template>

    <xsl:template name="generate-source-summary">
        <xsl:param name="source"/>
        <xsl:param name="cmp-result"/>

        <h3> Client Source Test Summary </h3>
        <table border="2">
            <tr>
                <th width="60">Source</th>
                <th width="60">Total Cases</th>
                <th width="60">Passed</th>
                <th width="60">Failed</th>
                <th width="60">Skipped</th>
                <th width="60">Passrate</th>
                <th width="60">Improved</th>
                <th width="60">Regression</th>
            </tr>

            <xsl:for-each select="$source/*">
                <xsl:variable name="item" select="."/>
                <xsl:variable name="all" select="count($item/*)"/>
                <xsl:variable name="passed" select="count($item/*[.='okay'])"/>
                <xsl:variable name="failed" select="count($item/*[.='failed'])"/>
                <xsl:variable name="status" select="$cmp-result/*[name(.)=name($item)]/@summary"/>
                <tr>
                    <td> <!-- server name -->
                        <a href="#{name($item)}">
                            <xsl:value-of select="name($item)"/>
                        </a>
                    </td>
                    <td> 
                        <xsl:value-of select="$all"/>
                    </td>
                    <td> 
                        <xsl:value-of select="$passed"/>
                    </td>
                    <td> 
                        <xsl:value-of select="$failed"/>
                    </td>
                    <td>
                        <xsl:value-of select="$all - $passed - $failed"/>
                    </td>
                    <td>
                        <xsl:variable name="passrate" select="$passed div ($passed+$failed) * 100"/>
                        <!--xsl:value-of select="concat(substring-before(string($passrate),'.'),'.', su)"/-->
                        <xsl:value-of select="substring(string($passrate),1,5)"/>
                        <xsl:value-of select="'%'"/>
                    </td>
                    <td>
                        <xsl:choose>
                            <xsl:when test="number($status) &gt; 0">
                                <xsl:attribute name="bgcolor">
                                    <xsl:call-template name="generate-color">
                                        <xsl:with-param name="status" select="$improved"/>
                                    </xsl:call-template>
                                </xsl:attribute>
                                <xsl:value-of select="$status"/>
                            </xsl:when>
                            <xsl:otherwise>
                                0
                            </xsl:otherwise>
                        </xsl:choose>
                    </td>
                    <td>
                        <xsl:choose>
                            <xsl:when test="number($status) &lt; 0">
                                <xsl:attribute name="bgcolor">
                                    <xsl:call-template name="generate-color">
                                        <xsl:with-param name="status" select="$regression"/>
                                    </xsl:call-template>
                                </xsl:attribute>
                                <xsl:value-of select="0 - number($status)"/>
                            </xsl:when>
                            <xsl:otherwise>
                                0
                            </xsl:otherwise>
                        </xsl:choose>
                    </td>
                </tr>
            </xsl:for-each>
        </table>
    </xsl:template>

    <!-- generate each server interoperability test -->
    <xsl:template name="generate-server-table">
        <xsl:param name="source"/>
        <xsl:param name="cmp-result"/>
        <xsl:param name="log-path"/> <!-- log file path -->
        <xsl:param name="desc-size" select="'3'"/>
        <xsl:param name="description" select="''"/>
        <xsl:param name="list-of-unit-cases"/>

        <xsl:comment>
            This is used to generate a table for each server test results for all PIMs
        </xsl:comment>
        <xsl:element name="h{$desc-size}"> 
            <a name="{name($source)}">
                <xsl:value-of select="$description"/> 
            </a>
        </xsl:element>
        <xsl:variable name="type-list" select="$source/*[not(contains(name(.),'Retry')) and not(contains(name(.),'Suspend'))]"/>

        <table border="2" cellspacing="1">
            <tr>
                <th>Item</th>
                <xsl:for-each select="$type-list">
                    <th width="20"><xsl:value-of select="name(.)"/></th>
                </xsl:for-each>
            </tr>

            <xsl:for-each select="$list-of-unit-cases">
                <xsl:sort select="name(.)" type="text"/>
                <xsl:variable name="unit" select="."/>
                <tr>
                    <td width="300">
                        <xsl:value-of select="name(.)"/>
                    </td>
                    <xsl:for-each select="$type-list">
                        <xsl:variable name="type" select="."/>
                        <xsl:variable name="status" select="$type/*[name(.)=name($unit)]"/>
                        <td width="20">
                            <xsl:variable name="color">
                                <xsl:variable name="item" select="$cmp-result/*[name(.)=name($type)]"/>
                                <xsl:call-template name="generate-color-with-list">
                                    <xsl:with-param name="unit" select="$status"/>
                                    <xsl:with-param name="cmp-result" select="$item"/>
                                </xsl:call-template>
                            </xsl:variable>
                            <xsl:if test="$color!=''">
                                <xsl:attribute name="bgcolor">
                                    <xsl:value-of select="$color"/>
                                </xsl:attribute>
                            </xsl:if>
                            <xsl:choose>
                                <xsl:when test="string($status)=''">
                                    skipped
                                </xsl:when>
                                <xsl:otherwise>
                                    <a href="{concat($log-path,string(@prefix),name($type),'_',name($unit),$log-file-suffix)}">
                                        <xsl:value-of select="$status"/>
                                    </a>
                                </xsl:otherwise>
                            </xsl:choose>
                        </td>
                    </xsl:for-each>
                </tr>
            </xsl:for-each>
            <!-- statistics -->
            <tr>
                <th>Total passed cases (all: <xsl:value-of select="count($list-of-unit-cases)"/>) </th>
                <xsl:for-each select="$type-list">
                    <xsl:variable name="type" select="."/>
                    <xsl:variable name="okays" select="$type/*[.='okay']"/>
                    <xsl:variable name="faileds" select="$type/*[.='failed']"/>
                    <th>
                        <xsl:variable name="color">
                            <xsl:variable name="item" select="$cmp-result/*[name(.)=name($type)]"/>
                            <xsl:call-template name="generate-color">
                                <xsl:with-param name="status" select="$item/@total-status"/>
                            </xsl:call-template>
                        </xsl:variable>
                        <xsl:if test="$color!=''">
                            <xsl:attribute name="bgcolor">
                                <xsl:value-of select="$color"/>
                            </xsl:attribute>
                        </xsl:if>

                        <!--table>
                            <tr><td>okay:   <xsl:value-of select="count($okays)"/></td></tr>
                            <tr><td>failed: <xsl:value-of select="count($faileds)"/></td></tr>
                            <tr><td>skipped:<xsl:value-of select="count($list-of-unit-cases)-count($okays)- count($faileds)"/></td></tr>
                        </table-->
                        <xsl:value-of select="count($okays)"/>
                    </th>
                </xsl:for-each>
            </tr>
        </table>
    </xsl:template>

    <xsl:template name="generate-desc-and-table">
        <xsl:param name="source"/>
        <xsl:param name="cmp-result"/>
        <xsl:param name="log-path"/> <!-- log file path -->
        <xsl:param name="desc-size" select="'3'"/>
        <xsl:param name="description" select="''"/>
        <xsl:variable name="list" select="$source/*"/>

        <xsl:element name="h{$desc-size}"> 
            <xsl:value-of select="$description"/> 
        </xsl:element>
        <!--xsl:element name="h{$desc-size - 1}"> 
            Test parameters:
            <xsl:value-of select="$source/@parameter"/> 
        </xsl:element-->
        
        <xsl:choose>
            <xsl:when test="count($list)">
                <xsl:for-each select="$list">
                    <xsl:variable name="item" select="."/>
                    <xsl:variable name="cmp-item" select="$cmp-result/*[name(.)=name($item)]"/>
                    <xsl:call-template name="generate-table-info">
                        <xsl:with-param name="source" select="$item"/>
                        <xsl:with-param name="cmp-result" select="$cmp-item"/>
                        <xsl:with-param name="log-path" select="concat($log-path,$item/@prefix,name($item))"/>
                        <xsl:with-param name="description" select="name($item)"/>
                    </xsl:call-template>
                    <br/>
                </xsl:for-each>
            </xsl:when>
            <xsl:otherwise>No <xsl:value-of select="$description"/> results!
                <br/>
            </xsl:otherwise>
        </xsl:choose>
    </xsl:template>

    <xsl:template name="generate-table-info">
        <xsl:param name="source"/>
        <xsl:param name="cmp-result"/>
        <xsl:param name="log-path"/>
        <xsl:param name="log-name" select="''"/>
        <xsl:param name="desc-size" select="'4'"/>
        <xsl:param name="description"/>
        <xsl:param name="desc-color" select="'black'"/>

        <xsl:comment>
            Generate a table for a list of unit test cases
        </xsl:comment>
        <xsl:variable name="list" select="$source/*"/>
        <xsl:element name="h{$desc-size}">
            <a name="{name($source)}">
                <font color="{$desc-color}"> <xsl:value-of select="$description"/> </font>
            </a>
        </xsl:element>
        <xsl:choose>
            <xsl:when test="count($list)">
                <table border="2">
                    <tr>
                        <th>Item</th>
                        <th>Value</th>
                    </tr>
                    <xsl:for-each select="$source/*">
                        <xsl:sort select="name(.)" data-type="text"/>
                        <tr>
                            <td width="300"><xsl:value-of select="name(.)"/></td>
                            <td width="20">
                                <xsl:variable name="color">
                                    <xsl:call-template name="generate-color-with-list">
                                        <xsl:with-param name="unit" select="."/>
                                        <xsl:with-param name="cmp-result" select="$cmp-result"/>
                                    </xsl:call-template>
                                </xsl:variable>
                                <xsl:if test="$color!=''">
                                    <xsl:attribute name="bgcolor">
                                        <xsl:value-of select="$color"/>
                                    </xsl:attribute>
                                </xsl:if>
                                <xsl:choose>
                                    <xsl:when test=".='skipped'">
                                        <xsl:value-of select="."/>
                                    </xsl:when>
                                    <xsl:when test="$log-name!='' and @path">
                                        <a href="{concat($log-path,string(@path),'/',$log-name)}">
                                            <xsl:value-of select="."/>
                                        </a>
                                    </xsl:when>
                                    <xsl:when test="@path">
                                        <a href="{concat($log-path,string(@path),'/',name(.),$log-file-suffix)}">
                                            <xsl:value-of select="."/>
                                        </a>
                                    </xsl:when>
                                    <xsl:otherwise>
                                        <a href="{$log-path}_{concat(name(.),$log-file-suffix)}">
                                            <xsl:value-of select="."/>
                                        </a>
                                    </xsl:otherwise>
                                </xsl:choose>

                            </td>
                        </tr>
                    </xsl:for-each>
                    <tr>
                        <th>Total passed cases (all: <xsl:value-of select="count($source/*)"/>) </th>
                        <th>
                            <xsl:variable name="okays" select="$source/*[.='okay']"/>
                            <xsl:variable name="color">
                                        <xsl:call-template name="generate-color">
                                            <xsl:with-param name="status" select="$cmp-result/@total-status"/>
                                        </xsl:call-template>
                            </xsl:variable>
                            <xsl:if test="$color!=''">
                                <xsl:attribute name="bgcolor">
                                    <xsl:value-of select="$color"/>
                                </xsl:attribute>
                            </xsl:if>
                            <xsl:value-of select="count($okays)"/>
                        </th>
                    </tr>
                </table>
            </xsl:when>
            <xsl:otherwise>No <xsl:value-of select="$description"/> information!
                <br/>
            </xsl:otherwise>
        </xsl:choose>
    </xsl:template>
    <xsl:template name="generate-color-with-list">
        <xsl:param name="unit"/>
        <xsl:param name="cmp-result"/>
        <xsl:for-each select="$cmp-result/*">
            <xsl:if test="name($unit)=name(.)">
                <xsl:call-template name="generate-color">
                    <xsl:with-param name="status" select="."/>
                    <xsl:with-param name="source" select="$unit"/>
                </xsl:call-template>
            </xsl:if>
        </xsl:for-each>
    </xsl:template>
    <xsl:template name="generate-color">
        <xsl:param name="status"/>
        <xsl:param name="source"/>
        <xsl:choose>
            <xsl:when test="string($status)=$improved">
                <xsl:text>green</xsl:text>
            </xsl:when>
            <xsl:when test="string($status)=$regression">
                <xsl:text>red</xsl:text>
            </xsl:when>
            <xsl:when test="(string($status)=$equal or string($status)=$equal-in-semantics)and $source='failed'">
                <xsl:text>gray</xsl:text>
            </xsl:when>
            <xsl:otherwise/>
        </xsl:choose>
    </xsl:template>

    <xsl:template name="generate-html-notes">
        Notes:
        <br/>
        <font color="red">Red</font>: regression 
        <font color="green">Green</font>: improvement 
        <font color="gray">Gray</font>: failed but not regression
        <br/>
    </xsl:template>

</xsl:stylesheet>

