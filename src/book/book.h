#ifndef BOOK_H_INCLUDED
#define BOOK_H_INCLUDED

namespace ShashChess {
class BookManager;

namespace Book {
namespace {
static const union {
    uint32_t i;
    char     c[4];
} Le                          = {0x01020304};
static const bool IsBigEndian = (Le.c[0] == 1);
}

class BookUtil {
   public:
    template<typename IntType>
    static IntType read_big_endian(const unsigned char* buffer, size_t& offset, size_t bufferLen) {
        IntType          result;
        constexpr size_t typeSize = sizeof(IntType);

        if (offset + typeSize > bufferLen)
        {
            assert(false);
            return IntType();
        }

        //Read the integer type and convert (if needed)
        memcpy(&result, buffer + offset, typeSize);

        if (!IsBigEndian)
        {
            unsigned char                              u[typeSize];
            typename std::make_unsigned<IntType>::type v = 0;

            memcpy(&u, &result, typeSize);
            for (size_t i = 0; i < typeSize; ++i)
                v = (v << 8) | u[i];

            memcpy(&result, &v, typeSize);
        }

        offset += typeSize;
        return result;
    }

    template<typename IntType>
    static IntType read_big_endian(const unsigned char* buffer, size_t bufferLen) {
        size_t offset = 0;
        return read_big_endian<IntType>(buffer, offset, bufferLen);
    }
};

class Book {
    friend class ShashChess::BookManager;

   private:
    static Book* create_book(const std::string& filename);

   public:
    Book() {}
    virtual ~Book() {}

    Book(const Book&)            = delete;
    Book& operator=(const Book&) = delete;

    virtual std::string type() const = 0;

    virtual bool open(const std::string& filename) = 0;
    virtual void close()                           = 0;

    virtual Move probe(const Position& pos, size_t width, bool onlyGreen) const = 0;
    virtual void show_moves(const Position& pos) const                          = 0;
};
}
}

#endif  // #ifndef BOOK_H_INCLUDED
