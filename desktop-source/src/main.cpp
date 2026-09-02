/*
    pdfcsv - PDF knowledge extraction to CSV
    C++20, Linux + Windows

    PDF extraction is delegated to the pdftotext utility from Poppler.
    This keeps the application portable and avoids binding pdfcsv to a
    particular PDF rendering ABI.

    Linux:
        sudo apt install poppler-utils

    Windows:
        Install a Poppler distribution and make pdftotext.exe available
        on PATH, or use --pdftotext "C:/path/to/pdftotext.exe".
*/

#include "anki_apkg.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "input_formats.hpp"


// ===== pdfcsv-0.4.2 EXTRACTION PRIORITY SWITCHES =====
// Italic text is NOT a priority signal by default.
// To make italicized PDF text receive higher extraction priority,
// change the following value to true and rebuild.
//
// This switch is intentionally located here so users can find it easily.
constexpr bool ENABLE_ITALIC_PRIORITY = false;

// When enabled, italic terms receive the same priority treatment as
// other explicitly formatted key terms. Italic text is still subject
// to the normal section exclusions and deduplication rules.
// ============================================================


// ===== pdfcsv-0.4.2 SEMANTIC FOUNDATION =====
static std::string lower(std::string s);
static std::string normalize_space(std::string s);
// Deterministic semantic layer; no AI model required.
// Evidence is accumulated from document structure and linguistic patterns.
// This record layer is designed for future embeddings/local-LLM adapters.
enum class SemanticCategory {
    Definition, Concept, Process, Method, Theory, Principle, Fact,
    Formula, Algorithm, Example, Warning, CauseEffect, Comparison,
    Application, Statistic, Information
};

static const char* category_name(SemanticCategory c) {
    switch (c) {
        case SemanticCategory::Definition: return "DEFINITION";
        case SemanticCategory::Concept: return "CONCEPT";
        case SemanticCategory::Process: return "PROCESS";
        case SemanticCategory::Method: return "METHOD";
        case SemanticCategory::Theory: return "THEORY";
        case SemanticCategory::Principle: return "PRINCIPLE";
        case SemanticCategory::Fact: return "FACT";
        case SemanticCategory::Formula: return "FORMULA";
        case SemanticCategory::Algorithm: return "ALGORITHM";
        case SemanticCategory::Example: return "EXAMPLE";
        case SemanticCategory::Warning: return "WARNING";
        case SemanticCategory::CauseEffect: return "CAUSE_EFFECT";
        case SemanticCategory::Comparison: return "COMPARISON";
        case SemanticCategory::Application: return "APPLICATION";
        case SemanticCategory::Statistic: return "STATISTIC";
        default: return "INFORMATION";
    }
}

static SemanticCategory classify_semantically(const std::string& sentence) {
    const std::string t = lower(sentence);
    if (std::regex_search(t, std::regex(R"(\b(is|are|means|refers to|defined as|denotes)\b)")))
        return SemanticCategory::Definition;
    if (std::regex_search(t, std::regex(R"(\b(step|steps|process|procedure|stages?|first|then|finally)\b)")))
        return SemanticCategory::Process;
    if (std::regex_search(t, std::regex(R"(\b(algorithm|pseudocode|sorts?|searches?)\b)")))
        return SemanticCategory::Algorithm;
    if (std::regex_search(t, std::regex(R"(\b(theorem|theory|hypothesis|model)\b)")))
        return SemanticCategory::Theory;
    if (std::regex_search(t, std::regex(R"(\b(principle|law|rule|axiom)\b)")))
        return SemanticCategory::Principle;
    if (std::regex_search(t, std::regex(R"(\b(example|for instance|such as)\b)")))
        return SemanticCategory::Example;
    if (std::regex_search(t, std::regex(R"(\b(warning|caution|danger|avoid|important)\b)")))
        return SemanticCategory::Warning;
    if (std::regex_search(t, std::regex(R"(\b(causes?|because|therefore|results? in|leads? to)\b)")))
        return SemanticCategory::CauseEffect;
    if (std::regex_search(t, std::regex(R"(\b(versus|vs\.?|compared with|different from|whereas)\b)")))
        return SemanticCategory::Comparison;
    if (std::regex_search(t, std::regex(R"(\b(used for|used to|application|applied to)\b)")))
        return SemanticCategory::Application;
    if (std::regex_search(t, std::regex(R"(\b\d+(?:\.\d+)?\s*(%|percent|million|billion|thousand)\b)")))
        return SemanticCategory::Statistic;
    return SemanticCategory::Information;
}

static double semantic_confidence(SemanticCategory category,
                                   bool definition,
                                   bool bold,
                                   bool italic,
                                   bool quoted,
                                   bool heading_like) {
    double score = 0.35;
    if (definition) score += 0.25;
    if (bold) score += 0.22;
    if (ENABLE_ITALIC_PRIORITY && italic) score += 0.16;
    if (quoted) score += 0.08;
    if (heading_like) score += 0.10;
    if (category == SemanticCategory::Definition) score += 0.08;
    return std::min(0.99, score);
}

static std::string semantic_evidence(bool definition, bool bold, bool italic,
                                     bool quoted, bool heading_like) {
    std::string e;
    auto add = [&](const char* x) {
        if (!e.empty()) e += ';';
        e += x;
    };
    if (definition) add("definition-pattern");
    if (bold) add("bold-term");
    if (ENABLE_ITALIC_PRIORITY && italic) add("italic-term");
    if (quoted) add("quoted-term");
    if (heading_like) add("heading-like");
    return e;
}

// Conservative relationship hooks for future knowledge-graph/embedding layers.
[[maybe_unused]] static std::vector<std::string> relationship_hints(const std::string& sentence) {
    std::vector<std::string> r;
    const std::string t = lower(sentence);
    if (std::regex_search(t, std::regex(R"(\b(is a type of|is an example of|is a kind of)\b)")))
        r.push_back("is-a");
    if (std::regex_search(t, std::regex(R"(\b(consists of|contains|includes)\b)")))
        r.push_back("contains");
    if (std::regex_search(t, std::regex(R"(\b(causes?|leads? to|results? in)\b)")))
        r.push_back("causes");
    if (std::regex_search(t, std::regex(R"(\b(used for|used to|applied to)\b)")))
        r.push_back("used-for");
    return r;
}

// ===== END 0.4.0 SEMANTIC FOUNDATION =====


// ===== pdfcsv 0.4.1 CONTEXT & DISCOURSE ENGINE =====
// Context-aware semantic extraction:
// - resolves conservative references such as "this process", "this method",
//   "this approach", "it", "they", "such techniques", etc.
// - recognizes discourse markers such as "for example", "for instance",
//   "in order to", "so that", "however", "therefore", and causal phrases.
// - evaluates a local context window around each candidate.
// This layer is deterministic and explainable; optional embeddings/LLMs can
// be added later without replacing the document parser.
// ================================================================

enum class SemanticRelation {
    Defines, Elaboration, ExampleOf, Purpose, Cause, Effect,
    Comparison, Contrast, Application, Qualification, Continuation, None
};

static const char* relation_name(SemanticRelation r) {
    switch (r) {
        case SemanticRelation::Defines: return "defines";
        case SemanticRelation::Elaboration: return "elaboration";
        case SemanticRelation::ExampleOf: return "example-of";
        case SemanticRelation::Purpose: return "purpose";
        case SemanticRelation::Cause: return "causes";
        case SemanticRelation::Effect: return "effect";
        case SemanticRelation::Comparison: return "comparison";
        case SemanticRelation::Contrast: return "contrast";
        case SemanticRelation::Application: return "application";
        case SemanticRelation::Qualification: return "qualification";
        case SemanticRelation::Continuation: return "continuation";
        default: return "";
    }
}

static bool has_phrase(const std::string& s, const std::string& p) {
    return lower(s).find(lower(p)) != std::string::npos;
}

static SemanticRelation discourse_relation(const std::string& sentence) {
    const std::string t = lower(sentence);

    if (std::regex_search(t, std::regex(R"(\b(for example|for instance|such as|e\.g\.)\b)")))
        return SemanticRelation::ExampleOf;

    if (std::regex_search(t, std::regex(R"(\b(in order to|so that|so as to|for the purpose of|with the goal of|designed to|intended to)\b)")))
        return SemanticRelation::Purpose;

    if (std::regex_search(t, std::regex(R"(\b(because|due to|owing to|causes?|results? in|leads? to)\b)")))
        return SemanticRelation::Cause;

    if (std::regex_search(t, std::regex(R"(\b(therefore|thus|consequently|as a result)\b)")))
        return SemanticRelation::Effect;

    if (std::regex_search(t, std::regex(R"(\b(however|although|whereas|in contrast|on the other hand|unlike)\b)")))
        return SemanticRelation::Contrast;

    if (std::regex_search(t, std::regex(R"(\b(compared with|in comparison|versus|vs\.?)\b)")))
        return SemanticRelation::Comparison;

    if (std::regex_search(t, std::regex(R"(\b(used for|used to|applied to)\b)")))
        return SemanticRelation::Application;

    if (std::regex_search(t, std::regex(R"(\b(although|except|unless|only when|provided that)\b)")))
        return SemanticRelation::Qualification;

    return SemanticRelation::None;
}

static bool is_context_reference(const std::string& sentence) {
    const std::string t = lower(sentence);
    return std::regex_search(t, std::regex(
        R"(\b(this|these|such|it|they|them|the former|the latter)\b)"
        R"(|\b(this|these)\s+(process|method|approach|technique|theory|principle|phenomenon|system|concept)\b)"));
}

