#include "input_formats.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace pdfcsv {
namespace fs = std::filesystem;

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return char(std::tolower(c)); });
    return s;
}
static std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    std::string o="\""; for(char c:s) o += (c=='"' ? "\\\"" : std::string(1,c)); return o+"\"";
#else
    std::string o="'"; for(char c:s) o += (c=='\'' ? "'\\''" : std::string(1,c)); return o+"'";
#endif
}
static fs::path temp_path(const std::string& suffix) {
    const auto n=std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return fs::temp_directory_path()/("pdfcsv_"+std::to_string(n)+suffix);
}
static std::string read_file(const fs::path& p) {
    std::ifstream in(p,std::ios::binary); if(!in) throw std::runtime_error("Cannot read: "+p.string());
    std::ostringstream s; s<<in.rdbuf(); return s.str();
}
static std::string run_to_file(const std::string& cmd, const fs::path& out) {
    int rc=std::system(cmd.c_str());
    if(rc!=0) throw std::runtime_error("External converter failed: "+cmd);
    return read_file(out);
}
static std::vector<DocumentUnit> units_from_text(const std::string& text, const std::string& source) {
    std::vector<DocumentUnit> out;
    // Preserve explicit form-feed page boundaries when a converter supplies them.
    size_t start=0; int n=1;
    while(start<=text.size()) {
        size_t end=text.find('\f',start); if(end==std::string::npos) end=text.size();
        std::string part=text.substr(start,end-start);
        out.push_back({n,"Page "+std::to_string(n),part,{},source}); ++n;
        if (end == text.size()) break;
        start = end + 1;
    }
    if(out.empty()) out.push_back({1,"Page 1","",{},source});
    return out;
}

static std::unordered_set<std::string> supported = {
    ".pdf",".epub",".mobi",".azw",".azw3",".docx",".odt",".doc",".rtf",
    ".pptx",".ppt",".odp",".xlsx",".xls",".ods",".csv",".tsv",
    ".md",".markdown",".txt",".text",".html",".htm",".xml",".tex",".json",".yaml",".yml",".log"
};
bool is_supported_input(const fs::path& p) { return supported.count(lower(p.extension().string()))!=0; }

static std::vector<DocumentUnit> extract_with_pandoc(const fs::path& p,const InputConfig& c) {
    fs::path out=temp_path(".txt");
    std::string cmd=shell_quote(c.pandoc)+" -f "+shell_quote(p.extension().string().substr(1))+" -t plain --wrap=none -o "+shell_quote(out.string())+" "+shell_quote(p.string());
    auto text=run_to_file(cmd,out); std::error_code ec; fs::remove(out,ec);
    return units_from_text(text,p.extension().string().substr(1));
}
static std::vector<DocumentUnit> extract_with_libreoffice(const fs::path& p,const InputConfig& c) {
    fs::path dir=temp_path("_lo"); fs::create_directories(dir);
    const auto ext=lower(p.extension().string());
    if(ext==".pptx" || ext==".ppt" || ext==".odp") {
        std::string cmd=shell_quote(c.libreoffice)+" --headless --convert-to pdf --outdir "+shell_quote(dir.string())+" "+shell_quote(p.string())+" >/dev/null 2>&1";
        if(std::system(cmd.c_str())!=0) { fs::remove_all(dir); throw std::runtime_error("LibreOffice presentation-to-PDF conversion failed: "+p.string()); }
        fs::path pdf=dir/(p.stem().string()+".pdf");
        if(!fs::exists(pdf)) { fs::remove_all(dir); throw std::runtime_error("LibreOffice did not create PDF for: "+p.string()); }
        fs::path out=dir/(p.stem().string()+".txt");
        std::string pdftxt=shell_quote(c.pdftotext)+" -layout -enc UTF-8 -q "+shell_quote(pdf.string())+" "+shell_quote(out.string());
        if(std::system(pdftxt.c_str())!=0 || !fs::exists(out)) { fs::remove_all(dir); throw std::runtime_error("pdftotext failed for presentation: "+p.string()); }
        auto text=read_file(out); fs::remove_all(dir); return units_from_text(text,p.extension().string().substr(1));
    }
    std::string cmd=shell_quote(c.libreoffice)+" --headless --convert-to txt:Text --outdir "+shell_quote(dir.string())+" "+shell_quote(p.string())+" >/dev/null 2>&1";
    int rc=std::system(cmd.c_str());
    if(rc!=0) { fs::remove_all(dir); throw std::runtime_error("LibreOffice conversion failed: "+p.string()); }
    fs::path out=dir/(p.stem().string()+".txt");
    if(!fs::exists(out)) { fs::remove_all(dir); throw std::runtime_error("LibreOffice did not create text output for: "+p.string()); }
    auto text=read_file(out); fs::remove_all(dir);
    return units_from_text(text,p.extension().string().substr(1));
}

std::vector<DocumentUnit> extract_document_units(const fs::path& p,const InputConfig& c,bool no_ocr) {
    const auto ext=lower(p.extension().string());
    if(ext==".pdf") {
        // The legacy PDF pipeline remains in main.cpp; this function is not used for PDFs.
        throw std::runtime_error("Internal error: PDF must use the PDF page extractor");
    }
    if(ext==".txt"||ext==".text"||ext==".md"||ext==".markdown"||ext==".csv"||ext==".tsv"||ext==".tex"||ext==".json"||ext==".yaml"||ext==".yml"||ext==".log")
        return units_from_text(read_file(p),ext.substr(1));

    if(ext==".mobi"||ext==".azw"||ext==".azw3") {
        fs::path epub=temp_path(".epub");
        std::string cmd=shell_quote(c.ebook_convert)+" "+shell_quote(p.string())+" "+shell_quote(epub.string());
        int rc=std::system(cmd.c_str());
        if(rc!=0 || !fs::exists(epub)) { std::error_code ec; fs::remove(epub,ec); throw std::runtime_error("MOBI/AZW conversion requires ebook-convert (Calibre): "+p.string()); }
        fs::path txt=temp_path(".txt");
        cmd=shell_quote(c.pandoc)+" -f epub -t plain --wrap=none -o "+shell_quote(txt.string())+" "+shell_quote(epub.string());
        auto text=run_to_file(cmd,txt); std::error_code ec; fs::remove(epub,ec); fs::remove(txt,ec);
        return units_from_text(text,"mobi/azw");
    }

    if(ext==".epub") return extract_with_pandoc(p,c);
    if(ext==".docx"||ext==".odt"||ext==".doc"||ext==".rtf"||ext==".pptx"||ext==".ppt"||ext==".odp") {
        try { return extract_with_pandoc(p,c); }
        catch(...) { return extract_with_libreoffice(p,c); }
    }
    if(ext==".xlsx"||ext==".xls"||ext==".ods") return extract_with_libreoffice(p,c);
    if(ext==".html"||ext==".htm"||ext==".xml") {
        try { return extract_with_pandoc(p,c); }
        catch(...) { return units_from_text(read_file(p),ext.substr(1)); }
    }
    (void)no_ocr;
    throw std::runtime_error("Unsupported input type: "+p.string());
}
} // namespace pdfcsv
