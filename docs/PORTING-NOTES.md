# Android porting notes

The desktop implementation is CLI-oriented and uses `std::system()` to invoke
Poppler, Pandoc, LibreOffice, Calibre and `zip`. Android apps cannot assume
those executables exist.

The Android port therefore separates the pipeline into:

1. Android-native document decoding.
2. JNI/C++ semantic extraction.
3. Android SQLite/ZIP packaging.

This also removes the desktop writer's dependency on a shell `zip` command.

## Planned next steps

1. Add an Android-native PDF text engine.
2. Preserve EPUB/DOCX embedded images and map them to Anki media.
3. Add PPTX/XLSX/ODS native ZIP/XML readers where practical.
4. Expand the JNI semantic extractor until it shares the desktop engine rather
   than the small Android-safe definition extractor.
5. Add Android share/export intents and public-document save location.
6. Add instrumented import-structure tests for generated APKG files.