static std::string reference_phrase(const std::string& sentence) {
    const std::string t = lower(sentence);
    const std::vector<std::string> phrases = {
        "this process", "this method", "this approach", "this technique",
        "this theory", "this principle", "this phenomenon", "this system",
        "this concept", "these processes", "these methods", "such techniques",
        "the former", "the latter", "it", "they", "them"
    };
    for (const auto& p : phrases)
        if (t.find(p) != std::string::npos) return p;
    return {};
}

struct ContextWindow {
    std::string previous;
    std::string current;
    std::string next;
};

static ContextWindow make_context_window(const std::vector<std::string>& sentences,
                                          size_t index) {
    ContextWindow w;
    if (index > 0) w.previous = sentences[index - 1];
    w.current = sentences[index];
    if (index + 1 < sentences.size()) w.next = sentences[index + 1];
    return w;
}

// Conservative antecedent selection: prefer the nearest previously identified
// term in the same paragraph/context window. Do not resolve ambiguous "it".
static std::string resolve_reference(const ContextWindow& w,
                                     const std::vector<std::string>& recent_terms) {
    if (!is_context_reference(w.current) || recent_terms.empty()) return {};

    const std::string ref = reference_phrase(w.current);
    if (ref == "it" || ref == "they" || ref == "them" ||
        ref == "the former" || ref == "the latter") {
        // Only resolve simple pronouns when exactly one recent term is available.
        if (recent_terms.size() == 1) return recent_terms.front();
        return {};
    }
    return recent_terms.back();
}

static double context_bonus(SemanticRelation relation, bool resolved_reference) {
    double bonus = 0.0;
    if (relation != SemanticRelation::None) bonus += 0.08;
    if (resolved_reference) bonus += 0.10;
    return bonus;
}

// ===== END 0.4.1 CONTEXT & DISCOURSE ENGINE =====


// ===== pdfcsv 0.4.2 DEFINITION CONSOLIDATOR =====
// Multi-sentence definition consolidation, enabled with -cl.
// It attaches nearby explanatory/example/purpose sentences when context
// strongly indicates that they belong to the same extracted concept.
static bool consolidation_heading_like(const std::string& line) {
    const std::string t = normalize_space(line);
    if (t.empty() || t.size() > 140) return false;
    return std::regex_search(t, std::regex(R"(^((chapter|part|section)\s+)?\d+(\.\d+)*[\.)]?\s+.+$)", std::regex::icase));
}

static bool consolidation_contains_word(const std::string& text, const std::string& word) {
    const std::string t = lower(text), w = lower(word);
    if (w.empty()) return false;
    std::string::size_type pos = t.find(w);
    while (pos != std::string::npos) {
        const bool left = pos == 0 || !std::isalnum(static_cast<unsigned char>(t[pos-1]));
        const auto end = pos + w.size();
        const bool right = end >= t.size() || !std::isalnum(static_cast<unsigned char>(t[end]));
        if (left && right) return true;
        pos = t.find(w, pos + 1);
    }
    return false;
}

struct ConsolidationOptions {
    bool enabled = false;
    unsigned max_following = 4;
    double threshold = 0.45;
};

static bool consolidation_boundary(const std::string& sentence) {
    if (consolidation_heading_like(sentence)) return true;
    const std::string t = lower(sentence);
    return std::regex_search(t, std::regex(
        R"(\b(is defined as|are defined as|refers to|means|is a|is an)\b)"));
}

static double consolidation_score(const std::string& key,
                                  const std::string& sentence,
                                  const std::string& previous) {
    double score = 0.0;
    const std::string t = lower(sentence);

    if (is_context_reference(sentence)) score += 0.42;
    if (!key.empty() && consolidation_contains_word(sentence, key)) score += 0.30;

    if (std::regex_search(t, std::regex(
        R"(\b(furthermore|additionally|moreover|in particular|specifically)\b)")))
        score += 0.18;

    const auto rel = discourse_relation(sentence);
    if (rel == SemanticRelation::ExampleOf ||
        rel == SemanticRelation::Purpose ||
        rel == SemanticRelation::Application)
        score += 0.22;

    if (!previous.empty() && is_context_reference(sentence))
        score += 0.08;

    if (sentence.size() >= 40 && sentence.size() <= 500)
        score += 0.05;

    return std::min(1.0, score);
}

static std::string join_consolidated(
    const std::vector<std::string>& sentences) {
    std::string out;
    for (const auto& sentence : sentences) {
        if (sentence.empty()) continue;
        if (!out.empty()) out += " ";
        out += normalize_space(sentence);
    }
    return out;
}

static std::pair<std::string, size_t> consolidate_definition(
    const std::string& key,
    const std::vector<std::string>& sentences,
    size_t start,
    const ConsolidationOptions& options) {

    if (!options.enabled || start >= sentences.size())
        return {sentences[start], 1};

    std::vector<std::string> group{sentences[start]};
    size_t consumed = 1;
    std::string previous = sentences[start];

    for (size_t i = start + 1;
         i < sentences.size() && consumed <= options.max_following; ++i) {
        const std::string& candidate = sentences[i];
        if (consolidation_boundary(candidate)) break;

        if (consolidation_score(key, candidate, previous) <
            options.threshold)
            break;

        group.push_back(candidate);
        previous = candidate;
        ++consumed;
    }

    return {join_consolidated(group), consumed};
}

// ===== END 0.4.2 DEFINITION CONSOLIDATOR =====

namespace fs = std::filesystem;
using pdfcsv::InputConfig;
using pdfcsv::extract_document_units;
using pdfcsv::is_supported_input;

struct Config {
    std::vector<fs::path> inputs;
    fs::path output_dir = ".";
    std::string pdftotext = "pdftotext";
    std::string pdftohtml = "pdftohtml";
    std::string pdftoppm = "pdftoppm";
    std::string tesseract = "tesseract";
    std::string pandoc = "pandoc";
    std::string libreoffice = "libreoffice";
    std::string ebook_convert = "ebook-convert";
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    bool overwrite = false;
    bool keep_empty = false;
    bool no_ocr = false;
    bool consolidate = true;
    bool anki_only = false;
    bool csv_and_anki = false;
    bool extract_images = true;
    bool spreadsheet_only = false;
    std::string output_name;
    std::string convert_to;
    bool timing = false;
    bool benchmark = false;
};

struct Page {
    int number = 0;
    std::string text;
    std::vector<std::string> bold_terms;
    bool ocr = false;
    std::vector<fs::path> images;
    std::vector<std::string> image_original_names;
    std::vector<int> image_global_pages;
    std::string table_text;
};

struct Section {
    std::string title = "Body";
    int start_page = 1;
    bool excluded = false;
};

struct Row {
    std::string key;
    std::string meat;
    int page = 0;
    std::string section;
    std::string category;
    std::string tags;
    double confidence = 0.0;
    std::string source;
    std::string evidence;
    std::vector<fs::path> images;
    std::vector<std::string> image_original_names;
    std::vector<int> image_global_pages;
    std::string table_text;
};


// 0.3.1 formatting-priority hook.
// The PDF XML/style extraction layer should pass whether a candidate is
// bold or italic to this function. Italic priority remains opt-in.
[[maybe_unused]] static double formatted_term_priority(bool is_bold, bool is_italic) {
    double score = 0.0;
    if (is_bold) score += 1.0;
    if (ENABLE_ITALIC_PRIORITY && is_italic) score += 1.0;
    return score;
}

static std::mutex cout_mutex;

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// Return guaranteed UTF-8. Invalid input bytes are replaced with U+FFFD.
// This is deliberately applied at every external-text/output boundary so a
// malformed PDF text layer can never poison CSV or Anki note fields.
static std::string utf8_sanitize(const std::string& in) {
    std::string out; out.reserve(in.size());
    const auto* b = reinterpret_cast<const unsigned char*>(in.data());
    size_t i = 0;
    auto replacement = [&](){ out += "\xEF\xBF\xBD"; };
    while (i < in.size()) {
        unsigned char c = b[i];
        if (c <= 0x7F) { out.push_back(static_cast<char>(c)); ++i; continue; }
        size_t n = 0; unsigned int cp = 0;
        if ((c & 0xE0) == 0xC0) { n=2; cp=c&0x1F; }
        else if ((c & 0xF0) == 0xE0) { n=3; cp=c&0x0F; }
        else if ((c & 0xF8) == 0xF0) { n=4; cp=c&0x07; }
        else { replacement(); ++i; continue; }
        if (i+n > in.size()) { replacement(); ++i; continue; }
        bool ok=true;
        for (size_t j=1;j<n;++j) { if ((b[i+j]&0xC0)!=0x80) {ok=false;break;} cp=(cp<<6)|(b[i+j]&0x3F); }
        if (!ok || (n==2 && cp<0x80) || (n==3 && cp<0x800) || (n==4 && cp<0x10000) ||
            cp>0x10FFFF || (cp>=0xD800 && cp<=0xDFFF)) { replacement(); ++i; continue; }
        out.append(in, i, n); i += n;
    }
    return out;
}

static std::string normalize_space(std::string s) {
    s = utf8_sanitize(s);
    std::string out;
    bool ws = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!ws) out.push_back(' ');
            ws = true;
        } else {
            out.push_back(static_cast<char>(c));
            ws = false;
        }
    }
    return trim(out);
}

static std::string csv_escape(const std::string& s) {
    const std::string clean = utf8_sanitize(s);
    std::string out = "\"";
    for (char c : clean) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += '"';
    return out;
}

static std::string stem_safe(const fs::path& p) {
    std::string s = p.stem().string();
    for (char& c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            continue;
        c = '_';
    }
    return s.empty() ? "output" : s;
}

