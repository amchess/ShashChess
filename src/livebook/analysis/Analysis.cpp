#ifdef USE_LIVEBOOK
    #include "Analysis.h"

Analysis::Analysis() :
    depth(0) {
    this->centi_pawns = nullptr;
    this->mate        = nullptr;
    this->wdl         = nullptr;
}

Analysis::Analysis(Cp* centi_pawns) :
    depth(0) {
    this->centi_pawns = centi_pawns;
    this->mate        = nullptr;
    this->wdl         = nullptr;
}

Analysis::Analysis(Cp* centi_pawns, const uint32_t depth) :
    depth(depth) {
    this->centi_pawns = centi_pawns;
    this->mate        = nullptr;
    this->wdl         = nullptr;
}

Analysis::Analysis(Wdl* wdl) :
    depth(0) {
    this->centi_pawns = nullptr;
    this->mate        = nullptr;
    this->wdl         = wdl;
}

Analysis::Analysis(Wdl* wdl, const uint32_t depth) :
    depth(depth) {
    this->centi_pawns = nullptr;
    this->mate        = nullptr;
    this->wdl         = wdl;
}

Analysis::Analysis(Mate* mate) :
    depth(0) {
    this->centi_pawns = nullptr;
    this->mate        = mate;
    this->wdl         = nullptr;
}

Analysis::Analysis(Mate* mate, const uint32_t depth) :
    depth(depth) {
    this->centi_pawns = nullptr;
    this->mate        = mate;
    this->wdl         = nullptr;
}

Analysis::Analysis(Cp* centi_pawns, Wdl* wdl, Mate* mate) :
    depth(0) {
    this->centi_pawns = centi_pawns;
    this->mate        = mate;
    this->wdl         = wdl;
}

Analysis::Analysis(Cp* centi_pawns, Wdl* wdl, Mate* mate, const uint32_t depth) :
    depth(depth) {
    this->centi_pawns = centi_pawns;
    this->mate        = mate;
    this->wdl         = wdl;
}

uint32_t Analysis::get_depth() const { return depth; }

bool Analysis::has_centi_pawns() const { return centi_pawns != nullptr; }

Cp* Analysis::get_centi_pawns() const { return centi_pawns; }

void Analysis::set_centi_pawns(Cp* value) { this->centi_pawns = value; }

bool Analysis::has_wdl() const { return wdl != nullptr; }

Wdl* Analysis::get_wdl() const { return wdl; }

void Analysis::set_wdl(Wdl* value) { this->wdl = value; }

bool Analysis::has_mate() const { return mate != nullptr; }

Mate* Analysis::get_mate() const { return mate; }

void Analysis::set_mate(Mate* value) { this->mate = value; }

Analysis* Analysis::flip() const {
    auto ret = new Analysis();

    if (centi_pawns != nullptr)
    {
        ret->set_centi_pawns(centi_pawns->flip());
    }

    if (wdl != nullptr)
    {
        ret->set_wdl(wdl->flip());
    }

    if (mate != nullptr)
    {
        ret->set_mate(mate->flip());
    }

    return ret;
}

bool Analysis::operator==(const Analysis& other) const {
    if (depth != other.depth)
    {
        return false;
    }

    if (centi_pawns == nullptr && other.centi_pawns != nullptr)
    {
        return false;
    }

    if (centi_pawns != nullptr && other.centi_pawns == nullptr)
    {
        return false;
    }

    if (centi_pawns != nullptr && *centi_pawns != *other.centi_pawns)
    {
        return false;
    }

    if (wdl == nullptr && other.wdl != nullptr)
    {
        return false;
    }

    if (wdl != nullptr && other.wdl == nullptr)
    {
        return false;
    }

    if (wdl != nullptr && *wdl != *other.wdl)
    {
        return false;
    }

    if (mate == nullptr && other.mate != nullptr)
    {
        return false;
    }

    if (mate != nullptr && other.mate == nullptr)
    {
        return false;
    }

    if (mate != nullptr && *mate != *other.mate)
    {
        return false;
    }

    return true;
}

bool Analysis::operator!=(const Analysis& other) const { return !(*this == other); }

bool Analysis::operator>(const Analysis& other) const {
    if (depth > other.depth)
    {
        return true;
    }

    if (depth < other.depth)
    {
        return false;
    }

    if (centi_pawns == nullptr && other.centi_pawns != nullptr)
    {
        return false;
    }

    if (centi_pawns != nullptr && other.centi_pawns == nullptr)
    {
        return true;
    }

    if (centi_pawns != nullptr && *centi_pawns > *other.centi_pawns)
    {
        return true;
    }

    if (centi_pawns != nullptr && *centi_pawns < *other.centi_pawns)
    {
        return false;
    }

    if (wdl == nullptr && other.wdl != nullptr)
    {
        return false;
    }

    if (wdl != nullptr && other.wdl == nullptr)
    {
        return true;
    }

    if (wdl != nullptr && *wdl > *other.wdl)
    {
        return true;
    }

    if (wdl != nullptr && *wdl < *other.wdl)
    {
        return false;
    }

    if (mate == nullptr && other.mate != nullptr)
    {
        return false;
    }

    if (mate != nullptr && other.mate == nullptr)
    {
        return true;
    }

    if (mate != nullptr && *mate > *other.mate)
    {
        return true;
    }

    return false;
}

bool Analysis::operator<(const Analysis& other) const { return other > *this; }

bool Analysis::operator>=(const Analysis& other) const { return !(*this < other); }

bool Analysis::operator<=(const Analysis& other) const { return !(*this > other); }
#endif
