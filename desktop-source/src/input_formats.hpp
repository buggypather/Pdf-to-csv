#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace pdfcsv {
namespace fs = std::filesystem;

struct DocumentUnit {
    int number = 1;
    std::string label;
    std::string text;
    std::vector<std::string> bold_terms;
    std::string source;
};

struct InputConfig {
    std::string pdftotext = "pdftotext";
    std::string pdftohtml = "pdftohtml";
    std::string pdftoppm = "pdftoppm";
    std::string tesseract = "tesseract";
    std::string pandoc = "pandoc";
    std::string libreoffice = "libreoffice";
    std::string ebook_convert = "ebook-convert";
};

bool is_supported_input(const fs::path& path);
std::vector<DocumentUnit> extract_document_units(const fs::path& path,
                                                  const InputConfig& cfg,
                                                  bool no_ocr);

} // namespace pdfcsv
