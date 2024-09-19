#ifndef ANALYSIS_H
#define ANALYSIS_H
#ifdef USE_LIVEBOOK
    #include "Mate.h"
    #include "Cp.h"
    #include "Wdl.h"


class Analysis {
   public:
    Analysis();
    explicit Analysis(Cp* centi_pawns);
    Analysis(Cp* centi_pawns, uint32_t depth);
    explicit Analysis(Wdl* wdl);
    Analysis(Wdl* wdl, uint32_t depth);
    explicit Analysis(Mate* mate);
    Analysis(Mate* mate, uint32_t depth);
    Analysis(Cp* centi_pawns, Wdl* wdl, Mate* mate);
    Analysis(Cp* centi_pawns, Wdl* wdl, Mate* mate, uint32_t depth);

    ~Analysis() = default;

    [[nodiscard]] uint32_t get_depth() const;

    [[nodiscard]] bool has_centi_pawns() const;
    [[nodiscard]] Cp*  get_centi_pawns() const;
    void               set_centi_pawns(Cp* value);

    [[nodiscard]] bool has_wdl() const;
    [[nodiscard]] Wdl* get_wdl() const;
    void               set_wdl(Wdl* value);

    [[nodiscard]] bool  has_mate() const;
    [[nodiscard]] Mate* get_mate() const;
    void                set_mate(Mate* value);

    [[nodiscard]] Analysis* flip() const;

    bool operator==(const Analysis& other) const;

    bool operator!=(const Analysis& other) const;

    bool operator>(const Analysis& other) const;

    bool operator<(const Analysis& other) const;

    bool operator>=(const Analysis& other) const;

    bool operator<=(const Analysis& other) const;

   protected:
    uint32_t depth;
    Cp*      centi_pawns;
    Wdl*     wdl;
    Mate*    mate;
};

#endif  //USE_LIVEBOOK
#endif  //ANALYSIS_H