static bool is_heading_like(const std::string& line) {
    const std::string t = trim(line);
    if (t.empty() || t.size() > 140) return false;

    // Numbered headings: 1 Introduction, 2.1 Methods, Chapter 3, etc.
    static const std::regex numbered(
        R"(^((chapter|part|section)\s+)?(\d+(\.\d+)*[\.)]?\s+).*$)",
        std::regex::icase);
    if (std::regex_match(t, numbered)) return true;

    // Short title-like lines.
    size_t words = 0;
    bool in_word = false;
    for (unsigned char c : t) {
        if (std::isspace(c)) {
            if (in_word) { ++words; in_word = false; }
        } else in_word = true;
    }
    if (in_word) ++words;

    if (words <= 12 && t.back() != '.' && t.back() != ',' &&
        t.back() != ';' && t.back() != '?') {
        size_t alpha = 0, upper = 0;
        for (unsigned char c : t) {
            if (std::isalpha(c)) {
                ++alpha;
                if (std::isupper(c)) ++upper;
            }
        }
        if (alpha > 0 && upper * 2 >= alpha) return true;
    }
    return false;
}

static bool looks_like_excluded_heading(const std::string& line) {
    std::string s = lower(normalize_space(line));

    static const std::vector<std::regex> patterns = {
        std::regex(R"(^table\s+of\s+contents\b)", std::regex::icase),
        std::regex(R"(^contents\b)", std::regex::icase),
        std::regex(R"(^preface\b)", std::regex::icase),
        std::regex(R"(^foreword\b)", std::regex::icase),
        std::regex(R"(^acknowledg(e)?ments?\b)", std::regex::icase),
        std::regex(R"(^about\s+the\s+author\b)", std::regex::icase),
        std::regex(R"(^about\s+the\s+authors?\b)", std::regex::icase),
        std::regex(R"(^references?\b)", std::regex::icase),
        std::regex(R"(^bibliography\b)", std::regex::icase),
        std::regex(R"(^works\s+cited\b)", std::regex::icase),
        std::regex(R"(^sources\b)", std::regex::icase)
    };

    for (const auto& p : patterns)
        if (std::regex_search(s, p)) return true;
    return false;
}

static bool looks_like_reference_line(const std::string& line) {
    const std::string s = normalize_space(line);
    if (s.size() < 20) return false;

    // Common bibliography signatures.
    static const std::regex doi(R"(10\.\d{4,9}/[-._;()/:A-Z0-9]+)",
                                std::regex::icase);
    static const std::regex year(R"(\b(19|20)\d{2}\b)");
    static const std::regex url(R"((https?://|www\.))", std::regex::icase);

    unsigned signals = 0;
    if (std::regex_search(s, doi)) ++signals;
    if (std::regex_search(s, url)) ++signals;

    size_t commas = std::count(s.begin(), s.end(), ',');
    size_t semis  = std::count(s.begin(), s.end(), ';');
    if (std::regex_search(s, year) && (commas >= 2 || semis >= 1)) ++signals;

    return signals >= 2;
}

static std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) out += (c == '"' ? "\\\"" : std::string(1, c));
    return out + "\"";
#else
    std::string out = "'";
    for (char c : s) out += (c == '\'' ? "'\\''" : std::string(1, c));
    return out + "'";
#endif
}

static std::string temp_file(const std::string& suffix) {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return "pdfcsv_" + std::to_string(stamp) + "_" +
           std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + suffix;
}

static std::string run_pdftotext(const fs::path& pdf, const std::string& executable) {
    fs::path temp = fs::temp_directory_path() / temp_file(".txt");
    std::string cmd = shell_quote(executable) + " -layout -enc UTF-8 -q " + shell_quote(pdf.string()) +
                      " " + shell_quote(temp.string());
    int rc = std::system(cmd.c_str());
    if (rc != 0) { std::error_code ec; fs::remove(temp, ec); throw std::runtime_error("pdftotext failed for: " + pdf.string()); }
    std::ifstream in(temp, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read extracted text for: " + pdf.string());
    std::ostringstream ss; ss << in.rdbuf();
    std::error_code ec; fs::remove(temp, ec);
    return ss.str();
}

static int page_count(const std::string& text) {
    const int breaks = static_cast<int>(std::count(text.begin(), text.end(), '\f'));
    const bool trailing_break = !text.empty() && text.back() == '\f';
    return std::max(1, breaks + (trailing_break ? 0 : 1));
}

static std::string extract_page_text(const fs::path& pdf, int page, const std::string& executable) {
    fs::path temp = fs::temp_directory_path() / temp_file(".txt");
    std::string cmd = shell_quote(executable) + " -layout -enc UTF-8 -q -f " + std::to_string(page) +
                      " -l " + std::to_string(page) + " " + shell_quote(pdf.string()) +
                      " " + shell_quote(temp.string());
    int rc = std::system(cmd.c_str());
    if (rc != 0) { std::error_code ec; fs::remove(temp, ec); throw std::runtime_error("pdftotext page extraction failed: " + pdf.string()); }
    std::ifstream in(temp, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read page text: " + pdf.string());
    std::ostringstream ss; ss << in.rdbuf();
    std::error_code ec; fs::remove(temp, ec);
    return ss.str();
}

static std::optional<std::string> ocr_page(const fs::path& pdf, int page, const Config& cfg) {
    fs::path prefix = fs::temp_directory_path() / temp_file("_ocr");
    fs::path image = prefix; image += ".png";
    fs::path text = prefix; text += ".txt";
    std::string render = shell_quote(cfg.pdftoppm) + " -f " + std::to_string(page) + " -l " +
        std::to_string(page) + " -singlefile -png -r 200 " + shell_quote(pdf.string()) +
        " " + shell_quote(prefix.string());
    if (std::system(render.c_str()) != 0) return std::nullopt;
    std::string ocr = shell_quote(cfg.tesseract) + " " + shell_quote(image.string()) +
        " " + shell_quote(prefix.string()) + " -l eng --psm 3";
    if (std::system(ocr.c_str()) != 0) return std::nullopt;
    std::ifstream in(text, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss; ss << in.rdbuf();
    std::error_code ec; fs::remove(image, ec); fs::remove(text, ec);
    return ss.str();
}

static std::string xml_unescape(std::string s) {
    const std::pair<const char*, const char*> repl[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&apos;", "'"}
    };
    for (const auto& [from, to] : repl) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, std::string(from).size(), to);
            pos += std::string(to).size();
        }
    }
    return s;
}

static std::vector<std::string> extract_bold_terms(const fs::path& pdf, int page, const std::string& pdftohtml) {
    fs::path temp = fs::temp_directory_path() / temp_file(".xml");
    std::vector<std::string> terms;
    std::string cmd = shell_quote(pdftohtml) + " -xml -f " + std::to_string(page) + " -l " +
        std::to_string(page) + " -stdout " + shell_quote(pdf.string()) + " > " + shell_quote(temp.string());
    if (std::system(cmd.c_str()) != 0) return terms;
    std::ifstream in(temp, std::ios::binary);
    if (!in) return terms;
    std::ostringstream ss; ss << in.rdbuf();
    const std::string xml = ss.str();
    std::error_code ec; fs::remove(temp, ec);

    // Poppler's XML output commonly wraps bold glyphs in <b>...</b>.
    // This is more reliable than guessing from the font family because many
    // PDFs use the same family name for regular and bold faces.
    static const std::regex bold_tag(R"REGEX(<b>(.*?)</b>)REGEX", std::regex::icase);
    for (std::sregex_iterator it(xml.begin(), xml.end(), bold_tag), end; it != end; ++it) {
        std::string term = std::regex_replace((*it)[1].str(), std::regex(R"REGEX(<[^>]+>)REGEX"), " ");
        term = normalize_space(xml_unescape(term));
        if (term.size() >= 2 && term.size() <= 120) terms.push_back(term);
    }
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    return terms;
}


static std::string html_escape(const std::string& s) {
    const std::string clean = utf8_sanitize(s);
    std::string o;
    for (char c : clean) {
        if (c=='&') o += "&amp;"; else if (c=='<') o += "&lt;";
        else if (c=='>') o += "&gt;"; else if (c=='\"') o += "&quot;"; else o += c;
    }
    return o;
}

static std::string detect_table_text(const std::string& text) {
    std::vector<std::string> rows;
    std::istringstream input(text); std::string raw;
    while (std::getline(input, raw)) {
        std::string line = trim(raw); if (line.empty()) continue;
        size_t tabs=std::count(line.begin(),line.end(),'\t');
        size_t pipes=std::count(line.begin(),line.end(),'|');
        size_t runs=0;
        for(size_t i=1;i<raw.size();++i)
            if(std::isspace((unsigned char)raw[i]) && std::isspace((unsigned char)raw[i-1])) ++runs;
        if(tabs>=1 || pipes>=2 || runs>=3) rows.push_back(line);
    }
    if(rows.size()<2) return {};
    std::ostringstream o; for(const auto& r:rows) o<<r<<'\n'; return trim(o.str());
}


