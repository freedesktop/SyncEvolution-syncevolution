/*
 * Copyright (C) 2012 Intel Corporation
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

/**
 * The boost::locale based implementation of locale-factory.h.
 */

#include "locale-factory.h"
#include "folks.h"

#include <libebook/libebook.h>

#include <phonenumbers/phonenumberutil.h>
#include <phonenumbers/logger.h>
#include <boost/locale.hpp>
#include <boost/lexical_cast.hpp>

#include <unicode/unistr.h>
#include <unicode/translit.h>
#include <unicode/bytestream.h>
#include <unicode/locid.h>

SE_GLIB_TYPE(EBookQuery, e_book_query)

SE_BEGIN_CXX

typedef boost::shared_ptr<EPhoneNumber> EPhoneNumberCXX;
EPhoneNumberCXX EPhoneNumberCXXNew(EPhoneNumber *number) { return EPhoneNumberCXX(number, e_phone_number_free); }

/**
 * Use higher levels to break ties between strings which are
 * considered equal at the lower levels. For example, "Façade" and
 * "facade" would be compared as equal when using only base
 * characters, but compare differently when also considering a higher
 * level which includes accents.
 *
 * The drawback of higher levels is that they are computationally more
 * expensive (transformation is slower and leads to longer transformed
 * strings, thus a longer string comparisons during compare).
 *
 * The default here pays attention to accents, case, and
 * punctuation. According to
 * http://userguide.icu-project.org/collation/concepts, it is required
 * for Japanese.
 */
static const boost::locale::collator_base::level_type DEFAULT_COLLATION_LEVEL =
    boost::locale::collator_base::quaternary;

class CompareBoost : public IndividualCompare, private boost::noncopyable {
    std::locale m_locale;
    const boost::locale::collator<char> &m_collator;
    std::auto_ptr<icu::Transliterator> m_trans;

public:
    CompareBoost(const std::locale &locale);

    std::string transform(const char *string) const;
    std::string transform(const std::string &string) const;
};

CompareBoost::CompareBoost(const std::locale &locale) :
    m_locale(locale),
    m_collator(std::use_facet< boost::locale::collator<char> >(m_locale))
{
    std::string language = std::use_facet<boost::locale::info>(m_locale).language();
    if (language == "zh") {
        // Hard-code Pinyin sorting for all Chinese countries.
        //
        // There are three different ways of sorting Chinese and Western names:
        // 1. Sort Chinese characters in pinyin order, but separate from Latin
        // 2. Sort them interleaved with Latin, by the first character.
        // 3. Sort them fully interleaved with Latin.
        // Source: Mark Davis, ICU, http://sourceforge.net/mailarchive/forum.php?thread_name=CAJ2xs_GEnN-u3%3D%2B7P5puaF1%2BU__fX-4tuA-kEybThN9xsw577Q%40mail.gmail.com&forum_name=icu-support
        //
        // Either 2 or 3 is what apparently more people expect. Implementing 2 is
        // harder, whereas 3 fits into the "generate keys, compare keys" concept
        // of IndividualCompare, so we kind of arbitrarily implement that.
        SE_LOG_DEBUG(NULL, "enabling Pinyin");

        UErrorCode status = U_ZERO_ERROR;
        icu::Transliterator *trans = icu::Transliterator::createInstance("Han-Latin", UTRANS_FORWARD, status);
        m_trans.reset(trans);
        if (U_FAILURE(status)) {
            SE_LOG_WARNING(NULL, "creating ICU Han-Latin Transliterator for Pinyin failed, error code %s; falling back to normal collation", u_errorName(status));
            m_trans.reset();
        } else if (!trans) {
            SE_LOG_WARNING(NULL, "creating ICU Han-Latin Transliterator for Pinyin failed, no error code; falling back to normal collation");
        }
    }
}

std::string CompareBoost::transform(const char *string) const
{
    if (!string) {
        return "";
    }
    return transform(std::string(string));
}

std::string CompareBoost::transform(const std::string &string) const
{
    // TODO: use e_collator_generate_key

    if (m_trans.get()) {
        // std::string result;
        // m_trans->transliterate(icu::StringPiece(string), icu::StringByteSink<std::string>(&result));
        icu::UnicodeString buffer(string.c_str());
        m_trans->transliterate(buffer);
        std::string result;
        buffer.toUTF8String(result);
        result = m_collator.transform(DEFAULT_COLLATION_LEVEL, result);
        return result;
    } else {
        return m_collator.transform(DEFAULT_COLLATION_LEVEL, string);
    }
}

class CompareFirstLastBoost : public CompareBoost {
public:
    CompareFirstLastBoost(const std::locale &locale) :
        CompareBoost(locale)
    {}

    virtual void createCriteria(FolksIndividual *individual, Criteria_t &criteria) const {
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *family = folks_structured_name_get_family_name(fn);
            const char *given = folks_structured_name_get_given_name(fn);
            criteria.push_back(transform(given));
            criteria.push_back(transform(family));
        }
    }
};

class CompareLastFirstBoost : public CompareBoost {
public:
    CompareLastFirstBoost(const std::locale &locale) :
        CompareBoost(locale)
    {}

    virtual void createCriteria(FolksIndividual *individual, Criteria_t &criteria) const {
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *family = folks_structured_name_get_family_name(fn);
            const char *given = folks_structured_name_get_given_name(fn);
            criteria.push_back(transform(family));
            criteria.push_back(transform(given));
        }
    }
};

