#include "terminology.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
namespace pdfcsv {
static std::string norm(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));});return s;}
bool TerminologyStore::load(const std::string&p){std::ifstream in(p);if(!in)return false;std::string l;while(std::getline(in,l)){if(l.empty()||l[0]=='#')continue;std::stringstream s(l);std::string c,cat,a;std::getline(s,c,'\t');std::getline(s,cat,'\t');std::getline(s,a,'\t');if(c.empty())continue;std::vector<std::string> as;std::stringstream x(a);for(std::string v;std::getline(x,v,'|');)if(!v.empty())as.push_back(v);add(c,cat,as);}return true;}
bool TerminologyStore::save(const std::string&p)const{std::ofstream o(p);if(!o)return false;o<<"# canonical<TAB>category<TAB>alias|alias|...\n";for(const auto&[k,e]:entries_){o<<e.canonical<<'\t'<<e.category<<'\t';for(size_t i=0;i<e.aliases.size();++i){if(i)o<<'|';o<<e.aliases[i];}o<<'\n';}return true;}
void TerminologyStore::add(std::string c,std::string cat,std::vector<std::string>a){entries_[norm(c)]={c,cat,a};for(auto&v:a)aliases_[norm(v)]=c;}
const TermEntry* TerminologyStore::lookup(const std::string&t)const{auto i=entries_.find(norm(t));if(i!=entries_.end())return &i->second;auto a=aliases_.find(norm(t));if(a==aliases_.end())return nullptr;auto e=entries_.find(norm(a->second));return e==entries_.end()?nullptr:&e->second;}
std::string TerminologyStore::canonicalize(const std::string&t)const{auto e=lookup(t);return e?e->canonical:t;}
}