// Render tables as responsive HTML. Do not use <pre>: fixed-width whitespace
// causes horizontal clipping on narrow portrait screens such as AnkiDroid.
static std::string table_to_responsive_html(const std::string& text) {
    std::istringstream in(text); std::string line; std::vector<std::vector<std::string>> rows;
    while (std::getline(in,line)) {
        line=trim(line); if(line.empty()) continue; std::vector<std::string> cells;
        if(line.find('\t')!=std::string::npos){ std::stringstream ss(line); std::string c; while(std::getline(ss,c,'\t')) cells.push_back(trim(c)); }
        else if(line.find('|')!=std::string::npos){ std::stringstream ss(line); std::string c; while(std::getline(ss,c,'|')) cells.push_back(trim(c)); }
        else { static const std::regex cols(R"(\s{2,})"); for(std::sregex_token_iterator it(line.begin(),line.end(),cols,-1),end;it!=end;++it) cells.push_back(trim(*it)); }
        if(!cells.empty()) rows.push_back(std::move(cells));
    }
    if(rows.empty()) return {};
    std::ostringstream out; out<<"<div class=\"pdfcsv-table-wrap\"><table class=\"pdfcsv-table\"><tbody>";
    for(size_t r=0;r<rows.size();++r){ out<<"<tr>"; for(const auto& cell:rows[r]) out<<(r==0?"<th>":"<td>")<<html_escape(cell)<<(r==0?"</th>":"</td>"); out<<"</tr>"; }
    out<<"</tbody></table></div>"; return out.str();
}

static std::string responsive_media_html(const fs::path& image) {
    return "<div class=\"pdfcsv-image-wrap\"><img class=\"pdfcsv-image\" src=\"" + html_escape(image.filename().string()) + "\"></div>";
}

static std::vector<fs::path> extract_pdf_page_images(const fs::path& pdf, int page,
                                                      const fs::path& media_dir,
                                                      const std::string& stem) {
    std::vector<fs::path> out; fs::create_directories(media_dir);
    fs::path prefix=media_dir/(stem+"_p"+std::to_string(page));
    std::string cmd="pdfimages -png -f "+std::to_string(page)+" -l "+std::to_string(page)+" "+
        shell_quote(pdf.string())+" "+shell_quote(prefix.string())+" >/dev/null 2>&1";
    if(std::system(cmd.c_str())!=0) return out;
    for(const auto& e:fs::directory_iterator(media_dir)) if(e.is_regular_file()) {
        auto n=e.path().filename().string(); if(n.rfind(prefix.filename().string(),0)==0 && lower(e.path().extension().string())==".png") out.push_back(e.path());
    }
    std::sort(out.begin(),out.end()); return out;
}

static bool looks_like_document_thumbnail(const fs::path& p) {
    const std::string n=lower(p.filename().string());
    return n=="thumbnail.jpeg"||n=="thumbnail.jpg"||n=="thumbnail.png"||n=="thumbnail.webp"||n=="thumbnails.jpeg"||n=="thumbnails.png";
}

static bool image_is_effectively_blank(const fs::path& p) {
#ifndef _WIN32
    // ImageMagick is optional. If it is absent or inspection fails, keep the
    // image rather than silently discarding user content.
    std::string cmd="identify -format '%m\\t%w\\t%h\\t%[fx:mean]' "+shell_quote(p.string())+" 2>/dev/null";
    FILE* pipe=popen(cmd.c_str(),"r"); if(!pipe) return false; char buf[256]{}; std::string out;
    while(fgets(buf,sizeof(buf),pipe)) out+=buf;
    int rc=pclose(pipe);
    if(rc!=0) return false;
    std::stringstream ss(out); std::string fmt,w,h,mean_s; std::getline(ss,fmt,'\t'); std::getline(ss,w,'\t'); std::getline(ss,h,'\t'); std::getline(ss,mean_s,'\t');
    try { long long width=std::stoll(w), height=std::stoll(h); double mean=std::stod(mean_s); return width<40||height<40||mean>0.985; } catch(...) { return false; }
#else
    (void)p; return false;
#endif
}

static std::vector<fs::path> extract_embedded_images_generic(const fs::path& input,
                                                              const fs::path& media_dir,
                                                              const std::string& stem,
                                                              std::vector<std::string>* original_names=nullptr) {
    std::vector<fs::path> out; auto ext=lower(input.extension().string()); if(ext==".pdf"||!fs::exists(input)) return out;
    fs::path tmp=fs::temp_directory_path()/temp_file("_img"); fs::create_directories(tmp);
    std::string cmd="unzip -qq -o "+shell_quote(input.string())+" -d "+shell_quote(tmp.string())+" >/dev/null 2>&1";
    if(std::system(cmd.c_str())!=0){fs::remove_all(tmp);return out;}
    fs::create_directories(media_dir); std::vector<fs::path> candidates;
    for(const auto& e:fs::recursive_directory_iterator(tmp)) if(e.is_regular_file()) {
        auto ex=lower(e.path().extension().string()); if(ex!=".png"&&ex!=".jpg"&&ex!=".jpeg"&&ex!=".gif"&&ex!=".webp"&&ex!=".svg") continue;
        if(looks_like_document_thumbnail(e.path())||image_is_effectively_blank(e.path())) continue;
        const std::string rel=lower(fs::relative(e.path(),tmp).generic_string());
        const bool media_dir_name=rel.find("word/media/")!=std::string::npos||rel.find("ppt/media/")!=std::string::npos||rel.find("xl/media/")!=std::string::npos||rel.find("pictures/")!=std::string::npos||rel.find("images/")!=std::string::npos||rel.find("media/")!=std::string::npos;
        if(media_dir_name) candidates.push_back(e.path());
    }
    std::sort(candidates.begin(),candidates.end()); size_t n=0;
    for(const auto& e:candidates){auto ex=lower(e.extension().string()); fs::path dest=media_dir/(stem+"_img"+std::to_string(n++)+ex); std::error_code ec; fs::copy_file(e,dest,fs::copy_options::overwrite_existing,ec); if(!ec){out.push_back(dest);if(original_names)original_names->push_back(e.filename().string());}}
    fs::remove_all(tmp); return out;
}

static std::vector<std::string> split_pdf_pages(const std::string& text) {
    std::vector<std::string> pages;
    size_t start=0;
    while (start <= text.size()) {
        size_t end=text.find('\f',start);
        if(end==std::string::npos){ pages.push_back(utf8_sanitize(text.substr(start))); break; }
        pages.push_back(utf8_sanitize(text.substr(start,end-start)));
        start=end+1;
        if(start==text.size()) break;
    }
    if(pages.empty()) pages.emplace_back();
    return pages;
}

// Run pdftohtml exactly once for the entire PDF and distribute bold terms by
// <page> block. This removes hundreds/thousands of process launches on books.
static std::vector<std::vector<std::string>> extract_all_bold_terms(
        const fs::path& pdf, size_t page_count, const std::string& pdftohtml) {
    std::vector<std::vector<std::string>> result(page_count);
    fs::path temp=fs::temp_directory_path()/temp_file(".xml");
    std::string cmd=shell_quote(pdftohtml)+" -xml -stdout "+shell_quote(pdf.string())+
                    " > "+shell_quote(temp.string());
    if(std::system(cmd.c_str())!=0) { std::error_code ec; fs::remove(temp,ec); return result; }
    std::ifstream in(temp,std::ios::binary); if(!in) return result;
    std::ostringstream ss; ss<<in.rdbuf(); std::string xml=utf8_sanitize(ss.str());
    std::error_code ec; fs::remove(temp,ec);
    static const std::regex page_re(R"REGEX(<page\b[^>]*>([\s\S]*?)</page>)REGEX",std::regex::icase);
    static const std::regex bold_re(R"REGEX(<b>([\s\S]*?)</b>)REGEX",std::regex::icase);
    size_t pi=0;
    for(std::sregex_iterator p(xml.begin(),xml.end(),page_re), pend; p!=pend && pi<result.size(); ++p,++pi){
        const std::string block=(*p)[1].str();
        for(std::sregex_iterator b(block.begin(),block.end(),bold_re), bend;b!=bend;++b){
            std::string term=std::regex_replace((*b)[1].str(),std::regex(R"REGEX(<[^>]+>)REGEX")," ");
            term=normalize_space(xml_unescape(term));
            if(term.size()>=2 && term.size()<=120) result[pi].push_back(term);
        }
        auto& v=result[pi]; std::sort(v.begin(),v.end()); v.erase(std::unique(v.begin(),v.end()),v.end());
    }
    return result;
}

static std::vector<Page> extract_pages(const fs::path& pdf, const Config& cfg) {
    // Single-pass text extraction: one pdftotext process for the entire book.
    const auto text_pages = split_pdf_pages(run_pdftotext(pdf, cfg.pdftotext));
    const auto bold_by_page = extract_all_bold_terms(pdf, text_pages.size(), cfg.pdftohtml);
    std::vector<std::future<Page>> futures;
    futures.reserve(text_pages.size());
    for (size_t idx = 0; idx < text_pages.size(); ++idx) {
        const int p=static_cast<int>(idx+1);
        futures.emplace_back(std::async(std::launch::async, [&, idx, p] {
            Page page{p, text_pages[idx], bold_by_page[idx], false, {}, {}, {}, ""};
            const std::string n = normalize_space(page.text);
            const size_t alnum = std::count_if(n.begin(), n.end(), [](unsigned char c){ return std::isalnum(c); });
            if (!cfg.no_ocr && alnum < 40) {
                if (auto ocr = ocr_page(pdf, p, cfg)) {
                    *ocr=utf8_sanitize(*ocr);
                    const size_t ocr_alnum = std::count_if(ocr->begin(), ocr->end(), [](unsigned char c){ return std::isalnum(c); });
                    if (ocr_alnum > alnum) { page.text = *ocr; page.ocr = true; page.bold_terms.clear(); }
                }
            }
            page.table_text = detect_table_text(page.text);
            if (cfg.extract_images) {
                page.images = extract_pdf_page_images(pdf, p, cfg.output_dir / (stem_safe(pdf)+"_media"), stem_safe(pdf));
                for(const auto& im:page.images) page.image_original_names.push_back(im.filename().string());
                page.image_global_pages.assign(page.images.size(), p);
            }
            return page;
        }));
    }
    std::vector<Page> pages; pages.reserve(futures.size());
    for(auto& f:futures) pages.push_back(f.get());
    return pages;
}