class CompareFullnameBoost : public CompareBoost {
public:
    CompareFullnameBoost(const std::locale &locale) :
        CompareBoost(locale)
    {}

    virtual void createCriteria(FolksIndividual *individual, Criteria_t &criteria) const {
        const char *fullname = folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual));
        if (fullname) {
            criteria.push_back(transform(fullname));
        } else {
            FolksStructuredName *fn =
                folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
            if (fn) {
                const char *given = folks_structured_name_get_given_name(fn);
                const char *middle = folks_structured_name_get_additional_names(fn);
                const char *family = folks_structured_name_get_family_name(fn);
                const char *suffix = folks_structured_name_get_suffixes(fn);
                std::string buffer;
                buffer.reserve(256);
#define APPEND(_str) \
                if (_str && *_str) { \
                    if (!buffer.empty()) { \
                        buffer += _str; \
                    } \
                }
                APPEND(given);
                APPEND(middle);
                APPEND(family);
                APPEND(suffix);
#undef APPEND
                criteria.push_back(transform(buffer));
            }
        }
    }
};

/**
 * Implements 'any-contains' and acts as utility base class
 * for the other text comparison operators.
 */
class AnyContainsBoost : public IndividualFilter
{
public:
    enum Mode {
        EXACT = 0,
        CASE_INSENSITIVE = 1<<0,
        ACCENT_INSENSITIVE = 1<<1,
        TRANSLITERATE = 1<<2,
        ALL =
        CASE_INSENSITIVE|
        ACCENT_INSENSITIVE|
        TRANSLITERATE|
        0
    };

    AnyContainsBoost(const std::locale &locale,
                     const std::string &searchValue,
                     int mode) :
        m_locale(locale),
        // For performance reasons we use ICU directly and thus need
        // an ICU::Locale.
        //         m_ICULocale(std::use_facet<boost::locale::info>(m_locale).language().c_str(),
        //            std::use_facet<boost::locale::info>(m_locale).country().c_str(),
        //            std::use_facet<boost::locale::info>(m_locale).variant().c_str()),
        // m_collator(std::use_facet<boost::locale::collator>(locale)),
        m_searchValue(searchValue),
        m_mode(mode)
    {
        if (mode & TRANSLITERATE) {
            UErrorCode status = U_ZERO_ERROR;
            m_transliterator.reset(Transliterator::createInstance ("Any-Latin", UTRANS_FORWARD, status));
            if (!m_transliterator ||
                U_FAILURE(status)) {
                SE_LOG_WARNING(NULL, "creating ICU Any-Latin Transliterator failed, error code %s; falling back to not transliterating", u_errorName(status));
                m_transliterator.reset();
                mode ^= TRANSLITERATE;
                m_mode = mode;
            }
        }

        switch (mode) {
        case EXACT:
            break;
        default:
            m_searchValueTransformed = transform(m_searchValue);
            break;
        }
        m_searchValueTel = normalizePhoneText(m_searchValue.c_str());
    }

    /**
     * Turn filter arguments into bit field.
     */
    static int getFilterMode(const std::vector<LocaleFactory::Filter_t> &terms,
                             size_t start);

    /** simplify according to mode */
    std::string transform(const char *in) const;
    std::string transform(const std::string &in) const { return transform(in.c_str()); }

    /**
     * The search text is not necessarily a full phone number,
     * therefore we cannot parse it with libphonenumber. Instead
     * do a sub-string search after telephone specific normalization,
     * to let the search ignore irrelevant formatting aspects:
     *
     * - Map ASCII characters to the corresponding digit.
     * - Reduce to just the digits before comparison (no spaces, no
     *   punctuation).
     *
     * Example: +1-800-FOOBAR -> 1800366227
     */
    static std::string normalizePhoneText(const char *tel)
    {
        static const i18n::phonenumbers::PhoneNumberUtil &util(*i18n::phonenumbers::PhoneNumberUtil::GetInstance());
        std::string res;
        char c;
        bool haveAlpha = false;
        while ((c = *tel) != '\0') {
            if (isdigit(c)) {
                res += c;
            } else if (isalpha(c)) {
                haveAlpha = true;
                res += c;
            }
            ++tel;
        }
        // Only scan through the string again if we really have to.
        if (haveAlpha) {
            util.ConvertAlphaCharactersInNumber(&res);
        }

        return res;
    }

    bool containsSearchText(const char *text) const
    {
        if (!text) {
            return false;
        }
        switch (m_mode) {
        case EXACT:
            return boost::contains(text, m_searchValue);
            break;
        default: {
            std::string transformed = transform(text);
            return boost::contains(transformed, m_searchValueTransformed);
            break;
        }
        }
        // not reached
        return false;
    }

    bool containsSearchTel(const char *text) const
    {
        std::string tel = normalizePhoneText(text);
        return boost::contains(tel, m_searchValueTel);
    }

    bool isSearchText(const char *text) const
    {
        if (!text) {
            return false;
        }
        switch (m_mode) {
        case EXACT:
            return boost::equals(text, m_searchValue);
            break;
        default: {
            std::string transformed = transform(text);
            return boost::equals(transformed, m_searchValueTransformed);
            break;
        }
        }
        // not reached
        return false;
    }

    bool isSearchTel(const char *text) const
    {
        std::string tel = normalizePhoneText(text);
        return boost::equals(tel, m_searchValueTel);
    }

