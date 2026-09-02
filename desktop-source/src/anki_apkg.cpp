#include "anki_apkg.hpp"
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <memory>

namespace fs = std::filesystem;

namespace pdfcsv {

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string utf8_sanitize(const std::string& in) {
    std::string out; out.reserve(in.size());
    const auto* b=reinterpret_cast<const unsigned char*>(in.data()); size_t i=0;
    auto repl=[&](){out+="\xEF\xBF\xBD";};
    while(i<in.size()){
        unsigned char c=b[i]; if(c<=0x7F){out.push_back((char)c);++i;continue;}
        size_t n=0; unsigned int cp=0;
        if((c&0xE0)==0xC0){n=2;cp=c&0x1F;} else if((c&0xF0)==0xE0){n=3;cp=c&0x0F;}
        else if((c&0xF8)==0xF0){n=4;cp=c&0x07;} else {repl();++i;continue;}
        if(i+n>in.size()){repl();++i;continue;} bool ok=true;
        for(size_t j=1;j<n;++j){if((b[i+j]&0xC0)!=0x80){ok=false;break;}cp=(cp<<6)|(b[i+j]&0x3F);}
        if(!ok||(n==2&&cp<0x80)||(n==3&&cp<0x800)||(n==4&&cp<0x10000)||cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF)){repl();++i;continue;}
        out.append(in,i,n);i+=n;
    } return out;
}

static std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default: o += c;
        }
    }
    return o;
}


static std::string shell_quote(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    o += "'";
    return o;
}
static std::string sql_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\'') o += "''";
        else o += c;
    }
    return o;
}