static std::vector<std::string> lines_from_text(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

static std::vector<Section> parse_sections(const std::vector<Page>& pages) {
    std::vector<Section> sections{{"Body", 1, false}};
    for (const auto& page : pages) {
        for (const auto& raw : lines_from_text(page.text)) {
            const std::string line = normalize_space(raw);
            if (line.empty()) continue;
            if (looks_like_excluded_heading(line)) sections.push_back({line, page.number, true});
            else if (is_heading_like(line)) sections.push_back({line, page.number, false});
        }
    }
    std::sort(sections.begin(), sections.end(), [](const Section& a, const Section& b){ return a.start_page < b.start_page; });
    return sections;
}

static const Section& section_for_page(const std::vector<Section>& sections, int page) {
    const Section* best = &sections.front();
    for (const auto& s : sections) { if (s.start_page <= page) best = &s; else break; }
    return *best;
}

static std::vector<std::string> split_sentences(const std::string& paragraph) {
    std::vector<std::string> result;
    std::string current;

    for (size_t i = 0; i < paragraph.size(); ++i) {
        char c = paragraph[i];
        current += c;

        if ((c == '.' || c == '?' || c == '!') &&
            (i + 1 == paragraph.size() || std::isspace(
                static_cast<unsigned char>(paragraph[i + 1])))) {
            result.push_back(trim(current));
            current.clear();
        }
    }

    if (!trim(current).empty()) result.push_back(trim(current));
    return result;
}

static std::optional<Row> extract_definition(const std::string& sentence, int page,
                                                const std::string& section, const std::string& source) {
    static const std::vector<std::regex> patterns = {
        std::regex(R"(^\s*([A-Z][A-Za-z0-9 _/\-]{1,90}?)\s+(is|are|means|refers to|denotes|represents)\s+(.+)$)", std::regex::icase),
        std::regex(R"(^\s*([A-Z][A-Za-z0-9 _/\-]{1,90}?)\s*:\s*(.+)$)")
    };
    std::smatch m;
    for (const auto& p : patterns) {
        if (!std::regex_match(sentence, m, p)) continue;
        std::string key = trim(m[1].str());
        std::string meat = m.size() == 4 ? trim(m[3].str()) : trim(m[2].str());
        if (key.size() >= 2 && meat.size() >= 15 && key.size() < meat.size() * 2) {
            Row out; out.key=key; out.meat=meat; out.page=page; out.section=section; out.category="DEFINITION"; out.confidence=0.90; out.source=source; out.evidence="definition-pattern"; return out;
        }
    }
    return std::nullopt;
}

static std::vector<std::string> important_phrases(const std::string& sentence) {
    std::vector<std::string> keys;
    static const std::regex cap(R"(\b(?:[A-Z][A-Za-z0-9&/\-]*)(?:\s+[A-Z][A-Za-z0-9&/\-]*){0,5}\b)");
    static const std::regex quoted(R"(["“]([^"”]{2,100})["”])");
    for (std::sregex_iterator it(sentence.begin(), sentence.end(), cap), end; it != end; ++it) {
        std::string k = trim(it->str());
        if (k.size() >= 3 && k.size() <= 100 && lower(k) != "the" && lower(k) != "this") keys.push_back(k);
    }
    for (std::sregex_iterator it(sentence.begin(), sentence.end(), quoted), end; it != end; ++it)
        keys.push_back(trim((*it)[1].str()));
    return keys;
}

static std::vector<std::string> split_sentences(const std::string& paragraph);

static std::vector<Row> extract_rows(const std::vector<Page>& pages, const std::vector<Section>& sections, bool consolidate) {
    std::vector<Row> rows;
    std::unordered_set<std::string> seen;
    for (const auto& page : pages) {
        const auto& section = section_for_page(sections, page.number);
        if (section.excluded) continue;
        std::string paragraph;
        auto flush = [&]() {
            if (paragraph.empty()) return;
            const auto sentences = split_sentences(paragraph);
            ConsolidationOptions consolidation_options;
            consolidation_options.enabled = consolidate;
            for (size_t si = 0; si < sentences.size(); ++si) {
                const auto& sentence = sentences[si];
                if (sentence.size() < 20) continue;
                if (auto def = extract_definition(sentence, page.number, section.title,
                                                  page.ocr ? "ocr" : "text-layer")) {
                    if (consolidation_options.enabled) {
                        auto consolidated = consolidate_definition(def->key, sentences, si, consolidation_options);
                        def->meat = consolidated.first;
                        def->evidence += ";multi-sentence-consolidated";
                        def->confidence = std::min(0.99, def->confidence + 0.05 * (consolidated.second > 1 ? 1.0 : 0.0));
                        si += consolidated.second - 1;
                    }
                    std::string id = lower(def->key + "\n" + def->meat);
                    def->images = page.images;
                    def->table_text = page.table_text;
                    if (seen.insert(id).second) {
                        def->category = "DEFINITION";
                        def->confidence = std::max(def->confidence, 0.90);
                        def->evidence = "definition-pattern";
                        if (!def->images.empty()) def->evidence += ";image-context";
                        if (!def->table_text.empty()) def->evidence += ";table-context";
                        rows.push_back(*def);
                    }
                    continue;
                }
                // Bold text is intentionally stronger than italic text.
                for (const auto& term : page.bold_terms) {
                    if (lower(sentence).find(lower(term)) == std::string::npos) continue;
                    std::string id = lower(term + "\n" + sentence);
                    if (seen.insert(id).second) {
                        auto cat = classify_semantically(sentence);
                        Row out; out.key=term; out.meat=sentence; out.page=page.number; out.section=section.title; out.category=category_name(cat); out.confidence=semantic_confidence(cat,false,true,false,false,is_heading_like(sentence)); out.source=page.ocr?"ocr":"text-layer"; out.evidence="bold-term"; out.images=page.images; out.image_original_names=page.image_original_names; out.image_global_pages=page.image_global_pages; out.table_text=page.table_text; rows.push_back(std::move(out));
                    }
                }
                auto keys = important_phrases(sentence);
                size_t emitted = 0;
                for (const auto& key : keys) {
                    std::string id = lower(key + "\n" + sentence);
                    if (seen.insert(id).second) {
                        auto cat = classify_semantically(sentence);
                        bool defp = std::regex_search(lower(sentence),
                            std::regex(R"(\b(is|are|means|refers to|defined as|denotes)\b)"));
                        bool quoted = sentence.find('"') != std::string::npos || sentence.find('\"') != std::string::npos;
                        Row out; out.key=key; out.meat=sentence; out.page=page.number; out.section=section.title; out.category=category_name(cat); out.confidence=semantic_confidence(cat,defp,false,false,quoted,is_heading_like(sentence)); out.source=page.ocr?"ocr":"text-layer"; out.evidence=semantic_evidence(defp,false,false,quoted,is_heading_like(sentence)); out.images=page.images; out.image_original_names=page.image_original_names; out.image_global_pages=page.image_global_pages; out.table_text=page.table_text; rows.push_back(std::move(out));
                        if (++emitted >= 3) break;
                    }
                }
            }
            paragraph.clear();
        };
        for (const auto& raw : lines_from_text(page.text)) {
            std::string line = normalize_space(raw);
            if (line.empty() || std::regex_match(line, std::regex(R"(^[-–—]?\s*\d+\s*[-–—]?$)"))) { flush(); continue; }
            if (looks_like_reference_line(line)) continue;
            const size_t tab_count = std::count(raw.begin(), raw.end(), '\t');
            const size_t pipe_count = std::count(raw.begin(), raw.end(), '|');
            size_t space_runs=0;
            for(size_t j=1;j<raw.size();++j) if(std::isspace((unsigned char)raw[j]) && std::isspace((unsigned char)raw[j-1])) ++space_runs;
            if (tab_count >= 1 || pipe_count >= 2 || space_runs >= 3) { flush(); continue; }
            const bool standalone_bold = std::any_of(page.bold_terms.begin(), page.bold_terms.end(),
                [&](const std::string& term){ return lower(term) == lower(line); });
            if (standalone_bold || is_heading_like(line)) { flush(); continue; }
            if (!paragraph.empty()) paragraph += ' ';
            paragraph += line;
            if (paragraph.size() > 6000) flush();
        }
        flush();
        if (!page.table_text.empty()) {
            Row tr; tr.key="Table (page "+std::to_string(page.number)+")"; tr.meat=page.table_text; tr.page=page.number; tr.section=section.title; tr.category="TABLE"; tr.tags="pdfcsv TABLE"; tr.confidence=0.92; tr.source=page.ocr?"ocr":"text-layer"; tr.evidence="table-structure"; tr.images=page.images; tr.image_original_names=page.image_original_names; tr.image_global_pages=page.image_global_pages; tr.table_text=page.table_text;
            rows.push_back(std::move(tr));
        }
    }
    // Global deduplication, retaining the highest-confidence occurrence.
    std::unordered_map<std::string, Row> best;
    for (auto& row : rows) {
        std::string id = lower(normalize_space(row.key) + "\n" + normalize_space(row.meat));
        auto it = best.find(id);
        if (it == best.end() || row.confidence > it->second.confidence) best[id] = std::move(row);
    }
    rows.clear();
    for (auto& [_, row] : best) rows.push_back(std::move(row));
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b){ return a.page != b.page ? a.page < b.page : a.key < b.key; });
    return rows;
}