    bool beginsWithSearchText(const char *text) const
    {
        if (!text) {
            return false;
        }
        switch (m_mode) {
        case EXACT:
            return boost::starts_with(text, m_searchValue);
            break;
        default:  {
            std::string transformed = transform(text);
            return boost::starts_with(transformed, m_searchValueTransformed);
            break;
        }
        }
        // not reached
        return false;
    }

    bool beginsWithSearchTel(const char *text) const
    {
        std::string tel = normalizePhoneText(text);
        return boost::starts_with(tel, m_searchValueTel);
    }

    bool endsWithSearchText(const char *text) const
    {
        if (!text) {
            return false;
        }
        switch (m_mode) {
        case EXACT:
            return boost::ends_with(text, m_searchValue);
            break;
        default: {
            std::string transformed = transform(text);
            return boost::ends_with(transformed, m_searchValueTransformed);
            break;
        }
        }
        // not reached
        return false;
    }

    bool endsWithSearchTel(const char *text) const
    {
        std::string tel = normalizePhoneText(text);
        return boost::ends_with(tel, m_searchValueTel);
    }

    virtual bool matches(const IndividualData &data) const
    {
        FolksIndividual *individual = data.m_individual.get();
        FolksNameDetails *name = FOLKS_NAME_DETAILS(individual);
        const char *fullname = folks_name_details_get_full_name(name);
        if (containsSearchText(fullname)) {
            return true;
        }
        const char *nickname = folks_name_details_get_nickname(name);
        if (containsSearchText(nickname)) {
            return true;
        }
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *given = folks_structured_name_get_given_name(fn);
            if (containsSearchText(given)) {
                return true;
            }
            const char *middle = folks_structured_name_get_additional_names(fn);
            if (containsSearchText(middle)) {
                return true;
            }
            const char *family = folks_structured_name_get_family_name(fn);
            if (containsSearchText(family)) {
                return true;
            }
        }
        FolksEmailDetails *emailDetails = FOLKS_EMAIL_DETAILS(individual);
        GeeSet *emails = folks_email_details_get_email_addresses(emailDetails);
        BOOST_FOREACH (FolksAbstractFieldDetails *email, GeeCollCXX<FolksAbstractFieldDetails *>(emails, ADD_REF)) {
            const gchar *value =
                reinterpret_cast<const gchar *>(folks_abstract_field_details_get_value(email));
            if (containsSearchText(value)) {
                return true;
            }
        }
        FolksPhoneDetails *phoneDetails = FOLKS_PHONE_DETAILS(individual);
        GeeSet *phones = folks_phone_details_get_phone_numbers(phoneDetails);
        BOOST_FOREACH (FolksAbstractFieldDetails *phone, GeeCollCXX<FolksAbstractFieldDetails *>(phones, ADD_REF)) {
            const gchar *value =
                reinterpret_cast<const gchar *>(folks_abstract_field_details_get_value(phone));
            if (containsSearchTel(value)) {
                return true;
            }
        }
        return false;
    }

private:
    std::locale m_locale;
    // icu::Locale m_ICULocale;
    boost::shared_ptr<icu::Transliterator> m_transliterator;
    std::string m_searchValue;
    std::string m_searchValueTransformed;
    std::string m_searchValueTel;
    int m_mode;
    // const bool (*m_contains)(const std::string &, const std::string &, const std::locale &);
};

std::string AnyContainsBoost::transform(const char *in) const
{
    icu::UnicodeString unicode = icu::UnicodeString::fromUTF8(in);
    if (m_mode & TRANSLITERATE) {
        m_transliterator->transliterate(unicode);
    }
    if (m_mode & CASE_INSENSITIVE) {
        unicode.foldCase();
    }
    std::string utf8;
    unicode.toUTF8String(utf8);
    if (m_mode & ACCENT_INSENSITIVE) {
        // Haven't found an easy way to do this with ICU.
        // Use e_util_utf8_remove_accents(), which also ensures
        // consistency with EDS.
        PlainGStr res = e_util_utf8_remove_accents(utf8.c_str());
        return std::string(res);
    } else {
        return utf8;
    }
}

int AnyContainsBoost::getFilterMode(const std::vector<LocaleFactory::Filter_t> &terms,
                                    size_t start)
{
    int mode = ALL;
    for (size_t i = start; i < terms.size(); i++) {
        const std::string &flag = LocaleFactory::getFilterString(terms[i], "any-contains flag");
        if (flag == "case-sensitive") {
            mode &= ~CASE_INSENSITIVE;
        } else if (flag == "case-insensitive") {
            mode |= CASE_INSENSITIVE;
        } else if (flag == "accent-sensitive") {
            mode &= ~ACCENT_INSENSITIVE;
        } else if (flag == "accent-insensitive") {
            mode |= ACCENT_INSENSITIVE;
        } else if (flag == "no-transliteration") {
            mode &= ~TRANSLITERATE;
        } else if (flag == "transliteration") {
            mode |= TRANSLITERATE;
        } else {
            SE_THROW("unsupported filter flag: " + flag);
        }
    }
    return mode;
}

class FilterFullName : public AnyContainsBoost
{
    bool (AnyContainsBoost::*m_operation)(const char *text) const;

public:
    FilterFullName(const std::locale &locale,
                   const std::string &searchValue,
                   int mode,
                   bool (AnyContainsBoost::*operation)(const char *text) const) :
        AnyContainsBoost(locale, searchValue, mode),
        m_operation(operation)
    {
    }

    virtual bool matches(const IndividualData &data) const
    {
        FolksIndividual *individual = data.m_individual.get();
        FolksNameDetails *name = FOLKS_NAME_DETAILS(individual);
        const char *fullname = folks_name_details_get_full_name(name);
        return (this->*m_operation)(fullname);
    }
};

