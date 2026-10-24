#include "shinkou/editor/EditorTextPreview.h"
#include <miniz.h>
#include <pugixml.hpp>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace shinkou::editor {
namespace {
constexpr std::size_t MaxText = 1024u * 1024u, MaxLines = 10000;
void utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xc0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xe0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
    else { out += static_cast<char>(0xf0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
}
bool decode(std::string& bytes, std::string& encoding, bool truncated, std::string& error) {
    if (bytes.size() >= 2 && ((static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe) ||
        (static_cast<unsigned char>(bytes[0]) == 0xfe && static_cast<unsigned char>(bytes[1]) == 0xff))) {
        const bool le = static_cast<unsigned char>(bytes[0]) == 0xff;
        encoding = le ? "UTF-16 LE" : "UTF-16 BE";
        std::string out;
        const auto unit = [&](std::size_t i) { const auto a=static_cast<unsigned char>(bytes[i]), b=static_cast<unsigned char>(bytes[i+1]); return std::uint32_t(le ? a|(b<<8) : (a<<8)|b); };
        for (std::size_t i=2; i+1<bytes.size(); i+=2) {
            auto cp=unit(i);
            if (cp>=0xd800 && cp<=0xdbff) {
                if (i+3>=bytes.size() && truncated) break;
                if (i+3>=bytes.size() || unit(i+2)<0xdc00 || unit(i+2)>0xdfff) { error="Invalid UTF-16 surrogate"; return false; }
                cp=0x10000+((cp-0xd800)<<10)+(unit(i+2)-0xdc00); i+=2;
            } else if (cp>=0xdc00 && cp<=0xdfff) { error="Invalid UTF-16 surrogate"; return false; }
            if (!cp) { error="Binary data is not a text document"; return false; }
            utf8(out,cp);
        }
        if (bytes.size()%2 && !truncated) { error="Incomplete UTF-16 code unit"; return false; }
        bytes=std::move(out); return true;
    }
    encoding="UTF-8";
    if (bytes.compare(0,3,"\xef\xbb\xbf")==0) { bytes.erase(0,3); encoding="UTF-8 BOM"; }
    for (std::size_t i=0;i<bytes.size();) {
        const auto c=static_cast<unsigned char>(bytes[i]);
        if (!c) { error="Binary data is not a text document"; return false; }
        if (c<0x80) { ++i; continue; }
        const int count=c>=0xc2 && c<=0xdf?2:c>=0xe0 && c<=0xef?3:c>=0xf0 && c<=0xf4?4:0;
        if (!count) { error="Text encoding is unsupported; save as UTF-8 or UTF-16 with BOM"; return false; }
        if (i+count>bytes.size()) { if(truncated) { bytes.resize(i); break; } error="Incomplete UTF-8 sequence"; return false; }
        std::uint32_t cp=c & (0x7f >> count);
        for(int j=1;j<count;++j) { const auto v=static_cast<unsigned char>(bytes[i+j]); if((v&0xc0)!=0x80) { error="Invalid UTF-8 sequence"; return false; } cp=(cp<<6)|(v&63); }
        if ((count==2 && cp<0x80)||(count==3 && cp<0x800)||(count==4 && cp<0x10000)||cp>0x10ffff||(cp>=0xd800 && cp<=0xdfff)) { error="Invalid UTF-8 code point"; return false; }
        i+=count;
    }
    return true;
}
std::string_view local(const char* name) { std::string_view s(name); const auto p=s.find(':'); return p==s.npos?s:s.substr(p+1); }
std::string attribute(pugi::xml_node node, std::string_view name) {
    for(auto a:node.attributes()) if(local(a.name())==name) return a.value();
    return {};
}
// Word text lives in runs/hyperlinks; namespace prefixes are not fixed by OOXML.
bool word_text(pugi::xml_node node, std::string& text, int depth=0) {
    if(depth>128) return false;
    for(auto n:node.children()) {
        const auto tag=local(n.name());
        if(tag=="t") text+=n.child_value();
        else if(tag=="tab") text+='\t';
        else if(tag=="br" || tag=="cr") text+='\n';
        else if(tag!="del" && !word_text(n,text,depth+1)) return false;
        if(text.size()>MaxText) return false;
    }
    return true;
}
struct Zip { mz_zip_archive value{}; ~Zip() { if(value.m_pState) mz_zip_reader_end(&value); } };
}
EditorTextPreviewResult load_editor_text_preview(const FileSystemService& files, std::string_view path,
    std::uint64_t generation, std::uint64_t stamp) {
    EditorTextPreviewResult result; result.path=path; result.generation=generation; result.sourceStamp=stamp;
    auto next=std::make_shared<EditorTextPreviewSnapshot>();
    auto ext=std::filesystem::u8path(path).extension().string();
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    std::string bytes;
    if(ext==".docx" || ext==".zip") {
        bool truncated=false;
        if(!files.read_text_limited(std::filesystem::u8path(path),64u*1024u*1024u,bytes,&truncated,&result.error)) return result;
        if(truncated) {result.error="Package exceeds the 64 MiB preview limit"; return result;}
        Zip zip;
        if(!mz_zip_reader_init_mem(&zip.value,bytes.data(),bytes.size(),0)) {result.error="Invalid or encrypted ZIP package";return result;}
        if(ext==".zip") {
            next->format="ZIP directory";next->encoding="UTF-8";
            const auto count=mz_zip_reader_get_num_files(&zip.value);
            for(mz_uint i=0;i<count && i<MaxLines;++i) {
                mz_zip_archive_file_stat info{};
                if(!mz_zip_reader_file_stat(&zip.value,i,&info)) {result.error="Invalid ZIP directory";return result;}
                next->lines.push_back(std::string(info.m_filename)+"  ("+std::to_string(info.m_uncomp_size)+" bytes)");
            }
            next->truncated=count>MaxLines;
        } else {
            const int index=mz_zip_reader_locate_file(&zip.value,"word/document.xml",nullptr,0);
            mz_zip_archive_file_stat info{};
            if(index<0 || !mz_zip_reader_file_stat(&zip.value,index,&info) || info.m_is_encrypted || info.m_uncomp_size>8u*1024u*1024u) {result.error="Missing, encrypted or oversized Word document.xml";return result;}
            std::string xml(static_cast<std::size_t>(info.m_uncomp_size),'\0');
            if(!mz_zip_reader_extract_to_mem(&zip.value,index,xml.data(),xml.size(),0)) {result.error="Word XML decompression failed";return result;}
            pugi::xml_document doc;
            const auto parsed=doc.load_buffer(xml.data(),xml.size());
            if(!parsed) {result.error=std::string("Invalid Word XML: ")+parsed.description();return result;}
            auto root=doc.document_element(); pugi::xml_node body;
            if(local(root.name())!="document") {result.error="Not a Word document";return result;}
            for(auto n:root.children()) if(local(n.name())=="body") body=n;
            if(!body) {result.error="Word body is missing";return result;}
            std::size_t total=0;
            const auto append=[&](std::string line,std::uint8_t style) {
                if(total+line.size()>MaxText || next->lines.size()>=MaxLines) {next->truncated=true;return false;}
                total+=line.size(); next->lines.push_back(std::move(line));next->styles.push_back(style);return true;
            };
            for(auto n:body.children()) {
                if(local(n.name())=="p") {
                    std::string text; if(!word_text(n,text)) {result.error="Word paragraph exceeds preview limits";return result;}
                    std::uint8_t style=0;
                    for(auto prop:n.children()) if(local(prop.name())=="pPr") for(auto p:prop.children()) if(local(p.name())=="pStyle") {
                        auto value=attribute(p,"val"); if(value.size()==8 && value.substr(0,7)=="Heading" && value[7]>='1' && value[7]<='9') style=static_cast<std::uint8_t>(value[7]-'0');
                    }
                    if(!append(std::move(text),style)) break;
                } else if(local(n.name())=="tbl") {
                    for(auto row:n.children()) if(local(row.name())=="tr") {
                        std::string text;
                        for(auto cell:row.children()) if(local(cell.name())=="tc") {
                            if(!text.empty()) text+=" | ";
                            if(!word_text(cell,text)) {result.error="Word table exceeds preview limits";return result;}
                        }
                        if(!append(std::move(text),10)) break;
                    }
                }
                if(next->truncated) break;
            }
            next->format="DOCX paragraphs and tables";next->encoding="OOXML / UTF-8";
        }
    } else {
        if(!files.read_text_limited(std::filesystem::u8path(path),MaxText,bytes,&next->truncated,&result.error)) return result;
        if(!decode(bytes,next->encoding,next->truncated,result.error)) return result;
        next->format=ext.empty()?"Text":ext.substr(1);
        std::istringstream stream(bytes); std::string line;
        while(std::getline(stream,line)) {
            if(next->lines.size()>=MaxLines) {next->truncated=true;break;}
            if(!line.empty() && line.back()=='\r') line.pop_back();
            next->lines.push_back(std::move(line));
        }
    }
    if(next->lines.empty()) next->lines.emplace_back();
    result.snapshot=std::move(next);return result;
}
}
