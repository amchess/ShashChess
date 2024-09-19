#ifndef CP_H
#define CP_H
#ifdef USE_LIVEBOOK
    #include <cstdint>

class Cp {
   public:
    explicit Cp(int32_t score);
    ~Cp() = default;

    [[nodiscard]] Cp* opponent() const;

    [[nodiscard]] int32_t get_score() const;

    [[nodiscard]] Cp* flip() const;

    bool operator==(const Cp& other) const;

    bool operator!=(const Cp& other) const;

    bool operator<(const Cp& other) const;

    bool operator>(const Cp& other) const;

    bool operator<=(const Cp& other) const;

    bool operator>=(const Cp& other) const;

   protected:
    int32_t score;
};


#endif
#endif  //CP_H