class FilterNickname : public AnyContainsBoost
{
    bool (AnyContainsBoost::*m_operation)(const char *text) const;

public:
    FilterNickname(const std::locale &locale,
                   const std::string &searchValue,
                   int mode,
                   bool (AnyContainsBoost::*operation)(const char *text) const) :
        AnyContainsBoost(locale, searchValue, mode),
        m_operation(operation)
    {
    }

    virtual bool matches(const IndividualData &data) const
    {
        FolksIndividual *individual = data.m_individual.get();
        FolksNameDetails *name = FOLKS_NAME_DETAILS(individual);
        const char *fullname = folks_name_details_get_nickname(name);
        return (this->*m_operation)(fullname);
    }
};

class FilterFamilyName : public AnyContainsBoost
{
    bool (AnyContainsBoost::*m_operation)(const char *text) const;

public:
    FilterFamilyName(const std::locale &locale,
                     const std::string &searchValue,
                     int mode,
                     bool (AnyContainsBoost::*operation)(const char *text) const) :
        AnyContainsBoost(locale, searchValue, mode),
        m_operation(operation)
    {
    }

    virtual bool matches(const IndividualData &data) const
    {
        FolksIndividual *individual = data.m_individual.get();
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *name = folks_structured_name_get_family_name(fn);
            return (this->*m_operation)(name);
        } else {
            return false;
        }
    }
};

class FilterGivenName : public AnyContainsBoost
{
    bool (AnyContainsBoost::*m_operation)(const char *text) const;

public:
    FilterGivenName(const std::locale &locale,
                    const std::string &searchValue,
                    int mode,
                    bool (AnyContainsBoost::*operation)(const char *text) const) :
        AnyContainsBoost(locale, searchValue, mode),
        m_operation(operation)
    {
    }

    virtual bool matches(const IndividualData &data) const
    {
        FolksIndividual *individual = data.m_individual.get();
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *name = folks_structured_name_get_given_name(fn);
            return (this->*m_operation)(name);
        } else {
            return false;
        }
    }
};

class FilterAdditionalName : public AnyContainsBoost
{
    bool (AnyContainsBoost::*m_operation)(const char *text) const;

public:
    FilterAdditionalName(const std::locale &locale,
                         const std::string &searchValue,
                         int mode,
                         bool (AnyContainsBoost::*operation)(const char *text) const) :
        AnyContainsBoost(locale, searchValue, mode),
        m_operation(operation)
    {
    }

    virtual bool matches(const IndividualData &data) const
    {
        FolksIndividual *individual = data.m_individual.get();
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *name = folks_structured_name_get_additional_names(fn);
            return (this->*m_operation)(name);
        } else {
            return false;
        }
    }
};

class FilterEmails : public AnyContainsBoost
{
    bool (AnyContainsBoost::*m_operation)(const char *text) const;

public:
    FilterEmails(const std::locale &locale,
                 const std::string &searchValue,
                 int mode,
                 bool (AnyContainsBoost::*operation)(const char *text) const) :
        AnyContainsBoost(locale, searchValue, mode),
        m_operation(operation)
    {
    }

    virtual bool matches(const IndividualData &data) const
    {
        FolksIndividual *individual = data.m_individual.get();
        FolksEmailDetails *emailDetails = FOLKS_EMAIL_DETAILS(individual);
        GeeSet *emails = folks_email_details_get_email_addresses(emailDetails);
        BOOST_FOREACH (FolksAbstractFieldDetails *email, GeeCollCXX<FolksAbstractFieldDetails *>(emails, ADD_REF)) {
            const gchar *value =
                reinterpret_cast<const gchar *>(folks_abstract_field_details_get_value(email));
            if ((this->*m_operation)(value)) {
                return true;
            }
        }
        return false;
    }
};

class FilterTel : public AnyContainsBoost
{
    bool (AnyContainsBoost::*m_operation)(const char *text) const;

public:
    FilterTel(const std::locale &locale,
              const std::string &searchValue,
              bool (AnyContainsBoost::*operation)(const char *text) const) :
        AnyContainsBoost(locale, searchValue, 0 /* doesn't matter */),
        m_operation(operation)
    {
    }

    virtual bool matches(const IndividualData &data) const
    {
        FolksIndividual *individual = data.m_individual.get();
        FolksPhoneDetails *phoneDetails = FOLKS_PHONE_DETAILS(individual);
        GeeSet *phones = folks_phone_details_get_phone_numbers(phoneDetails);
        BOOST_FOREACH (FolksAbstractFieldDetails *phone, GeeCollCXX<FolksAbstractFieldDetails *>(phones, ADD_REF)) {
            const gchar *value =
                reinterpret_cast<const gchar *>(folks_abstract_field_details_get_value(phone));
            if ((this->*m_operation)(value)) {
                return true;
            }
        }
        return false;
    }
};

class FilterAddr : public AnyContainsBoost
{
protected:
    bool (AnyContainsBoost::*m_operation)(const char *text) const;

public:
    FilterAddr(const std::locale &locale,
               const std::string &searchValue,
               int mode,
               bool (AnyContainsBoost::*operation)(const char *text) const) :
        AnyContainsBoost(locale, searchValue, mode),
        m_operation(operation)
    {
    }

