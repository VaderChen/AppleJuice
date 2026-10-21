#pragma once
#include <string_view>
#include <utility>
namespace x64arm::ldap_api {
inline std::string_view canonical(std::string_view name){
    static constexpr std::pair<std::string_view,std::string_view> ordinals[]{
        {"#143","ldap_initA"},{"#200","ldap_memfreeA"},{"#211","ldap_set_optionA"},{"#22","ldap_err2stringA"},
        {"#26","ldap_first_entry"},{"#27","ldap_next_entry"},{"#30","ldap_get_dnA"},{"#301","ber_free"},
        {"#32","ldap_first_attributeA"},{"#33","ldap_next_attributeA"},{"#35","ldap_get_values_lenA"},
        {"#41","ldap_msgfree"},{"#46","ldap_unbind_s"},{"#50","ldap_search_sA"},{"#60","ldap_simple_bind_sA"},{"#79","ldap_value_free_len"}};
    for(const auto& [ordinal,symbol]:ordinals)if(name==ordinal){name=symbol;break;}
    return name;
}
inline bool uses_heap(std::string_view input){
    const auto name=canonical(input);
    return name!="LdapGetLastError" && !name.starts_with("ldap_get_option") && !name.starts_with("ldap_set_option") && !name.starts_with("ldap_simple_bind") && !name.starts_with("ldap_count_");
}
}
