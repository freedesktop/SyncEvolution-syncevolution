/*
 * Copyright (C) 2014 Intel Corporation
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

#include <config.h>

#if defined(USE_REFRESH_TOKEN) || defined(STATIC_REFRESH_TOKEN)

#include "oauth2.h"

#include <syncevo/IdentityProvider.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static class OAuth2Provider : public IdentityProvider
{
public:
    OAuth2Provider() :
	IdentityProvider("refresh_token",
			 "refresh_token:<parameters>\n"
			 "   Authentication using refresh token.\n"
			 "   GVariant text dump suitable for g_variant_parse() (see\n"
			 "   https://developer.gnome.org/glib/stable/gvariant-text.html).\n"
			 "   It must contain a hash with keys 'TokenHost', 'TokenPath', \n"
			 "   'Scope', 'ClientID', 'ClientSecret'\n")
    {}

    virtual boost::shared_ptr<AuthProvider> create(const InitStateString &username,
                                                   const InitStateString &password)
    {
        boost::shared_ptr<AuthProvider> provider;
        provider = createOAuth2AuthProvider(username, password);
        return provider;
    }
} gsso;

SE_END_CXX

#endif // is enabled
