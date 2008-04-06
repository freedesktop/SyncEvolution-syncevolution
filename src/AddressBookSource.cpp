/*
 * Copyright (C) 2007 Patrick Ohly
 * Copyright (C) 2007 Funambol
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <memory>
#include <map>
#include <sstream>
#include <list>
using namespace std;

#include "config.h"

#ifdef ENABLE_ADDRESSBOOK

#ifdef IPHONE
# define ABAddRecord ABCAddRecord
# define ABCopyArrayOfAllPeople ABCCopyArrayOfAllPeople
# define ABGetSharedAddressBook ABCGetSharedAddressBook
# define ABMultiValueAdd ABCMultiValueAdd
# define ABMultiValueCopyLabelAtIndex ABCMultiValueCopyLabelAtIndex
# define ABMultiValueCopyValueAtIndex ABCMultiValueCopyValueAtIndex
# define ABMultiValueCount ABCMultiValueGetCount
# define ABMultiValueCreateMutable ABCMultiValueCreateMutable
// # define ABPersonCopyImageData ABCPersonCopyImageData
# define PersonCreateWrapper(_addressbook) ABCPersonCreateNewPerson(_addressbook)
/**
 * The iPhone stores photos in three (?) different sizes.
 * Storing just one copy is okay, albeit a bit inefficient:
 * it needs to be scaled down each time it is accessed.
 *
 * @todo When importing photos into the address book, create
 * all three different sizes.
 */
enum {
    IPHONE_PHOTO_SIZE_THUMBNAIL,
    IPHONE_PHOTO_SIZE_MEDIUM,
    IPHONE_PHOTO_SIZE_ORIGINAL
};
# define PersonSetImageDataWrapper(_person, _dataref) ABCPersonSetImageDataAndCropRect(_person, IPHONE_PHOTO_SIZE_THUMBNAIL, _dataref, 0,0,0,0)
# define ABRecordCopyValue ABCRecordCopyValue
# define ABRecordRemoveValue ABCRecordRemoveValue
# define ABRecordSetValue ABCRecordSetValue
# define ABRemoveRecord ABCRemoveRecord
# define ABSave ABCSave
# define kABAIMInstantProperty kABCAIMInstantProperty
# define kABAddressCityKey kABCAddressCityKey
# define kABAddressCountryKey kABCAddressCountryKey
# define kABAddressHomeLabel kABCAddressHomeLabel
# define kABAddressProperty kABCAddressProperty
# define kABAddressStateKey kABCAddressStateKey
# define kABAddressStreetKey kABCAddressStreetKey
# define kABAddressWorkLabel kABCAddressWorkLabel
# define kABAddressZIPKey kABCAddressZIPKey
# define kABAssistantLabel kABCAssistantLabel
# define kABBirthdayProperty kABCBirthdayProperty
# define kABCreationDateProperty kABCCreationDateProperty
# define kABDepartmentProperty kABCDepartmentProperty
# define kABEmailHomeLabel kABCEmailHomeLabel
# define kABEmailProperty kABCEmailProperty
# define kABEmailWorkLabel kABCEmailWorkLabel
# define kABFirstNameProperty kABCFirstNameProperty
# define kABHomePageLabel kABCHomePageLabel
/* # define kABHomePageProperty kABCHomePageProperty */
# define kABICQInstantProperty kABCICQInstantProperty
# define kABJabberHomeLabel kABCJabberHomeLabel
# define kABJabberInstantProperty kABCJabberInstantProperty
# define kABJabberWorkLabel kABCJabberWorkLabel
# define kABJobTitleProperty kABCJobTitleProperty
# define kABLastNameProperty kABCLastNameProperty
# define kABMSNInstantProperty kABCMSNInstantProperty
# define kABManagerLabel kABCManagerLabel
# define kABMiddleNameProperty kABCMiddleNameProperty
# define kABModificationDateProperty kABCModificationDateProperty
# define kABNicknameProperty kABCNicknameProperty
# define kABNoteProperty kABCNoteProperty
# define kABOrganizationProperty kABCOrganizationProperty
# define kABOtherDatesProperty kABCOtherDatesProperty
# define kABPhoneHomeFAXLabel kABCPhoneHomeFAXLabel
# define kABPhoneHomeLabel kABCPhoneHomeLabel
# define kABPhoneMainLabel kABCPhoneMainLabel
# define kABPhoneMobileLabel kABCPhoneMobileLabel
# define kABPhonePagerLabel kABCPhonePagerLabel
# define kABPhoneProperty kABCPhoneProperty
# define kABPhoneWorkFAXLabel kABCPhoneWorkFAXLabel
# define kABPhoneWorkLabel kABCPhoneWorkLabel
# define kABRelatedNamesProperty kABCRelatedNamesProperty
# define kABSpouseLabel kABCSpouseLabel
# define kABSuffixProperty kABCSuffixProperty
// # define kABTitleProperty kABCTitleProperty
// # define kABURLsProperty kABCURLsProperty
# define kABYahooInstantProperty kABCYahooInstantProperty
#else
# define PersonCreateWrapper(_addressbook) ABPersonCreate()
# define PersonSetImageDataWrapper(_person, _dataref) ABPersonSetImageData(_person, _dataref)
#endif
#include "EvolutionSyncClient.h"
#include "AddressBookSource.h"

#include <common/base/Log.h>
#include <common/base/util/StringBuffer.h>
#include "vocl/VConverter.h"

#include <CoreFoundation/CoreFoundation.h>

using namespace vocl;

/** converts a CFString to std::string in UTF-8 - does not free input, throws exception if conversion impossible */
static string CFString2Std(CFStringRef cfstring)
{
    const char *str = CFStringGetCStringPtr(cfstring, kCFStringEncodingUTF8);
    if (str) {
        return string(str);
    }

    CFIndex len = CFStringGetLength(cfstring) * 2 + 1;
    for (int tries = 0; tries < 3; tries++) {
        boost::scoped_array<char> buf(new char[len], "buffer");
        if (CFStringGetCString(cfstring, buf, len, kCFStringEncodingUTF8)) {
            return string((char *)buf);
        }
        len *= 2;
    }
    EvolutionSyncClient::throwError("converting CF string failed");
}

/** converts a string in UTF-8 into a CFString - throws an exception if no valid reference can be generated */
static CFStringRef Std2CFString(const string &str)
{
    ref<CFStringRef> cfstring(CFStringCreateWithCString(NULL, str.c_str(), kCFStringEncodingUTF8), "conversion from CFString");
    return cfstring.release();
}

/** generic label for 'other' items in a multi-value list */
static const CFStringRef otherLabel(CFSTR("_$!<Other>!$_"));
/** generic label for 'work' items in a multi-value list */
static const CFStringRef workLabel(CFSTR("_$!<Work>!$_"));
/** custom label used for "TEL;PREF;WORK" */
static const CFStringRef mainWorkLabel(CFSTR("main work"));

#ifdef IPHONE

