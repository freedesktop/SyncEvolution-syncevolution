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
    <!-- This program is used to compare two days' test result and generate comparison info -->
    <!-- TODO: Add more checkings to avoid generate un-welformed xml files -->

    <!--xsl:strip-space elements="*"/-->
    <xsl:param name="cmp_file" select="''"/>
	
	<xsl:output method="xml" indent="yes" encoding="UTF-8"/>

    <!-- comparison result strings -->
    <xsl:variable name="equal" select="'equal'"/>
    <xsl:variable name="equal-in-semantics" select="'equal-in-semantics'"/>
    <xsl:variable name="regression" select="'regression'"/>
    <xsl:variable name="improved" select="'improved'"/>
    <xsl:variable name="skipped" select="'skipped'"/>
    <xsl:variable name="invalid-value" select="'invalid-value'"/>
	
    <xsl:template match="/">
            <xsl:choose>
                <xsl:when test="$cmp_file!=''">
                    <xsl:variable name="cmp_tree" select="document($cmp_file)"/>
                    <comparison>
                        <xsl:call-template name="nightly-test">
                            <xsl:with-param name="cmp_tree" select="$cmp_tree"/>
                        </xsl:call-template>
                    </comparison>
                </xsl:when>
                <xsl:otherwise>
                    <xsl:text>No comparison_file, please set comparison_file by: 
                        --stringparam cmp_file [your file path]
                    </xsl:text>
                </xsl:otherwise>
            </xsl:choose>
    </xsl:template>

    <xsl:template name="nightly-test">
        <xsl:param name="cmp_tree"/>

        <xsl:variable name="prepare" select="nightly-test/prepare"/>
        <xsl:variable name="target-prepare" select="$cmp_tree/nightly-test/prepare"/>
        <xsl:variable name="client-test" select="nightly-test/client-test"/>
        <xsl:variable name="target-client-test" select="$cmp_tree/nightly-test/client-test"/>

        <xsl:call-template name="cmp-prepare">
            <xsl:with-param name="source" select="$prepare"/>
            <xsl:with-param name="target" select="$target-prepare"/>
        </xsl:call-template>
        <xsl:call-template name="cmp-client-test">
            <xsl:with-param name="source" select="$client-test"/>
            <xsl:with-param name="target" select="$target-client-test"/>
        </xsl:call-template>
    </xsl:template>

    <xsl:template name="cmp-prepare">
        <xsl:param name="source"/>
        <xsl:param name="target"/>
        <xsl:element name="{name($source)}">
            <!--xsl:call-template name="cmp-list-of-lists-of-units"-->
            <xsl:call-template name="cmp-list-of-units">
                <xsl:with-param name="source" select="$source"/>
                <xsl:with-param name="target" select="$target"/>
            </xsl:call-template>
        </xsl:element>
    </xsl:template>

    <xsl:template name="cmp-client-test">
        <xsl:param name="source"/>
        <xsl:param name="target"/>

        <xsl:element name="{name($source)}">
            <xsl:choose>
                <xsl:when test="$source">
                    <xsl:call-template name="cmp-client-test-source">
                        <xsl:with-param name="source" select="$source/source"/>
                        <xsl:with-param name="target" select="$target/source"/>
                    </xsl:call-template>

                    <xsl:call-template name="cmp-client-test-sync">
                        <xsl:with-param name="source" select="$source/sync"/>
                        <xsl:with-param name="target" select="$target/sync"/>
                    </xsl:call-template>
                </xsl:when>
                <xsl:otherwise>
                </xsl:otherwise>
            </xsl:choose>
        </xsl:element>
    </xsl:template>

    <xsl:template name="cmp-client-test-source">
        <xsl:param name="source"/>
        <xsl:param name="target"/>
        <xsl:element name="{name($source)}">
            <xsl:call-template name="cmp-list-of-lists-of-units">
                <xsl:with-param name="source" select="$source"/>
                <xsl:with-param name="target" select="$target"/>
            </xsl:call-template>
        </xsl:element>
    </xsl:template>
    <xsl:template name="cmp-client-test-sync">
        <xsl:param name="source"/>
        <xsl:param name="target"/>

        <xsl:element name="{name($source)}">
            <xsl:variable name="servers" select="$source/*[name(.)!='template']"/>
            <!--xsl:variable name="target-servers" select="$target/*"/-->
            <xsl:for-each select="$servers">
                <xsl:sort select="name(.)" data-type="text"/>
                <xsl:variable name="server" select="."/>
                <xsl:variable name="okays" select="$server/*/*[.='okay']"/>
                <xsl:variable name="target-server" select="$target/*[name(.)=name($server)]"/>
                <xsl:variable name="old-okays" select="$target-server/*/*[.='okay']"/>
                <xsl:variable name="status" select="count($okays) - count($old-okays)"/>
                <xsl:element name="{name($server)}">
                    <xsl:attribute name="summary">
                        <xsl:value-of select="$status"/>
                    </xsl:attribute>
                    <xsl:call-template name="cmp-list-of-lists-of-units">
                        <xsl:with-param name="source" select="$server"/>
                        <xsl:with-param name="target" select="$target-server"/>
                    </xsl:call-template>
                </xsl:element>
            </xsl:for-each>
        </xsl:element>
    </xsl:template>
    
    <!-- list of lists to be compared -->
    <xsl:template name="cmp-list-of-lists-of-units">
        <xsl:param name="source"/>
        <xsl:param name="target"/>

        <xsl:for-each select="$source/*">
            <xsl:variable name="pim" select="."/>

            <xsl:element name="{name($pim)}">
                <xsl:variable name="new-okays" select="./*[.='okay']"/>
                <xsl:variable name="target-pim" select="$target/*[name(.)=name($pim)]"/>
                <xsl:variable name="old-okays" select="$target-pim/*[.='okay']"/>
                <xsl:attribute name="total-status">
                    <xsl:choose>
                        <xsl:when test="count($new-okays)=count($old-okays)">
                            <xsl:value-of select="$equal"/>
                        </xsl:when>
                        <xsl:when test="count($new-okays)&lt;count($old-okays)">
                            <xsl:value-of select="$regression"/>
                        </xsl:when>
                        <xsl:otherwise>
                            <xsl:value-of select="$improved"/>
                        </xsl:otherwise>
                    </xsl:choose>
                </xsl:attribute>
                <xsl:attribute name="summary">
                    <xsl:value-of select="count($new-okays)-count($old-okays)"/>
                </xsl:attribute>
                <xsl:call-template name="cmp-list-of-units">
                    <xsl:with-param name="source" select="$pim"/>
                    <xsl:with-param name="target" select="$target-pim"/>
                </xsl:call-template>
            </xsl:element>
        </xsl:for-each>
    </xsl:template>

    <!-- this template compares two list of unit test cases, such as compile status and client-test unit test status -->
    <xsl:template name="cmp-list-of-units">
        <xsl:param name="source"/>
        <xsl:param name="target"/>

        <xsl:for-each select="$source/*">
            <xsl:variable name="unittest" select="."/>
            <xsl:variable name="target-unittest" select="$target/*[name(.)=name($unittest)]"/>

            <xsl:element name="{name($unittest)}">
                <xsl:choose>
                    <xsl:when test="$unittest=$target-unittest">
                        <xsl:value-of select="$equal"/>
                    </xsl:when>
                    <xsl:when test="$unittest='okay'">
                        <xsl:value-of select="$improved"/>
                    </xsl:when>
                    <xsl:when test="$unittest='failed' and (not($target-unittest) or $target-unittest!='okay')">
                        <xsl:value-of select="$equal-in-semantics"/>
                    </xsl:when>
                    <xsl:when test="$unittest='skipped' and (not($target-unittest) or $target-unittest!='okay')">
                        <xsl:value-of select="$equal-in-semantics"/>
                    </xsl:when>
                    <xsl:when test="$target-unittest='okay'">
                        <xsl:value-of select="$regression"/>
                    </xsl:when>
                    <xsl:otherwise>
                        <xsl:value-of select="$invalid-value"/>
                    </xsl:otherwise>
                </xsl:choose>
            </xsl:element>
        </xsl:for-each>
    </xsl:template>

</xsl:stylesheet>

