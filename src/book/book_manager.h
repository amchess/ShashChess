#ifndef BOOKMANAGER_H_INCLUDED
#define BOOKMANAGER_H_INCLUDED

namespace ShashChess {
namespace Book {
class Book;
}

class BookManager {
   public:
    static constexpr int NumberOfBooks = 2;

   private:
    Book::Book* books[NumberOfBooks];

   public:
    BookManager();
    virtual ~BookManager();

    BookManager(const BookManager&)            = delete;
    BookManager& operator=(const BookManager&) = delete;

    void init(const OptionsMap& options);
    void init(int index, const OptionsMap& options);
    Move probe(const Position& pos, const OptionsMap& options) const;
    void show_moves(const Position& pos, const OptionsMap& options) const;
};
}

#endif
