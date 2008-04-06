/**
 * Copyright (C) 2003-2006 Funambol
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include "posixadapter.h"

#include "base/util/utils.h"
#include "base/util/WString.h"
#include "base/Log.h"
#include "VObject.h"

namespace vocl {

VObject::VObject() {
    productID = NULL;
    version = NULL;
    properties = new ArrayList();
}

VObject::~VObject() {
    if (productID) {
        delete [] productID; productID = NULL;
    }
    if (version) {
        delete [] version; version = NULL;
    }
    if (properties) {
        delete properties; properties = NULL;
    }
}

void VObject::set(wchar_t** p, const wchar_t* v) {
    if (*p) {
        delete [] *p;
    }
    *p = (v) ? wstrdup(v) : NULL;
}

void VObject::setVersion(const wchar_t* ver) {
    set(&version, ver);
}

void VObject::setProdID(const wchar_t* prodID) {
    set(&productID, prodID);
}

wchar_t* VObject::getVersion() {
    return version;
}

wchar_t* VObject::getProdID() {
    return productID;
}

void VObject::addProperty(VProperty* vProp) {
    properties->add((ArrayElement&) *vProp);
}

int VObject::propertiesCount() {
    return properties->size();
}

bool VObject::removeProperty(int index) {
    if(index < 0 || index >= propertiesCount())
        return false;
    properties->removeElementAt(index);
    return true;
}

void VObject::removeProperty(wchar_t* propName) {
    for (int i=0; i<properties->size(); i++) {
        VProperty *property; 
        property = (VProperty* )properties->get(i);
        if(!wcscmp(property->getName(), propName)) {
            properties->removeElementAt(i);
            break;
        }
    }
}

bool VObject::containsProperty(wchar_t* propName) {
    for (int i=0; i<properties->size(); i++) {
        VProperty *property; 
        property = (VProperty* )properties->get(i);
        if(!wcscmp(property->getName(), propName)) {
            return true;
        }
    }
    return false;
}

VProperty* VObject::getProperty(int index) {
    return (VProperty*)properties->get(index);
}

VProperty* VObject::getProperty(const wchar_t* propName) {
    for (int i=0; i<properties->size(); i++) {
        
        VProperty *property; 
        property = (VProperty* )properties->get(i);
		
        if(!wcscmp(property->getName(), propName)) {
            return property;
        }
    }
    return NULL;
}

wchar_t* VObject::toString() {
    
    WString strVObject;
    const wchar_t* eof;

    // vcard 2.1 and 3.0 both use \r\n as line ending
    eof = TEXT("\r\n");

    for (int i=0; i<properties->size(); i++) {
        VProperty *property;
        property = (VProperty*)properties->get(i);
        if(property->containsParameter(TEXT("GROUP"))) {
           strVObject.append(property->getParameterValue(TEXT("GROUP")));
           strVObject.append(TEXT("."));
           property->removeParameter(TEXT("GROUP"));
        }
        strVObject.append(property->getName());
        
        for(int k=0; k<property->parameterCount(); k++) {
            strVObject.append(TEXT(";"));
			
            wchar_t* paramName = new wchar_t[wcslen(property->getParameter(k))+1];
            wcscpy(paramName, property->getParameter(k));
			
            strVObject.append(paramName);
            const wchar_t *value = property->getParameterValue(k);
            if(value) {
                strVObject.append(TEXT("="));
                strVObject.append(value);
            }
            delete [] paramName; paramName = NULL;
        }

        strVObject.append(TEXT(":"));
        if(property->getValue()) {
            if(property->equalsEncoding(TEXT("BASE64"))) {
                wchar_t delim[] = TEXT("\r\n ");
                int fold = 76;
                int sizeOfValue = int(wcslen(property->getValue()));
                int size = sizeOfValue + (int)(sizeOfValue/fold + 2)*int(wcslen(delim));
                int index = 0;

                wchar_t* output = new wchar_t[size + 1];
                wcscpy(output, TEXT("\0"));

                while (index<sizeOfValue)
                {  
                    wcscat(output,delim);
                    wcsncat(output,property->getValue()+index,fold);
                    index+=fold;
                }
                
                strVObject.append(output);
                // the extra empty line is needed because the Bachus-Naur 
                // specification of vCard 2.1 says so
                strVObject.append(eof);
                delete [] output;
            } 
            else
                strVObject.append(property->getValue());
        }
        strVObject.append(eof);
    }		    

    // memory must be free by caller with delete []
    wchar_t *str = new wchar_t[strVObject.length() + 1];
    wcscpy(str, strVObject.c_str());
    return str;
}

void VObject::insertProperty(VProperty* property) {

    if (propertiesCount() == 0 || wcscmp(getProperty(propertiesCount()-1)->getName(),TEXT("END")))
        addProperty(property);
    else {
        VProperty* lastProperty = getProperty(TEXT("END"));
        removeProperty(TEXT("END"));
        addProperty(property);
        addProperty(lastProperty);
    }
}

void VObject::addFirstProperty(VProperty* property) {
    properties->add(0,(ArrayElement&)*property);
}

void VObject::removeAllProperies(wchar_t* propName) {
    for(int i = 0, m = propertiesCount(); i < m ; i++)
        if(!wcscmp(getProperty(i)->getName(), propName)) {
            removeProperty(i);
            --i;
            --m;
        }
}


// Patrick Ohly: hack below, see header file

static int hex2int( wchar_t x )
{
    return (x >= '0' && x <= '9') ? x - '0' :
        (x >= 'A' && x <= 'F') ? x - 'A' + 10 :
        (x >= 'a' && x <= 'f') ? x - 'a' + 10 :
        0;
}

void VObject::toNativeEncoding()
{
    bool is_30 = !wcscmp(getVersion(), TEXT("3.0"));
    // line break is encoded with either one or two
    // characters on different platforms
    const int linebreaklen = wcslen(SYNC4J_LINEBREAK);

    for (int index = propertiesCount() - 1; index >= 0; index--) {
        VProperty *vprop = getProperty(index);
        wchar_t *name = vprop->getName();
        wchar_t *foreign = vprop->getValue();
        // the native encoding is always shorter than the foreign one
        wchar_t *native = new wchar_t[wcslen(foreign) + 1];

        if (vprop->equalsEncoding(TEXT("QUOTED-PRINTABLE"))) {
            int in = 0, out = 0;
            wchar_t curr;

            // this is a very crude quoted-printable decoder,
            // based on Wikipedia's explanation of quoted-printable
            while ((curr = foreign[in]) != 0) {
                in++;
                if (curr == '=') {
                    wchar_t values[2];
                    values[0] = foreign[in];
                    in++;
                    if (!values[0]) {
                        // incomplete?!
                        break;
                    }
                    values[1] = foreign[in];
                    in++;
                    if (values[0] == '\r' && values[1] == '\n') {
                        // soft line break, ignore it
                    } else {
                        native[out] = (hex2int(values[0]) << 4) |
                            hex2int(values[1]);
                        out++;

                        // replace \r\n with \n?
                        if ( linebreaklen == 1 &&
                             out >= 2 &&
                             native[out - 2] == '\r' &&
                             native[out - 1] == '\n' ) {
                            native[out - 2] = SYNC4J_LINEBREAK[0];
                            out--;
                        }
                        
                        // the conversion to wchar on Windows is
                        // probably missing here
                    }
                } else {
                    native[out] = curr;
                    out++;
                }
            }
            native[out] = 0;
            out++;
        } else {
            wcscpy(native, foreign);
        }

        // decode escaped characters after backslash:
        // \n is line break only in 3.0
        wchar_t curr;
        int in = 0, out = 0;
        while ((curr = native[in]) != 0) {
            in++;
            switch (curr) {
             case '\\':
                curr = native[in];
                in++;
                switch (curr) {
                 case 'n':
                    if (is_30) {
                        // replace with line break
                        wcsncpy(native + out, SYNC4J_LINEBREAK, linebreaklen);
                        out += linebreaklen;
                    } else {
                        // normal escaped character
                        native[out] = curr;
                        out++;
                    }
                    break;
                 case 0:
                    // unexpected end of string
                    break;
                 default:
                    // just copy next character
                    native[out] = curr;
                    out++;
                    break;
                }
                break;
             case ';':
                // Might be field separator, but beware:
                // in vCard 2.1 a single, unescaped semicolon is valid in all
                // properties which are single values and not structured.
                // Some encoders even do that in 3.0, so always accept a literal
                // ; as it is in properties which are not multi-value.
                if (!wcsicmp(name, "N") ||
                    !wcsicmp(name, "ADR") ||
                    !wcsicmp(name, "ORG")) {
                    // must replace with something special
                    // so that we can encode it again in fromNativeEncoding()
                    native[out] = SEMICOLON_REPLACEMENT;
                    out++;
                } else {
                    // copy literally
                    native[out] = ';';
                    out++;
                }
                break;
             default:
                native[out] = curr;
                out++;
            }
        }
        native[out] = 0;
        out++;

        // charset handling:
        // - doesn't exist at the moment, vCards have to be in ASCII or UTF-8
        // - an explicit CHARSET parameter is removed because its parameter
        //   value might differ between 2.1 and 3.0 (quotation marks allowed in
        //   3.0 but not 2.1) and thus would require extra code to convert it;
        //   when charsets really get supported this needs to be addressed
        wchar_t *charset = vprop->getParameterValue(TEXT("CHARSET"));
        if (charset) {
            // proper decoding of the value and the property value text
            // would go here, for the time being we just remove the
            // value
            if (_wcsicmp(charset, TEXT("UTF-8")) &&
                _wcsicmp(charset, TEXT("\"UTF-8\""))) {
                LOG.error("ignoring unsupported charset");
            }
            vprop->removeParameter(TEXT("CHARSET"));
        }

        vprop->setValue(native);
        delete [] native;
    }
}

void VObject::fromNativeEncoding()
{
    bool is_30 = !wcscmp(getVersion(), TEXT("3.0"));

    for (int index = propertiesCount() - 1; index >= 0; index--) {
        VProperty *vprop = getProperty(index);

        wchar_t *native = vprop->getValue();
        // in the worst case every comma/linebreak is replaced with
        // two characters and each \n with =0D=0A
        wchar_t *foreign = new wchar_t[6 * wcslen(native) + 1];
        wchar_t curr;
        int in = 0, out = 0;
        // line break is encoded with either one or two
        // characters on different platforms
        const int linebreaklen = wcslen(SYNC4J_LINEBREAK);
        
        // use backslash for special characters,
        // if necessary do quoted-printable encoding
        bool doquoted = !is_30 &&
            wcsstr(native, SYNC4J_LINEBREAK) != NULL;

        if (vprop->equalsEncoding(TEXT("QUOTED-PRINTABLE"))) {
            // remove it, recreate if doing 2.1
            vprop->removeParameter(TEXT("ENCODING"));
            if (!is_30) {
                doquoted = true;
            }
        }
        
        // non-ASCII character encountered
        bool utf8 = false;
        
        while ((curr = native[in]) != 0) {
            in++;
            switch (curr) {
             case ',':
                if (!is_30) {
                    // normal character
                    foreign[out] = curr;
                    out++;
                    break;
                }
                // no break!
             case ';':
             case '\\':
                foreign[out] = '\\';
                out++;
                foreign[out] = curr; 
                out++;
                break;
             case SEMICOLON_REPLACEMENT:
                foreign[out] = ';';
                out++;
                break;
             default:
                bool currIsUTF8 = (unsigned char)curr >= 128;

                if (currIsUTF8) {
                    utf8 = true;
                    if (!is_30 && !doquoted) {
                        // vCard 2.1 defaults to 7-bit; instead of
                        // overriding that we fall back to quoted-printable
                        doquoted = true;
                    }
                }

                if (doquoted &&
                    (curr == '=' || currIsUTF8)) {
                    // escape = and non-ASCII characters
                    wsprintf(foreign + out, TEXT("=%02X"), (unsigned int)(unsigned char)curr);
                    out += 3;
                } else if (!wcsncmp(native + in - 1,
                                    SYNC4J_LINEBREAK,
                                    linebreaklen)) {
                    // line break
                    if (is_30) {
                        foreign[out] = '\\';
                        out++;
                        foreign[out] = 'n';
                        out++;
                    } else {
                        wcscpy(foreign + out, TEXT("=0D=0A"));
                        out += 6;
                    }
                    in += linebreaklen - 1;
                } else {
                    foreign[out] = curr;
                    out++;
                }
                break;
            }
        }
        foreign[out] = 0;
        vprop->setValue(foreign);
        delete [] foreign;
        if (doquoted) {
            // we have used quoted-printable encoding
            vprop->addParameter(TEXT("ENCODING"), TEXT("QUOTED-PRINTABLE"));
        }
        if (utf8 &&
            !is_30 &&
            !vprop->getParameterValue("CHARSET")) {
            vprop->addParameter("CHARSET", "UTF-8");
        }
    }
}

};
