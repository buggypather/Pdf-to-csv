#pragma once
#include <string>
#include <vector>

namespace pdfcsv {

struct AnkiTiming { double database_seconds=0; double media_seconds=0; double packaging_seconds=0; double total_seconds=0; size_t notes=0; size_t cards=0; size_t media_files=0; };

struct AnkiCard {
    std::string front;
    std::string back;
    std::string tags;
    // Paths are local source files. The basename is used as the HTML/media filename.
    std::vector<std::string> media_files;
};

class AnkiPackageWriter {
public:
    bool write(const std::string& output_apkg,
               const std::string& deck_name,
               const std::vector<AnkiCard>& cards,
               std::string* error = nullptr, AnkiTiming* timing = nullptr) const;
};

} // namespace pdfcsv