/** declarations and functions which are missing in iPhone framework */
extern "C" {
    extern const CFStringRef kABCHomePageProperty;
    extern const CFStringRef kABCURLProperty;
    
    ABPersonRef ABCPersonCreateNewPerson(ABAddressBookRef addressbook);

    ABRecordRef ABCPersonGetRecordForUniqueID(ABAddressBookRef addressBook, SInt32 uid);
    ABRecordRef ABCopyRecordForUniqueId(ABAddressBookRef addressBook, CFStringRef uniqueId) {
        SInt32 uid = CFStringGetIntValue(uniqueId);
        return ABCPersonGetRecordForUniqueID(addressBook, uid);
    }

    SInt32 ABCRecordGetUniqueId(ABRecordRef record);
    CFStringRef ABRecordCopyUniqueId(ABRecordRef record) {
        SInt32 uid = ABCRecordGetUniqueId(record);
        return CFStringCreateWithFormat(NULL, NULL, CFSTR("%d"), uid);
    }

    CFDataRef ABCPersonCopyImageData(ABPersonRef person, int format);
    bool ABCPersonSetImageData(ABPersonRef person, int format, CFDataRef data);
    bool ABCPersonSetImageDataAndCropRect(ABPersonRef person, int format, CFDataRef data, int crop_x, int crop_y, int crop_width, int crop_height);
}

#endif


/**
 * a strtok_r() which does no skip delimiters at the start and end and does 
 * not merge consecutive delimiters, i.e. returned string may be empty
 *
 * @return NULL if no further tokens
 */
static char *my_strtok_r(char *buffer, char delim, char **ptr, char **endptr)
{
    char *res;

    if (buffer) {
        *ptr = buffer;
        *endptr = buffer + strlen(buffer);
    }
    res = *ptr;
    if (res == *endptr) {
        return NULL;
    }

    while (**ptr) {
        if (**ptr == delim) {
            **ptr = 0;
            (*ptr)++;
            break;
        }
        (*ptr)++;
    }

    return res;
}

/** converts between vCard and ABPerson and back */
class vCard2ABPerson {
public:
    vCard2ABPerson(string &vcard, ABPersonRef person) :
        m_vcard(vcard),
        m_person(person) {
    }

    /** parses vcard and stores result in person */
    void toPerson() {
        std::auto_ptr<VObject> vobj(VConverter::parse((char *)m_vcard.c_str()));
        if (vobj.get() == 0) {
            throwError("parsing contact");
        }
        vobj->toNativeEncoding();

        // Remove all properties from person that we might set:
        // those still found in the vCard will be recreated.
        // Properties that we do not support are left untouched.
        for (int mapindex = 0;
             m_mapping[mapindex].m_vCardProp;
             mapindex++) {
            const mapping &map = m_mapping[mapindex];
            if (map.m_abPersonProp) {
                if (!ABRecordRemoveValue(m_person, *map.m_abPersonProp)) {
                    throwError("removing old value "
#ifndef IPHONE
                               + CFString2Std(*map.m_abPersonProp) + " " +
#endif
                               "failed");
                }
            }
        }
        for (int multi = 0; multi < MAX_MULTIVALUE; multi++) {
            if (!ABRecordRemoveValue(m_person, *m_multiProp[multi])) {
                throwError(string("removing old value ")
#ifndef IPHONE
                           + CFString2Std(*m_multiProp[multi]) + " "
#endif
                           + "failed");
            }
        }

        // walk through all properties and handle them
        int propindex = 0;
        VProperty *vprop;
        while ((vprop = vobj->getProperty(propindex)) != NULL) {
            for (int mapindex = 0;
                 m_mapping[mapindex].m_vCardProp;
                 mapindex++) {
                const mapping &map = m_mapping[mapindex];
                if (!strcmp(map.m_vCardProp, vprop->getName())) {
                    toPerson_t handler = map.m_toPerson;
                    if (!handler) {
                        handler = &vCard2ABPerson::toPersonString;
                    }
                    (this->*handler)(map, *vprop);
                    break;
                }
            }
            propindex++;
        }

        // now copy all those values to the person which did not map directly
        for (int multi = 0; multi < MAX_MULTIVALUE; multi++) {
            if (m_multi[multi]) {
                setPersonProp(*m_multiProp[multi], m_multi[multi], false);
            }
        }

        VProperty *photo = vobj->getProperty("PHOTO");
        if (photo) {
            int len;
            boost::scoped_array<char> decoded((char *)b64_decode(len, photo->getValue()), "photo");
            ref<CFDataRef> data(CFDataCreate(NULL, (UInt8 *)(char *)decoded, len));
            if (!PersonSetImageDataWrapper(m_person, data)) {
                EvolutionSyncClient::throwError("cannot set photo data");
            }
        }
    }

    /** convert person into vCard 2.1 or 3.0 and store it in string */
    void fromPerson(bool asVCard30) {
        string tmp;
        const unsigned char *text;
        
        // VObject is so broken that it neither as a reset nor
        // an assignment operator - no, I didn't write it :-/
        //
        // Reseting m_vobj was supposed to allow repeated calls
        // to fromPerson, but this is not really necessary.
        // m_vobj = VObject();

        m_vobj.addProperty("BEGIN", "VCARD");
        m_vobj.addProperty("VERSION", asVCard30 ? "3.0" : "2.1");
        m_vobj.setVersion(asVCard30 ? "3.0" : "2.1");

        // iterate over all person properties and handle them
        for (int mapindex = 0;
             m_mapping[mapindex].m_vCardProp;
             mapindex++ ) {
            const mapping &map = m_mapping[mapindex];
            if (map.m_abPersonProp) {
#ifdef IPHONE
                // some of the properties returned on the iPhone can neither
                // be printed nor released: trying it leads to crashes, so
                // avoid it
                CFTypeRef value = ABRecordCopyValue(m_person, *map.m_abPersonProp);
#else
                ref<CFTypeRef> value(ABRecordCopyValue(m_person, *map.m_abPersonProp));
#endif
                if (value) {
                    fromPerson_t handler = map.m_fromPerson;
                    if (!handler) {
                        handler = &vCard2ABPerson::fromPersonString;
                    }
                    (this->*handler)(map, value);
                }
            }
        }

        // add properties which did not map directly
        string n;
        n += m_strings[LAST_NAME];
        n += VObject::SEMICOLON_REPLACEMENT;
        n += m_strings[FIRST_NAME];
        n += VObject::SEMICOLON_REPLACEMENT;
        n += m_strings[MIDDLE_NAME];
        n += VObject::SEMICOLON_REPLACEMENT;
        n += m_strings[TITLE];
        n += VObject::SEMICOLON_REPLACEMENT;
        n += m_strings[SUFFIX];
        m_vobj.addProperty("N", n.c_str());

        if (m_strings[ORGANIZATION].size() ||
            m_strings[DEPARTMENT].size() ) {
            string org;
            org += m_strings[ORGANIZATION];
            org += VObject::SEMICOLON_REPLACEMENT;
            org += m_strings[DEPARTMENT];
            m_vobj.addProperty("ORG", org.c_str());
        }

        ref<CFDataRef> photo;
#ifdef IPHONE
        // ask for largets size first
        for(int format = IPHONE_PHOTO_SIZE_ORIGINAL; format >= 0; format--) {
            photo.set(ABCPersonCopyImageData(m_person, format));
            if (photo) {
                break;
            }
        }
#else
        photo.set(ABPersonCopyImageData(m_person));
#endif
        if (photo) {
            StringBuffer encoded;
            b64_encode(encoded, (void *)CFDataGetBytePtr(photo), CFDataGetLength(photo));
            VProperty vprop("PHOTO");
            vprop.addParameter("ENCODING", asVCard30 ? "B" : "BASE64");
            vprop.setValue(encoded.c_str());
            m_vobj.addProperty(&vprop);
        }

        m_vobj.addProperty("END", "VCARD");
        m_vobj.fromNativeEncoding();
        boost::scoped_array<char> finalstr(m_vobj.toString(), "VOCL string");
        m_vcard = (char *)finalstr;
    }

private:
    string &m_vcard;
    ABPersonRef m_person;
    VObject m_vobj;

