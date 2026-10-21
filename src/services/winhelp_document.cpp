#include "winhelp_document.hpp"
#include <stdexcept>
namespace x64arm::winhelp {
Document::Document(const File& file):system_(file.system),fonts_(winhelp::fonts(file)),index_(topic_index(file)),keywords_(keyword_index(file)){
    const auto records=topic_records(file);const auto phrases=phrase_table(file);addresses_=topic_addresses(file,records);
    for(std::size_t i=0;i<records.size();++i){const auto& record=records[i];
        if(record.kind==2){pages_.push_back({i,topic_header(phrases,record),{}});records_.emplace(i,DocumentLocation{pages_.size()-1,0,0,true});}
        else{
            if(pages_.empty())throw std::invalid_argument("WinHelp 正文缺少主題標頭");
            auto layout=text_layout(phrases,record);
            for(const auto& paragraph:layout.paragraphs)for(const auto& run:paragraph.runs)if(run.opcode==0x80 && run.argument>=fonts_.size())throw std::invalid_argument("WinHelp 字型索引越界");
            auto& page=pages_.back();records_.emplace(i,DocumentLocation{pages_.size()-1,page.parts.size(),0,false});page.parts.push_back({i,addresses_[i].offset,std::move(layout)});
        }
    }
}
std::optional<DocumentLocation> Document::locate(std::uint32_t offset)const{
    const auto address=locate_topic(addresses_,offset);if(!address)return std::nullopt;
    const auto found=records_.find(address->record);if(found==records_.end())return std::nullopt;auto result=found->second;result.character=address->character;return result;
}
std::optional<DocumentLocation> Document::context(std::uint32_t id)const{
    const auto found=index_.context_ids.find(id);return found==index_.context_ids.end()?std::nullopt:locate(found->second);
}
std::optional<DocumentLocation> Document::named_context(std::string_view name)const{
    return hashed_context(context_hash(name));
}
std::optional<DocumentLocation> Document::hashed_context(std::uint32_t hash)const{
    const auto found=index_.context_hashes.find(hash);return found==index_.context_hashes.end()?std::nullopt:locate(found->second);
}
std::optional<DocumentLocation> Document::contents()const{
    if(system_.contents)return locate(*system_.contents);
    if(pages_.empty())return std::nullopt;
    return DocumentLocation{0,0,0,true};
}
}