    virtual bool matches(const IndividualData &data) const
    {
        FolksIndividual *individual = data.m_individual.get();
        FolksPostalAddressDetails *addressDetails = FOLKS_POSTAL_ADDRESS_DETAILS(individual);
        GeeSet *addresses = folks_postal_address_details_get_postal_addresses(addressDetails);
        BOOST_FOREACH (FolksPostalAddressFieldDetails *address, GeeCollCXX<FolksPostalAddressFieldDetails *>(addresses, ADD_REF)) {
            const FolksPostalAddress *value =
                reinterpret_cast<const FolksPostalAddress *>(folks_abstract_field_details_get_value(FOLKS_ABSTRACT_FIELD_DETAILS(address)));
            if (matchesAddr(value)) {
                return true;
            }
        }
        return false;
    }

    virtual bool matchesAddr(const FolksPostalAddress *addr) const = 0;
};

class FilterAddrPOBox : public FilterAddr
{
public:
    FilterAddrPOBox(const std::locale &locale,
                    const std::string &searchValue,
                    int mode,
                    bool (AnyContainsBoost::*operation)(const char *text) const) :
        FilterAddr(locale, searchValue, mode, operation)
    {
    }

    virtual bool matchesAddr(const FolksPostalAddress *addr) const
    {
        const char *attr = folks_postal_address_get_po_box(const_cast<FolksPostalAddress *>(addr));
        return (this->*m_operation)(attr);
    }
};

class FilterAddrExtension : public FilterAddr
{
public:
    FilterAddrExtension(const std::locale &locale,
                        const std::string &searchValue,
                        int mode,
                        bool (AnyContainsBoost::*operation)(const char *text) const) :
        FilterAddr(locale, searchValue, mode, operation)
    {
    }

    virtual bool matchesAddr(const FolksPostalAddress *addr) const
    {
        const char *attr = folks_postal_address_get_extension(const_cast<FolksPostalAddress *>(addr));
        return (this->*m_operation)(attr);
    }
};

class FilterAddrStreet : public FilterAddr
{
public:
    FilterAddrStreet(const std::locale &locale,
                     const std::string &searchValue,
                     int mode,
                     bool (AnyContainsBoost::*operation)(const char *text) const) :
        FilterAddr(locale, searchValue, mode, operation)
    {
    }

    virtual bool matchesAddr(const FolksPostalAddress *addr) const
    {
        const char *attr = folks_postal_address_get_street(const_cast<FolksPostalAddress *>(addr));
        return (this->*m_operation)(attr);
    }
};

class FilterAddrLocality : public FilterAddr
{
public:
    FilterAddrLocality(const std::locale &locale,
                       const std::string &searchValue,
                       int mode,
                       bool (AnyContainsBoost::*operation)(const char *text) const) :
        FilterAddr(locale, searchValue, mode, operation)
    {
    }

    virtual bool matchesAddr(const FolksPostalAddress *addr) const
    {
        const char *attr = folks_postal_address_get_locality(const_cast<FolksPostalAddress *>(addr));
        return (this->*m_operation)(attr);
    }
};

class FilterAddrRegion : public FilterAddr
{
public:
    FilterAddrRegion(const std::locale &locale,
                     const std::string &searchValue,
                     int mode,
                     bool (AnyContainsBoost::*operation)(const char *text) const) :
        FilterAddr(locale, searchValue, mode, operation)
    {
    }

    virtual bool matchesAddr(const FolksPostalAddress *addr) const
    {
        const char *attr = folks_postal_address_get_region(const_cast<FolksPostalAddress *>(addr));
        return (this->*m_operation)(attr);
    }
};

class FilterAddrPostalCode : public FilterAddr
{
public:
    FilterAddrPostalCode(const std::locale &locale,
                         const std::string &searchValue,
                         int mode,
                         bool (AnyContainsBoost::*operation)(const char *text) const) :
        FilterAddr(locale, searchValue, mode, operation)
    {
    }

    virtual bool matchesAddr(const FolksPostalAddress *addr) const
    {
        const char *attr = folks_postal_address_get_postal_code(const_cast<FolksPostalAddress *>(addr));
        return (this->*m_operation)(attr);
    }
};

class FilterAddrCountry : public FilterAddr
{
public:
    FilterAddrCountry(const std::locale &locale,
                      const std::string &searchValue,
                      int mode,
                      bool (AnyContainsBoost::*operation)(const char *text) const) :
        FilterAddr(locale, searchValue, mode, operation)
    {
    }

    virtual bool matchesAddr(const FolksPostalAddress *addr) const
    {
        const char *attr = folks_postal_address_get_country(const_cast<FolksPostalAddress *>(addr));
        return (this->*m_operation)(attr);
    }
};


SimpleE164 String2E164(const char *tel, const char *country)
{
    SimpleE164 e164;
    GErrorCXX gerror;
    EPhoneNumberCXX number(EPhoneNumberCXXNew(e_phone_number_from_string(tel, country, gerror)));
    if (!number) {
        gerror.throwError(SE_HERE, "parsing number");
    }
    EPhoneNumberCountrySource source;
    e164.m_countryCode = e_phone_number_get_country_code(number.get(), &source);
    if (source == E_PHONE_NUMBER_COUNTRY_FROM_DEFAULT) {
        e164.m_countryCode = 0;
    }
    PlainGStr national(e_phone_number_get_national_number(number.get()));
    e164.m_nationalNumber = national.get() ?
        boost::lexical_cast<SimpleE164::NationalNumber_t>(national.get()) :
        0;
    return e164;
}