    void throwError(const string &error) {
        EvolutionSyncClient::throwError(string("vCard<->Addressbook conversion: ") + error);
    }

    /** intermediate storage for strings gathered from either vcard or person */
    enum {
        FIRST_NAME,
        MIDDLE_NAME,
        LAST_NAME,
        TITLE,
        SUFFIX,
        ORGANIZATION,
        DEPARTMENT,
        MAX_STRINGS
    };
    string m_strings[MAX_STRINGS];

    /** intermediate storage for multi-value data later passed to ABPerson - keep in sync with m_multiProp */
    enum {
        URLS,
        EMAILS,
        PHONES,
#ifndef IPHONE
        DATES,
        AIM,
        JABBER,
        MSN,
        YAHOO,
        ICQ,
#endif
        NAMES,
        ADDRESSES,
        MAX_MULTIVALUE
    };
    ref<ABMutableMultiValueRef, IPHONE_RELEASE> m_multi[MAX_MULTIVALUE];
    /**
     * the ABPerson property which corresponds to the m_multi array:
     * a pointer because the tool chain for the iPhone did not properly
     * handle the constants when referenced in data initialization directly
     */
    static const CFStringRef *m_multiProp[MAX_MULTIVALUE];

    struct mapping;
    /** member function which handles one specific vCard property */
    typedef void (vCard2ABPerson::*toPerson_t)(const mapping &map, VProperty &vprop);
    /** member function which handles one specific ABPerson property */
    typedef void (vCard2ABPerson::*fromPerson_t)(const mapping &map, CFTypeRef cftype);

    /** store a string in the ABPerson */
    void setPersonProp(CFStringRef property, const string &str) {
        ref<CFStringRef> cfstring(Std2CFString(str));
        setPersonProp(property, cfstring);
    }
    /** store a string in the ABPerson */
    void setPersonProp(CFStringRef property, const char *str) {
        ref<CFStringRef> cfstring(Std2CFString(str));
        setPersonProp(property, cfstring);
    }
    /**
     * store a generic property in the ABPerson
     * @param dump     avoid CFCopyDescription() for some properties (iPhone bug)
     */
    void setPersonProp(CFStringRef property, CFTypeRef cftype, bool dump = true) {
        ref<CFStringRef> descr;
        if (dump) {
            descr.set(CFCopyDescription(cftype));
        }
        if (!ABRecordSetValue(m_person, property, cftype)) {
            if (dump) {
                throwError(string("setting ") +
#ifndef IPHONE
                           CFString2Std(property) +
#else
                           "property " +
#endif
                           + " to " + CFString2Std(descr) + "'");
            } else {
                throwError(string("setting ") + 
#ifndef IPHONE
                           CFString2Std(property)
#else
                           "property"
#endif
                           );
            }
        }
    }

    /** add another label/value pair to a multi-value list */
    void toPersonMultiVal(const mapping &map, CFStringRef label, CFTypeRef value) {
        if (!m_multi[map.m_customInt]) {
            m_multi[map.m_customInt].set(ABMultiValueCreateMutable(), "multivalue");
        }
        CFStringRef res;
        if (!ABMultiValueAdd(m_multi[map.m_customInt],
                             value,
                             label,
                             &res)) {
            throwError(string("adding multi value for ") + map.m_vCardProp);
        } else {
#ifndef IPHONE
            CFRelease(res);
#endif
        }
    }

    /**
     * mapping between vCard and ABPerson properties
     */
    static const struct mapping {
        /** the name of the vCard property, e.g. "ADDR", NULL terminates array */
        const char *m_vCardProp;
        /** address of ABPerson property, NULL pointer if none matches directly */
        const CFStringRef *m_abPersonProp;
        /** called when the property is found in the VObject: default is to copy string */
        toPerson_t m_toPerson;
        /** called when the property is found in the ABPerson: default is to copy string */
        fromPerson_t m_fromPerson;
        /** custom value available to callbacks */
        int m_customInt;
        /** custom value available to callbacks */
        CFStringRef m_customString;
    } m_mapping[];

    /** copy normal string directly */
    void fromPersonString(const mapping &map, CFTypeRef cftype) {
        string value(CFString2Std((CFStringRef)cftype));
        m_vobj.addProperty(map.m_vCardProp, value.c_str());
    }

    /** copy normal string directly */
    void toPersonString(const mapping &map, VProperty &vprop) {
        const char *value = vprop.getValue();
        /*
         * Empty strings are not properly ignored by the iPhone GUI,
         * better not add empty string properties. Empty vcard
         * properties as an indication that the property is to be
         * cleared are still handled because all known properties
         * were removed in toPerson().
         */
        if (value && *value) {
            setPersonProp(*map.m_abPersonProp, value);
        }
    }

    /** remember string to compose a more complex vCard property later (e.g. "N") */
    void fromPersonStoreString(const mapping &map, CFTypeRef cftype) {
        m_strings[map.m_customInt] = CFString2Std((CFStringRef)cftype);
    }

    /**
     * add a generic string with a predefined label
     * (map.m_customString) or a work/home label to multi-value
     */
    void toPersonStore(const mapping &map, VProperty &vprop) {
        const char *value = vprop.getValue();
        if (!value || !value[0]) {
            return;
        }
        ref<CFStringRef> cfstring(Std2CFString(value));
        CFStringRef label = map.m_customString;
        if (!label) {
            // IM property: label depends on type;
            // same simplification as in fromPersonChat
            if (map.m_customString) {
                label = map.m_customString;
            } else if (vprop.isType("HOME")) {
                label = kABJabberHomeLabel;
            } else if (vprop.isType("WORK")) {
                label = kABJabberHomeLabel;
            } else {
                label = otherLabel;
            }
        }

        toPersonMultiVal(map, label, cfstring);
    }

    /** copy date */
    void fromPersonDate(const mapping &map, CFTypeRef cftype) {
        ref<CFTimeZoneRef> tz(CFTimeZoneCopyDefault());
        CFGregorianDate date = CFAbsoluteTimeGetGregorianDate(CFDateGetAbsoluteTime((CFDateRef)cftype), tz);
        char buffer[40];
        sprintf(buffer, "%04d-%02d-%02d", date.year, date.month, date.day);
        m_vobj.addProperty(map.m_vCardProp, buffer);
    }