static fs::path output_path_for(const Config& cfg, const fs::path& input);


// Spreadsheet inputs intentionally have a different default: a user opening a
// CSV/XLSX/ODS is usually asking for study material, so we create an Anki deck
// unless they explicitly request spreadsheet-only output.
static bool is_spreadsheet_input(const fs::path& p) {
    const auto e = lower(p.extension().string());
    return e==".csv" || e==".tsv" || e==".xlsx" || e==".xls" || e==".ods";
}

static std::vector<std::vector<std::string>> parse_delimited(const std::string& text, char sep) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row; std::string field; bool quoted=false;
    for(size_t i=0;i<text.size();++i){
        char c=text[i];
        if(c=='"') {
            if(quoted && i+1<text.size() && text[i+1]=='"'){ field+='"'; ++i; }
            else quoted=!quoted;
        } else if(c==sep && !quoted){ row.push_back(field); field.clear(); }
        else if((c=='\n' || c=='\r') && !quoted){
            if(c=='\r' && i+1<text.size() && text[i+1]=='\n') ++i;
            row.push_back(field); field.clear();
            if(!row.empty() && std::any_of(row.begin(),row.end(),[](const std::string& x){return !x.empty();})) rows.push_back(std::move(row));
            row.clear();
        } else field+=c;
    }
    if(!field.empty() || !row.empty()){ row.push_back(field); rows.push_back(std::move(row)); }
    return rows;
}

static std::vector<std::vector<std::string>> read_spreadsheet_rows(const Config& cfg, const fs::path& input) {
    const auto ext=lower(input.extension().string());
    fs::path csv=input;
    fs::path tempdir;
    if(ext!=".csv" && ext!=".tsv") {
        tempdir=fs::temp_directory_path()/(std::string("pdfcsv_")+temp_file("_sheet")); fs::create_directories(tempdir);
        std::string cmd=shell_quote(cfg.libreoffice)+" --headless --convert-to csv --outdir "+shell_quote(tempdir.string())+" "+shell_quote(input.string())+" >/dev/null 2>&1";
        if(std::system(cmd.c_str())!=0) { fs::remove_all(tempdir); throw std::runtime_error("Spreadsheet conversion to CSV failed: "+input.string()); }
        csv=tempdir/(input.stem().string()+".csv");
        if(!fs::exists(csv)){ fs::remove_all(tempdir); throw std::runtime_error("LibreOffice did not create CSV for: "+input.string()); }
    }
    std::ifstream in(csv,std::ios::binary); std::ostringstream ss; ss<<in.rdbuf(); std::string text=ss.str(); if(!tempdir.empty()) fs::remove_all(tempdir);
    char sep=(ext==".tsv")?'\t':',';
    return parse_delimited(text,sep);
}

static int find_column(const std::vector<std::string>& h, const std::vector<std::string>& names, int fallback) {
    for(size_t i=0;i<h.size();++i){ std::string x=lower(trim(h[i])); for(const auto& n:names) if(x==n) return (int)i; }
    return fallback;
}

static size_t process_one_spreadsheet(const Config& cfg, const fs::path& input) {
    auto table=read_spreadsheet_rows(cfg,input);
    if(table.empty()) return 0;
    std::vector<std::string> h=table.front();
    int front=find_column(h,{"front","question","term","key","prompt"},0);
    int back=find_column(h,{"back","answer","definition","meat","response","explanation"},h.size()>1?1:0);
    int tags=find_column(h,{"tags","tag"},-1);
    int image=find_column(h,{"image","images","image-file","media"},-1);
    int page=find_column(h,{"page","pages","image-pages"},-1);
    [[maybe_unused]] int global=find_column(h,{"image-global-page-number","global-page","global-page-number"},-1);
    int category=find_column(h,{"category","type"},-1);
    int start=1;
    bool header=false;
    for(const auto& x:h){ auto y=lower(trim(x)); if(y=="front"||y=="question"||y=="term"||y=="back"||y=="answer"||y=="definition") {header=true;break;} }
    if(!header){
        table.insert(table.begin(), {"Front","Back","Tags","Images","Page","Image Global Page Number","Category"});
        h=table.front(); front=0; back=1; tags=2; image=3; page=4; global=5; category=6; start=1;
    }
    std::vector<pdfcsv::AnkiCard> cards;
    std::vector<Row> rows;
    for(size_t i=start;i<table.size();++i){
        auto r=table[i]; r.resize(h.size());
        if(trim(r[front]).empty() && trim(r[back]).empty()) continue;
        Row row; row.key=r[front]; row.meat=r[back]; row.page=page>=0?std::atoi(r[page].c_str()):int(i); row.section="Spreadsheet"; row.category=category>=0?r[category]:"SPREADSHEET"; row.tags=tags>=0?r[tags]:"pdfcsv"; row.confidence=1.0; row.source=input.extension().string(); row.evidence="spreadsheet-row";
        if(cfg.extract_images && image>=0 && !trim(r[image]).empty()) { std::string list=r[image]; size_t pos=0; while(pos<list.size()){size_t e=list.find(';',pos); if(e==std::string::npos)e=list.size(); std::string f=trim(list.substr(pos,e-pos)); if(!f.empty()) { fs::path ip=f; if(ip.is_relative()) ip=input.parent_path()/ip; row.images.push_back(ip); row.image_original_names.push_back(fs::path(f).filename().string()); row.image_global_pages.push_back(row.page); } pos=e+1;} }
        rows.push_back(row);
    }
    fs::create_directories(cfg.output_dir);
    const std::string stem=cfg.output_name.empty()?stem_safe(input):cfg.output_name;
    const bool auto_anki = !cfg.spreadsheet_only && !cfg.csv_and_anki && !cfg.anki_only;
    const bool write_csv = cfg.spreadsheet_only || cfg.csv_and_anki || (!auto_anki && !cfg.anki_only) || !cfg.output_name.empty();
    const bool write_anki = !cfg.spreadsheet_only;
    if(write_csv){
        fs::path out=cfg.output_dir/(stem+".csv");
        if(fs::exists(out)&&!cfg.overwrite) throw std::runtime_error("Output exists (use --overwrite): "+out.string());
        std::ofstream csv(out,std::ios::binary); if(!csv) throw std::runtime_error("Cannot write: "+out.string());
        csv << "\"key\",\"meat\",\"page\",\"section\",\"category\",\"confidence\",\"source\",\"evidence\",\"images\",\"table\",\"image-original-names\",\"image-pages\",\"image-global-page-number\"\n";
        for(const auto& r:rows){ std::string imgs,names,pages,globals; for(size_t j=0;j<r.images.size();++j){if(j){imgs+=';';names+=';';pages+=';';globals+=';';} imgs+=r.images[j].filename().string(); names+=r.images[j].filename().string(); pages+=std::to_string(r.page); globals+=std::to_string(r.page);} csv<<csv_escape(r.key)<<','<<csv_escape(r.meat)<<','<<r.page<<','<<csv_escape(r.section)<<','<<csv_escape(r.category)<<','<<r.confidence<<','<<csv_escape(r.source)<<','<<csv_escape(r.evidence)<<','<<csv_escape(imgs)<<','<<csv_escape(r.table_text)<<','<<csv_escape(names)<<','<<csv_escape(pages)<<','<<csv_escape(globals)<<'\n'; }
    }
    if(write_anki){
        cards.reserve(rows.size());
        for(const auto& r:rows){ pdfcsv::AnkiCard card; card.front=html_escape(r.key); card.back=html_escape(r.meat); if(!r.table_text.empty()) card.back+="<br><br><b>Table:</b><br>"+table_to_responsive_html(r.table_text); for(size_t mi=0; mi<r.images.size(); ++mi){ card.back += responsive_media_html(r.images[mi]); card.media_files.push_back(r.images[mi].string()); } card.tags=r.tags.empty()?"pdfcsv":r.tags; cards.push_back(std::move(card)); }
        fs::path apkg=cfg.output_dir/(stem+".apkg"); if(fs::exists(apkg)&&!cfg.overwrite) throw std::runtime_error("Output exists (use --overwrite): "+apkg.string()); pdfcsv::AnkiPackageWriter w; std::string err; pdfcsv::AnkiTiming timing{}; if(!w.write(apkg.string(),stem,cards,&err,cfg.timing ? &timing : nullptr)) throw std::runtime_error("Anki package creation failed: "+err);
        if (cfg.timing) { std::lock_guard lock(cout_mutex); std::cout << "[pdfcsv] timing: anki-db=" << timing.database_seconds << "s packaging=" << timing.packaging_seconds << "s total=" << timing.total_seconds << "s notes=" << timing.notes << " cards=" << timing.cards << " media=" << timing.media_files << '\n'; }
    }
    return rows.size();
}

// Standalone spreadsheet conversion utility. This deliberately delegates the
// format-specific work to LibreOffice so pdfcsv remains small and offline-capable.
static bool convert_spreadsheet(const Config& cfg, const fs::path& input) {
    const auto fmt=lower(cfg.convert_to); if(fmt.empty()) return false;
    static const std::unordered_set<std::string> allowed={"csv","tsv","xlsx","xls","ods"};
    if(!allowed.count(fmt)) throw std::runtime_error("Unsupported spreadsheet output format: "+fmt);
    fs::create_directories(cfg.output_dir);
    std::string target=fmt=="tsv"?"csv":fmt;
    std::string cmd=shell_quote(cfg.libreoffice)+" --headless --convert-to "+shell_quote(target)+" --outdir "+shell_quote(cfg.output_dir.string())+" "+shell_quote(input.string())+" >/dev/null 2>&1";
    if(std::system(cmd.c_str())!=0) throw std::runtime_error("Spreadsheet conversion failed: "+input.string());
    if(fmt=="tsv"){
        fs::path csv=cfg.output_dir/(input.stem().string()+".csv"), tsv=cfg.output_dir/(input.stem().string()+".tsv");
        std::ifstream in(csv,std::ios::binary); std::ostringstream ss; ss<<in.rdbuf(); auto rows=parse_delimited(ss.str(),','); std::ofstream out(tsv); for(auto& row:rows){for(size_t i=0;i<row.size();++i){if(i)out<<'\t';out<<row[i];}out<<'\n';} fs::remove(csv);
    }
    return true;
}

