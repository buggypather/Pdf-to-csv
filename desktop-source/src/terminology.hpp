#pragma once
#include <string>
#include <unordered_map>
#include <vector>
namespace pdfcsv {
struct TermEntry { std::string canonical, category; std::vector<std::string> aliases; };
class TerminologyStore {
public: bool load(const std::string&); bool save(const std::string&) const;
void add(std::string,std::string,std::vector<std::string> aliases = {});
const TermEntry* lookup(const std::string&) const; std::string canonicalize(const std::string&) const;
private: std::unordered_map<std::string,TermEntry> entries_; std::unordered_map<std::string,std::string> aliases_;
};
}
