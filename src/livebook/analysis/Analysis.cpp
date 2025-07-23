#ifdef USE_LIVEBOOK
    #include "Analysis.h"

Analysis::Analysis() :
    depth(0) {
    this->centi_pawns = nullptr;
    this->mate        = nullptr;
    this->wdl         = nullptr;
}

Analysis::Analysis(Cp* centi_pawns_) :
    depth(0) {
    this->centi_pawns = centi_pawns_;
    this->mate        = nullptr;
    this->wdl         = nullptr;
}

Analysis::Analysis(Cp* centi_pawns_, const uint32_t depth_) :
    depth(depth_) {
    this->centi_pawns = centi_pawns_;
    this->mate        = nullptr;
    this->wdl         = nullptr;
}

Analysis::Analysis(Wdl* wdl_) :
    depth(0) {
    this->centi_pawns = nullptr;
    this->mate        = nullptr;
    this->wdl         = wdl_;
}

Analysis::Analysis(Wdl* wdl_, const uint32_t depth_) :
    depth(depth_) {
    this->centi_pawns = nullptr;
    this->mate        = nullptr;
    this->wdl         = wdl_;
}

Analysis::Analysis(Mate* mate_) :
    depth(0) {
    this->centi_pawns = nullptr;
    this->mate        = mate_;
    this->wdl         = nullptr;
}

Analysis::Analysis(Mate* mate_, const uint32_t depth_) :
    depth(depth_) {
    this->centi_pawns = nullptr;
    this->mate        = mate_;
    this->wdl         = nullptr;
}

Analysis::Analysis(Cp* centi_pawns_, Wdl* wdl_, Mate* mate_) :
    depth(0) {
    this->centi_pawns = centi_pawns_;
    this->mate        = mate_;
    this->wdl         = wdl_;
}

Analysis::Analysis(Cp* centi_pawns_, Wdl* wdl_, Mate* mate_, const uint32_t depth_) :
    depth(depth_) {
    this->centi_pawns = centi_pawns_;
    this->mate        = mate_;
    this->wdl         = wdl_;
}

uint32_t Analysis::get_depth() const { return depth; }

bool Analysis::has_centi_pawns() const { return centi_pawns != nullptr; }

Cp* Analysis::get_centi_pawns() const { return centi_pawns; }

void Analysis::set_centi_pawns(Cp* value_) { this->centi_pawns = value_; }

bool Analysis::has_wdl() const { return wdl != nullptr; }

Wdl* Analysis::get_wdl() const { return wdl; }

void Analysis::set_wdl(Wdl* value_) { this->wdl = value_; }

bool Analysis::has_mate() const { return mate != nullptr; }

Mate* Analysis::get_mate() const { return mate; }

void Analysis::set_mate(Mate* value_) { this->mate = value_; }

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

bool Analysis::operator==(const Analysis& other_) const {
    if (depth != other_.depth)
    {
        return false;
    }

    if (centi_pawns == nullptr && other_.centi_pawns != nullptr)
    {
        return false;
    }

    if (centi_pawns != nullptr && other_.centi_pawns == nullptr)
    {
        return false;
    }

    if (centi_pawns != nullptr && *centi_pawns != *other_.centi_pawns)
    {
        return false;
    }

    if (wdl == nullptr && other_.wdl != nullptr)
    {
        return false;
    }

    if (wdl != nullptr && other_.wdl == nullptr)
    {
        return false;
    }

    if (wdl != nullptr && *wdl != *other_.wdl)
    {
        return false;
    }

    if (mate == nullptr && other_.mate != nullptr)
    {
        return false;
    }

    if (mate != nullptr && other_.mate == nullptr)
    {
        return false;
    }

    if (mate != nullptr && *mate != *other_.mate)
    {
        return false;
    }

    return true;
}

bool Analysis::operator!=(const Analysis& other_) const { return !(*this == other_); }

bool Analysis::operator>(const Analysis& other_) const {
    if (depth > other_.depth)
    {
        return true;
    }

    if (depth < other_.depth)
    {
        return false;
    }

    if (centi_pawns == nullptr && other_.centi_pawns != nullptr)
    {
        return false;
    }

    if (centi_pawns != nullptr && other_.centi_pawns == nullptr)
    {
        return true;
    }

    if (centi_pawns != nullptr && *centi_pawns > *other_.centi_pawns)
    {
        return true;
    }

    if (centi_pawns != nullptr && *centi_pawns < *other_.centi_pawns)
    {
        return false;
    }

    if (wdl == nullptr && other_.wdl != nullptr)
    {
        return false;
    }

    if (wdl != nullptr && other_.wdl == nullptr)
    {
        return true;
    }

    if (wdl != nullptr && *wdl > *other_.wdl)
    {
        return true;
    }

    if (wdl != nullptr && *wdl < *other_.wdl)
    {
        return false;
    }

    if (mate == nullptr && other_.mate != nullptr)
    {
        return false;
    }

    if (mate != nullptr && other_.mate == nullptr)
    {
        return true;
    }

    if (mate != nullptr && *mate > *other_.mate)
    {
        return true;
    }

    return false;
}

bool Analysis::operator<(const Analysis& other_) const { return other_ > *this; }

bool Analysis::operator>=(const Analysis& other_) const { return !(*this < other_); }

bool Analysis::operator<=(const Analysis& other_) const { return !(*this > other_); }
#endif
