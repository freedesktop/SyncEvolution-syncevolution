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
#include "base/util/StringBuffer.h"
#include "VProperty.h"

namespace vocl {

VProperty::VProperty(wchar_t* propname, wchar_t* propvalue) {
    
    name = (propname) ? wstrdup(propname) : NULL;
    value = (propvalue) ? wstrdup(propvalue) : NULL;	
    parameters = new ArrayList();
}

VProperty::~VProperty() {
    
    if (name) {
        delete [] name; name = NULL;
    }
    if (value) {
        delete [] value; value = NULL;
    }
    if (parameters) {
        delete parameters; parameters = NULL;
    }
}	 

void VProperty::setName (const wchar_t* s) {
    
    set(&name, s);
}

void VProperty::setValue (const wchar_t* s) {
    
    set(&value, s);
}

wchar_t* VProperty::getName(wchar_t* buf, int size) {
    
    if (buf == NULL) {
        return name;
    }

    if (size >= 0) {
        wcsncpy(buf, name, size);
    } 
    else {
        wcscpy(buf, name);
    }

    return buf;
}

wchar_t* VProperty::getValue(wchar_t* buf, int size) {
    
    if (buf == NULL) {
        return value;
    }

    if (size >= 0) {
        wcsncpy(buf, value, size);
	} 
    else {
        wcscpy(buf, value);
    }

	return buf;
}

void VProperty::addParameter (const wchar_t* paramName, const wchar_t* paramValue) {
	
    if(paramName) {
    WKeyValuePair *parameter = new WKeyValuePair(paramName, paramValue);
    parameters->add((ArrayElement &)*parameter);

    delete parameter; parameter = NULL;
    }
}

void VProperty::removeParameter(wchar_t* paramName) {
    
    if (parameters != NULL) {
        for (int i=0; i<parameters->size(); i++){
            WKeyValuePair *parameter; 
            parameter = (WKeyValuePair* )parameters->get(i);
            if(!wcscmp(parameter->getKey(), paramName)) {
                parameters->removeElementAt(i);
                break;
            }
        }
    }
}

void VProperty::removeParameter(int index) {
    parameters->removeElementAt(index);
}
   
bool VProperty::containsParameter(wchar_t* paramName) {
    
    if (parameters != NULL) {
        for (int i=0; i<parameters->size(); i++){
            WKeyValuePair *parameter; 
            parameter = (WKeyValuePair* )parameters->get(i);
            if(!wcscmp(parameter->getKey(), paramName)){
                return true;
            }
        }
    }

    return false;
}
wchar_t* VProperty::getParameterValue(wchar_t* paramName) {
    
    if (parameters != NULL) {
	    for (int i=0; i<parameters->size(); i++) {
            WKeyValuePair *parameter; 
            parameter = (WKeyValuePair* )parameters->get(i);
            if(!wcscmp(parameter->getKey(), paramName))
                return ((wchar_t *)parameter->getValue());
        }
    }

    return NULL;
}
wchar_t* VProperty::getParameterValue(int index) {
    
    if (parameters != NULL) {
        WKeyValuePair *parameter;
        parameter = (WKeyValuePair*)parameters->get(index);
        return parameter ? (wchar_t *)parameter->getValue() : NULL;
    }

    return NULL;
}

void VProperty::set(wchar_t** p, const wchar_t* v) {
	
    if (*p) {
        delete [] *p;
    }
    *p = (v) ? wstrdup(v) : NULL;
}

ArrayElement* VProperty::clone() {

    if(name) { 

        VProperty *cloneProperty = new VProperty(name);
        
        if(value)
            cloneProperty->setValue(value);

        if (parameters != NULL) {
            for (int i=0; i<parameters->size(); i++) {
                WKeyValuePair* parameterCopy;
                parameterCopy = (WKeyValuePair*)parameters->get(i)->clone();
                cloneProperty->addParameter(parameterCopy->getKey(), parameterCopy->getValue());
                delete parameterCopy;
            }
        }
        return cloneProperty;
    }
    return NULL;
}

int VProperty::parameterCount() {

    return parameters->size();
}

wchar_t* VProperty::toString() {

    wchar_t *propertyString = new wchar_t[VPROPETY_BUFFER];
    if (name!=NULL){
        wcscpy(propertyString, name);
        if(parameterCount()>0) {
            for (int i=0; i<parameters->size(); i++) {
                WKeyValuePair *parameter;
                parameter = (WKeyValuePair*)parameters->get(i);
                if(parameter->getKey()!= NULL) {
                    wcscat(propertyString, TEXT(";"));
                    wcscat(propertyString, parameter->getKey());
			    }
                if(parameter->getValue()!= NULL) {
                    wcscat(propertyString, TEXT("="));
                    wcscat(propertyString, parameter->getValue());
                }
	        }	
        }
        wcscat(propertyString, TEXT(":"));
        if(value!=NULL) {
            wcscat(propertyString, value);	
        }		
    }
    return propertyString;
}

wchar_t* VProperty::getParameter(int index){

    WKeyValuePair *parameter;
    parameter = (WKeyValuePair*)parameters->get(index);
    return (wchar_t *)parameter->getKey();
}

bool VProperty::equalsEncoding(wchar_t* encoding) {

    if ((encoding != NULL) && ((containsParameter(TEXT("ENCODING")) && 
		!wcscmp(getParameterValue(TEXT("ENCODING")),encoding)) || 
        containsParameter(encoding)))
        return true;
    return false;
}

wchar_t* VProperty::getPropComponent(int i) {
    

    if (!getValue() || !wcscmp(getValue(),TEXT("")))
        return NULL;

    wchar_t *value = new wchar_t[wcslen(getValue()) + 1];
    wchar_t* component = new wchar_t[wcslen(getValue()) + 1];

    wcscpy(value, getValue());
    wchar_t* componentIndex;
    int j=0;
    
    while (j < i) {
        componentIndex = wcschr(value, ';');
        if(componentIndex) {            
            value[componentIndex - value] = 0;
            wcscpy(component, value);
            wcscpy(value, ++componentIndex);
            j++;
        }
        else
            if(j<i-1)
                return NULL;
            else
                return value;
    }

    delete [] value; value = NULL; 
    if(component && wcscmp(component, TEXT("")))
        return component;
    else
        return NULL;
}

bool VProperty::isType(wchar_t* type) {
    if(containsParameter(type))
        return true;

    for (int paramindex = 0;
         paramindex < parameterCount();
         paramindex++) {
        wchar_t *value = getParameterValue(paramindex);
        wchar_t *param = getParameter(paramindex);
        if (value && param && !strcasecmp(param, "TYPE")) {
            wchar_t seps[] = TEXT(",");
            wchar_t* token;

            StringBuffer buff(value);

            token = wcstok((char *)buff.c_str(), seps);
            while( token != NULL ) {
                if(!wcscmp(type, token))
                    return true;
                token = wcstok( NULL, seps );
            }
        }
    }

    return false;
}

};
