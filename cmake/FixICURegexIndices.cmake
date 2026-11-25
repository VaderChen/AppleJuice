# 在固定 ICU 來源套用已核對的索引修正；客體產碼可另指定輸出，不改動輸入。
file(READ "${ICU_INDEX_SOURCE}" original)
set(source "${original}")
function(replace_checked old new)
  string(FIND "${source}" "${old}" found)
  if(found EQUAL -1)
    string(FIND "${source}" "${new}" applied)
    if(applied EQUAL -1)
      message(FATAL_ERROR "無法辨識固定 ICU 來源中的索引處理")
    endif()
  else()
    string(REPLACE "${old}" "${new}" source "${source}")
    set(source "${source}" PARENT_SCOPE)
  endif()
endfunction()
get_filename_component(unit "${ICU_INDEX_SOURCE}" NAME)
if(unit STREQUAL "uregex.cpp")
  replace_checked("return regexp->fMatcher->regionStart();" "return regexp->fMatcher->regionStart64();")
  replace_checked("return regexp->fMatcher->regionEnd();" "return regexp->fMatcher->regionEnd64();")
  replace_checked("if (destCapacity == 0 || regexp->fText != nullptr) {"
                  "if (regexp->fText != nullptr && UTEXT_USES_U16(regexp->fMatcher->inputText())) {")
  replace_checked("if (regexp->fMatcher->find()) {" [=[UErrorCode findStatus = U_ZERO_ERROR;
        const UBool found = regexp->fMatcher->find(findStatus);
        if (U_FAILURE(findStatus)) {
            *status = findStatus;
            return i;
        }
        if (found) {]=])
elseif(unit STREQUAL "utext.cpp")
  # 原生索引可超出 int32；只有確認位於可直接索引的 UTF-16 前綴後才能縮窄。
  replace_checked("else if((int32_t)(index - ut->chunkNativeStart) <= ut->nativeIndexingLimit)"
                  "else if(index - ut->chunkNativeStart <= ut->nativeIndexingLimit)")
else()
  message(FATAL_ERROR "未支援的 ICU 索引來源")
endif()
if(NOT DEFINED ICU_INDEX_OUTPUT)
  set(ICU_INDEX_OUTPUT "${ICU_INDEX_SOURCE}")
endif()
if(NOT source STREQUAL original OR NOT ICU_INDEX_OUTPUT STREQUAL ICU_INDEX_SOURCE)
  string(RANDOM LENGTH 16 ALPHABET "0123456789abcdef" suffix)
  set(temporary "${ICU_INDEX_OUTPUT}.${suffix}.tmp")
  file(WRITE "${temporary}" "${source}")
  file(RENAME "${temporary}" "${ICU_INDEX_OUTPUT}")
endif()