    /** copy date */
    void toPersonDate(const mapping &map, VProperty &vprop) {
        int year, month, day;
        const char *value = vprop.getValue();
        if (!value || !value[0]) {
            return;
        }
        if (sscanf(value, "%d-%d-%d", &year, &month, &day) == 3) {
            CFGregorianDate date;
            memset(&date, 0, sizeof(date));
            date.year = year;
            date.month = month;
            date.day = day;

            /*
             * The iPhone stores absolute times for dates, but
             * interprets them according to the current time zone.
             * The effect is that a birthday changes as the system
             * timezone is changed.
             *
             * To mitigate this problem dates are created with
             * an absolute time in the current time zone, just like
             * the iPhone GUI does.
             */
            ref<CFTimeZoneRef> tz(CFTimeZoneCopyDefault());
            ref<CFDateRef> cfdate(CFDateCreate(NULL, CFGregorianDateGetAbsoluteTime(date, tz)));
            if (cfdate) {
                // assert(map.m_abPersonProp);
                setPersonProp(*map.m_abPersonProp, cfdate);
            }
        }
    }

    /** map URL multi-value to vCard URL with different TYPEs */
    void fromPersonURLs(const mapping &map, CFTypeRef cftype) {
        int index = ABMultiValueCount((ABMultiValueRef)cftype) - 1;
        while (index >= 0) {
            ref<CFStringRef> label((CFStringRef)ABMultiValueCopyLabelAtIndex((ABMultiValueRef)cftype, index), "label");
            ref<CFStringRef> value((CFStringRef)ABMultiValueCopyValueAtIndex((ABMultiValueRef)cftype, index), "value");

            VProperty vprop("URL");
            string url = CFString2Std(value);
            vprop.setValue(url.c_str());
            if (CFStringCompare(label, (CFStringRef)kABHomePageLabel, 0) == kCFCompareEqualTo) {
                // leave type blank
            } else if (CFStringCompare(label, (CFStringRef)workLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "WORK");
            } else if (CFStringCompare(label, otherLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "OTHER");
            } else {
                string labelstr = CFString2Std(label);
                vprop.addParameter("TYPE", labelstr.c_str());
            }
            m_vobj.addProperty(&vprop);

            index--;
        }
    }

    /** iPhone: add another URL to multi-value (Mac OS X only has one string property) */
    void toPersonURLs(const mapping &map, VProperty &vprop) {
        const char *value = vprop.getValue();
        if (!value || !value[0]) {
            return;
        }
        boost::scoped_array<char> buffer(wstrdup(value));
        char *saveptr, *endptr;

        ref<CFStringRef> cfvalue(Std2CFString(value));
        CFStringRef label;
        ref<CFStringRef> custom;
        const char *type = vprop.getParameterValue("TYPE");
        if (vprop.isType("WORK")) {
            label = workLabel;
        } else if(vprop.isType("HOME")) {
            label = (CFStringRef)kABHomePageLabel;
        } else if(vprop.isType("OTHER")) {
            label = otherLabel;
        } else if (type) {
            custom.set(Std2CFString(type));
            label = custom;
        } else {
            label = (CFStringRef)kABHomePageLabel;
        }
        toPersonMultiVal(map, label, cfvalue);
    }

    /** map email multi-value to vCard EMAIL with different TYPEs */
    void fromPersonEMail(const mapping &map, CFTypeRef cftype) {
        int index = ABMultiValueCount((ABMultiValueRef)cftype) - 1;
        while (index >= 0) {
            ref<CFStringRef> label((CFStringRef)ABMultiValueCopyLabelAtIndex((ABMultiValueRef)cftype, index), "label");
            ref<CFStringRef> value((CFStringRef)ABMultiValueCopyValueAtIndex((ABMultiValueRef)cftype, index), "value");
            VProperty vprop("EMAIL");

            if (CFStringCompare(label, kABEmailWorkLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "WORK");
            } else if (CFStringCompare(label, kABEmailHomeLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "HOME");
            } else {
                string labelstr = CFString2Std(label);
                vprop.addParameter("TYPE", labelstr.c_str());
            }

            string email = CFString2Std(value);
            vprop.setValue(email.c_str());
            m_vobj.addProperty(&vprop);

            index--;
        }
    }

    /** add another EMAIL to the email multi-value */
    void toPersonEMail(const mapping &map, VProperty &vprop) {
        const char *value = vprop.getValue();
        if (!value || !value[0]) {
            return;
        }
        boost::scoped_array<char> buffer(wstrdup(value));
        char *saveptr, *endptr;

        ref<CFStringRef> cfvalue(Std2CFString(value));
        CFStringRef label;
        ref<CFStringRef> custom;
        const char *type = vprop.getParameterValue("TYPE");
        if (vprop.isType("WORK")) {
            label = kABEmailWorkLabel;
        } else if(vprop.isType("HOME")) {
            label = kABEmailHomeLabel;
        } else if (type) {
            custom.set(Std2CFString(type));
            label = custom;
        } else {
            label = otherLabel;
        }

        toPersonMultiVal(map, label, cfvalue);
    }

    /** map address multi-value to vCard ADR with different TYPEs */
    void fromPersonAddr(const mapping &map, CFTypeRef cftype) {
        int index = ABMultiValueCount((ABMultiValueRef)cftype) - 1;
        while (index >= 0) {
            ref<CFStringRef> label((CFStringRef)ABMultiValueCopyLabelAtIndex((ABMultiValueRef)cftype, index), "label");
            ref<CFDictionaryRef> value((CFDictionaryRef)ABMultiValueCopyValueAtIndex((ABMultiValueRef)cftype, index), "value");
            CFStringRef part;
            VProperty vprop((char *)map.m_vCardProp);

            string adr;
            // no PO box
            adr += VObject::SEMICOLON_REPLACEMENT;
            // no extended address
            adr += VObject::SEMICOLON_REPLACEMENT;
            // street
            part = (CFStringRef)CFDictionaryGetValue(value, kABAddressStreetKey);
            if (part) {
                adr += CFString2Std(part);
            }
            adr += VObject::SEMICOLON_REPLACEMENT;
            // city
            part = (CFStringRef)CFDictionaryGetValue(value, kABAddressCityKey);
            if (part) {
                adr += CFString2Std(part);
            }
            adr += VObject::SEMICOLON_REPLACEMENT;
            // region
            part = (CFStringRef)CFDictionaryGetValue(value, kABAddressStateKey);
            if (part) {
                adr += CFString2Std(part);
            }
            adr += VObject::SEMICOLON_REPLACEMENT;
            // ZIP code
            part = (CFStringRef)CFDictionaryGetValue(value, kABAddressZIPKey);
            if (part) {
                adr += CFString2Std(part);
            }
            adr += VObject::SEMICOLON_REPLACEMENT;
            // country
            part = (CFStringRef)CFDictionaryGetValue(value, kABAddressCountryKey);
            if (part) {
                adr += CFString2Std(part);
            }
            adr += VObject::SEMICOLON_REPLACEMENT;

            // not supported: kABAddressCountryCodeKey

            if (CFStringCompare(label, kABAddressWorkLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "WORK");
            } else if (CFStringCompare(label, kABAddressHomeLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "HOME");
            }

            vprop.setValue(adr.c_str());
            m_vobj.addProperty(&vprop);

            index--;
        }
    }

