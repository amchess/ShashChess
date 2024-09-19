#ifndef MATE_H
#define MATE_H
#ifdef USE_LIVEBOOK
    #include <cstdint>


class Mate {
   public:
    explicit Mate(int32_t mate);
    ~Mate() = default;

    [[nodiscard]] Mate* opponent() const;

    [[nodiscard]] int32_t get_mate() const;

    [[nodiscard]] Mate* flip() const;

    bool operator==(const Mate& other) const;

    bool operator!=(const Mate& other) const;

    bool operator<(const Mate& other) const;

    bool operator>(const Mate& other) const;

    bool operator<=(const Mate& other) const;

    bool operator>=(const Mate& other) const;

   protected:
    int32_t mate;
};

#endif
#endif  //MATE_H