bool AnkiPackageWriter::write(const std::string& output_apkg,
                              const std::string& deck_name,
                              const std::vector<AnkiCard>& cards,
                              std::string* error, AnkiTiming* timing) const {
    fs::path temp = fs::temp_directory_path() /
        ("pdfcsv_anki_" + std::to_string(now_ms()));
    fs::create_directories(temp);
    fs::path db = temp / "collection.anki2";

    sqlite3* conn = nullptr;
    if (sqlite3_open(db.string().c_str(), &conn) != SQLITE_OK) {
        if (error) *error = "Unable to create Anki collection database";
        return false;
    }

    auto exec = [&](const std::string& q) {
        char* msg = nullptr;
        int rc = sqlite3_exec(conn, q.c_str(), nullptr, nullptr, &msg);
        if (rc != SQLITE_OK) {
            if (error) *error = msg ? msg : "SQLite error";
            sqlite3_free(msg);
            return false;
        }
        return true;
    };

    const long long crt = now_ms();
    const std::string deck_id = std::to_string(crt);

    // Compatible baseline schema used by modern Anki collection packages.
    if (!exec("PRAGMA journal_mode=DELETE;") ||
        !exec("CREATE TABLE col (id integer primary key, crt integer not null, "
              "mod integer not null, scm integer not null, ver integer not null, "
              "dty integer not null, usn integer not null, ls integer not null, "
              "conf text not null, models text not null, decks text not null, "
              "dconf text not null, tags text not null);") ||
        !exec("CREATE TABLE notes (id integer primary key, guid text not null, "
              "mid integer not null, mod integer not null, usn integer not null, "
              "tags text not null, flds text not null, sfld integer not null, "
              "csum integer not null, flags integer not null, data text not null);") ||
        !exec("CREATE TABLE cards (id integer primary key, nid integer not null, "
              "did integer not null, ord integer not null, mod integer not null, "
              "usn integer not null, type integer not null, queue integer not null, "
              "due integer not null, ivl integer not null, factor integer not null, "
              "reps integer not null, lapses integer not null, left integer not null, "
              "odue integer not null, odid integer not null, flags integer not null, "
              "data text not null);") ||
        !exec("CREATE TABLE revlog (id integer primary key, cid integer not null, "
              "usn integer not null, ease integer not null, ivl integer not null, "
              "lastIvl integer not null, factor integer not null, time integer not null, "
              "type integer not null);") ||
        !exec("CREATE TABLE graves (usn integer not null, oid integer not null, "
              "type integer not null);")) {
        sqlite3_close(conn);
        fs::remove_all(temp);
        return false;
    }

    const std::string model_id = std::to_string(crt + 1);
    const std::string models =
        "{\"" + model_id + "\":{\"id\":" + model_id +
        ",\"name\":\"pdfcsv Basic\",\"type\":0,\"mod\":" + std::to_string(crt/1000) +
        ",\"usn\":0,\"sortf\":0,\"did\":" + deck_id +
        ",\"tmpls\":[{\"id\":" + std::to_string(crt + 4) + ",\"name\":\"Card 1\",\"ord\":0,\"qfmt\":\"{{Front}}\","
        "\"afmt\":\"{{FrontSide}}<br><br><hr id=answer><br><br>{{Back}}\","
        "\"bqfmt\":\"\",\"bafmt\":\"\",\"did\":null,\"bfont\":\"\","
        "\"bsize\":0}],\"flds\":[{\"id\":" + std::to_string(crt + 2) + ",\"name\":\"Front\",\"ord\":0,\"sticky\":false,"
        "\"rtl\":false,\"font\":\"Arial\",\"size\":20},{\"id\":" + std::to_string(crt + 3) + ",\"name\":\"Back\",\"ord\":1,"
        "\"sticky\":false,\"rtl\":false,\"font\":\"Arial\",\"size\":20}],"
        "\"css\":\".card { font-family: arial; font-size: 20px; text-align: left; max-width: 100%; overflow-wrap: anywhere; word-break: break-word; } .pdfcsv-image-wrap { width: 100%; max-width: 100%; text-align: center; overflow: hidden; margin: 0.5em 0; } .pdfcsv-image { display: inline-block; max-width: 100%; width: auto; height: auto; object-fit: contain; } .pdfcsv-table-wrap { width: 100%; max-width: 100%; overflow-x: hidden; } .pdfcsv-table { width: 100%; max-width: 100%; table-layout: fixed; border-collapse: collapse; } .pdfcsv-table th, .pdfcsv-table td { border: 1px solid #999; padding: 0.35em; vertical-align: top; overflow-wrap: anywhere; word-break: break-word; white-space: normal; }\"}}";

    const std::string decks =
        "{\"1\":{\"id\":1,\"mod\":" + std::to_string(crt/1000) +
        ",\"name\":\"Default\",\"usn\":0,\"lrnToday\":[0,0],\"revToday\":[0,0],"
        "\"newToday\":[0,0],\"timeToday\":[0,0],\"collapsed\":false,\"browserCollapsed\":false,"
        "\"desc\":\"\",\"dyn\":0,\"conf\":1,\"extendNew\":0,\"extendRev\":0,"
        "\"reviewLimit\":null,\"newLimit\":null,\"reviewLimitToday\":null,\"newLimitToday\":null},\"" +
        deck_id + "\":{\"id\":" + deck_id + ",\"mod\":" + std::to_string(crt/1000) +
        ",\"name\":\"" + json_escape(utf8_sanitize(deck_name)) + "\",\"usn\":0,\"lrnToday\":[0,0],\"revToday\":[0,0],"
        "\"newToday\":[0,0],\"timeToday\":[0,0],\"collapsed\":false,\"browserCollapsed\":false,"
        "\"desc\":\"\",\"dyn\":0,\"conf\":1,\"extendNew\":0,\"extendRev\":0,"
        "\"reviewLimit\":null,\"newLimit\":null,\"reviewLimitToday\":null,\"newLimitToday\":null}}";

    const std::string conf =
        "{\"1\":{\"id\":1,\"name\":\"Default\",\"usn\":0,\"mod\":" + std::to_string(crt/1000) +
        ",\"new\":{\"delays\":[1,10],\"ints\":[1,4],\"initialFactor\":2500},"
        "\"lapse\":{\"delays\":[10],\"mult\":0.0,\"minInt\":1,\"leechFails\":8},"
        "\"rev\":{\"perDay\":200,\"ease4\":1.3,\"fuzz\":0.05,\"ivlFct\":1.0,\"maxIvl\":36500,\"hardFactor\":1.2},"
        "\"dyn\":false,\"newMix\":0,\"newPerDayMinimum\":0,\"interdayLearningMix\":0,\"maxTaken\":60,\"autoplay\":true,\"timer\":0,\"replayq\":true}}";

    std::string col =
        "INSERT INTO col VALUES(1," + std::to_string(crt/1000) + "," +
        std::to_string(crt/1000) + "," + std::to_string(crt) +
        ",11,0,0,0,'{}','" + sql_escape(models) + "','" +
        sql_escape(decks) + "','" + sql_escape(conf) + "','{}');";
    if (!exec(col)) {
        sqlite3_close(conn); fs::remove_all(temp); return false;
    }

    const auto db_start = std::chrono::steady_clock::now();
    if (!exec("BEGIN IMMEDIATE TRANSACTION;")) { sqlite3_close(conn); fs::remove_all(temp); return false; }

    sqlite3_stmt* note_stmt = nullptr;
    sqlite3_stmt* card_stmt = nullptr;
    const char* note_sql =
        "INSERT INTO notes VALUES(?,?,?,?,?,?,?,?,?,?,?);";
    const char* card_sql =
        "INSERT INTO cards VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(conn, note_sql, -1, &note_stmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(conn, card_sql, -1, &card_stmt, nullptr) != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(conn);
        if (note_stmt) sqlite3_finalize(note_stmt);
        if (card_stmt) sqlite3_finalize(card_stmt);
        sqlite3_exec(conn, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(conn); fs::remove_all(temp); return false;
    }

    long long nid = crt * 1000;
    long long cid = nid;
    std::vector<std::pair<std::string,std::string>> media;
    auto rollback = [&]() {
        sqlite3_finalize(note_stmt); sqlite3_finalize(card_stmt);
        sqlite3_exec(conn, "ROLLBACK;", nullptr, nullptr, nullptr);
    };

    for (const auto& card : cards) {
        ++nid; ++cid;
        const std::string guid = std::to_string(nid);
        for (const auto& m : card.media_files) {
            fs::path p(m);
            if (!fs::exists(p) || !fs::is_regular_file(p)) {
                if (error) *error = "Missing Anki media file: " + p.string();
                rollback(); sqlite3_close(conn); fs::remove_all(temp); return false;
            }
            const std::string filename = p.filename().string();
            bool found = false;
            for (const auto& existing : media)
                if (fs::path(existing.second).filename().string() == filename) { found = true; break; }
            if (!found) media.emplace_back(std::to_string(media.size()), p.string());
        }
        const std::string front = utf8_sanitize(card.front);
        const std::string back = utf8_sanitize(card.back);
        const std::string tags = utf8_sanitize(card.tags);
        const std::string flds = front + "\x1f" + back;
        const int checksum = static_cast<int>(std::hash<std::string>{}(front) & 0x7fffffff);

        sqlite3_bind_int64(note_stmt,1,nid); sqlite3_bind_text(note_stmt,2,guid.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(note_stmt,3,std::stoll(model_id)); sqlite3_bind_int64(note_stmt,4,crt/1000);
        sqlite3_bind_int(note_stmt,5,-1); sqlite3_bind_text(note_stmt,6,tags.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(note_stmt,7,flds.c_str(),-1,SQLITE_TRANSIENT); sqlite3_bind_int64(note_stmt,8,0);
        sqlite3_bind_int(note_stmt,9,checksum); sqlite3_bind_int(note_stmt,10,0); sqlite3_bind_text(note_stmt,11,"",-1,SQLITE_STATIC);
        if (sqlite3_step(note_stmt) != SQLITE_DONE) {
            if (error) *error = sqlite3_errmsg(conn); rollback(); sqlite3_close(conn); fs::remove_all(temp); return false;
        }
        sqlite3_reset(note_stmt); sqlite3_clear_bindings(note_stmt);

        sqlite3_bind_int64(card_stmt,1,cid); sqlite3_bind_int64(card_stmt,2,nid); sqlite3_bind_int64(card_stmt,3,std::stoll(deck_id));
        sqlite3_bind_int(card_stmt,4,0); sqlite3_bind_int64(card_stmt,5,crt/1000); sqlite3_bind_int(card_stmt,6,-1);
        sqlite3_bind_int(card_stmt,7,0); sqlite3_bind_int(card_stmt,8,0); sqlite3_bind_int64(card_stmt,9,nid);
        sqlite3_bind_int(card_stmt,10,0); sqlite3_bind_int(card_stmt,11,0); sqlite3_bind_int(card_stmt,12,0);
        sqlite3_bind_int(card_stmt,13,0); sqlite3_bind_int(card_stmt,14,0); sqlite3_bind_int64(card_stmt,15,0);
        sqlite3_bind_int64(card_stmt,16,0); sqlite3_bind_int(card_stmt,17,0); sqlite3_bind_text(card_stmt,18,"",-1,SQLITE_STATIC);
        if (sqlite3_step(card_stmt) != SQLITE_DONE) {
            if (error) *error = sqlite3_errmsg(conn); rollback(); sqlite3_close(conn); fs::remove_all(temp); return false;
        }
        sqlite3_reset(card_stmt); sqlite3_clear_bindings(card_stmt);
    }

    sqlite3_finalize(note_stmt); sqlite3_finalize(card_stmt);
    if (!exec("COMMIT;")) { sqlite3_exec(conn,"ROLLBACK;",nullptr,nullptr,nullptr); sqlite3_close(conn); fs::remove_all(temp); return false; }
    if (timing) {
        timing->database_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-db_start).count();
        timing->notes = cards.size(); timing->cards = cards.size(); timing->media_files = media.size();
    }


    const auto media_start = std::chrono::steady_clock::now();
    // Package using Anki's legacy .apkg layout. Current Anki exports a
    // collection.anki21 plus a dummy collection.anki2 for older clients,
    // followed by numeric media members and a JSON media map.
    fs::path package_dir = temp / "package";
    fs::create_directories(package_dir);
    fs::copy_file(db, package_dir/"collection.anki21", fs::copy_options::overwrite_existing);
    // Match Anki's compatibility strategy: collection.anki21 is the real
    // collection. collection.anki2 is deliberately tiny and only exists so
    // legacy clients do not mistake the package for an old-format export.
    // It advertises schema 11 but contains no user notes/cards.
    {
        fs::path dummy=package_dir/"collection.anki2";
        sqlite3* d=nullptr;
        if(sqlite3_open(dummy.string().c_str(),&d)!=SQLITE_OK){
            if(error)*error="Unable to create compatibility collection.anki2";
            if(d)sqlite3_close(d); fs::remove_all(temp); return false;
        }
        const char* dummy_sql=
            "CREATE TABLE col (id integer primary key, crt integer not null, mod integer not null, scm integer not null, ver integer not null, dty integer not null, usn integer not null, ls integer not null, conf text not null, models text not null, decks text not null, dconf text not null, tags text not null);"
            "CREATE TABLE notes (id integer primary key, guid text not null, mid integer not null, mod integer not null, usn integer not null, tags text not null, flds text not null, sfld integer not null, csum integer not null, flags integer not null, data text not null);"
            "CREATE TABLE cards (id integer primary key, nid integer not null, did integer not null, ord integer not null, mod integer not null, usn integer not null, type integer not null, queue integer not null, due integer not null, ivl integer not null, factor integer not null, reps integer not null, lapses integer not null, left integer not null, odue integer not null, odid integer not null, flags integer not null, data text not null);"
            "CREATE TABLE revlog (id integer primary key, cid integer not null, usn integer not null, ease integer not null, ivl integer not null, lastIvl integer not null, factor integer not null, time integer not null, type integer not null);"
            "CREATE TABLE graves (usn integer not null, oid integer not null, type integer not null);"
            "INSERT INTO col VALUES(1,0,0,0,11,0,0,0,'{}','{}','{}','{}','{}');";
        char* msg=nullptr; int drc=sqlite3_exec(d,dummy_sql,nullptr,nullptr,&msg);
        if(drc!=SQLITE_OK){if(error)*error=msg?msg:"Unable to initialize compatibility database";sqlite3_free(msg);sqlite3_close(d);fs::remove_all(temp);return false;}
        sqlite3_close(d);
    }

    std::ostringstream mj;
    mj << "{";
    for (size_t i=0; i<media.size(); ++i) {
        if (i) mj << ",";
        mj << "\"" << media[i].first << "\":\""
           << json_escape(fs::path(media[i].second).filename().string()) << "\"";
        fs::copy_file(media[i].second, package_dir/media[i].first,
                      fs::copy_options::overwrite_existing);
    }
    mj << "}";
    { std::ofstream mf(package_dir/"media", std::ios::binary); mf << mj.str(); }

    // Validate every referenced media filename has a manifest entry before
    // touching the final output. This prevents silent broken-image cards.
    for (const auto& card : cards) {
        for (const auto& m : card.media_files) {
            const std::string filename = fs::path(m).filename().string();
            bool mapped = false;
            for (const auto& entry : media)
                if (fs::path(entry.second).filename().string() == filename) { mapped = true; break; }
            if (!mapped) {
                if (error) *error = "Anki media validation failed for: " + filename;
                fs::remove_all(temp); return false;
            }
        }
    }

    std::error_code ec;
    fs::remove(output_apkg, ec);
    const std::string out_abs = fs::absolute(output_apkg).string();
    std::string cmd = "cd " + shell_quote(package_dir.string()) + " && zip -q " +
        shell_quote(out_abs) + " collection.anki21 collection.anki2 media";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        if (error) *error = "Unable to create .apkg: local zip utility failed";
        fs::remove_all(temp); return false;
    }
    if (!media.empty()) {
        // Anki stores ordinary media uncompressed in legacy packages.
        cmd = "cd " + shell_quote(package_dir.string()) + " && zip -q -0 " +
            shell_quote(out_abs);
        for (const auto& entry : media) cmd += " " + shell_quote(entry.first);
        rc = std::system(cmd.c_str());
        if (rc != 0) {
            if (error) *error = "Unable to add Anki media: local zip utility failed";
            fs::remove_all(temp); return false;
        }
    }

    if (timing) {
        timing->media_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-media_start).count();
        timing->packaging_seconds = timing->media_seconds;
        timing->total_seconds = timing->database_seconds + timing->packaging_seconds;
    }
    fs::remove_all(temp);
    return true;
}

} // namespace pdfcsv