/**
 * Search value must be a valid caller ID (with or without a country
 * code). The telephone numbers in the contacts may or may not be
 * valid; only valid ones will match. The user is expected to clean up
 * that data to get exact matches for the others.
 *
 * The matching uses the same semantic as EQUALS_NATIONAL_PHONE_NUMBER:
 * - If both numbers have an explicit country code, that code must be
 *   the same for a match.
 * - If one or both numbers have no country code, matching the national
 *   part is enough for a match.
 */
class PhoneStartsWith : public IndividualFilter
{
public:
    PhoneStartsWith(const std::locale &m_locale,
                    const std::string &tel) :
        m_phoneNumberUtil(*i18n::phonenumbers::PhoneNumberUtil::GetInstance()),
        m_simpleEDSSearch(getenv("SYNCEVOLUTION_PIM_EDS_SUBSTRING") || !e_phone_number_is_supported()),
        m_country(std::use_facet<boost::locale::info>(m_locale).country())
    {
        m_number = String2E164(tel.c_str(), m_country.c_str());
    }

    virtual bool matches(const IndividualData &data) const
    {
        BOOST_FOREACH(const SimpleE164 &number, data.m_precomputed.m_phoneNumbers) {
            // National part must always match, country code only if
            // set explicitly in both (NSN_MATCH in libphonenumber,
            // EQUALS_NATIONAL_PHONE_NUMBER in EDS).
            if (number.m_nationalNumber == m_number.m_nationalNumber &&
                (!number.m_countryCode || !m_number.m_countryCode ||
                 number.m_countryCode == m_number.m_countryCode)) {
                return true;
            }
        }
        return false;
    }

    virtual std::string getEBookFilter() const
    {
        std::string tel = m_number.toString();
        size_t len = std::min((size_t)4, tel.size());
        EBookQueryCXX query(m_simpleEDSSearch ?
                            // A suffix match with a limited number of digits is most
                            // likely to find the right contacts.
                            e_book_query_field_test(E_CONTACT_TEL, E_BOOK_QUERY_ENDS_WITH,
                                                    tel.substr(tel.size() - len, len).c_str()) :
                            // We use EQUALS_NATIONAL_PHONE_NUMBER
                            // instead of EQUALS_PHONE_NUMBER here,
                            // because it will also match contacts
                            // were the country code was not set
                            // explicitly. EQUALS_PHONE_NUMBER would
                            // do a stricter comparison and not match
                            // those.
                            //
                            // If the contact has a country code set,
                            // then EQUALS_NATIONAL_PHONE_NUMBER will
                            // check that and not return a false match
                            // if the country code is different.
                            //
                            // We try to pass the E164 string here. If
                            // the search term had no country code,
                            // that's a bit difficult because we can't
                            // just add the default country code.
                            // That would break the
                            // NATIONAL_PHONE_NUMBER semantic because
                            // EDS wouldn't know that the search term
                            // had no country code. We resort to the
                            // format of SimpleE164.toString(), which
                            // is passing the national number
                            // formatted as string.
                            e_book_query_field_test(E_CONTACT_TEL, E_BOOK_QUERY_EQUALS_NATIONAL_PHONE_NUMBER,
                                                    tel.c_str()),
                            TRANSFER_REF);
        PlainGStr filter(e_book_query_to_string(query.get()));
        return filter.get();
    }

private:
    const i18n::phonenumbers::PhoneNumberUtil &m_phoneNumberUtil;
    bool m_simpleEDSSearch;
    std::string m_country;
    SimpleE164 m_number;
};

class PhoneNumberLogger : public i18n::phonenumbers::Logger
{
    const char *getPrefix()
    {
        switch (level()) {
        case i18n::phonenumbers::LOG_FATAL:
            return "phonenumber fatal";
            break;
        case i18n::phonenumbers::LOG_ERROR:
            return "phonenumber error";
            break;
        case i18n::phonenumbers::LOG_WARNING:
            return "phonenumber warning";
            break;
        case i18n::phonenumbers::LOG_INFO:
            return "phonenumber info";
            break;
        case i18n::phonenumbers::LOG_DEBUG:
            return "phonenumber debug";
            break;
        default:
            return "phonenumber ???";
            break;
        }
    }

public:
    virtual void WriteMessage(const std::string &msg)
    {
        SE_LOG(getPrefix(),
               level() == i18n::phonenumbers::LOG_FATAL ? SyncEvo::Logger::ERROR : SyncEvo::Logger::DEBUG,
               "%s", msg.c_str());
    }
};

class LocaleFactoryBoost : public LocaleFactory
{
    const i18n::phonenumbers::PhoneNumberUtil &m_phoneNumberUtil;
    bool m_edsSupportsPhoneNumbers;
    std::locale m_locale;
    std::string m_country;
    std::string m_defaultCountryCode;
    PhoneNumberLogger m_logger;

public:
    LocaleFactoryBoost() :
        m_phoneNumberUtil(*i18n::phonenumbers::PhoneNumberUtil::GetInstance()),
        m_edsSupportsPhoneNumbers(e_phone_number_is_supported() && !getenv("SYNCEVOLUTION_PIM_EDS_NO_E164")),
        m_locale(genLocale()),
        m_country(std::use_facet<boost::locale::info>(m_locale).country()),
        m_defaultCountryCode(StringPrintf("+%d", m_phoneNumberUtil.GetCountryCodeForRegion(m_country)))
    {
        // Redirect output of libphonenumber and make it a bit quieter
        // than it is by default. We map fatal libphonenumber errors
        // to ERROR and everything else to DEBUG.
        //
        // The PhoneNumberUtil instance owns the logger, so we don't
        // need (and must not) free it. libphonenumber < r571 has the
        // same API and thus this code compiles. However, older
        // libphonenumer does not actually free the instance, causing
        // a minor memory leak.
        i18n::phonenumbers::PhoneNumberUtil::GetInstance()->SetLogger(new PhoneNumberLogger);
    }

