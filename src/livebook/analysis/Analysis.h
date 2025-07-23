#ifndef ANALYSIS_H
#define ANALYSIS_H
#ifdef USE_LIVEBOOK
    #include "Mate.h"
    #include "Cp.h"
    #include "Wdl.h"


class Analysis {
   public:
    Analysis();
    explicit Analysis(Cp* centi_pawns_);
    Analysis(Cp* centi_pawns_, uint32_t depth_);
    explicit Analysis(Wdl* wdl_);
    Analysis(Wdl* wdl, uint32_t depth_);
    explicit Analysis(Mate* mate_);
    Analysis(Mate* mate_, uint32_t depth_);
    Analysis(Cp* centi_pawns_, Wdl* wdl_, Mate* mate_);
    Analysis(Cp* centi_pawns_, Wdl* wdl_, Mate* mate_, uint32_t depth_);

    ~Analysis() = default;

    [[nodiscard]] uint32_t get_depth() const;

    [[nodiscard]] bool has_centi_pawns() const;
    [[nodiscard]] Cp*  get_centi_pawns() const;
    void               set_centi_pawns(Cp* value_);

    [[nodiscard]] bool has_wdl() const;
    [[nodiscard]] Wdl* get_wdl() const;
    void               set_wdl(Wdl* value_);

    [[nodiscard]] bool  has_mate() const;
    [[nodiscard]] Mate* get_mate() const;
    void                set_mate(Mate* value_);

    [[nodiscard]] Analysis* flip() const;

    bool operator==(const Analysis& other_) const;

    bool operator!=(const Analysis& other_) const;

    bool operator>(const Analysis& other_) const;

    bool operator<(const Analysis& other_) const;

    bool operator>=(const Analysis& other_) const;

    bool operator<=(const Analysis& other_) const;

   protected:
    uint32_t depth;
    Cp*      centi_pawns;
    Wdl*     wdl;
    Mate*    mate;
};

#endif  //USE_LIVEBOOK
#endif  //ANALYSIS_H