    /** add another ADR to address multi-value */
    void toPersonAddr(const mapping &map, VProperty &vprop) {
        const char *value = vprop.getValue();
        if (!value || !value[0]) {
            return;
        }
        boost::scoped_array<char> buffer(wstrdup(value));
        char *saveptr, *endptr;

        ref<CFMutableDictionaryRef> dict(CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

        // cannot store PO box and extended address
        char *pobox = my_strtok_r(buffer, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        char *extadr = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);

        char *street = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (street && *street) {
            ref<CFStringRef> cfstring(Std2CFString(street));
            CFDictionarySetValue(dict, kABAddressStreetKey, cfstring);
        }
        char *city = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (city && *city) {
            ref<CFStringRef> cfstring(Std2CFString(city));
            CFDictionarySetValue(dict, kABAddressCityKey, cfstring);
        }
        char *region = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (region && *region) {
            ref<CFStringRef> cfstring(Std2CFString(region));
            CFDictionarySetValue(dict, kABAddressStateKey, cfstring);
        }
        char *zip = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (zip && *zip) {
            ref<CFStringRef> cfstring(Std2CFString(zip));
            CFDictionarySetValue(dict, kABAddressZIPKey, cfstring);
        }
        char *country = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (country && *country) {
            ref<CFStringRef> cfstring(Std2CFString(country));
            CFDictionarySetValue(dict, kABAddressCountryKey, cfstring);
        }

        CFStringRef label;
        if (vprop.isType("WORK")) {
            label = kABAddressWorkLabel;
        } else if(vprop.isType("HOME")) {
            label = kABAddressHomeLabel;
        } else {
            label = otherLabel;
        }

        toPersonMultiVal(map, label, dict);
    }

    /** map phone multi-value to vCard TEL with different TYPEs */
    void fromPersonPhone(const mapping &map, CFTypeRef cftype) {
        int index = ABMultiValueCount((ABMultiValueRef)cftype) - 1;
        while (index >= 0) {
            ref<CFStringRef> label((CFStringRef)ABMultiValueCopyLabelAtIndex((ABMultiValueRef)cftype, index), "label");
            ref<CFStringRef> value((CFStringRef)ABMultiValueCopyValueAtIndex((ABMultiValueRef)cftype, index), "value");
            VProperty vprop("TEL");

            if (CFStringCompare(label, kABPhoneWorkLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "WORK");
                vprop.addParameter("TYPE", "VOICE");
            } else if (CFStringCompare(label, mainWorkLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "WORK");
                vprop.addParameter("TYPE", "PREF");
            } else if (CFStringCompare(label, kABPhoneHomeLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "HOME");
                vprop.addParameter("TYPE", "VOICE");
            } else if (CFStringCompare(label, kABPhoneMobileLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "CELL");
            } else if (CFStringCompare(label, kABPhoneMainLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "PREF");
                vprop.addParameter("TYPE", "VOICE");
            } else if (CFStringCompare(label, kABPhoneHomeFAXLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "HOME");
                vprop.addParameter("TYPE", "FAX");
            } else if (CFStringCompare(label, kABPhoneWorkFAXLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "WORK");
                vprop.addParameter("TYPE", "FAX");
            } else if (CFStringCompare(label,kABPhonePagerLabel , 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "PAGER");
            } else {
                // custom phone types not supported
                vprop.addParameter("TYPE", "VOICE");
            }

            string phone = CFString2Std(value);
            vprop.setValue(phone.c_str());
            m_vobj.addProperty(&vprop);

            index--;
        }
    }

    /** add another phone to the multi-value */
    void toPersonPhone(const mapping &map, VProperty &vprop) {
        const char *value = vprop.getValue();
        if (!value || !value[0]) {
            return;
        }
        boost::scoped_array<char> buffer(wstrdup(value));
        char *saveptr, *endptr;

        ref<CFStringRef> cfvalue(Std2CFString(value));
        CFStringRef label;
        if (vprop.isType("WORK")) {
            if (vprop.isType("FAX")) {
                label = kABPhoneWorkFAXLabel;
            } else if (vprop.isType("PREF")) {
                label = mainWorkLabel;
            } else {
                label = kABPhoneWorkLabel;
            }
        } else if(vprop.isType("HOME")) {
            if (vprop.isType("FAX")) {
                label = kABPhoneHomeFAXLabel;
            } else {
                label = kABPhoneHomeLabel;
            }
        } else if(vprop.isType("PREF") || vprop.isType("VOICE")) {
            label = kABPhoneMainLabel;
        } else if(vprop.isType("PAGER")) {
            label = kABPhonePagerLabel;
        } else if(vprop.isType("CELL")) {
            label = kABPhoneMobileLabel;
        } else {
            label = otherLabel;
        }

        toPersonMultiVal(map, label, cfvalue);
    }

    /**
     * map chat contact multi-value to respective vCard X- properties
     *
     * complementary operation is toPersonStore()
     */
    void fromPersonChat(const mapping &map, CFTypeRef cftype) {
        int index = ABMultiValueCount((ABMultiValueRef)cftype) - 1;
        while (index >= 0) {
            ref<CFStringRef> label((CFStringRef)ABMultiValueCopyLabelAtIndex((ABMultiValueRef)cftype, index), "label");
            ref<CFStringRef> value((CFStringRef)ABMultiValueCopyValueAtIndex((ABMultiValueRef)cftype, index), "value");
            VProperty vprop((char *)map.m_vCardProp);

            // this is a slight over-simplification:
            // the assumption is that the labels for all IM properties are interchangeable
            // although the header file has different constants for them
            if (CFStringCompare(label, kABJabberWorkLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "WORK");
            } else if (CFStringCompare(label, kABJabberHomeLabel, 0) == kCFCompareEqualTo) {
                vprop.addParameter("TYPE", "HOME");
            } else {
                // custom IM types not supported
            }

            string im = CFString2Std(value);
            vprop.setValue(im.c_str());
            m_vobj.addProperty(&vprop);

            index--;
        }
    }

    /** map related names multi-value to some vCard extension properties */
    void fromPersonNames(const mapping &map, CFTypeRef cftype) {
        int index = ABMultiValueCount((ABMultiValueRef)cftype) - 1;
        while (index >= 0) {
            ref<CFStringRef> label((CFStringRef)ABMultiValueCopyLabelAtIndex((ABMultiValueRef)cftype, index), "label");
            ref<CFStringRef> value((CFStringRef)ABMultiValueCopyValueAtIndex((ABMultiValueRef)cftype, index), "value");
            string name = CFString2Std(value);

            // there are no standard fields for all these related names:
            // use the ones from Evolution because some SyncML servers have
            // been extended to support them
            if (CFStringCompare(label, kABManagerLabel, 0) == kCFCompareEqualTo) {
                m_vobj.addProperty("X-EVOLUTION-MANAGER", name.c_str());
            } else if (CFStringCompare(label, kABAssistantLabel, 0) == kCFCompareEqualTo) {
                m_vobj.addProperty("X-EVOLUTION-ASSISTANT", name.c_str());
            } else if (CFStringCompare(label, kABSpouseLabel, 0) == kCFCompareEqualTo) {
                m_vobj.addProperty("X-EVOLUTION-SPOUSE", name.c_str());
            } else {
                // many related names not supported
            }

            index--;
        }
    }

    /**
     * decode vCard N and store in person properties
     *
     * complementary operation is fromPersonStoreString()
     */
    void toPersonName(const mapping &map, VProperty &vprop) {
        const char *value = vprop.getValue();
        if (!value || !value[0]) {
            return;
        }
        boost::scoped_array<char> buffer(wstrdup(value));
        char *saveptr, *endptr;

        char *last = my_strtok_r(buffer, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (last && *last) {
            setPersonProp(kABLastNameProperty, last);
        }

        char *first = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (first && *first) {
            setPersonProp(kABFirstNameProperty, first);
        }

        char *middle = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (middle && *middle) {
            setPersonProp(kABMiddleNameProperty, middle);
        }

        char *prefix = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
#ifndef IPHONE
        if (prefix && *prefix) {
            setPersonProp(kABTitleProperty, prefix);
        }
#endif

        char *suffix = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (suffix && *suffix) {
            setPersonProp(kABSuffixProperty, suffix);
        }
    }

    /**
     * decode ORG and store in person properties
     *
     * complementary operation is fromPersonStoreString()
     */
    void toPersonOrg(const mapping &map, VProperty &vprop) {
        const char *value = vprop.getValue();
        if (!value || !value[0]) {
            return;
        }
        boost::scoped_array<char> buffer(wstrdup(value));
        char *saveptr, *endptr;

        char *company = my_strtok_r(buffer, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (company && *company) {
            setPersonProp(kABOrganizationProperty, company);
        }

        char *department = my_strtok_r(NULL, VObject::SEMICOLON_REPLACEMENT, &saveptr, &endptr);
        if (department && *department) {
            setPersonProp(kABDepartmentProperty, department);
        }
    }
};

const CFStringRef *vCard2ABPerson::m_multiProp[MAX_MULTIVALUE] = {
#ifdef IPHONE
    &kABCURLProperty,
#else
    (CFStringRef*)&kABURLsProperty,
#endif
    &kABEmailProperty,
    &kABPhoneProperty,
#ifndef IPHONE
    &kABOtherDatesProperty,
    &kABAIMInstantProperty,
    &kABJabberInstantProperty,
    &kABMSNInstantProperty,
    &kABYahooInstantProperty,
    &kABICQInstantProperty,
#endif
    &kABRelatedNamesProperty,
    &kABAddressProperty
};

const vCard2ABPerson::mapping vCard2ABPerson::m_mapping[] = {
    { "", &kABFirstNameProperty, NULL, &vCard2ABPerson::fromPersonStoreString, FIRST_NAME },
    { "", &kABLastNameProperty, NULL, &vCard2ABPerson::fromPersonStoreString, LAST_NAME },
    { "", &kABMiddleNameProperty, NULL, &vCard2ABPerson::fromPersonStoreString, MIDDLE_NAME },
#ifndef IPHONE
    { "", &kABTitleProperty, NULL, &vCard2ABPerson::fromPersonStoreString, TITLE },
#endif
    { "", &kABSuffixProperty, NULL, &vCard2ABPerson::fromPersonStoreString, SUFFIX },
    { "N", 0, &vCard2ABPerson::toPersonName },
    /* "FN" */
    /* kABFirstNamePhoneticProperty */
    /* kABLastNamePhoneticProperty */
    /* kABMiddleNamePhoneticProperty */
    { "BDAY", &kABBirthdayProperty, &vCard2ABPerson::toPersonDate, &vCard2ABPerson::fromPersonDate },

    { "", &kABOrganizationProperty, NULL, &vCard2ABPerson::fromPersonStoreString, ORGANIZATION },
    { "", &kABDepartmentProperty, NULL, &vCard2ABPerson::fromPersonStoreString, DEPARTMENT },
    { "ORG", 0, &vCard2ABPerson::toPersonOrg },

    { "TITLE", &kABJobTitleProperty },
    /* "ROLE" */

#ifdef IPHONE
    { "URL", &kABCURLProperty, &vCard2ABPerson::toPersonURLs, &vCard2ABPerson::fromPersonURLs, URLS },
#else
    /**
     * bug in the header files for kABHomePageProperty and kABURLsProperty,
     * typecast required
     */
    { "URL", (CFStringRef *)&kABHomePageProperty },
    { "", (CFStringRef *)&kABURLsProperty, NULL, &vCard2ABPerson::fromPersonURLs },
#endif
#if 0
kABHomePageLabel
#endif

    { "EMAIL", &kABEmailProperty, &vCard2ABPerson::toPersonEMail, &vCard2ABPerson::fromPersonEMail, EMAILS },
#if 0
kABEmailWorkLabel
kABEmailHomeLabel
#endif
        
    { "ADR", &kABAddressProperty, &vCard2ABPerson::toPersonAddr, &vCard2ABPerson::fromPersonAddr, ADDRESSES },
#if 0
kABAddressWorkLabel
kABAddressHomeLabel

kABAddressStreetKey
kABAddressCityKey
kABAddressStateKey
kABAddressZIPKey
kABAddressCountryKey
kABAddressCountryCodeKey
#endif
    /* LABEL */

    { "TEL", &kABPhoneProperty, &vCard2ABPerson::toPersonPhone, &vCard2ABPerson::fromPersonPhone, PHONES },

#if 0
kABPhoneWorkLabel
kABPhoneHomeLabel
kABPhoneMobileLabel
kABPhoneMainLabel
kABPhoneHomeFAXLabel
kABPhoneWorkFAXLabel
kABPhonePagerLabel
#endif
#ifndef IPHONE
    { "X-AIM", &kABAIMInstantProperty, &vCard2ABPerson::toPersonStore, &vCard2ABPerson::fromPersonChat, AIM },
    { "X-JABBER", &kABJabberInstantProperty, &vCard2ABPerson::toPersonStore, &vCard2ABPerson::fromPersonChat, JABBER },
    { "X-MSN", &kABMSNInstantProperty, &vCard2ABPerson::toPersonStore, &vCard2ABPerson::fromPersonChat, MSN },
    { "X-YAHOO", &kABYahooInstantProperty, &vCard2ABPerson::toPersonStore, &vCard2ABPerson::fromPersonChat, YAHOO },
    { "X-ICQ", &kABICQInstantProperty, &vCard2ABPerson::toPersonStore, &vCard2ABPerson::fromPersonChat, ICQ },
#endif
    /* "X-GROUPWISE */
    { "NOTE", &kABNoteProperty },
    { "NICKNAME", &kABNicknameProperty },
    
    /* kABMaidenNameProperty */
    /* kABOtherDatesProperty */
#ifndef IPHONE
    { "", &kABRelatedNamesProperty, NULL, &vCard2ABPerson::fromPersonNames },
#endif
#if 0
kABMotherLabel
kABFatherLabel
kABParentLabel
kABSisterLabel
kABBrotherFAXLabel
kABChildLabel
kABFriendLabel
kABSpouseLabel
kABPartnerLabel
kABAssistantLabel
kABManagerLabel
#endif
    { "X-EVOLUTION-MANAGER", 0, &vCard2ABPerson::toPersonStore, NULL, NAMES, kABManagerLabel },
    { "X-EVOLUTION-ASSISTANT", 0, &vCard2ABPerson::toPersonStore, NULL, NAMES, kABAssistantLabel },
    { "X-EVOLUTION-SPOUSE", 0, &vCard2ABPerson::toPersonStore, NULL, NAMES, kABSpouseLabel },

    /* kABPersonFlags */
    /* X-EVOLUTION-FILE-AS */
    /* CATEGORIES */
    /* CALURI */
    /* FBURL */
    /* X-EVOLUTION-VIDEO-URL */
    /* X-MOZILLA-HTML */
    /* X-EVOLUTION-ANNIVERSARY */

    { NULL }
};


double AddressBookSource::getModTime(ABRecordRef record)
{
    double absolute;
#ifdef IPHONE
    absolute = (double)(int)ABRecordCopyValue(record,
                                              kABModificationDateProperty);
#else
    ref<CFDateRef> itemModTime((CFDateRef)ABRecordCopyValue(record,
                                                            kABModificationDateProperty));
    if (!itemModTime) {
        itemModTime.set((CFDateRef)ABRecordCopyValue(record,
                                                     kABCreationDateProperty));
    }
    if (!itemModTime) {
        throwError("extracting time stamp");
    }
    absolute = CFDateGetAbsoluteTime(itemModTime);
#endif

    // round up to next full second:
    // together with a sleep of 1 second in endSyncThrow() this ensures
    // that our time stamps are always >= the stored time stamp even if
    // the time stamp is rounded in the database
    return ceil(absolute);
}


AddressBookSource::AddressBookSource(const string &name,
                                     EvolutionSyncSourceConfig *sc,
                                     const string &changeId,
                                     const string &id,
                                     const string &configPath) :
    EvolutionSyncSource(name, sc, changeId, id),
    m_asVCard30(false)
{
    m_modNodeName = configPath + "/changes_" + changeId;

    // hack: when configured to talk with ScheduleWorld "card3" URI then
    // switch to sending vCard 3.0. A better solution would be to wait for
    // server SyncCap information, but this is not currently possible (not sent
    // by SW, not made available to Sync Source (?)).
    if (sc && sc->getURI() && !strcasecmp(sc->getURI(), "card3")) {
        setVCard30(true);
    }
}

AddressBookSource::AddressBookSource(const AddressBookSource &other) :
    EvolutionSyncSource(other),
    m_modNodeName(other.m_modNodeName)
{}

EvolutionSyncSource::sources AddressBookSource::getSyncBackends()
{
    EvolutionSyncSource::sources result;

    result.push_back(EvolutionSyncSource::source("<<system>>", ""));
    return result;
}

void AddressBookSource::open()
{
    m_addressbook = ABGetSharedAddressBook();
    if (!m_addressbook) {
        throwError("opening address book");
    }
    m_modTimes.set(new spdm::DeviceManagementNode(m_modNodeName.c_str()), "change management node");
    m_modTimes->setAutosave(FALSE);
}

void AddressBookSource::beginSyncThrow(bool needAll,
                                       bool needPartial,
                                       bool deleteLocal)
{
    ref<CFArrayRef> allPersons(ABCopyArrayOfAllPeople(m_addressbook), "list of all people");

    for (CFIndex i = 0; i < CFArrayGetCount(allPersons); i++) {
        ref<CFStringRef> cfuid(ABRecordCopyUniqueId((ABRecordRef)CFArrayGetValueAtIndex(allPersons, i)), "reading UID");
        string uid(CFString2Std(cfuid));

        if (deleteLocal) {
            if (!ABRemoveRecord(m_addressbook, (ABRecordRef)CFArrayGetValueAtIndex(allPersons, i))) {
                throwError("deleting contact");
            }
        } else {
            m_allItems.addItem(uid);
            if (needPartial) {
                eptr<char> serverModTimeStr(m_modTimes->readPropertyValue(uid.c_str()));
                double itemModTime = getModTime((ABRecordRef)CFArrayGetValueAtIndex(allPersons, i));
                char buffer[80];

                sprintf(buffer, "%.8f", itemModTime);
                if (!serverModTimeStr || !serverModTimeStr[0]) {
                    m_newItems.addItem(uid);
                    m_modTimes->setPropertyValue(uid.c_str(), buffer);
                } else {
                    double serverModTime = strtod(serverModTimeStr, NULL);
                    if (itemModTime > serverModTime) {
                        m_updatedItems.addItem(uid);
                        m_modTimes->setPropertyValue(uid.c_str(), buffer);
                    }
                }
            }
        }
    }

    if (needPartial) {
        ArrayList uids;
        ArrayList modTimes;
        m_modTimes->readProperties(&uids, &modTimes);
        for (int i = 0; i < uids.size(); i++ ) {
            const StringBuffer *uid = (StringBuffer *)uids[i];
            if (m_allItems.find(uid->c_str()) == m_allItems.end()) {
                m_deletedItems.addItem(uid->c_str());
                m_modTimes->removeProperty(uid->c_str());
            }
        }
    }
            
    if (!needAll) {
        m_allItems.clear();
    }
}

void AddressBookSource::endSyncThrow()
{
    resetItems();

    if (m_addressbook && !hasFailed()) {
        LOG.debug("flushing address book");

        // store changes persistently
        if (!ABSave(m_addressbook)) {
            throwError("saving address book");
        }

        m_modTimes->update(FALSE);

        // time stamps are rounded to next second,
        // so to prevent changes in that range of inaccurracy
        // sleep a bit before returning control
        sleep(2);

        LOG.debug("done with address book");
    }
}

void AddressBookSource::close()
{
    endSyncThrow();
    m_addressbook = NULL;
    m_modTimes = NULL;
}

void AddressBookSource::exportData(ostream &out)
{
    ref<CFArrayRef> allPersons(ABCopyArrayOfAllPeople(m_addressbook), "list of all people");

    for (CFIndex i = 0; i < CFArrayGetCount(allPersons); i++) {
        ABRecordRef person = (ABRecordRef)CFArrayGetValueAtIndex(allPersons, i);
        CFStringRef descr = CFCopyDescription(person);
        ref<CFStringRef> cfuid(ABRecordCopyUniqueId(person), "reading UID");
        string uid(CFString2Std(cfuid));
        eptr<SyncItem> item(createItem(uid, true), "sync item");

        out << (char *)item->getData() << "\n";
    }
}

SyncItem *AddressBookSource::createItem(const string &uid, bool asVCard30)
{
    logItem(uid, "extracting from address book", true);

    ref<CFStringRef> cfuid(Std2CFString(uid));
    ref<ABPersonRef> person((ABPersonRef)ABCopyRecordForUniqueId(m_addressbook, cfuid), "contact");
    auto_ptr<SyncItem> item(new SyncItem(uid.c_str()));

#ifdef USE_ADDRESS_BOOK_VCARD
    ref<CFDataRef> vcard(ABPersonCopyVCardRepresentation(person), "vcard");
    LOG.debug("%*s", (int)CFDataGetLength(vcard), (const char *)CFDataGetBytePtr(vcard));
    item->setData(CFDataGetBytePtr(vcard), CFDataGetLength(vcard));
#else
    string vcard;
    try {
        vCard2ABPerson conv(vcard, person);
        conv.fromPerson(asVCard30);
    } catch (const std::exception &ex) {
        throwError("creating vCard for " + uid + " failed: " + ex.what());
    }
    item->setData(vcard.c_str(), vcard.size());
#endif

    item->setDataType(getMimeType());
    item->setModificationTime(0);
    item->setState(state);

    return item.release();
}

int AddressBookSource::addItemThrow(SyncItem& item)
{
    return insertItem(item, NULL);
}

int AddressBookSource::updateItemThrow(SyncItem& item)
{
    return insertItem(item, item.getKey());
}

int AddressBookSource::insertItem(SyncItem &item, const char *uid)
{
    int status = STC_OK;
    string data(getData(item));
    ref<ABPersonRef> person;

#ifdef USE_ADDRESS_BOOK_VCARD
    if (uid) {
        // overwriting the UID of a new contact failed - resort to deleting the old contact and inserting a new one
        deleteItemThrow(item);
    }

    ref<CFDataRef> vcard(CFDataCreate(NULL, (const UInt8 *)data.c_str(), data.size()), "vcard");
    person.set((ABPersonRef)ABPersonCreateWithVCardRepresentation(vcard));
    if (!person) {
        throwError(string("parsing vcard ") + data);
    }
#else
    if (uid) {
        // overwrite existing contact
        ref<CFStringRef> cfuid(Std2CFString(uid));
        person.set((ABPersonRef)ABCopyRecordForUniqueId(m_addressbook, cfuid), "contact");
    } else {
        // new contact
        person.set(PersonCreateWrapper(m_addressbook), "contact");
    }
    try {
        LOG.debug("storing vCard for %s:\n%s",
                  uid ? uid : "new contact",
                  data.c_str());
        vCard2ABPerson converter(data, person);
        converter.toPerson();
    } catch (const std::exception &ex) {
        throwError(string("storing vCard for ") + (uid ? uid : "new contact") + " failed: " + ex.what());
    }
#endif


    // make sure we have a modification time stamp, otherwise the address book
    // sets one at random times
    CFAbsoluteTime nowabs = CFAbsoluteTimeGetCurrent();
#ifdef IPHONE
    void *now = (void *)(int)round(nowabs);
#else
    ref<CFDateRef> now(CFDateCreate(NULL, nowabs), "current time");
#endif
    if (!ABRecordSetValue(person, kABModificationDateProperty, now)) {
        throwError("setting mod time");
    }

    // existing contacts do not have to (and cannot) be added (?)
    if (uid || ABAddRecord(m_addressbook, person)) {
#ifdef IPHONE
        /* need to save to get UID? */
        ABSave(m_addressbook);
#endif

        ref<CFStringRef> cfuid(ABRecordCopyUniqueId(person), "uid");
        string uidstr(CFString2Std(cfuid));
        item.setKey(uidstr.c_str());

        char buffer[80];
        sprintf(buffer, "%.8f", getModTime(person));
        LOG.debug("inserted contact %s with modification time %s",
                  uidstr.c_str(), buffer);
        m_modTimes->setPropertyValue(uidstr.c_str(), buffer);
    } else {
        throwError("storing new contact");
    }

    return status;
}

int AddressBookSource::deleteItemThrow(SyncItem& item)
{
    int status = STC_OK;
    ref<CFStringRef> cfuid(Std2CFString(item.getKey()));
    ref<ABPersonRef> person((ABPersonRef)ABCopyRecordForUniqueId(m_addressbook, cfuid));

    if (person) {
        if (!ABRemoveRecord(m_addressbook, person)) {
            throwError(string("deleting contact ") + item.getKey());
        }
    } else {
        LOG.debug("%s: %s: request to delete non-existant contact ignored",
                  getName(), item.getKey());
    }
    m_modTimes->removeProperty(item.getKey());

    return status;
}

void AddressBookSource::logItem(const string &uid, const string &info, bool debug)
{
    if (LOG.getLevel() >= (debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO)) {
        string line;

#if 0
        // TODO

        if (e_book_get_contact( m_addressbook,
                                uid.c_str(),
                                &contact,
                                &gerror )) {
            const char *fileas = (const char *)e_contact_get_const( contact, E_CONTACT_FILE_AS );
            if (fileas) {
                line += fileas;
            } else {
                const char *name = (const char *)e_contact_get_const( contact, E_CONTACT_FULL_NAME );
                if (name) {
                    line += name;
                } else {
                    line += "<unnamed contact>";
                }
            }
        } else {
            line += "<name unavailable>";
        }
#endif

        line += " (";
        line += uid;
        line += "): ";
        line += info;
        
        (LOG.*(debug ? &Log::debug : &Log::info))( "%s: %s", getName(), line.c_str() );
    }
}

void AddressBookSource::logItem(const SyncItem &item, const string &info, bool debug)
{
    if (LOG.getLevel() >= (debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO)) {
        string line;
        const char *data = (const char *)item.getData();
        int datasize = item.getDataSize();
        if (datasize <= 0) {
            data = "";
            datasize = 0;
        }
        string vcard( data, datasize );

        size_t offset = vcard.find( "FN:");
        if (offset != vcard.npos) {
            int len = vcard.find( "\r", offset ) - offset - 3;
            line += vcard.substr( offset + 3, len );
        } else {
            line += "<unnamed contact>";
        }

        if (!item.getKey() ) {
            line += ", NULL UID (?!)";
        } else if (!strlen( item.getKey() )) {
            line += ", empty UID";
        } else {
            line += ", ";
            line += item.getKey();

#if 0
            // TODO
            EContact *contact;
            GError *gerror = NULL;
            if (e_book_get_contact( m_addressbook,
                                    item.getKey(),
                                    &contact,
                                    &gerror )) {
                line += ", EV ";
                const char *fileas = (const char *)e_contact_get_const( contact, E_CONTACT_FILE_AS );
                if (fileas) {
                    line += fileas;
                } else {
                    const char *name = (const char *)e_contact_get_const( contact, E_CONTACT_FULL_NAME );
                    if (name) {
                        line += name;
                    } else {
                        line += "<unnamed contact>";
                    }
                }
            } else {
                line += ", not in Evolution";
            }
#endif
        }
        line += ": ";
        line += info;
        
        (LOG.*(debug ? &Log::debug : &Log::info))( "%s: %s", getName(), line.c_str() );
    }
}

#endif /* ENABLE_ADDRESSBOOK */
