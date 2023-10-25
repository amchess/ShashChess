#include "../misc.h"
#include "../uci.h"
#include "polyglot/polyglot.h"
#include "ctg/ctg.h"
#include "book.h"

using namespace std;

namespace Stockfish::Book {
namespace {
Book* create_book(const string& filename) {
    size_t extIndex = filename.find_last_of('.');
    if (extIndex == string::npos)
        return nullptr;

    string ext = filename.substr(extIndex + 1);

    if (ext == "ctg" || ext == "cto" || ext == "ctb")
        return new CTG::CtgBook();
    else if (ext == "bin")
        return new Polyglot::PolyglotBook();
    else
        return nullptr;
}
}

constexpr size_t NumBooks = 2;
Book*            books[NumBooks];

void init() {
    for (size_t i = 0; i < NumBooks; ++i)
        books[i] = nullptr;

    on_book(0, (string) Options["CTG/BIN Book 1 File"]);
    on_book(1, (string) Options["CTG/BIN Book 2 File"]);
}

void finalize() {
    for (size_t i = 0; i < NumBooks; ++i)
    {
        delete books[i];
        books[i] = nullptr;
    }
}

void on_book(int index, const string& filename) {
    //Close previous book if any
    delete books[index];
    books[index] = nullptr;

    //Load new book
    if (Utility::is_empty_filename(filename))
        return;

    //Create book object for the given book type
    string fn   = Utility::map_path(filename);
    Book*  book = create_book(fn);
    if (book == nullptr)
    {
        sync_cout << "info string Unknown book type: " << filename << sync_endl;
        return;
    }

    //Open/Initialize the book
    if (!book->open(fn))
    {
        delete book;
        return;
    }

    books[index] = book;
}

Move probe(const Position& pos) {
    int  moveNumber = 1 + pos.game_ply() / 2;
    Move bookMove   = MOVE_NONE;

    for (size_t i = 0; i < NumBooks; ++i)
    {
        if (books[i] != nullptr
            && (int) Options[Utility::format_string("Book %d Depth", i + 1)] >= moveNumber)
        {
            bookMove = books[i]->probe(
              pos, (size_t) (int) Options[Utility::format_string("Book %d Width", i + 1)],
              (bool) Options[Utility::format_string("(CTG) Book %d Only Green", i + 1)]);
            if (bookMove != MOVE_NONE)
                break;
        }
    }

    return bookMove;
}

void show_moves(const Position& pos) {
    cout << pos << endl << endl;

    for (size_t i = 0; i < NumBooks; ++i)
    {
        if (books[i] == nullptr)
        {
            cout << "Book " << i + 1 << ": No book loaded" << endl;
        }
        else
        {
            cout << "Book " << i + 1 << " (" << books[i]->type() << "): "
                 << (std::string) Options[Utility::format_string("CTG/BIN Book %d File", i + 1)]
                 << endl;
            books[i]->show_moves(pos);
        }
    }
}
}