    static std::locale genLocale()
    {
        // Get current locale from environment. Configure the
        // generated locale so that it supports what we need and
        // nothing more.
        boost::locale::generator gen;
        gen.characters(boost::locale::char_facet);
        gen.categories(boost::locale::collation_facet |
                       boost::locale::convert_facet |
                       boost::locale::information_facet);
        // Hard-code "phonebook" collation for certain languages
        // where we know that it is desirable. We could use it
        // in all cases, except that ICU has a bug where it does not
        // fall back properly to the base collation. See
        // http://sourceforge.net/mailarchive/message.php?msg_id=30802924
        // and http://bugs.icu-project.org/trac/ticket/10149
        std::locale locale = gen("");
        std::string name = std::use_facet<boost::locale::info>(locale).name();
        std::string language = std::use_facet<boost::locale::info>(locale).language();
        std::string country = std::use_facet<boost::locale::info>(locale).country();
        SE_LOG_DEV(NULL, "PIM Manager running with locale %s = language %s in country %s",
                   name.c_str(),
                   language.c_str(),
                   country.c_str());
        if (language == "de" ||
            language == "fi") {
            SE_LOG_DEV(NULL, "enabling phonebook collation for language %s", language.c_str());
            locale = gen(name + "@collation=phonebook");
        }
        return locale;
    }

    virtual boost::shared_ptr<IndividualCompare> createCompare(const std::string &order)
    {
        boost::shared_ptr<IndividualCompare> res;
        if (order == "first/last") {
            res.reset(new CompareFirstLastBoost(m_locale));
        } else if (order == "last/first") {
            res.reset(new CompareLastFirstBoost(m_locale));
        } else if (order == "fullname") {
            res.reset(new CompareFullnameBoost(m_locale));
        } else {
            SE_THROW("boost locale factory: sort order '" + order + "' not supported");
        }
        return res;
    }

    virtual boost::shared_ptr<IndividualFilter> createFilter(const Filter_t &filter, int level)
    {
        boost::shared_ptr<IndividualFilter> res;

        try {
            const std::vector<Filter_t> &terms = getFilterArray(filter, "array of terms");

            // Only handle arrays where the first entry is a string
            // that we recognize. All other cases are handled by the generic
            // LocaleFactory.
            if (!terms.empty() &&
                boost::get<std::string>(&terms[0])) {
                const std::string &operation = getFilterString(terms[0], "operation name");

                // Pick default operation. Will be replaced with
                // telephone-specific operation once we know that the
                // field is 'phones/value'.
                bool (AnyContainsBoost::*func)(const char *text) const;
                if (operation == "contains") {
                    func = &AnyContainsBoost::containsSearchText;
                } else if (operation == "is") {
                    func = &AnyContainsBoost::isSearchText;
                } else if (operation == "begins-with") {
                    func = &AnyContainsBoost::beginsWithSearchText;
                } else if (operation == "ends-with") {
                    func = &AnyContainsBoost::endsWithSearchText;
                } else {
                    func = NULL;
                }
                if (func) {
                    switch (terms.size()) {
                    case 1:
                        SE_THROW("missing field name and search value");
                        break;
                    case 2:
                        SE_THROW("missing search value");
                        break;
                    }
                    const std::string &field = getFilterString(terms[1], "search field");
                    const std::string &value = getFilterString(terms[2], "search string");
                    if (field == "phones/value") {
                        if (terms.size() > 3) {
                            SE_THROW("Additional entries after 'phones/value' field filter not allowed.");
                        }
                        // Use the telephone specific functions.
                        res.reset(new FilterTel(m_locale, value,
                                                func == &AnyContainsBoost::containsSearchText ? &AnyContainsBoost::containsSearchTel :
                                                func == &AnyContainsBoost::isSearchText ? &AnyContainsBoost::isSearchTel :
                                                func == &AnyContainsBoost::beginsWithSearchText ? &AnyContainsBoost::beginsWithSearchTel :
                                                func == &AnyContainsBoost::endsWithSearchText ? &AnyContainsBoost::endsWithSearchTel :
                                                func));
                    } else {
                        int mode = AnyContainsBoost::getFilterMode(terms, 3);
                        if (field == "full-name") {
                            res.reset(new FilterFullName(m_locale, value, mode, func));
                        } else if (field == "nickname") {
                            res.reset(new FilterNickname(m_locale, value, mode, func));
                        } else if (field == "structured-name/family") {
                            res.reset(new FilterFamilyName(m_locale, value, mode, func));
                        } else if (field == "structured-name/given") {
                            res.reset(new FilterGivenName(m_locale, value, mode, func));
                        } else if (field == "structured-name/additional") {
                            res.reset(new FilterAdditionalName(m_locale, value, mode, func));
                        } else if (field == "emails/value") {
                            res.reset(new FilterEmails(m_locale, value, mode, func));
                        } else if (field == "addresses/po-box") {
                            res.reset(new FilterAddrPOBox(m_locale, value, mode, func));
                        } else if (field == "addresses/extension") {
                            res.reset(new FilterAddrExtension(m_locale, value, mode, func));
                        } else if (field == "addresses/street") {
                            res.reset(new FilterAddrStreet(m_locale, value, mode, func));
                        } else if (field == "addresses/locality") {
                            res.reset(new FilterAddrLocality(m_locale, value, mode, func));
                        } else if (field == "addresses/region") {
                            res.reset(new FilterAddrRegion(m_locale, value, mode, func));
                        } else if (field == "addresses/postal-code") {
                            res.reset(new FilterAddrPostalCode(m_locale, value, mode, func));
                        } else if (field == "addresses/country") {
                            res.reset(new FilterAddrCountry(m_locale, value, mode, func));
                        } else {
                            SE_THROW("Unknown field name: " + field);
                        }
                    }
                } else if (operation == "any-contains") {
                    if (terms.size() < 2) {
                        SE_THROW("missing search value");
                    }
                    const std::string &value = getFilterString(terms[1], "search string");
                    int mode = AnyContainsBoost::getFilterMode(terms, 2);
                    res.reset(new AnyContainsBoost(m_locale, value, mode));
                } else if (operation == "phone") {
                    if (terms.size() != 2) {
                        SE_THROW("'phone' filter needs exactly one parameter.");
                    }
                    const std::string &value = getFilterString(terms[1], "search string");
                    res.reset(new PhoneStartsWith(m_locale, value));
                }
            }
        } catch (const Exception &ex) {
            handleFilterException(filter, level, &ex.m_file, ex.m_line);
        } catch (...) {
            handleFilterException(filter, level, NULL, 0);
        }

        // Let base class handle it if we didn't recognize the operation.
        return res ? res : LocaleFactory::createFilter(filter, level);
    }