static size_t process_one(const Config& cfg, const fs::path& pdf);

static size_t process_one_generic(const Config& cfg, const fs::path& input) {
    if (lower(input.extension().string()) == ".pdf") return process_one(cfg, input);
    if (is_spreadsheet_input(input) && (cfg.spreadsheet_only || cfg.output_name.size() || cfg.anki_only || cfg.csv_and_anki || (lower(input.extension().string())==".csv" || lower(input.extension().string())==".tsv"))) return process_one_spreadsheet(cfg,input);
    { std::lock_guard lock(cout_mutex); std::cout << "[pdfcsv] scanning: " << input << '\n'; }
    InputConfig ic;
    ic.pdftotext = cfg.pdftotext; ic.pdftohtml = cfg.pdftohtml;
    ic.pdftoppm = cfg.pdftoppm; ic.tesseract = cfg.tesseract;
    ic.pandoc = cfg.pandoc; ic.libreoffice = cfg.libreoffice;
    ic.ebook_convert = cfg.ebook_convert;
    auto units = extract_document_units(input, ic, cfg.no_ocr);
    std::vector<Page> pages;
    pages.reserve(units.size());
    for (const auto& u : units) pages.push_back(Page{u.number, u.text, u.bold_terms, false, {}, {}, {}, detect_table_text(u.text)});
    if (cfg.extract_images && !pages.empty()) {
        pages.front().images = extract_embedded_images_generic(input, cfg.output_dir / (stem_safe(input)+"_media"), stem_safe(input), &pages.front().image_original_names);
        pages.front().image_global_pages.assign(pages.front().images.size(), pages.front().number);
    }
    auto sections = parse_sections(pages);
    auto rows = extract_rows(pages, sections, cfg.consolidate);
    for(auto& r: rows){ for(const auto& pg:pages) if(pg.number==r.page){ r.image_original_names=pg.image_original_names; r.image_global_pages=pg.image_global_pages; break; } }
    fs::create_directories(cfg.output_dir);
    if (rows.empty() && !cfg.keep_empty) return 0;

    const std::string stem = stem_safe(input);
    if (!cfg.anki_only) {
        fs::path out = cfg.output_dir / (stem + ".csv");
        if (fs::exists(out) && !cfg.overwrite)
            throw std::runtime_error("Output exists (use --overwrite): " + out.string());
        std::ofstream csv(out, std::ios::binary);
        if (!csv) throw std::runtime_error("Cannot write: " + out.string());
        csv << "\"key\",\"meat\",\"page\",\"section\",\"category\",\"confidence\",\"source\",\"evidence\",\"images\",\"table\",\"image-original-names\",\"image-pages\",\"image-global-page-number\"\n";
        csv << std::fixed << std::setprecision(3);
        for (const auto& r : rows) {
            csv << csv_escape(r.key) << ',' << csv_escape(r.meat) << ',' << r.page << ','
                << csv_escape(r.section) << ',' << csv_escape(r.category) << ',' << r.confidence << ','
                << csv_escape(r.source) << ',' << csv_escape(r.evidence) << ',';
            std::string imgs,names,pages,globals;
            for(size_t i=0;i<r.images.size();++i){ if(i){imgs+=';';names+=';';pages+=';';globals+=';';} imgs+=r.images[i].filename().string(); if(i<r.image_original_names.size()) names+=r.image_original_names[i]; else names+=r.images[i].filename().string(); if(i<r.image_global_pages.size()) pages+=std::to_string(r.image_global_pages[i]); else pages+=std::to_string(r.page); globals+=std::to_string(i<r.image_global_pages.size()?r.image_global_pages[i]:r.page);}
            csv << csv_escape(imgs) << ',' << csv_escape(r.table_text) << ',' << csv_escape(names) << ',' << csv_escape(pages) << ',' << csv_escape(globals) << '\n';
        }
        { std::lock_guard lock(cout_mutex); std::cout << "[pdfcsv] wrote " << rows.size() << " rows: " << out << '\n'; }
    }

    if (cfg.anki_only || cfg.csv_and_anki) {
        std::vector<pdfcsv::AnkiCard> cards;
        cards.reserve(rows.size());
        for (const auto& r : rows) {
            pdfcsv::AnkiCard card;
            card.front = r.key.empty() ? "What is this?" : r.key;
            card.back = html_escape(r.meat);
            if(!r.table_text.empty()) card.back += "<br><br><b>Table:</b><br>"+table_to_responsive_html(r.table_text);
            for(size_t mi=0; mi<r.images.size(); ++mi){
                card.back += responsive_media_html(r.images[mi]);
                card.media_files.push_back(r.images[mi].string());
            }
            card.tags = r.category.empty() ? "pdfcsv" : "pdfcsv " + r.category;
            cards.push_back(std::move(card));
        }
        fs::path apkg = cfg.output_dir / (stem + ".apkg");
        if (fs::exists(apkg) && !cfg.overwrite)
            throw std::runtime_error("Output exists (use --overwrite): " + apkg.string());
        pdfcsv::AnkiPackageWriter writer;
        std::string error;
        pdfcsv::AnkiTiming timing{};
        if (!writer.write(apkg.string(), stem, cards, &error, cfg.timing ? &timing : nullptr))
            throw std::runtime_error("Anki package creation failed: " + error);
        { std::lock_guard lock(cout_mutex); std::cout << "[pdfcsv] wrote " << cards.size() << " Anki cards: " << apkg << '\n'; if(cfg.timing) std::cout << "[pdfcsv] timing: anki-db=" << timing.database_seconds << "s packaging=" << timing.packaging_seconds << "s total=" << timing.total_seconds << "s notes=" << timing.notes << " cards=" << timing.cards << " media=" << timing.media_files << '\n'; }
    }
    return rows.size();
}

static fs::path output_path_for(const Config& cfg, const fs::path& input) {
    return cfg.output_dir / (stem_safe(input) + ".csv");
}

static size_t process_one(const Config& cfg, const fs::path& pdf) {
    { std::lock_guard lock(cout_mutex); std::cout << "[pdfcsv] scanning: " << pdf << '\n'; }
    auto pages = extract_pages(pdf, cfg);
    auto sections = parse_sections(pages);
    auto rows = extract_rows(pages, sections, cfg.consolidate);
    for(auto& r: rows){ for(const auto& pg:pages) if(pg.number==r.page){ r.image_original_names=pg.image_original_names; r.image_global_pages=pg.image_global_pages; break; } }
    fs::create_directories(cfg.output_dir);
    if (rows.empty() && !cfg.keep_empty) return 0;
    const std::string stem = stem_safe(pdf);
    if (!cfg.anki_only) {
        fs::path out = cfg.output_dir / (stem + ".csv");
        if (fs::exists(out) && !cfg.overwrite) throw std::runtime_error("Output exists (use --overwrite): " + out.string());
        std::ofstream csv(out, std::ios::binary);
        if (!csv) throw std::runtime_error("Cannot write: " + out.string());
        csv << "\"key\",\"meat\",\"page\",\"section\",\"category\",\"confidence\",\"source\",\"evidence\",\"images\",\"table\",\"image-original-names\",\"image-pages\",\"image-global-page-number\"\n";
        csv << std::fixed << std::setprecision(3);
        for (const auto& r : rows) {
            csv << csv_escape(r.key) << ',' << csv_escape(r.meat) << ',' << r.page << ','
                << csv_escape(r.section) << ',' << csv_escape(r.category) << ',' << r.confidence << ','
                << csv_escape(r.source) << ',' << csv_escape(r.evidence) << ',';
            std::string imgs,names,pages,globals;
            for(size_t i=0;i<r.images.size();++i){ if(i){imgs+=';';names+=';';pages+=';';globals+=';';} imgs+=r.images[i].filename().string(); if(i<r.image_original_names.size()) names+=r.image_original_names[i]; else names+=r.images[i].filename().string(); if(i<r.image_global_pages.size()) pages+=std::to_string(r.image_global_pages[i]); else pages+=std::to_string(r.page); globals+=std::to_string(i<r.image_global_pages.size()?r.image_global_pages[i]:r.page);}
            csv << csv_escape(imgs) << ',' << csv_escape(r.table_text) << ',' << csv_escape(names) << ',' << csv_escape(pages) << ',' << csv_escape(globals) << '\n';
        }
        { std::lock_guard lock(cout_mutex); std::cout << "[pdfcsv] wrote " << rows.size() << " rows: " << out << '\n'; }
    }
    if (cfg.anki_only || cfg.csv_and_anki) {
        std::vector<pdfcsv::AnkiCard> cards; cards.reserve(rows.size());
        for (const auto& r : rows) {
            pdfcsv::AnkiCard card;
            card.front = r.key.empty() ? "What is this?" : html_escape(r.key);
            card.back = html_escape(r.meat);
            if(!r.table_text.empty()) card.back += "<br><br><b>Table:</b><br>"+table_to_responsive_html(r.table_text);
            for(size_t mi=0; mi<r.images.size(); ++mi){ card.back += responsive_media_html(r.images[mi]); card.media_files.push_back(r.images[mi].string()); }
            card.tags = r.category.empty() ? "pdfcsv" : "pdfcsv " + r.category;
            cards.push_back(std::move(card));
        }
        fs::path apkg = cfg.output_dir / (stem + ".apkg");
        if (fs::exists(apkg) && !cfg.overwrite) throw std::runtime_error("Output exists (use --overwrite): " + apkg.string());
        pdfcsv::AnkiPackageWriter writer; std::string error; pdfcsv::AnkiTiming timing{};
        if (!writer.write(apkg.string(), stem, cards, &error, cfg.timing ? &timing : nullptr)) throw std::runtime_error("Anki package creation failed: " + error);
        { std::lock_guard lock(cout_mutex); std::cout << "[pdfcsv] wrote " << cards.size() << " Anki cards: " << apkg << '\n'; if(cfg.timing) std::cout << "[pdfcsv] timing: anki-db=" << timing.database_seconds << "s packaging=" << timing.packaging_seconds << "s total=" << timing.total_seconds << "s notes=" << timing.notes << " cards=" << timing.cards << " media=" << timing.media_files << '\n'; }
    }
    return rows.size();
}

