#include "../misc.h"
#include "../uci.h"
#include "polyglot/polyglot.h"
#include "ctg/ctg.h"
#include "book.h"

namespace ShashChess {
namespace Book {
/*static*/ Book* Book::create_book(const std::string& filename) {
    size_t extIndex = filename.find_last_of('.');
    if (extIndex == std::string::npos)
        return nullptr;

    std::string ext = filename.substr(extIndex + 1);

    if (ext == "ctg" || ext == "cto" || ext == "ctb")
        return new CTG::CtgBook();
    else if (ext == "bin")
        return new Polyglot::PolyglotBook();
    else
        return nullptr;
}
}
}