    virtual bool precompute(FolksIndividual *individual, Precomputed &precomputed) const
    {
        LocaleFactory::Precomputed old;
        std::swap(old, precomputed);

        FolksPhoneDetails *phoneDetails = FOLKS_PHONE_DETAILS(individual);
        GeeSet *phones = folks_phone_details_get_phone_numbers(phoneDetails);
        precomputed.m_phoneNumbers.reserve(gee_collection_get_size(GEE_COLLECTION(phones)));
        BOOST_FOREACH (FolksAbstractFieldDetails *phone, GeeCollCXX<FolksAbstractFieldDetails *>(phones, ADD_REF)) {
            const gchar *value =
                reinterpret_cast<const gchar *>(folks_abstract_field_details_get_value(phone));
            if (value) {
                if (m_edsSupportsPhoneNumbers) {
                    // Check X-EVOLUTION-E164 (made lowercase by folks!).
                    //
                    // It has the format <local number>,<country code>,
                    // where <country code> happens to be in quotation marks.
                    // This ends up being split into individual values which
                    // are returned in random order by folks (a bug?!).
                    //
                    // Example: TEL;X-EVOLUTION-E164=891234,"+49":+49-89-1234
                    // => value '+49-89-1234', params [ '+49', '891234' ].
                    //
                    // We restore the right order by sorting, which puts the
                    // country code first, and then joining.
                    GeeCollectionCXX coll(folks_abstract_field_details_get_parameter_values(phone, "x-evolution-e164"), TRANSFER_REF);
                    if (coll) {
                        std::vector<std::string> components;
                        components.reserve(2);
                        BOOST_FOREACH (const gchar *component, GeeStringCollection(coll)) {
                            // Empty component represents an unset
                            // country code. Note that it is not
                            // certain whether we get to see the empty
                            // component. At the moment (EDS 3.7,
                            // folks 0.9.1), someone swallows it.
                            components.push_back(component);
                        }
                        if (!components.empty()) {
                            // Only one component? We must still miss the country code.
                            if (components.size() == 1) {
                                components.push_back("");
                            }
                            std::sort(components.begin(), components.end());
                            try {
                                SimpleE164 number;
                                number.m_countryCode = components[0].empty() ?
                                    0 :
                                    boost::lexical_cast<SimpleE164::CountryCode_t>(components[0]);
                                number.m_nationalNumber = components[1].empty() ?
                                    0 :
                                    boost::lexical_cast<SimpleE164::NationalNumber_t>(components[1]);
                                precomputed.m_phoneNumbers.push_back(number);
                            } catch (const boost::bad_lexical_cast &ex) {
                                SE_LOG_WARNING(NULL, "ignoring malformed X-EVOLUTION-E164 (sorted): %s",
                                               boost::join(components, ", ").c_str());
                            }
                        }
                    }
                    // Either EDS had a normalized value or there is none because
                    // the value is not a phone number. No need to try parsing again.
                    continue;
                }

                try {
                    // This fallback for missing X-EVOLUTION-E164 in EDS still relies
                    // on libphonenumber support in libebook, so it does not really help
                    // if EDS was compiled without libphonenumber. It is primarily useful
                    // for testing (see TestContacts.testLocaledPhone).
                    SimpleE164 e164 = String2E164(value, m_country.c_str());
                    if (e164.m_countryCode || e164.m_nationalNumber) {
                        precomputed.m_phoneNumbers.push_back(e164);
                    }
                } catch (const Exception &ex) {
                    // Silently ignore parse errors.
                    SE_LOG_DEBUG(NULL, "ignoring unparsable TEL '%s': %s", value, ex.what());
                }
            }
        }

        // Now check if any phone number changed.
        return old != precomputed;
    }
};

boost::shared_ptr<LocaleFactory> LocaleFactory::createFactory()
{
    return boost::shared_ptr<LocaleFactory>(new LocaleFactoryBoost());
}

SE_END_CXX