static void usage() {
    std::cout <<
R"(pdfcsv - extract useful key/value knowledge from common documents

Usage:
  pdfcsv [options] file1 [file2 ...]
  pdfcsv [options] --input-dir directory
  pdfcsv [options] --recursive directory

Supported input types:
  PDF, EPUB, MOBI/AZW/AZW3, DOCX, DOC, ODT, RTF,
  PPTX, PPT, ODP, XLSX, XLS, ODS, Markdown, TXT, CSV, TSV,
  HTML, XML, and TeX.

Options:
  -o, --output DIR       Output directory (default: current directory)
  -t, --threads N        Maximum concurrent jobs
      --pdftotext PATH   Path to pdftotext
      --pdftohtml PATH   Path to pdftohtml (bold-term detection)
      --pdftoppm PATH    Path to pdftoppm (OCR rendering)
      --tesseract PATH   Path to tesseract (OCR)
      --pandoc PATH      Path to pandoc (EPUB/Office/text conversion)
      --libreoffice PATH Path to LibreOffice (Office/spreadsheet fallback)
      --ebook-convert PATH Path to Calibre ebook-convert (MOBI/AZW)
      --input-dir DIR    Process supported files directly inside DIR
      --recursive DIR    Recursively process supported files below DIR
      --no-ocr           Disable PDF OCR fallback
      --overwrite        Replace existing CSV files
      --keep-empty       Write CSVs even when no rows are extracted
      --name NAME        Base output name (spreadsheet inputs also create Anki)
      --convert-to FMT   Convert spreadsheet to csv, tsv, xlsx, xls, or ods
  -s                    Spreadsheet-only output; disables automatic Anki creation
  -a                    Create Anki .apkg cards only
  -c                    Create both CSV and Anki .apkg cards
  -u                    Unconsolidated: disable multi-sentence consolidation
  -i                    Disable semantic image extraction
  -cl                   Explicitly enable consolidation (now the default)
      --timing           Report Anki database/package timing
      --benchmark        Alias for --timing plus object counts
  -h, --help             Show this help

Examples:
  pdfcsv book.pdf
  pdfcsv textbook.epub lecture.pptx notes.docx
  pdfcsv data.xlsx notes.md chapter.txt
  pdfcsv --recursive ./library -o ./csv
  pdfcsv --recursive ./books
  pdfcsv -a textbook.epub          # EPUB -> Anki .apkg only
  pdfcsv -c textbook.epub          # EPUB -> CSV + Anki .apkg
  pdfcsv -c -u textbook.pdf
  pdfcsv --pandoc /usr/bin/pandoc book.epub
)";
}

static unsigned parse_uint(const std::string& s, const char* name) {
    try {
        unsigned long n = std::stoul(s);
        if (n == 0 || n > 4096) throw std::exception();
        return static_cast<unsigned>(n);
    } catch (...) {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + s);
    }
}

static void add_directory_inputs(Config& cfg, const fs::path& dir, bool recursive) {
    if (!fs::is_directory(dir)) throw std::runtime_error("Not a directory: " + dir.string());
    auto add=[&](const fs::directory_entry& e){
        if (e.is_regular_file() && is_supported_input(e.path())) cfg.inputs.push_back(e.path());
    };
    if (recursive) for (const auto& e: fs::recursive_directory_iterator(dir)) add(e);
    else for (const auto& e: fs::directory_iterator(dir)) add(e);
}

static Config parse_args(int argc, char** argv) {
    Config cfg;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-cl") {
            cfg.consolidate = true;
            continue;
        }
        if (std::string(argv[i]) == "-u") {
            cfg.consolidate = false;
            continue;
        }
        if (std::string(argv[i]) == "-a") {
            cfg.anki_only = true;
            continue;
        }
        if (std::string(argv[i]) == "-c") {
            cfg.csv_and_anki = true;
            continue;
        }
        if (std::string(argv[i]) == "-i") { cfg.extract_images = false; continue; }
        if (std::string(argv[i]) == "-s") { cfg.spreadsheet_only = true; continue; }

        std::string a = argv[i];

        auto need_value = [&](const char* option) -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string("Missing value for ") + option);
            return argv[++i];
        };

        if (a == "-h" || a == "--help") {
            usage();
            std::exit(0);
        } else if (a == "-o" || a == "--output") {
            cfg.output_dir = need_value(a.c_str());
        } else if (a == "-t" || a == "--threads") {
            cfg.threads = parse_uint(need_value(a.c_str()), "thread count");
        } else if (a == "--pdftotext") {
            cfg.pdftotext = need_value(a.c_str());
        } else if (a == "--pdftohtml") {
            cfg.pdftohtml = need_value(a.c_str());
        } else if (a == "--pdftoppm") {
            cfg.pdftoppm = need_value(a.c_str());
        } else if (a == "--tesseract") {
            cfg.tesseract = need_value(a.c_str());
        } else if (a == "--pandoc") {
            cfg.pandoc = need_value(a.c_str());
        } else if (a == "--libreoffice") {
            cfg.libreoffice = need_value(a.c_str());
        } else if (a == "--ebook-convert") {
            cfg.ebook_convert = need_value(a.c_str());
        } else if (a == "--no-ocr") {
            cfg.no_ocr = true;
        } else if (a == "--input-dir") {
            add_directory_inputs(cfg, need_value(a.c_str()), false);
        } else if (a == "--recursive") {
            add_directory_inputs(cfg, need_value(a.c_str()), true);
        } else if (a == "--overwrite") {
            cfg.overwrite = true;
        } else if (a == "--keep-empty") {
            cfg.keep_empty = true;
        } else if (a == "--name") {
            cfg.output_name = need_value(a.c_str());
        } else if (a == "--convert-to") {
            cfg.convert_to = need_value(a.c_str());
        } else if (a == "--timing") {
            cfg.timing = true;
        } else if (a == "--benchmark") {
            cfg.timing = true; cfg.benchmark = true;
        } else if (!a.empty() && a[0] == '-') {
            throw std::runtime_error("Unknown option: " + a);
        } else {
            cfg.inputs.emplace_back(a);
        }
    }

    if (cfg.inputs.empty()) {
        usage();
        throw std::runtime_error("No supported input files supplied.");
    }

    if (cfg.anki_only && cfg.csv_and_anki)
        throw std::runtime_error("-a and -c are mutually exclusive");

    std::sort(cfg.inputs.begin(), cfg.inputs.end());
    cfg.inputs.erase(std::unique(cfg.inputs.begin(), cfg.inputs.end()), cfg.inputs.end());

    return cfg;
}

int main(int argc, char** argv) {

    try {
        Config cfg = parse_args(argc, argv);

        for (const auto& p : cfg.inputs) {
            if (!fs::exists(p)) throw std::runtime_error("Input does not exist: " + p.string());
            if (!fs::is_regular_file(p) || !is_supported_input(p))
                throw std::runtime_error("Unsupported input file: " + p.string());
        }
        const auto& inputs = cfg.inputs;
        if (!cfg.convert_to.empty()) { if (inputs.size()!=1 || !is_spreadsheet_input(inputs.front())) throw std::runtime_error("--convert-to requires exactly one spreadsheet input"); convert_spreadsheet(cfg, inputs.front()); return 0; }

        std::atomic<size_t> next{0};
        std::vector<std::future<bool>> workers;

        const unsigned nworkers =
            std::min<unsigned>(cfg.threads, static_cast<unsigned>(inputs.size()));

        for (unsigned w = 0; w < nworkers; ++w) {
            workers.push_back(std::async(std::launch::async, [&]() {
                bool ok = true;
                while (true) {
                    size_t i = next.fetch_add(1);
                    if (i >= inputs.size()) break;
                    try {
                        size_t rows = process_one_generic(cfg, inputs[i]);
                        if (rows == 0 && !cfg.keep_empty) {
                            std::lock_guard lock(cout_mutex);
                            std::cout << "[pdfcsv] no extractable rows: " << inputs[i] << '\n';
                        }
                    } catch (const std::exception& e) {
                        std::lock_guard lock(cout_mutex);
                        std::cerr << "[pdfcsv] ERROR: " << e.what() << '\n';
                        ok = false;
                    }
                }
                return ok;
            }));
        }

        bool ok = true;
        for (auto& f : workers) ok = f.get() && ok;

        return ok ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "pdfcsv: " << e.what() << '\n';
        return 1;
    }
}
